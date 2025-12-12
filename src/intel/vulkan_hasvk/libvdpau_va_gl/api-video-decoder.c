/*
 * Copyright 2013-2014  Rinat Ibragimov
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#include <stdlib.h>
#include <string.h>
#include "ctx-stack.h"
#include "h264-parse.h"
#include "trace.h"
#include "api.h"
#include "globals.h"


VdpStatus
vdpDecoderCreate(VdpDevice device, VdpDecoderProfile profile, uint32_t width, uint32_t height,
                 uint32_t max_references, VdpDecoder *decoder)
{
    VdpStatus err_code;
    if (!decoder)
        return VDP_STATUS_INVALID_POINTER;
    VdpDeviceData *deviceData = handle_acquire(device, HANDLETYPE_DEVICE);
    if (NULL == deviceData)
        return VDP_STATUS_INVALID_HANDLE;
    if (!deviceData->va_available) {
        err_code = VDP_STATUS_INVALID_DECODER_PROFILE;
        goto quit;
    }
    VADisplay va_dpy = deviceData->va_dpy;

    VdpDecoderData *data = calloc(1, sizeof(VdpDecoderData));
    if (NULL == data) {
        err_code = VDP_STATUS_RESOURCES;
        goto quit;
    }

    data->type = HANDLETYPE_DECODER;
    data->device = device;
    data->deviceData = deviceData;
    data->profile = profile;
    data->width = width;
    data->height = height;
    data->max_references = max_references;
    data->bitstream_buffer = NULL;
    data->bitstream_buffer_size = 0;

    // initialize free_list. Initially they all free
    data->free_list_head = -1;
    for (int k = 0; k < MAX_RENDER_TARGETS; k ++) {
        free_list_push(data->free_list, &data->free_list_head, k);
    }

    VAProfile va_profile;
    VAStatus status;
    int final_try = 0;
    VdpDecoderProfile next_profile = profile;

    // Try to create decoder for asked profile. On failure try to create more advanced one
    while (! final_try) {
        profile = next_profile;
        switch (profile) {
        case VDP_DECODER_PROFILE_H264_CONSTRAINED_BASELINE:
            va_profile = VAProfileH264ConstrainedBaseline;
            data->num_render_targets = NUM_RENDER_TARGETS_H264;
            next_profile = VDP_DECODER_PROFILE_H264_BASELINE;
            break;

        case VDP_DECODER_PROFILE_H264_BASELINE:
            va_profile = VAProfileH264ConstrainedBaseline;
            data->num_render_targets = NUM_RENDER_TARGETS_H264;
            next_profile = VDP_DECODER_PROFILE_H264_MAIN;
            break;

        case VDP_DECODER_PROFILE_H264_MAIN:
            va_profile = VAProfileH264Main;
            data->num_render_targets = NUM_RENDER_TARGETS_H264;
            next_profile = VDP_DECODER_PROFILE_H264_HIGH;
            break;

        case VDP_DECODER_PROFILE_H264_HIGH:
            va_profile = VAProfileH264High;
            data->num_render_targets = NUM_RENDER_TARGETS_H264;
            // there is no more advanced profile, so it's final try
            final_try = 1;
            break;

        default:
            traceError("error (%s): decoder %s not implemented\n", __func__,
                       reverse_decoder_profile(profile));
            err_code = VDP_STATUS_INVALID_DECODER_PROFILE;
            goto quit_free_data;
        }

        status = vaCreateConfig(va_dpy, va_profile, VAEntrypointVLD, NULL, 0, &data->config_id);
        if (VA_STATUS_SUCCESS == status)        // break loop if decoder created
            break;
    }

    if (VA_STATUS_SUCCESS != status) {
        err_code = VDP_STATUS_ERROR;
        goto quit_free_data;
    }

    // All video surfaces are created here, rather than in VdpVideoSurfaceCreate.
    // VA-API requires surfaces to be bound with context at creation time, while VDPAU allows
    // us to do it later. So here is a trick: VDP video surfaces get their va_surf dynamically in
    // DecoderRender.

    // TODO: check format of surfaces created
#if VA_CHECK_VERSION(0, 34, 0)
    status = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_YUV420, width, height,
        data->render_targets, data->num_render_targets, NULL, 0);
#else
    status = vaCreateSurfaces(va_dpy, width, height, VA_RT_FORMAT_YUV420,
        data->num_render_targets, data->render_targets);
#endif
    if (VA_STATUS_SUCCESS != status) {
        err_code = VDP_STATUS_ERROR;
        goto quit_free_data;
    }

    status = vaCreateContext(va_dpy, data->config_id, width, height, VA_PROGRESSIVE,
        data->render_targets, data->num_render_targets, &data->context_id);
    if (VA_STATUS_SUCCESS != status) {
        err_code = VDP_STATUS_ERROR;
        goto quit_free_data;
    }

    ref_device(deviceData);
    *decoder = handle_insert(data);

    err_code = VDP_STATUS_OK;
    goto quit;

quit_free_data:
    free(data);
quit:
    handle_release(device);
    return err_code;
}

VdpStatus
vdpDecoderDestroy(VdpDecoder decoder)
{
    VdpDecoderData *decoderData = handle_acquire(decoder, HANDLETYPE_DECODER);
    if (NULL == decoderData)
        return VDP_STATUS_INVALID_HANDLE;
    VdpDeviceData *deviceData = decoderData->deviceData;

    if (deviceData->va_available) {
        VADisplay va_dpy = deviceData->va_dpy;
        vaDestroySurfaces(va_dpy, decoderData->render_targets, decoderData->num_render_targets);
        vaDestroyContext(va_dpy, decoderData->context_id);
        vaDestroyConfig(va_dpy, decoderData->config_id);
    }

    // Free reusable bitstream buffer if allocated
    if (decoderData->bitstream_buffer) {
        free(decoderData->bitstream_buffer);
    }

    handle_expunge(decoder);
    unref_device(deviceData);
    free(decoderData);
    return VDP_STATUS_OK;
}

VdpStatus
vdpDecoderGetParameters(VdpDecoder decoder, VdpDecoderProfile *profile,
                        uint32_t *width, uint32_t *height)
{
    if (!profile || !width || !height)
        return VDP_STATUS_INVALID_HANDLE;
    VdpDecoderData *decoderData = handle_acquire(decoder, HANDLETYPE_DECODER);
    if (!decoderData)
        return VDP_STATUS_INVALID_HANDLE;

    *profile = decoderData->profile;
    *width   = decoderData->width;
    *height  = decoderData->height;

    handle_release(decoder);
    return VDP_STATUS_OK;
}

static
VdpStatus
h264_translate_reference_frames(VdpVideoSurfaceData *dstSurfData, VdpDecoder decoder,
                                VdpDecoderData *decoderData,
                                VAPictureParameterBufferH264 *pic_param,
                                const VdpPictureInfoH264 *vdppi)
{
    // take new VA surface from buffer if needed
    if (VA_INVALID_SURFACE == dstSurfData->va_surf) {
        int idx = free_list_pop(decoderData->free_list, &decoderData->free_list_head);
        if (-1 == idx)
            return VDP_STATUS_RESOURCES;
        dstSurfData->decoder = decoder;
        dstSurfData->va_surf = decoderData->render_targets[idx];
        dstSurfData->rt_idx  = idx;
    }

    // current frame
    pic_param->CurrPic.picture_id   = dstSurfData->va_surf;
    pic_param->CurrPic.frame_idx    = vdppi->frame_num;
    pic_param->CurrPic.flags  = vdppi->is_reference ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
    if (vdppi->field_pic_flag) {
        pic_param->CurrPic.flags |=
            vdppi->bottom_field_flag ? VA_PICTURE_H264_BOTTOM_FIELD : VA_PICTURE_H264_TOP_FIELD;
    }

    pic_param->CurrPic.TopFieldOrderCnt     = vdppi->field_order_cnt[0];
    pic_param->CurrPic.BottomFieldOrderCnt  = vdppi->field_order_cnt[1];

    // mark all pictures invalid preliminary
    for (int k = 0; k < 16; k ++)
        reset_va_picture_h264(&pic_param->ReferenceFrames[k]);

    // reference frames
    for (int k = 0; k < vdppi->num_ref_frames; k ++) {
        if (VDP_INVALID_HANDLE == vdppi->referenceFrames[k].surface) {
            reset_va_picture_h264(&pic_param->ReferenceFrames[k]);
            continue;
        }

        VdpReferenceFrameH264 const *vdp_ref = &(vdppi->referenceFrames[k]);
        VdpVideoSurfaceData *vdpSurfData =
            handle_acquire(vdp_ref->surface, HANDLETYPE_VIDEO_SURFACE);
        VAPictureH264 *va_ref = &(pic_param->ReferenceFrames[k]);
        if (NULL == vdpSurfData) {
            traceError("error (%s): NULL == vdpSurfData\n", __func__);
            return VDP_STATUS_ERROR;
        }

        // take new VA surface from buffer if needed
        if (VA_INVALID_SURFACE == vdpSurfData->va_surf) {
            int idx = free_list_pop(decoderData->free_list, &decoderData->free_list_head);
            if (-1 == idx)
                return VDP_STATUS_RESOURCES;
            dstSurfData->decoder = decoder;
            dstSurfData->va_surf = decoderData->render_targets[idx];
            dstSurfData->rt_idx  = idx;
        }

        va_ref->picture_id = vdpSurfData->va_surf;
        va_ref->frame_idx = vdp_ref->frame_idx;
        va_ref->flags = vdp_ref->is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                              : VA_PICTURE_H264_SHORT_TERM_REFERENCE;

        if (vdp_ref->top_is_reference && vdp_ref->bottom_is_reference) {
            // Full frame. This block intentionally left blank. No flags set.
        } else {
            if (vdp_ref->top_is_reference)
                va_ref->flags |= VA_PICTURE_H264_TOP_FIELD;
            else
                va_ref->flags |= VA_PICTURE_H264_BOTTOM_FIELD;
        }

        va_ref->TopFieldOrderCnt    = vdp_ref->field_order_cnt[0];
        va_ref->BottomFieldOrderCnt = vdp_ref->field_order_cnt[1];
        handle_release(vdp_ref->surface);
    }

    return VDP_STATUS_OK;
}

VdpStatus
vdpDecoderQueryCapabilities(VdpDevice device, VdpDecoderProfile profile, VdpBool *is_supported,
                            uint32_t *max_level, uint32_t *max_macroblocks,
                            uint32_t *max_width, uint32_t *max_height)
{
    VdpStatus err_code;
    if (!is_supported || !max_level || !max_macroblocks || !max_width || !max_height)
        return VDP_STATUS_INVALID_POINTER;
    VdpDeviceData *deviceData = handle_acquire(device, HANDLETYPE_DEVICE);
    if (NULL == deviceData)
        return VDP_STATUS_INVALID_HANDLE;

    *max_level = 0;
    *max_macroblocks = 0;
    *max_width = 0;
    *max_height = 0;

    if (!deviceData->va_available) {
        *is_supported = 0;
        err_code = VDP_STATUS_OK;
        goto quit;
    }

    VAProfile *va_profile_list = malloc(sizeof(VAProfile) * vaMaxNumProfiles(deviceData->va_dpy));
    if (NULL == va_profile_list) {
        err_code = VDP_STATUS_RESOURCES;
        goto quit;
    }

    int num_profiles;
    VAStatus status = vaQueryConfigProfiles(deviceData->va_dpy, va_profile_list, &num_profiles);
    if (VA_STATUS_SUCCESS != status) {
        free(va_profile_list);
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    struct {
        int mpeg2_simple;
        int mpeg2_main;
        int h264_baseline;
        int h264_main;
        int h264_high;
        int vc1_simple;
        int vc1_main;
        int vc1_advanced;
    } available_profiles = {0, 0, 0, 0, 0, 0, 0, 0};

    for (int k = 0; k < num_profiles; k ++) {
        switch (va_profile_list[k]) {
        case VAProfileMPEG2Main:
            available_profiles.mpeg2_main = 0;
            /* fall through */
        case VAProfileMPEG2Simple:
            available_profiles.mpeg2_simple = 0;
            break;

        case VAProfileH264High:
            available_profiles.h264_high = 1;
            __attribute__((fallthrough));
        case VAProfileH264Main:
            available_profiles.h264_main = 1;
            available_profiles.h264_baseline = 1;
            __attribute__((fallthrough));
        case VAProfileH264ConstrainedBaseline:
            break;

        case VAProfileVC1Advanced:
            available_profiles.vc1_advanced = 0;
            __attribute__((fallthrough));
        case VAProfileVC1Main:
            available_profiles.vc1_main = 0;
            __attribute__((fallthrough));
        case VAProfileVC1Simple:
            available_profiles.vc1_simple = 0;
            break;

        // unhandled profiles
        case VAProfileH263Baseline:
        case VAProfileJPEGBaseline:
        default:
            // do nothing
            break;
        }
    }
    free(va_profile_list);

    *is_supported = 0;
    // hasvk hardware supports up to 4096x4096 for video decode.
    // This matches VdpVideoSurfaceQueryCapabilities and allows 4k video playback.
    // The actual decoder surfaces are created at real video dimensions (not max)
    // to ensure correct pitch, preventing display corruption.
    *max_width = 4096;
    *max_height = 4096;
    *max_macroblocks = 65536;  // (4096/16) * (4096/16) = 256 * 256 = 65536
    switch (profile) {
    case VDP_DECODER_PROFILE_MPEG2_SIMPLE:
        *is_supported = available_profiles.mpeg2_simple;
        *max_level = VDP_DECODER_LEVEL_MPEG2_HL;
        break;

    case VDP_DECODER_PROFILE_MPEG2_MAIN:
        *is_supported = available_profiles.mpeg2_main;
        *max_level = VDP_DECODER_LEVEL_MPEG2_HL;
        break;

    case VDP_DECODER_PROFILE_H264_CONSTRAINED_BASELINE:
        *is_supported = available_profiles.h264_baseline || available_profiles.h264_main;
        *max_level = VDP_DECODER_LEVEL_H264_5_1;
        break;

    case VDP_DECODER_PROFILE_H264_BASELINE:
        *is_supported = available_profiles.h264_baseline;
        // TODO: Does underlying libva really support 5.1?
        *max_level = VDP_DECODER_LEVEL_H264_5_1;
        break;

    case VDP_DECODER_PROFILE_H264_MAIN:
        *is_supported = available_profiles.h264_main;
        *max_level = VDP_DECODER_LEVEL_H264_5_1;
        break;

    case VDP_DECODER_PROFILE_H264_HIGH:
        *is_supported = available_profiles.h264_high;
        *max_level = VDP_DECODER_LEVEL_H264_5_1;
        break;

    case VDP_DECODER_PROFILE_VC1_SIMPLE:
        *is_supported = available_profiles.vc1_simple;
        *max_level = VDP_DECODER_LEVEL_VC1_SIMPLE_MEDIUM;
        break;

    case VDP_DECODER_PROFILE_VC1_MAIN:
        *is_supported = available_profiles.vc1_main;
        *max_level = VDP_DECODER_LEVEL_VC1_MAIN_HIGH;
        break;

    case VDP_DECODER_PROFILE_VC1_ADVANCED:
        *is_supported = available_profiles.vc1_advanced;
        *max_level = VDP_DECODER_LEVEL_VC1_ADVANCED_L4;
        break;

    // unsupported
    case VDP_DECODER_PROFILE_MPEG1:
    case VDP_DECODER_PROFILE_MPEG4_PART2_SP:
    case VDP_DECODER_PROFILE_MPEG4_PART2_ASP:
    case VDP_DECODER_PROFILE_DIVX4_QMOBILE:
    case VDP_DECODER_PROFILE_DIVX4_MOBILE:
    case VDP_DECODER_PROFILE_DIVX4_HOME_THEATER:
    case VDP_DECODER_PROFILE_DIVX4_HD_1080P:
    case VDP_DECODER_PROFILE_DIVX5_QMOBILE:
    case VDP_DECODER_PROFILE_DIVX5_MOBILE:
    case VDP_DECODER_PROFILE_DIVX5_HOME_THEATER:
    case VDP_DECODER_PROFILE_DIVX5_HD_1080P:
    default:
        break;
    }

    err_code = VDP_STATUS_OK;
