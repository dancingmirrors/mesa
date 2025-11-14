/*
 * Copyright © 2024 Mesa Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * VA-API Bridge Module for HasVK
 * 
 * This module implements a bridge between Vulkan Video decode operations
 * and VA-API. It allows HasVK to leverage the stable VA-API implementation
 * on Gen7/7.5/8 hardware through the crocus driver, avoiding GPU hangs
 * that occur with direct hardware programming.
 * 
 * Key Components:
 * - VA-API session management
 * - Communication layer between Vulkan and VA-API
 * - Resource sharing via DMA-buf
 */

#include "anv_video_vaapi_bridge.h"
#include "anv_private.h"

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "vk_video/vulkan_video_codecs_common.h"

/* Define VA-API constants and macros for compatibility with legacy intel-vaapi-driver */
#ifndef VA_FOURCC
#define VA_FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#endif

#ifndef VA_FOURCC_NV12
#define VA_FOURCC_NV12 VA_FOURCC('N','V','1','2')
#endif

#ifndef VA_RT_FORMAT_YUV420
#define VA_RT_FORMAT_YUV420 0x00000001
#endif

#ifndef VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
#define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME 0x00000008
#endif

/**
 * Map Vulkan video profile to VA-API profile
 */
static VAProfile
get_va_profile(const VkVideoProfileInfoKHR *profile)
{
   if (profile->videoCodecOperation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
      /* For now, default to H.264 Main profile
       * TODO: Parse profile info to determine Baseline/Main/High
       */
      return VAProfileH264Main;
   }
   
   /* Unsupported codec */
   return VAProfileNone;
}

/**
 * Map Vulkan video profile to VA-API entrypoint
 */
static VAEntrypoint
get_va_entrypoint(const VkVideoProfileInfoKHR *profile)
{
   /* All decode operations use VLD (Variable Length Decode) */
   if (profile->videoCodecOperation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
      return VAEntrypointVLD;
   }
   
   return 0; /* Invalid */
}

/**
 * Get or create VA display from device
 */
VADisplay
anv_vaapi_get_display(struct anv_device *device)
{
   /* Check if VA display already exists */
   if (device->va_display)
      return device->va_display;
   
   /* Create VA display from DRM file descriptor */
   VADisplay va_display = vaGetDisplayDRM(device->fd);
   if (!va_display) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to get VA display from DRM fd");
      return NULL;
   }
   
   /* Initialize VA-API */
   int major, minor;
   VAStatus va_status = vaInitialize(va_display, &major, &minor);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to initialize VA-API: %d", va_status);
      return NULL;
   }
   
   vk_logi(VK_LOG_OBJS(&device->vk.base),
           "VA-API initialized: version %d.%d", major, minor);
   
   device->va_display = va_display;
   return va_display;
}

/**
 * Create VA-API session for video decoding
 */
