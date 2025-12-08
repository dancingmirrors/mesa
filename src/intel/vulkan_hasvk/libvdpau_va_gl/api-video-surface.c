/*
 * Copyright 2013-2014  Rinat Ibragimov
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#define GL_GLEXT_PROTOTYPES
#include "ctx-stack.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include "compat.h"
#include "shaders.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#ifdef HAVE_VA_DRM_PRIME
#include <va/va_drmcommon.h>
#endif
#include <vdpau/vdpau.h>
#include "api.h"
#include "trace.h"
#include "globals.h"


VdpStatus
vdpVideoSurfaceCreate(VdpDevice device, VdpChromaType chroma_type, uint32_t width, uint32_t height,
                      VdpVideoSurface *surface)
{
    VdpStatus err_code;
    if (!surface)
        return VDP_STATUS_INVALID_POINTER;
    if (chroma_type != VDP_CHROMA_TYPE_420 &&
        chroma_type != VDP_CHROMA_TYPE_422 &&
        chroma_type != VDP_CHROMA_TYPE_444)
    {
        return VDP_STATUS_INVALID_CHROMA_TYPE;
    }

    VdpDeviceData *deviceData = handle_acquire(device, HANDLETYPE_DEVICE);
    if (NULL == deviceData)
        return VDP_STATUS_INVALID_HANDLE;

    VdpVideoSurfaceData *data = calloc(1, sizeof(VdpVideoSurfaceData));
    if (NULL == data) {
        err_code = VDP_STATUS_RESOURCES;
        goto quit;
    }

    data->type = HANDLETYPE_VIDEO_SURFACE;
    data->device = device;
    data->deviceData = deviceData;
    data->chroma_type = chroma_type;
    data->width = width;
    data->height = height;

    switch (chroma_type) {
    case VDP_CHROMA_TYPE_420:
        data->chroma_width = ((width + 1) & (~1u)) / 2;
        data->chroma_height = ((height + 1) & (~1u)) / 2;
        data->stride = (width + 0xfu) & (~0xfu);
        break;
    case VDP_CHROMA_TYPE_422:
        data->chroma_width = ((width + 1) & (~1u)) / 2;
        data->chroma_height = height;
        data->stride = (width + 2 * data->chroma_width + 0xfu) & (~0xfu);
        break;
    case VDP_CHROMA_TYPE_444:
        data->chroma_width = width;
        data->chroma_height = height;
        data->stride = (4 * width + 0xfu) & (~0xfu);
        break;
    }
    data->chroma_stride = (data->chroma_width + 0xfu) & (~0xfu);

    if (unlikely(global.quirks.log_stride)) {
        traceInfo("hasvk: vdpVideoSurfaceCreate - Surface parameters:\n");
        traceInfo("  Size: %ux%u\n", width, height);
        traceInfo("  Chroma type: %d\n", chroma_type);
        traceInfo("  Y stride: %u\n", data->stride);
        traceInfo("  Chroma size: %ux%u\n", data->chroma_width, data->chroma_height);
        traceInfo("  Chroma stride: %u\n", data->chroma_stride);
    }

    data->va_surf = VA_INVALID_SURFACE;
    data->tex_id = 0;
    data->sync_va_to_glx = 0;
    data->decoder = VDP_INVALID_HANDLE;
    data->y_plane = NULL;
    data->u_plane = NULL;
    data->v_plane = NULL;

    glx_ctx_push_thread_local(deviceData);
    glGenTextures(1, &data->tex_id);
    glBindTexture(GL_TEXTURE_2D, data->tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, data->width, data->height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &data->fbo_id);
    glBindFramebuffer(GL_FRAMEBUFFER, data->fbo_id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, data->tex_id, 0);
    GLenum gl_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (GL_FRAMEBUFFER_COMPLETE != gl_status) {
        traceError("error (%s): framebuffer not ready, %d, %s\n", __func__, gl_status,
                   gluErrorString(gl_status));
        glx_ctx_pop();
        free(data);
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }
    glFinish();

    GLenum gl_error = glGetError();
    glx_ctx_pop();
    if (GL_NO_ERROR != gl_error) {
        traceError("error (%s): gl error %d\n", __func__, gl_error);
        free(data);
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    // no VA surface creation here. Actual pool of VA surfaces should be allocated already
    // by VdpDecoderCreate. VdpDecoderCreate will update ->va_surf field as needed.

    ref_device(deviceData);
    *surface = handle_insert(data);

    err_code = VDP_STATUS_OK;
quit:
    handle_release(device);
    return err_code;
}

VdpStatus
vdpVideoSurfaceDestroy(VdpVideoSurface surface)
{
    VdpVideoSurfaceData *videoSurfData = handle_acquire(surface, HANDLETYPE_VIDEO_SURFACE);
    if (NULL == videoSurfData)
        return VDP_STATUS_INVALID_HANDLE;
    VdpDeviceData *deviceData = videoSurfData->deviceData;

    glx_ctx_push_thread_local(deviceData);
    glDeleteTextures(1, &videoSurfData->tex_id);
    GLenum gl_error = glGetError();
    glx_ctx_pop();

    if (GL_NO_ERROR != gl_error) {
        traceError("error (%s): gl error %d\n", __func__, gl_error);
        handle_release(surface);
        return VDP_STATUS_ERROR;
    }

    if (deviceData->va_available) {
        // return VA surface to the free list
        if (videoSurfData->decoder != VDP_INVALID_HANDLE) {
            VdpDecoderData *dd = handle_acquire(videoSurfData->decoder, HANDLETYPE_DECODER);
            if (NULL != dd) {
                free_list_push(dd->free_list, &dd->free_list_head, videoSurfData->rt_idx);
                handle_release(videoSurfData->decoder);
            }
        }
        // .va_surf will be freed in VdpDecoderDestroy
    }

    if (videoSurfData->y_plane)
        free(videoSurfData->y_plane);
    if (videoSurfData->u_plane)
        free(videoSurfData->u_plane);
    // do not free videoSurfData->v_plane, it's just pointer into the middle of u_plane

    unref_device(deviceData);
    handle_expunge(surface);
    free(videoSurfData);
    return VDP_STATUS_OK;
}

VdpStatus
vdpVideoSurfaceGetBitsYCbCr(VdpVideoSurface surface,
                            VdpYCbCrFormat destination_ycbcr_format,
                            void *const *destination_data, uint32_t const *destination_pitches)
{
    VdpStatus err_code;
    if (!destination_data || !destination_pitches)
        return VDP_STATUS_INVALID_POINTER;
    VdpVideoSurfaceData *srcSurfData = handle_acquire(surface, HANDLETYPE_VIDEO_SURFACE);
    if (NULL == srcSurfData)
        return VDP_STATUS_INVALID_HANDLE;
    VdpDeviceData *deviceData = srcSurfData->deviceData;
    VADisplay va_dpy = deviceData->va_dpy;

    if (deviceData->va_available) {
        VAImage q;

        vaDeriveImage(va_dpy, srcSurfData->va_surf, &q);

        if (unlikely(global.quirks.log_stride)) {
            const char *fourcc_str = (const char *)&q.format.fourcc;
            traceInfo("hasvk: vdpVideoSurfaceGetBitsYCbCr - VA-API Image Info:\n");
            traceInfo("  FOURCC: %c%c%c%c (0x%08x)\n",
                     fourcc_str[0], fourcc_str[1], fourcc_str[2], fourcc_str[3],
                     q.format.fourcc);
            traceInfo("  Surface size: %ux%u\n", srcSurfData->width, srcSurfData->height);
            traceInfo("  Image dimensions: %ux%u\n", q.width, q.height);
            traceInfo("  Num planes: %u\n", q.num_planes);
            for (unsigned int i = 0; i < q.num_planes && i < 3; i++) {
                traceInfo("  Plane[%u]: pitch=%u offset=%u\n",
                         i, q.pitches[i], q.offsets[i]);
            }
            traceInfo("  Destination format: %s\n",
                     reverse_ycbcr_format(destination_ycbcr_format));
            traceInfo("  Destination pitches: [0]=%u [1]=%u [2]=%u\n",
                     destination_pitches[0], destination_pitches[1],
                     destination_pitches[2]);
        }

        if (VA_FOURCC('N', 'V', '1', '2') == q.format.fourcc &&
            VDP_YCBCR_FORMAT_NV12 == destination_ycbcr_format)
        {
            uint8_t *img_data;
            vaMapBuffer(va_dpy, q.buf, (void **)&img_data);

            /* Note: q.pitches[] may be larger than srcSurfData->width due to row padding.
             * When pitches match destination_pitches it's safe to bulk-copy the entire
             * plane including padding (q.pitches[0] * height). Otherwise fall back
             * to row-by-row copy using the reported pitches.
             */

            uint8_t *src_y = img_data + q.offsets[0];
            uint8_t *src_uv = img_data + q.offsets[1];
            uint8_t *dst_y = destination_data[0];
            uint8_t *dst_uv = destination_data[1];

            if (destination_pitches[0] == q.pitches[0] &&
                destination_pitches[1] == q.pitches[1])
            {
                /* copy full rows including padding */
                size_t y_total = (size_t)q.pitches[0] * (size_t)srcSurfData->height;
                size_t uv_total = (size_t)q.pitches[1] * (size_t)(srcSurfData->height / 2);

                if (unlikely(global.quirks.log_stride)) {
                    traceInfo("  NV12: Using bulk copy (pitches match)\n");
                    traceInfo("    Y plane: copying %zu bytes (%u x %u)\n",
                             y_total, q.pitches[0], srcSurfData->height);
                    traceInfo("    UV plane: copying %zu bytes (%u x %u)\n",
                             uv_total, q.pitches[1], srcSurfData->height / 2);
                }

                memcpy(dst_y, src_y, y_total);
                memcpy(dst_uv, src_uv, uv_total);
            } else {
                /* copy row-by-row, honoring source and destination pitches
                 * PERFORMANCE: For large surfaces (e.g., 4K video), optimize by copying
                 * rows in batches to reduce loop overhead and improve cache locality.
                 */
                if (unlikely(global.quirks.log_stride)) {
                    traceInfo("  NV12: Using row-by-row copy (pitch mismatch)\n");
                    traceInfo("    Y plane: src_pitch=%u dst_pitch=%u width=%u height=%u\n",
                             q.pitches[0], destination_pitches[0],
                             srcSurfData->width, srcSurfData->height);
                    traceInfo("    UV plane: src_pitch=%u dst_pitch=%u width=%u height=%u\n",
                             q.pitches[1], destination_pitches[1],
                             srcSurfData->width, srcSurfData->height / 2);
                }

                /* Calculate actual bytes to copy per row (full width of data) */
                uint32_t y_row_bytes = srcSurfData->width;
                uint32_t uv_row_bytes = srcSurfData->width;  /* NV12: UV is interleaved, same width as Y */

                /* Copy Y plane - use optimized path for contiguous rows when possible */
                if (q.pitches[0] == y_row_bytes && destination_pitches[0] == y_row_bytes) {
                    /* Rows are contiguous in both source and dest - single copy */
                    if (unlikely(global.quirks.log_stride)) {
                        traceInfo("  Y plane: OPTIMIZED bulk copy (pitch=%u, width=%u, %u bytes total)\n",
                                 q.pitches[0], srcSurfData->width, y_row_bytes * srcSurfData->height);
                    }
                    memcpy(dst_y, src_y, (size_t)y_row_bytes * srcSurfData->height);
                } else {
                    /* Need row-by-row copy due to pitch differences */
                    if (unlikely(global.quirks.log_stride)) {
                        traceInfo("  Y plane: SLOW row-by-row copy (src_pitch=%u dst_pitch=%u width=%u, %u rows)\n",
                                 q.pitches[0], destination_pitches[0], srcSurfData->width, srcSurfData->height);
                    }
                    for (unsigned int y = 0; y < srcSurfData->height; y++) {
                        memcpy(dst_y, src_y, y_row_bytes);
                        src_y += q.pitches[0];
                        dst_y += destination_pitches[0];
                    }
                }

                /* Copy UV plane - same optimization */
                if (q.pitches[1] == uv_row_bytes && destination_pitches[1] == uv_row_bytes) {
                    /* Rows are contiguous in both source and dest - single copy */
                    if (unlikely(global.quirks.log_stride)) {
                        traceInfo("  UV plane: OPTIMIZED bulk copy (pitch=%u, width=%u, %u bytes total)\n",
                                 q.pitches[1], srcSurfData->width, uv_row_bytes * (srcSurfData->height / 2));
                    }
                    memcpy(dst_uv, src_uv, (size_t)uv_row_bytes * (srcSurfData->height / 2));
                } else {
                    /* Need row-by-row copy due to pitch differences */
                    if (unlikely(global.quirks.log_stride)) {
                        traceInfo("  UV plane: SLOW row-by-row copy (src_pitch=%u dst_pitch=%u width=%u, %u rows)\n",
                                 q.pitches[1], destination_pitches[1], srcSurfData->width, srcSurfData->height / 2);
                    }
                    for (unsigned int y = 0; y < srcSurfData->height / 2; y++) {
                        memcpy(dst_uv, src_uv, uv_row_bytes);
                        src_uv += q.pitches[1];
                        dst_uv += destination_pitches[1];
                    }
                }
            }

            vaUnmapBuffer(va_dpy, q.buf);
        } else if (VA_FOURCC('N', 'V', '1', '2') == q.format.fourcc &&
                   VDP_YCBCR_FORMAT_YV12 == destination_ycbcr_format)
        {
            uint8_t *img_data;
            vaMapBuffer(va_dpy, q.buf, (void **)&img_data);

            uint8_t *src_y = img_data + q.offsets[0];
            uint8_t *dst_y = destination_data[0];

            /* Y plane */
            if (destination_pitches[0] == q.pitches[0]) {
                size_t y_total = (size_t)q.pitches[0] * (size_t)srcSurfData->height;

                if (unlikely(global.quirks.log_stride)) {
                    traceInfo("  YV12: Y plane bulk copy (pitches match)\n");
                    traceInfo("    Copying %zu bytes (%u x %u)\n",
                             y_total, q.pitches[0], srcSurfData->height);
                }

                memcpy(dst_y, src_y, y_total);
            } else {
                if (unlikely(global.quirks.log_stride)) {
                    traceInfo("  YV12: Y plane row-by-row copy (pitch mismatch)\n");
                    traceInfo("    src_pitch=%u dst_pitch=%u width=%u height=%u\n",
                             q.pitches[0], destination_pitches[0],
                             srcSurfData->width, srcSurfData->height);
                }

                for (unsigned int y = 0; y < srcSurfData->height; y ++) {
                    memcpy (dst_y, src_y, srcSurfData->width);
                    src_y += q.pitches[0];
                    dst_y += destination_pitches[0];
                }
            }

            /* unpack mixed UV to separate planes (row-by-row required for unpack) */
            if (unlikely(global.quirks.log_stride)) {
                traceInfo("  YV12: Unpacking interleaved UV to separate V and U planes\n");
                traceInfo("    src_pitch=%u dst_pitch_u=%u dst_pitch_v=%u\n",
                         q.pitches[1], destination_pitches[1], destination_pitches[2]);
                traceInfo("    chroma_width=%u chroma_height=%u\n",
                         srcSurfData->width/2, srcSurfData->height/2);
            }

            for (unsigned int y = 0; y < srcSurfData->height/2; y ++) {
                uint8_t *src = img_data + q.offsets[1] + y * q.pitches[1];
                uint8_t *dst_u = destination_data[1] + y * destination_pitches[1];
                uint8_t *dst_v = destination_data[2] + y * destination_pitches[2];

                for (unsigned int x = 0; x < srcSurfData->width/2; x++) {
                    *dst_v++ = *src++;
                    *dst_u++ = *src++;
                }
            }

            vaUnmapBuffer(va_dpy, q.buf);
        } else {
            const char *c = (const char *)&q.format.fourcc;
            traceError("error (%s): not implemented conversion VA FOURCC %c%c%c%c -> %s\n",
                       __func__, *c, *(c+1), *(c+2), *(c+3),
                       reverse_ycbcr_format(destination_ycbcr_format));
            vaDestroyImage(va_dpy, q.image_id);
            err_code = VDP_STATUS_INVALID_Y_CB_CR_FORMAT;
            goto quit;
        }
        vaDestroyImage(va_dpy, q.image_id);
    } else {
        // software fallback
        traceError("error (%s): not implemented software fallback\n", __func__);
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    GLenum gl_error = glGetError();
    if (GL_NO_ERROR != gl_error) {
        traceError("error (%s): gl error %d\n", __func__, gl_error);
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    err_code = VDP_STATUS_OK;
quit:
    handle_release(surface);
    return err_code;
}

VdpStatus
vdpVideoSurfaceGetParameters(VdpVideoSurface surface, VdpChromaType *chroma_type,
                             uint32_t *width, uint32_t *height)
{
    if (!chroma_type || !width || !height)
        return VDP_STATUS_INVALID_POINTER;
    VdpVideoSurfaceData *videoSurf = handle_acquire(surface, HANDLETYPE_VIDEO_SURFACE);
    if (NULL == videoSurf)
        return VDP_STATUS_INVALID_HANDLE;

    *chroma_type = videoSurf->chroma_type;
    *width       = videoSurf->width;
    *height      = videoSurf->height;

    handle_release(surface);
    return VDP_STATUS_OK;
}

static
VdpStatus
_video_surface_ensure_allocated(VdpVideoSurfaceData *surf)
{
    const uint32_t chroma_plane_size =
        (surf->chroma_stride * surf->chroma_height + 0xfu) & (~0xfu);
    if (surf->y_plane)
        return VDP_STATUS_OK;

    switch (surf->chroma_type) {
    case VDP_CHROMA_TYPE_420:
        surf->y_plane = malloc(surf->stride * surf->height);
        if (!surf->y_plane)
            return VDP_STATUS_RESOURCES;
        surf->u_plane = malloc(chroma_plane_size * 2);
        if (!surf->u_plane) {
            free(surf->y_plane);
            return VDP_STATUS_RESOURCES;
        }
        surf->v_plane = surf->u_plane + chroma_plane_size;
        return VDP_STATUS_OK;

    case VDP_CHROMA_TYPE_422:
        surf->y_plane = malloc(surf->stride * surf->height);
        if (!surf->y_plane)
            return VDP_STATUS_RESOURCES;
        surf->u_plane = surf->v_plane = NULL;
        return VDP_STATUS_OK;

    case VDP_CHROMA_TYPE_444:
        surf->y_plane = malloc(surf->stride * surf->height);
        if (!surf->y_plane)
            return VDP_STATUS_RESOURCES;
        surf->u_plane = surf->v_plane = NULL;
        return VDP_STATUS_OK;

    default:
        return VDP_STATUS_INVALID_CHROMA_TYPE;
    }
}

static
VdpStatus
vdpVideoSurfacePutBitsYCbCr_glsl(VdpVideoSurface surface, VdpYCbCrFormat source_ycbcr_format,
                                 void const *const *source_data, uint32_t const *source_pitches)
{
    VdpStatus err_code;
    if (!source_data || !source_pitches)
        return VDP_STATUS_INVALID_POINTER;
    // TODO: implement VDP_YCBCR_FORMAT_UYVY
    // TODO: implement VDP_YCBCR_FORMAT_YUYV
    // TODO: implement VDP_YCBCR_FORMAT_Y8U8V8A8
    // TODO: implement VDP_YCBCR_FORMAT_V8U8Y8A8

    VdpVideoSurfaceData *dstSurfData = handle_acquire(surface, HANDLETYPE_VIDEO_SURFACE);
    if (NULL == dstSurfData)
        return VDP_STATUS_INVALID_HANDLE;
    VdpDeviceData *deviceData = dstSurfData->deviceData;

    switch (source_ycbcr_format) {
    case VDP_YCBCR_FORMAT_NV12:
    case VDP_YCBCR_FORMAT_YV12:
        /* do nothing */
        break;
    case VDP_YCBCR_FORMAT_UYVY:
    case VDP_YCBCR_FORMAT_YUYV:
    case VDP_YCBCR_FORMAT_Y8U8V8A8:
    case VDP_YCBCR_FORMAT_V8U8Y8A8:
    default:
        traceError("error (%s): not implemented source YCbCr format '%s'\n", __func__,
                   reverse_ycbcr_format(source_ycbcr_format));
        err_code = VDP_STATUS_INVALID_Y_CB_CR_FORMAT;
        goto err;
    }

    glx_ctx_push_thread_local(deviceData);
    glBindFramebuffer(GL_FRAMEBUFFER, dstSurfData->fbo_id);

    GLuint tex_id[2];
    glGenTextures(2, tex_id);
    glEnable(GL_TEXTURE_2D);

    switch (source_ycbcr_format) {
    case VDP_YCBCR_FORMAT_NV12:
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_id[1]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        // UV plane
        glPixelStorei(GL_UNPACK_ROW_LENGTH, source_pitches[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dstSurfData->width/2, dstSurfData->height/2, 0,
                     GL_RG, GL_UNSIGNED_BYTE, source_data[1]);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_id[0]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        // Y plane
        glPixelStorei(GL_UNPACK_ROW_LENGTH, source_pitches[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dstSurfData->width, dstSurfData->height, 0, GL_RED,
                     GL_UNSIGNED_BYTE, source_data[0]);
        break;
    case VDP_YCBCR_FORMAT_YV12:
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_id[1]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dstSurfData->width/2, dstSurfData->height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, NULL);
        // U plane
        glPixelStorei(GL_UNPACK_ROW_LENGTH, source_pitches[2]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dstSurfData->width/2, dstSurfData->height/2, GL_RED,
                        GL_UNSIGNED_BYTE, source_data[2]);
        // V plane
        glPixelStorei(GL_UNPACK_ROW_LENGTH, source_pitches[1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dstSurfData->height/2, dstSurfData->width/2,
                        dstSurfData->height/2, GL_RED, GL_UNSIGNED_BYTE, source_data[1]);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_id[0]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        // Y plane
        glPixelStorei(GL_UNPACK_ROW_LENGTH, source_pitches[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dstSurfData->width, dstSurfData->height, 0, GL_RED,
                     GL_UNSIGNED_BYTE, source_data[0]);
        break;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, dstSurfData->width, 0, dstSurfData->height, -1.0f, 1.0f);
    glViewport(0, 0, dstSurfData->width, dstSurfData->height);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();

    glDisable(GL_BLEND);

    switch (source_ycbcr_format) {
    case VDP_YCBCR_FORMAT_NV12:
        glUseProgram(deviceData->shaders[glsl_NV12_RGBA].program);
        glUniform1i(deviceData->shaders[glsl_NV12_RGBA].uniform.tex_0, 0);
        glUniform1i(deviceData->shaders[glsl_NV12_RGBA].uniform.tex_1, 1);
        break;
    case VDP_YCBCR_FORMAT_YV12:
        glUseProgram(deviceData->shaders[glsl_YV12_RGBA].program);
        glUniform1i(deviceData->shaders[glsl_YV12_RGBA].uniform.tex_0, 0);
        glUniform1i(deviceData->shaders[glsl_YV12_RGBA].uniform.tex_1, 1);
        break;
    }

    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f(dstSurfData->width, 0);
        glTexCoord2f(1, 1); glVertex2f(dstSurfData->width, dstSurfData->height);
        glTexCoord2f(0, 1); glVertex2f(0, dstSurfData->height);
    glEnd();

    glUseProgram(0);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(2, tex_id);

    GLenum gl_error = glGetError();
    glx_ctx_pop();
    if (GL_NO_ERROR != gl_error) {
        traceError("error (%s): gl error %d\n", __func__, gl_error);
        err_code = VDP_STATUS_ERROR;
        goto err;
    }

    err_code = VDP_STATUS_OK;
err:
    handle_release(surface);
    return err_code;
}

VdpStatus
vdpVideoSurfacePutBitsYCbCr(VdpVideoSurface surface, VdpYCbCrFormat source_ycbcr_format,
                            void const *const *source_data, uint32_t const *source_pitches)
{
    return vdpVideoSurfacePutBitsYCbCr_glsl(surface, source_ycbcr_format, source_data,
                                           source_pitches);
}

VdpStatus
vdpVideoSurfaceQueryCapabilities(VdpDevice device, VdpChromaType surface_chroma_type,
                                 VdpBool *is_supported, uint32_t *max_width, uint32_t *max_height)
{
    if (!is_supported || !max_width || !max_height)
        return VDP_STATUS_INVALID_POINTER;
    (void)device; (void)surface_chroma_type;
    // TODO: implement
    *is_supported = 1;
    *max_width = 4096;
    *max_height = 4096;

    return VDP_STATUS_OK;
}

VdpStatus
vdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities(VdpDevice device, VdpChromaType surface_chroma_type,
                                                VdpYCbCrFormat bits_ycbcr_format,
                                                VdpBool *is_supported)
{
    if (!is_supported)
        return VDP_STATUS_INVALID_POINTER;
    (void)device; (void)surface_chroma_type; (void)bits_ycbcr_format;
    // TODO: implement
    *is_supported = 1;
    return VDP_STATUS_OK;
}

/**
 * Export VDPAU video surface as DMA-buf file descriptor (hasvk extension)
 *
 * This function enables zero-copy GPU-to-GPU transfer by exporting the
 * underlying VA-API surface as a DMA-buf that can be imported into Vulkan.
 *
 * This is a Mesa-specific extension to VDPAU for the hasvk driver.
 * It's not part of the standard VDPAU API.
 *
 * @param surface        VDPAU video surface to export
 * @param fd_out         Output: DMA-buf file descriptor (caller must close)
 * @param width_out      Output: Surface width
 * @param height_out     Output: Surface height
 * @param fourcc_out     Output: Surface fourcc format
 * @param num_planes_out Output: Number of planes
 * @param pitches_out    Output: Array of plane pitches (at least 3 elements)
 * @param offsets_out    Output: Array of plane offsets (at least 3 elements)
 * @param modifier_out   Output: DRM format modifier (e.g., Y-tiling)
 * @return VDP_STATUS_OK on success, error code otherwise
 *
 * NOTE: The caller is responsible for closing the FD when done.
 */
__attribute__ ((visibility("default")))
VdpStatus
vdpVideoSurfaceExportDmaBufhasvk(VdpVideoSurface surface,
                                 int *fd_out,
                                 uint32_t *width_out,
                                 uint32_t *height_out,
                                 uint32_t *fourcc_out,
                                 uint32_t *num_planes_out,
                                 uint32_t *pitches_out,
                                 uint32_t *offsets_out,
                                 uint64_t *modifier_out)
{
    /* Validate parameters */
    if (!fd_out || !width_out || !height_out || !fourcc_out ||
        !num_planes_out || !pitches_out || !offsets_out || !modifier_out) {
        return VDP_STATUS_INVALID_POINTER;
    }

    VdpVideoSurfaceData *surfData = handle_acquire(surface, HANDLETYPE_VIDEO_SURFACE);
    if (NULL == surfData)
        return VDP_STATUS_INVALID_HANDLE;

    VdpDeviceData *deviceData = surfData->deviceData;
    if (!deviceData || !deviceData->va_available) {
        handle_release(surface);
        return VDP_STATUS_RESOURCES;
    }

#ifdef HAVE_VA_DRM_PRIME
    /* DMA-buf export is only available if VA-API DRM PRIME support is compiled in */
    VADisplay va_dpy = deviceData->va_dpy;
    VASurfaceID va_surf = surfData->va_surf;
    VAStatus va_status;

    if (va_surf == VA_INVALID_SURFACE) {
        handle_release(surface);
        return VDP_STATUS_INVALID_HANDLE;
    }

    /* Try to export the VA-API surface as DMA-buf using VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 */
    VADRMPRIMESurfaceDescriptor prime_desc;
    memset(&prime_desc, 0, sizeof(prime_desc));

    va_status = vaExportSurfaceHandle(va_dpy, va_surf,
                                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                               VA_EXPORT_SURFACE_READ_ONLY |
                                               VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                               &prime_desc);

    if (va_status != VA_STATUS_SUCCESS) {
        traceError("hasvk DMA-buf export: vaExportSurfaceHandle failed with status %d\n", va_status);
        handle_release(surface);
        return VDP_STATUS_RESOURCES;
    }

    /* Validate the export succeeded */
    if (prime_desc.num_objects == 0 || prime_desc.num_layers == 0) {
        traceError("hasvk DMA-buf export: Invalid prime descriptor\n");
        /* Close any FDs that might have been opened */
        for (uint32_t i = 0; i < prime_desc.num_objects; i++) {
            if (prime_desc.objects[i].fd >= 0)
                close(prime_desc.objects[i].fd);
        }
        handle_release(surface);
        return VDP_STATUS_RESOURCES;
    }

    /* Fill output parameters
     * For NV12, we expect 2 planes (Y and UV) in a single object
     */
    *fd_out = prime_desc.objects[0].fd;
    *width_out = prime_desc.width;
    *height_out = prime_desc.height;
    *fourcc_out = prime_desc.fourcc;
    *modifier_out = prime_desc.objects[0].drm_format_modifier;

    /* Count planes across all layers */
    uint32_t total_planes = 0;
    for (uint32_t layer = 0; layer < prime_desc.num_layers; layer++) {
        for (uint32_t plane = 0; plane < prime_desc.layers[layer].num_planes; plane++) {
            if (total_planes < 3) {  /* Limit to 3 planes max */
                pitches_out[total_planes] = prime_desc.layers[layer].pitch[plane];
                offsets_out[total_planes] = prime_desc.layers[layer].offset[plane];
                total_planes++;
            }
        }
    }
    *num_planes_out = total_planes;

    if (unlikely(global.quirks.log_stride)) {
        traceInfo("hasvk DMA-buf export successful:\n");
        traceInfo("  FD: %d\n", *fd_out);
        traceInfo("  Size: %ux%u\n", *width_out, *height_out);
        traceInfo("  FOURCC: 0x%08x\n", *fourcc_out);
        traceInfo("  Modifier: 0x%016lx\n", *modifier_out);
        traceInfo("  Planes: %u\n", *num_planes_out);
        for (uint32_t i = 0; i < *num_planes_out; i++) {
            traceInfo("    [%u]: pitch=%u offset=%u\n", i, pitches_out[i], offsets_out[i]);
        }
    }

    /* Close FDs for any additional objects (we only use the first one) */
    for (uint32_t i = 1; i < prime_desc.num_objects; i++) {
        if (prime_desc.objects[i].fd >= 0)
            close(prime_desc.objects[i].fd);
    }

    handle_release(surface);
    return VDP_STATUS_OK;
#else
    /* DMA-buf export is not available without VA-API DRM PRIME support */
    if (unlikely(global.quirks.log_stride)) {
        traceInfo("hasvk DMA-buf export: Not available (VA-API DRM PRIME support not compiled in)\n");
    }

    handle_release(surface);
    return VDP_STATUS_NO_IMPLEMENTATION;
#endif /* HAVE_VA_DRM_PRIME */
}