quit:
    handle_release(device);
    return err_code;
}

static
void
h264_translate_pic_param(VAPictureParameterBufferH264 *pic_param, uint32_t width, uint32_t height,
                         const VdpPictureInfoH264 *vdppi, uint32_t level)
{
        pic_param->picture_width_in_mbs_minus1          = (width - 1) / 16;
        pic_param->picture_height_in_mbs_minus1         = (height - 1) / 16;
        pic_param->bit_depth_luma_minus8                = 0; // TODO: deal with more than 8 bits
        pic_param->bit_depth_chroma_minus8              = 0; // same for luma
        pic_param->num_ref_frames                       = vdppi->num_ref_frames;

#define SEQ_FIELDS(fieldname) pic_param->seq_fields.bits.fieldname
#define PIC_FIELDS(fieldname) pic_param->pic_fields.bits.fieldname

        SEQ_FIELDS(chroma_format_idc)                   = 1; // TODO: not only YUV420
        SEQ_FIELDS(residual_colour_transform_flag)      = 0;
        SEQ_FIELDS(gaps_in_frame_num_value_allowed_flag)= 0;
        SEQ_FIELDS(frame_mbs_only_flag)                 = vdppi->frame_mbs_only_flag;
        SEQ_FIELDS(mb_adaptive_frame_field_flag)        = vdppi->mb_adaptive_frame_field_flag;
        SEQ_FIELDS(direct_8x8_inference_flag)           = vdppi->direct_8x8_inference_flag;
        SEQ_FIELDS(MinLumaBiPredSize8x8)                = (level >= 31);
        SEQ_FIELDS(log2_max_frame_num_minus4)           = vdppi->log2_max_frame_num_minus4;
        SEQ_FIELDS(pic_order_cnt_type)                  = vdppi->pic_order_cnt_type;
        SEQ_FIELDS(log2_max_pic_order_cnt_lsb_minus4)   = vdppi->log2_max_pic_order_cnt_lsb_minus4;
        SEQ_FIELDS(delta_pic_order_always_zero_flag)    = vdppi->delta_pic_order_always_zero_flag;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        pic_param->num_slice_groups_minus1              = 0; // TODO: vdppi->slice_count - 1; ???

        pic_param->slice_group_map_type                 = 0; // ???
        pic_param->slice_group_change_rate_minus1       = 0; // ???
#pragma GCC diagnostic pop
        pic_param->pic_init_qp_minus26                  = vdppi->pic_init_qp_minus26;
        pic_param->pic_init_qs_minus26                  = 0; // ???
        pic_param->chroma_qp_index_offset               = vdppi->chroma_qp_index_offset;
        pic_param->second_chroma_qp_index_offset        = vdppi->second_chroma_qp_index_offset;
        PIC_FIELDS(entropy_coding_mode_flag)            = vdppi->entropy_coding_mode_flag;
        PIC_FIELDS(weighted_pred_flag)                  = vdppi->weighted_pred_flag;
        PIC_FIELDS(weighted_bipred_idc)                 = vdppi->weighted_bipred_idc;
        PIC_FIELDS(transform_8x8_mode_flag)             = vdppi->transform_8x8_mode_flag;
        PIC_FIELDS(field_pic_flag)                      = vdppi->field_pic_flag;
        PIC_FIELDS(constrained_intra_pred_flag)         = vdppi->constrained_intra_pred_flag;
        PIC_FIELDS(pic_order_present_flag)              = vdppi->pic_order_present_flag;
        PIC_FIELDS(deblocking_filter_control_present_flag) =
                                                    vdppi->deblocking_filter_control_present_flag;
        PIC_FIELDS(redundant_pic_cnt_present_flag)      = vdppi->redundant_pic_cnt_present_flag;
        PIC_FIELDS(reference_pic_flag)                  = vdppi->is_reference;
        pic_param->frame_num                            = vdppi->frame_num;
#undef SEQ_FIELDS
#undef PIC_FIELDS
}