VkResult
anv_vaapi_session_create(struct anv_device *device,
                         struct anv_video_session *vid,
                         const VkVideoSessionCreateInfoKHR *pCreateInfo)
{
   VAStatus va_status;
   
   /* Allocate VA-API session structure */
   vid->vaapi_session = vk_alloc(&device->vk.alloc, sizeof(struct anv_vaapi_session),
                                  8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!vid->vaapi_session)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   
   memset(vid->vaapi_session, 0, sizeof(struct anv_vaapi_session));
   
   struct anv_vaapi_session *session = vid->vaapi_session;
   
   /* Get or create VA display */
   session->va_display = anv_vaapi_get_display(device);
   if (!session->va_display) {
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Store video dimensions */
   session->width = pCreateInfo->maxCodedExtent.width;
   session->height = pCreateInfo->maxCodedExtent.height;
   
   /* Get VA profile and entrypoint from Vulkan profile */
   session->va_profile = get_va_profile(pCreateInfo->pVideoProfile);
   VAEntrypoint va_entrypoint = get_va_entrypoint(pCreateInfo->pVideoProfile);
   
   if (session->va_profile == VAProfileNone || va_entrypoint == 0) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Unsupported video codec profile");
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
   }
   
   /* Create VA config */
   va_status = vaCreateConfig(session->va_display,
                              session->va_profile,
                              va_entrypoint,
                              NULL, 0,
                              &session->va_config);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to create VA config: %d", va_status);
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Allocate DPB surfaces array
    * maxDpbSlots indicates the maximum number of reference frames
    */
   session->num_surfaces = pCreateInfo->maxDpbSlots + 1; /* +1 for current frame */
   session->va_surfaces = vk_alloc(&device->vk.alloc,
                                   session->num_surfaces * sizeof(VASurfaceID),
                                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!session->va_surfaces) {
      vaDestroyConfig(session->va_display, session->va_config);
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   
   /* Initialize surface IDs to invalid */
   for (uint32_t i = 0; i < session->num_surfaces; i++) {
      session->va_surfaces[i] = VA_INVALID_SURFACE;
   }
   
   /* Create VA context
    * Note: Surfaces will be created later when images are bound
    */
   va_status = vaCreateContext(session->va_display,
                               session->va_config,
                               session->width,
                               session->height,
                               VA_PROGRESSIVE,
                               session->va_surfaces,
                               session->num_surfaces,
                               &session->va_context);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to create VA context: %d", va_status);
      vk_free(&device->vk.alloc, session->va_surfaces);
      vaDestroyConfig(session->va_display, session->va_config);
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Initialize buffer IDs to invalid */
   session->va_picture_param = VA_INVALID_ID;
   session->va_slice_param = VA_INVALID_ID;
   session->va_slice_data = VA_INVALID_ID;
   
   vk_logi(VK_LOG_OBJS(&device->vk.base),
           "VA-API session created: %ux%u, profile=%d",
           session->width, session->height, session->va_profile);
   
   return VK_SUCCESS;
}

/**
 * Destroy VA-API session
 */
void
anv_vaapi_session_destroy(struct anv_device *device,
                          struct anv_video_session *vid)
{
   if (!vid->vaapi_session)
      return;
   
   struct anv_vaapi_session *session = vid->vaapi_session;
   
   /* Destroy parameter buffers */
   if (session->va_picture_param != VA_INVALID_ID)
      vaDestroyBuffer(session->va_display, session->va_picture_param);
   if (session->va_slice_param != VA_INVALID_ID)
      vaDestroyBuffer(session->va_display, session->va_slice_param);
   if (session->va_slice_data != VA_INVALID_ID)
      vaDestroyBuffer(session->va_display, session->va_slice_data);
   
   /* Destroy surfaces */
   if (session->va_surfaces) {
      for (uint32_t i = 0; i < session->num_surfaces; i++) {
         if (session->va_surfaces[i] != VA_INVALID_SURFACE) {
            vaDestroySurfaces(session->va_display, &session->va_surfaces[i], 1);
         }
      }
      vk_free(&device->vk.alloc, session->va_surfaces);
   }
   
   /* Destroy context and config */
   if (session->va_context)
      vaDestroyContext(session->va_display, session->va_context);
   if (session->va_config)
      vaDestroyConfig(session->va_display, session->va_config);
   
   /* Note: VA display is managed by the device and not destroyed here */
   
   vk_free(&device->vk.alloc, vid->vaapi_session);
   vid->vaapi_session = NULL;
   
   vk_logi(VK_LOG_OBJS(&device->vk.base),
           "VA-API session destroyed");
}

/**
 * Decode a frame using VA-API
 * 
 * Translates Vulkan Video decode parameters to VA-API and submits the decode operation.
 */
VkResult
anv_vaapi_decode_frame(struct anv_cmd_buffer *cmd_buffer,
                       const VkVideoDecodeInfoKHR *frame_info)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_video_session *vid = cmd_buffer->video.vid;
   struct anv_vaapi_session *session = vid->vaapi_session;
   VAStatus va_status;
   VkResult result;
   
   if (!vid || !session) {
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Get H.264-specific picture info */
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   if (!h264_pic_info) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Missing H.264 picture info in decode");
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }
   
   /* Get destination image view and extract image */
   ANV_FROM_HANDLE(anv_image_view, dst_image_view, frame_info->dstPictureResource.imageViewBinding);
   if (!dst_image_view || !dst_image_view->image) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Invalid destination image view for decode");
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   const struct anv_image *dst_image = dst_image_view->image;
   
   /* Import destination surface to VA-API (or get cached surface) */
   VASurfaceID dst_surface;
   result = anv_vaapi_import_surface_from_image(device, (struct anv_image *)dst_image, &dst_surface);
   if (result != VK_SUCCESS) {
      return result;
   }
   
   /* Get video session parameters - already a pointer, no need for FROM_HANDLE */
   struct anv_video_session_params *params = cmd_buffer->video.params;
   if (!params) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "No video session parameters bound");
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Translate picture parameters */
   VAPictureParameterBufferH264 va_pic_param;
   anv_vaapi_translate_h264_picture_params(device, frame_info, h264_pic_info,
                                           &params->vk, dst_surface, &va_pic_param);
   
   /* Create picture parameter buffer */
   VABufferID pic_param_buf;
   va_status = vaCreateBuffer(session->va_display, session->va_context,
                              VAPictureParameterBufferType,
                              sizeof(VAPictureParameterBufferH264), 1,
                              &va_pic_param, &pic_param_buf);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to create VA picture parameter buffer: %d", va_status);
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Get bitstream buffer */
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   if (!src_buffer || !src_buffer->address.bo) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Invalid source buffer for decode");
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Map the bitstream buffer to get its contents */
   void *bitstream_data = anv_gem_mmap(device, src_buffer->address.bo->gem_handle,
                                       0, frame_info->srcBufferRange, 0);
   if (!bitstream_data) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to map bitstream buffer");
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   }
   
   /* Create slice parameter buffer - simplified version */
   VASliceParameterBufferH264 va_slice_param;
   anv_vaapi_translate_h264_slice_params(frame_info, h264_pic_info,
                                         0, frame_info->srcBufferRange,
                                         &va_slice_param);
   
   VABufferID slice_param_buf;
   va_status = vaCreateBuffer(session->va_display, session->va_context,
                              VASliceParameterBufferType,
                              sizeof(VASliceParameterBufferH264), 1,
                              &va_slice_param, &slice_param_buf);
   if (va_status != VA_STATUS_SUCCESS) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to create VA slice parameter buffer: %d", va_status);
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Create slice data buffer with the actual bitstream */
   VABufferID slice_data_buf;
   va_status = vaCreateBuffer(session->va_display, session->va_context,
                              VASliceDataBufferType,
                              frame_info->srcBufferRange, 1,
                              bitstream_data + frame_info->srcBufferOffset,
                              &slice_data_buf);
   if (va_status != VA_STATUS_SUCCESS) {
      vaDestroyBuffer(session->va_display, slice_param_buf);
      vaDestroyBuffer(session->va_display, pic_param_buf);
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to create VA slice data buffer: %d", va_status);
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Submit decode operation to VA-API */
   va_status = vaBeginPicture(session->va_display, session->va_context, dst_surface);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "vaBeginPicture failed: %d", va_status);
      goto cleanup_buffers;
   }
   
   /* Render picture parameters */
   va_status = vaRenderPicture(session->va_display, session->va_context,
                               &pic_param_buf, 1);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "vaRenderPicture (picture params) failed: %d", va_status);
      vaEndPicture(session->va_display, session->va_context);
      goto cleanup_buffers;
   }
   
   /* Render slice parameters */
   va_status = vaRenderPicture(session->va_display, session->va_context,
                               &slice_param_buf, 1);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "vaRenderPicture (slice params) failed: %d", va_status);
      vaEndPicture(session->va_display, session->va_context);
      goto cleanup_buffers;
   }
   
   /* Render slice data */
   va_status = vaRenderPicture(session->va_display, session->va_context,
                               &slice_data_buf, 1);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "vaRenderPicture (slice data) failed: %d", va_status);
      vaEndPicture(session->va_display, session->va_context);
      goto cleanup_buffers;
   }
   
   /* End picture and execute decode */
   va_status = vaEndPicture(session->va_display, session->va_context);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "vaEndPicture failed: %d", va_status);
      goto cleanup_buffers;
   }
   
   /* Sync - wait for decode to complete */
   va_status = vaSyncSurface(session->va_display, dst_surface);
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "vaSyncSurface failed: %d", va_status);
   }
   
   vk_logi(VK_LOG_OBJS(&device->vk.base),
           "Successfully decoded H.264 frame via VA-API");
   
