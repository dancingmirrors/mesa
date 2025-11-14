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
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "vk_video/vulkan_video_codecs_common.h"

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
 * This is a placeholder implementation. The full implementation will:
 * 1. Translate Vulkan decode info to VA-API parameters
 * 2. Map bitstream buffer
 * 3. Create VA buffers for picture/slice params
 * 4. Submit decode operation
 * 5. Handle synchronization
 */
VkResult
anv_vaapi_decode_frame(struct anv_cmd_buffer *cmd_buffer,
                       const VkVideoDecodeInfoKHR *frame_info)
{
   ANV_FROM_HANDLE(anv_video_session, vid, frame_info->videoSession);
   
   if (!vid->vaapi_session) {
      return vk_error(cmd_buffer->device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* TODO: Implement full decode pipeline
    * This includes:
    * - Translating VkVideoDecodeH264PictureInfoKHR to VAPictureParameterBufferH264
    * - Translating slice parameters
    * - Mapping and submitting bitstream data
    * - Managing DPB (Decoded Picture Buffer)
    * - Synchronization with Vulkan timeline
    */
   
   vk_logw(VK_LOG_OBJS(&cmd_buffer->device->vk.base),
           "anv_vaapi_decode_frame: Not yet fully implemented");
   
   return VK_SUCCESS;
}

/**
 * Export Vulkan image as DMA-buf
 * 
 * This is a placeholder for DMA-buf export functionality.
 * The full implementation will export memory associated with
 * a Vulkan image as a DMA-buf file descriptor.
 */
VkResult
anv_vaapi_export_video_surface_dmabuf(struct anv_device *device,
                                      struct anv_image *image,
                                      int *fd_out)
{
   /* TODO: Implement DMA-buf export
    * This will use VK_KHR_external_memory_fd or similar mechanisms
    * to export the image's backing memory as a DMA-buf fd
    */
   
   vk_logw(VK_LOG_OBJS(&device->vk.base),
           "anv_vaapi_export_video_surface_dmabuf: Not yet implemented");
   
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}

/**
 * Import DMA-buf into VA-API surface
 * 
 * This is a placeholder for DMA-buf import functionality.
 * The full implementation will create a VA-API surface from
 * a DMA-buf exported from a Vulkan image.
 */
VkResult
anv_vaapi_import_surface_from_image(struct anv_device *device,
                                    struct anv_image *image,
                                    VASurfaceID *surface_id)
{
   /* TODO: Implement DMA-buf import to VA-API
    * Steps:
    * 1. Export image as DMA-buf using anv_vaapi_export_video_surface_dmabuf
    * 2. Set up VASurfaceAttribExternalBuffers descriptor
    * 3. Create VA surface with vaCreateSurfaces using DRM PRIME attributes
    */
   
   vk_logw(VK_LOG_OBJS(&device->vk.base),
           "anv_vaapi_import_surface_from_image: Not yet implemented");
   
   return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
}