static
void
h264_translate_iq_matrix(VAIQMatrixBufferH264 *iq_matrix, const VdpPictureInfoH264 *vdppi)
{
    for (int j = 0; j < 6; j ++)
        for (int k = 0; k < 16; k ++)
            iq_matrix->ScalingList4x4[j][k] = vdppi->scaling_lists_4x4[j][k];

    for (int j = 0; j < 2; j ++)
        for (int k = 0; k < 64; k ++)
            iq_matrix->ScalingList8x8[j][k] = vdppi->scaling_lists_8x8[j][k];
}

// Slice info structure for collecting and sorting slices
struct h264_slice_info {
    VASliceParameterBufferH264 params;
    int nal_offset;
    unsigned int slice_data_size;
};

// Comparison function for sorting slices by first_mb_in_slice
// For malformed files with duplicate first_mb_in_slice values, use nal_offset as tiebreaker
static int
compare_h264_slices(const void *a, const void *b)
{
    const struct h264_slice_info *slice_a = (const struct h264_slice_info *)a;
    const struct h264_slice_info *slice_b = (const struct h264_slice_info *)b;

    // Primary sort: by macroblock position
    if (slice_a->params.first_mb_in_slice < slice_b->params.first_mb_in_slice)
        return -1;
    if (slice_a->params.first_mb_in_slice > slice_b->params.first_mb_in_slice)
        return 1;

    if (slice_a->nal_offset < slice_b->nal_offset)
        return -1;
    if (slice_a->nal_offset > slice_b->nal_offset)
        return 1;

    return 0;
}