cleanup_buffers:
   vaDestroyBuffer(session->va_display, slice_data_buf);
   vaDestroyBuffer(session->va_display, slice_param_buf);
   vaDestroyBuffer(session->va_display, pic_param_buf);
   
   /* Unmap the bitstream buffer */
   anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
   
   return va_status == VA_STATUS_SUCCESS ? VK_SUCCESS : 
          vk_error(device, VK_ERROR_UNKNOWN);
}

/**
 * Export Vulkan image as DMA-buf
 * 
 * Exports the memory backing a Vulkan video image as a DMA-buf
 * file descriptor for sharing with VA-API.
 */
VkResult
anv_vaapi_export_video_surface_dmabuf(struct anv_device *device,
                                      struct anv_image *image,
                                      int *fd_out)
{
   /* Get the main memory binding for the image */
   struct anv_image_binding *binding = &image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];
   
   if (!binding->address.bo) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Image has no backing memory");
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   struct anv_bo *bo = binding->address.bo;
   
   /* Ensure the BO is marked for external use */
   if (!bo->is_external) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Image memory is not marked as external");
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Export the BO as a DMA-buf file descriptor */
   return anv_device_export_bo(device, bo, fd_out);
}

/**
 * Import DMA-buf into VA-API surface
 * 
 * Creates a VA-API surface from a DMA-buf exported from a Vulkan image.
 * This enables resource sharing between Vulkan (hasvk) and VA-API (crocus).
 */