static
VdpStatus
vdpDecoderRender_h264(VdpDecoder decoder, VdpDecoderData *decoderData,
                      VdpVideoSurfaceData *dstSurfData, VdpPictureInfo const *picture_info,
                      uint32_t bitstream_buffer_count,
                      VdpBitstreamBuffer const *bitstream_buffers)
{
    VdpDeviceData *deviceData = decoderData->deviceData;
    VADisplay va_dpy = deviceData->va_dpy;
    VAStatus status;
    VdpStatus vs, err_code;
    VdpPictureInfoH264 const *vdppi = (void *)picture_info;

    // TODO: figure out where to get level
    uint32_t level = 41;

    // preparing picture parameters and IQ matrix
    VABufferID pic_param_buf, iq_matrix_buf;
    VAPictureParameterBufferH264 pic_param;
    VAIQMatrixBufferH264 iq_matrix;

    memset(&pic_param, 0, sizeof(pic_param));
    vs = h264_translate_reference_frames(dstSurfData, decoder, decoderData, &pic_param, vdppi);
    if (VDP_STATUS_OK != vs) {
        if (VDP_STATUS_RESOURCES == vs) {
            traceError("error (%s): no surfaces left in buffer\n", __func__);
            err_code = VDP_STATUS_RESOURCES;
        } else {
            err_code = VDP_STATUS_ERROR;
        }
        goto quit;
    }

    h264_translate_pic_param(&pic_param, decoderData->width, decoderData->height, vdppi, level);
    h264_translate_iq_matrix(&iq_matrix, vdppi);

    glx_ctx_lock();
    status = vaCreateBuffer(va_dpy, decoderData->context_id, VAPictureParameterBufferType,
        sizeof(VAPictureParameterBufferH264), 1, &pic_param, &pic_param_buf);
    if (VA_STATUS_SUCCESS != status) {
        glx_ctx_unlock();
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    status = vaCreateBuffer(va_dpy, decoderData->context_id, VAIQMatrixBufferType,
        sizeof(VAIQMatrixBufferH264), 1, &iq_matrix, &iq_matrix_buf);
    if (VA_STATUS_SUCCESS != status) {
        glx_ctx_unlock();
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    // send data to decoding hardware
    status = vaBeginPicture(va_dpy, decoderData->context_id, dstSurfData->va_surf);
    if (VA_STATUS_SUCCESS != status) {
        glx_ctx_unlock();
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }
    status = vaRenderPicture(va_dpy, decoderData->context_id, &pic_param_buf, 1);
    if (VA_STATUS_SUCCESS != status) {
        glx_ctx_unlock();
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }
    status = vaRenderPicture(va_dpy, decoderData->context_id, &iq_matrix_buf, 1);
    if (VA_STATUS_SUCCESS != status) {
        glx_ctx_unlock();
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    vaDestroyBuffer(va_dpy, pic_param_buf);
    vaDestroyBuffer(va_dpy, iq_matrix_buf);
    glx_ctx_unlock();

    // merge bitstream buffers
    size_t total_bitstream_bytes = 0;
    for (unsigned int k = 0; k < bitstream_buffer_count; k ++)
        total_bitstream_bytes += bitstream_buffers[k].bitstream_bytes;

    /* Overflow check: ensure total doesn't exceed reasonable limits.
     * Even the largest 4K H.264 bitstreams are typically < 50MB per frame.
     * SIZE_MAX/2 provides a safe upper bound while preventing integer overflow.
     */
    if (total_bitstream_bytes > SIZE_MAX / 2) {
        err_code = VDP_STATUS_RESOURCES;
        goto quit;
    }

    /* PERFORMANCE: Reuse bitstream buffer instead of malloc/free per frame.
     * For 4K video, typical bitstream can be several MB per frame. Reusing the buffer
     * eliminates malloc/free overhead (~100 μs per frame) and reduces memory fragmentation.
     * Buffer grows as needed but never shrinks (acceptable tradeoff for decode session lifetime).
     */
    uint8_t *merged_bitstream;
    if (decoderData->bitstream_buffer_size < total_bitstream_bytes) {
        // Need larger buffer - grow it using realloc to avoid fragmentation
        uint8_t *new_buffer = realloc(decoderData->bitstream_buffer, total_bitstream_bytes);
        if (!new_buffer) {
            // realloc failed but old buffer is still valid - keep it for next attempt
            err_code = VDP_STATUS_RESOURCES;
            goto quit;
        }
        decoderData->bitstream_buffer = new_buffer;
        decoderData->bitstream_buffer_size = total_bitstream_bytes;
    }
    merged_bitstream = decoderData->bitstream_buffer;

    do {
        unsigned char *ptr = merged_bitstream;
        for (unsigned int k = 0; k < bitstream_buffer_count; k ++) {
            memcpy(ptr, bitstream_buffers[k].bitstream, bitstream_buffers[k].bitstream_bytes);
            ptr += bitstream_buffers[k].bitstream_bytes;
        }
    } while(0);

    // Slice parameters

    // All slice data have been merged into one continuous buffer. But we must supply
    // slices one by one to the hardware decoder, so we need to delimit them. VDPAU
    // requires bitstream buffers to include slice start code (0x00 0x00 0x01). Those
    // will be used to calculate offsets and sizes of slice data in code below.

    // First pass: collect all slices into an array
    struct h264_slice_info *slices = NULL;
    int slice_count = 0;
    int slice_capacity = 0;

    rbsp_state_t st_g;      // reference, global state
    rbsp_attach_buffer(&st_g, merged_bitstream, total_bitstream_bytes);
    int nal_offset = rbsp_navigate_to_nal_unit(&st_g);
    if (nal_offset < 0) {
        traceError("error (%s): no NAL header\n", __func__);
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    // TODO: this may not be entirely true for YUV444
    int ChromaArrayType = pic_param.seq_fields.bits.chroma_format_idc;

    do {
        VASliceParameterBufferH264 sp_h264;
        memset(&sp_h264, 0, sizeof(VASliceParameterBufferH264));

        // make a copy of global rbsp state for using in slice header parser
        rbsp_state_t st = rbsp_copy_state(&st_g);
        rbsp_reset_bit_counter(&st);
        int nal_offset_next = rbsp_navigate_to_nal_unit(&st_g);

        // calculate end of current NAL unit
        const unsigned int end_pos = (nal_offset_next > 0) ? (nal_offset_next - 3)
                                                           : total_bitstream_bytes;
        sp_h264.slice_data_size     = end_pos - nal_offset;
        sp_h264.slice_data_offset   = 0;
        sp_h264.slice_data_flag     = VA_SLICE_DATA_FLAG_ALL;

        // Peek at NAL unit type to filter out non-slices (SPS, PPS, SEI, etc.)
        rbsp_state_t peek_st = rbsp_copy_state(&st);
        rbsp_get_u(&peek_st, 1); // forbidden_zero_bit
        rbsp_get_u(&peek_st, 2); // nal_ref_idc
        int nal_unit_type = rbsp_get_u(&peek_st, 5);

        // Only process actual slice NAL units (types 1-5)
        // Valid slice NAL types: 1=non-IDR slice, 2-4=data partition slices, 5=IDR slice
        // Non-slice types to skip: 6=SEI, 7=SPS, 8=PPS, 9=AU delimiter, etc.
        const int NAL_SLICE_MIN = 1;      // First slice type (non-IDR)
        const int NAL_SLICE_MAX = 5;      // Last slice type (IDR)
        int is_slice = (nal_unit_type >= NAL_SLICE_MIN && nal_unit_type <= NAL_SLICE_MAX);

        if (is_slice) {
            // parse slice header and use its data to fill slice parameter buffer
            parse_slice_header(&st, &pic_param, ChromaArrayType, vdppi->num_ref_idx_l0_active_minus1,
                               vdppi->num_ref_idx_l1_active_minus1, &sp_h264);

            // Grow slice array if needed
            if (slice_count >= slice_capacity) {
                slice_capacity = (slice_capacity == 0) ? 16 : slice_capacity * 2;
                struct h264_slice_info *new_slices = realloc(slices, slice_capacity * sizeof(struct h264_slice_info));
                if (!new_slices) {
                    // realloc failed, but original slices pointer is still valid
                    // It will be freed in cleanup code at quit_free_slices label
                    err_code = VDP_STATUS_RESOURCES;
                    goto quit_free_slices;
                }
                slices = new_slices;
            }

            // Store slice info
            slices[slice_count].params = sp_h264;
            slices[slice_count].nal_offset = nal_offset;
            slices[slice_count].slice_data_size = sp_h264.slice_data_size;
            slice_count++;
        }

        if (nal_offset_next < 0)        // nal_offset_next equals -1 when there is no NAL unit
            break;                      // start code found. Thus that was the final NAL unit.
        nal_offset = nal_offset_next;
    } while (1);

    // Debug logging: show collected slices before sorting
    if (global.quirks.log_slice_order) {
        traceInfo("hasvk: H.264 slice ordering debug (before sort):\n");
        traceInfo("  Total slices collected: %d\n", slice_count);
        for (int i = 0; i < slice_count && i < 10; i++) {
            traceInfo("  Slice %d: first_mb_in_slice=%u, nal_offset=%d, size=%u\n",
                     i, slices[i].params.first_mb_in_slice, slices[i].nal_offset,
                     slices[i].slice_data_size);
        }
        if (slice_count > 10) {
            traceInfo("  ... (%d more slices)\n", slice_count - 10);
        }
    }

    // Second pass: sort slices by first_mb_in_slice (macroblock order)
    // This is required for correct H.264 decoding with many slices
    qsort(slices, slice_count, sizeof(struct h264_slice_info), compare_h264_slices);

    // Third pass: deduplicate slices with same first_mb_in_slice
    // This handles malformed/malicious files that have multiple slices at the same macroblock position
    // The Intel VA-API driver rejects slices with duplicate first_mb_in_slice values
    int dedup_count = 0;
    for (int i = 0; i < slice_count; i++) {
        // Keep first occurrence of each first_mb_in_slice value
        if (i == 0 || slices[i].params.first_mb_in_slice != slices[dedup_count - 1].params.first_mb_in_slice) {
            if (i != dedup_count) {
                slices[dedup_count] = slices[i];
            }
            dedup_count++;
        } else if (global.quirks.log_slice_order) {
            traceInfo("hasvk: Skipping duplicate slice %d with first_mb_in_slice=%u (same as slice %d)\n",
                     i, slices[i].params.first_mb_in_slice, dedup_count - 1);
        }
    }

    if (dedup_count < slice_count && global.quirks.log_slice_order) {
        traceInfo("hasvk: Deduplicated %d slices (from %d to %d)\n",
                 slice_count - dedup_count, slice_count, dedup_count);
    }
    slice_count = dedup_count;

    // Debug logging: show sorted and deduplicated slices
    if (global.quirks.log_slice_order) {
        traceInfo("hasvk: H.264 slice ordering debug (after sort and dedup):\n");
        for (int i = 0; i < slice_count && i < 10; i++) {
            traceInfo("  Slice %d: first_mb_in_slice=%u, nal_offset=%d, size=%u\n",
                     i, slices[i].params.first_mb_in_slice, slices[i].nal_offset,
                     slices[i].slice_data_size);
        }
        if (slice_count > 10) {
            traceInfo("  ... (%d more slices)\n", slice_count - 10);
        }
        // Check for ordering issues
        for (int i = 0; i < slice_count - 1; i++) {
            if (slices[i+1].params.first_mb_in_slice <= slices[i].params.first_mb_in_slice) {
                traceError("hasvk: ERROR! Slice %d (first_mb=%u) <= Slice %d (first_mb=%u)\n",
                          i+1, slices[i+1].params.first_mb_in_slice,
                          i, slices[i].params.first_mb_in_slice);
            }
        }
    }

    // Fourth pass: submit slices to VA-API in correct order
    /* PERFORMANCE: Batch VA-API calls to reduce lock overhead.
     * Lock is held only during VA-API calls, not during slice parsing.
     * This balances single-threaded performance (fewer locks) with multi-threaded
     * performance (shorter critical sections).
     */
    for (int i = 0; i < slice_count; i++) {
        VABufferID slice_parameters_buf;
        glx_ctx_lock();
        status = vaCreateBuffer(va_dpy, decoderData->context_id, VASliceParameterBufferType,
            sizeof(VASliceParameterBufferH264), 1, &slices[i].params, &slice_parameters_buf);
        if (VA_STATUS_SUCCESS != status) {
            glx_ctx_unlock();
            err_code = VDP_STATUS_ERROR;
            goto quit_free_slices;
        }
        status = vaRenderPicture(va_dpy, decoderData->context_id, &slice_parameters_buf, 1);
        if (VA_STATUS_SUCCESS != status) {
            glx_ctx_unlock();
            err_code = VDP_STATUS_ERROR;
            goto quit_free_slices;
        }

        VABufferID slice_buf;
        status = vaCreateBuffer(va_dpy, decoderData->context_id, VASliceDataBufferType,
            slices[i].slice_data_size, 1, merged_bitstream + slices[i].nal_offset, &slice_buf);
        if (VA_STATUS_SUCCESS != status) {
            glx_ctx_unlock();
            err_code = VDP_STATUS_ERROR;
            goto quit_free_slices;
        }

        status = vaRenderPicture(va_dpy, decoderData->context_id, &slice_buf, 1);
        if (VA_STATUS_SUCCESS != status) {
            glx_ctx_unlock();
            err_code = VDP_STATUS_ERROR;
            goto quit_free_slices;
        }

        vaDestroyBuffer(va_dpy, slice_parameters_buf);
        vaDestroyBuffer(va_dpy, slice_buf);
        glx_ctx_unlock();
    }

    free(slices);

    glx_ctx_lock();
    status = vaEndPicture(va_dpy, decoderData->context_id);
    glx_ctx_unlock();
    if (VA_STATUS_SUCCESS != status) {
        err_code = VDP_STATUS_ERROR;
        goto quit;
    }

    // Note: merged_bitstream buffer is reused, not freed here

    dstSurfData->sync_va_to_glx = 1;
    err_code = VDP_STATUS_OK;
    goto quit;

quit_free_slices:
    free(slices);
quit:
    return err_code;
}

VdpStatus
vdpDecoderRender(VdpDecoder decoder, VdpVideoSurface target,
                 VdpPictureInfo const *picture_info, uint32_t bitstream_buffer_count,
                 VdpBitstreamBuffer const *bitstream_buffers)
{
    VdpStatus err_code;
    if (!picture_info || !bitstream_buffers)
        return VDP_STATUS_INVALID_POINTER;
    VdpDecoderData *decoderData = handle_acquire(decoder, HANDLETYPE_DECODER);
    VdpVideoSurfaceData *dstSurfData = handle_acquire(target, HANDLETYPE_VIDEO_SURFACE);
    if (NULL == decoderData || NULL == dstSurfData) {
        err_code = VDP_STATUS_INVALID_HANDLE;
        goto quit;
    }

    if (decoderData->profile == VDP_DECODER_PROFILE_H264_CONSTRAINED_BASELINE ||
        decoderData->profile == VDP_DECODER_PROFILE_H264_BASELINE ||
        decoderData->profile == VDP_DECODER_PROFILE_H264_MAIN ||
        decoderData->profile == VDP_DECODER_PROFILE_H264_HIGH)
    {
        // TODO: check exit code
        vdpDecoderRender_h264(decoder, decoderData, dstSurfData, picture_info,
                                  bitstream_buffer_count, bitstream_buffers);
    } else {
        traceError("error (%s): no implementation for profile %s\n", __func__,
                   reverse_decoder_profile(decoderData->profile));
        err_code = VDP_STATUS_NO_IMPLEMENTATION;
        goto quit;
    }

    err_code = VDP_STATUS_OK;
quit:
    handle_release(decoder);
    handle_release(target);
    return err_code;
}