VkResult
anv_vaapi_import_surface_from_image(struct anv_device *device,
                                    struct anv_image *image,
                                    VASurfaceID *surface_id)
{
   VAStatus va_status;
   int fd = -1;
   
   /* Get VA display */
   VADisplay va_display = anv_vaapi_get_display(device);
   if (!va_display) {
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Export image memory as DMA-buf */
   VkResult result = anv_vaapi_export_video_surface_dmabuf(device, image, &fd);
   if (result != VK_SUCCESS) {
      return result;
   }
   
   /* Get image layout information for stride and offsets */
   uint32_t plane = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_COLOR_BIT);
   const struct anv_surface *surface = &image->planes[plane].primary_surface;
   
   /* Set up DMA-buf descriptor for VA-API
    * For NV12 format (YUV 4:2:0), we have 2 planes:
    * - Plane 0: Y (luma)
    * - Plane 1: UV (chroma, interleaved)
    * 
    * Note: The VA-API driver (crocus/i965) will automatically handle Y-tiling
    * for i915 DRM buffers on Gen7/7.5/8 hardware.
    */
   VASurfaceAttribExternalBuffers extbuf = {
      .pixel_format = VA_FOURCC_NV12,
      .width = image->vk.extent.width,
      .height = image->vk.extent.height,
      .num_buffers = 1,
      .buffers = (uintptr_t *)&fd,
      .flags = 0,
      .num_planes = 2,  /* Y and UV for NV12 */
   };
   
   /* Set stride (row pitch) for both planes
    * For NV12:
    * - Y plane has full width stride
    * - UV plane has same stride (U and V are interleaved)
    */
   extbuf.pitches[0] = surface->isl.row_pitch_B;
   extbuf.pitches[1] = surface->isl.row_pitch_B;
   
   /* Set offsets for each plane
    * For NV12:
    * - Y plane starts at offset 0
    * - UV plane starts after Y plane data
    */
   extbuf.offsets[0] = 0;
   /* UV plane offset = Y plane size = height * stride */
   extbuf.offsets[1] = image->vk.extent.height * surface->isl.row_pitch_B;
   
   /* Set up surface attributes for DRM PRIME import */
   VASurfaceAttrib attribs[2] = {
      {
         .type = VASurfaceAttribMemoryType,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value.type = VAGenericValueTypeInteger,
         .value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
      },
      {
         .type = VASurfaceAttribExternalBufferDescriptor,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value.type = VAGenericValueTypePointer,
         .value.value.p = &extbuf,
      },
   };
   
   /* Create VA surface from DMA-buf */
   va_status = vaCreateSurfaces(
      va_display,
      VA_RT_FORMAT_YUV420,  /* NV12 is YUV 4:2:0 */
      image->vk.extent.width,
      image->vk.extent.height,
      surface_id,
      1,  /* num_surfaces */
      attribs,
      2   /* num_attribs */
   );
   
   /* Close the fd - VA-API will duplicate it internally */
   close(fd);
   
   if (va_status != VA_STATUS_SUCCESS) {
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Failed to create VA surface from DMA-buf: %d", va_status);
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   vk_logi(VK_LOG_OBJS(&device->vk.base),
           "Created VA surface %u from Vulkan image (DMA-buf sharing)",
           *surface_id);
   
   return VK_SUCCESS;
}
