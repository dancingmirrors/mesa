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
 * - Synchronization between VA-API and Vulkan using GEM set_domain
 */

#include "anv_video_vaapi_bridge.h"
#include "anv_private.h"

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#include "vk_video/vulkan_video_codecs_common.h"
#include "drm-uapi/i915_drm.h"
#include "drm-uapi/drm_fourcc.h"

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
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to get VA display from DRM fd\n");
      }
      return NULL;
   }
   
   /* Initialize VA-API */
   int major, minor;
   VAStatus va_status = vaInitialize(va_display, &major, &minor);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to initialize VA-API: %d\n", va_status);
      }
      return NULL;
   }
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "VA-API initialized: version %d.%d\n", major, minor);
   }
   
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
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Unsupported video codec profile\n");
      }
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
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to create VA config: %d\n", va_status);
      }
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
   
   /* Allocate surface mapping for DPB management */
   session->surface_map_capacity = session->num_surfaces;
   session->surface_map_size = 0;
   session->surface_map = vk_alloc(&device->vk.alloc,
                                   session->surface_map_capacity * sizeof(struct anv_vaapi_surface_map),
                                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!session->surface_map) {
      vk_free(&device->vk.alloc, session->va_surfaces);
      vaDestroyConfig(session->va_display, session->va_config);
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   
   /* Create VA context
    * Pass NULL for render_targets since surfaces will be created dynamically
    * when images are bound during decode operations.
    */
   va_status = vaCreateContext(session->va_display,
                               session->va_config,
                               session->width,
                               session->height,
                               VA_PROGRESSIVE,
                               NULL,  /* render_targets - populated dynamically */
                               0,     /* num_render_targets */
                               &session->va_context);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to create VA context: %d\n", va_status);
      }
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
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "VA-API session created: %ux%u, profile=%d\n",
              session->width, session->height, session->va_profile);
   }
   
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
   
   /* Free surface mapping */
   if (session->surface_map) {
      vk_free(&device->vk.alloc, session->surface_map);
   }
   
   /* Destroy context and config */
   if (session->va_context)
      vaDestroyContext(session->va_display, session->va_context);
   if (session->va_config)
      vaDestroyConfig(session->va_display, session->va_config);
   
   /* Note: VA display is managed by the device and not destroyed here */
   
   vk_free(&device->vk.alloc, vid->vaapi_session);
   vid->vaapi_session = NULL;
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "VA-API session destroyed\n");
   }
}

/**
 * Add or update a surface mapping in the session
 */
void
anv_vaapi_add_surface_mapping(struct anv_vaapi_session *session,
                               const struct anv_image *image,
                               VASurfaceID va_surface)
{
   /* Check if already mapped */
   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      if (session->surface_map[i].image == image) {
         session->surface_map[i].va_surface = va_surface;
         return;
      }
   }
   
   /* Add new mapping if space available */
   if (session->surface_map_size < session->surface_map_capacity) {
      session->surface_map[session->surface_map_size].image = image;
      session->surface_map[session->surface_map_size].va_surface = va_surface;
      session->surface_map_size++;
   }
}

/**
 * Lookup VA surface ID for a given image
 */
VASurfaceID
anv_vaapi_lookup_surface(struct anv_vaapi_session *session,
                          const struct anv_image *image)
{
   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      if (session->surface_map[i].image == image) {
         return session->surface_map[i].va_surface;
      }
   }
   return VA_INVALID_SURFACE;
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
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Missing H.264 picture info in decode\n");
      }
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }
   
   /* Get destination image view and extract image */
   ANV_FROM_HANDLE(anv_image_view, dst_image_view, frame_info->dstPictureResource.imageViewBinding);
   if (!dst_image_view || !dst_image_view->image) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Invalid destination image view for decode\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   const struct anv_image *dst_image = dst_image_view->image;
   
   /* Import destination surface to VA-API (or get cached surface)
    * Check if this surface is already mapped to avoid creating duplicates
    */
   VASurfaceID dst_surface = anv_vaapi_lookup_surface(session, dst_image);
   if (dst_surface == VA_INVALID_SURFACE) {
      /* Not in cache, import it now */
      result = anv_vaapi_import_surface_from_image(device, (struct anv_image *)dst_image, &dst_surface);
      if (result != VK_SUCCESS) {
         return result;
      }
      
      /* Add destination surface to mapping for future use */
      anv_vaapi_add_surface_mapping(session, dst_image, dst_surface);
   } else if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "VA-API decode: Reusing cached destination surface %u\n", dst_surface);
   }
   
   /* CRITICAL SYNCHRONIZATION (Phase 3, Part 4 - Before VA-API decode):
    * Before VA-API starts decoding to the surface, we need to ensure any
    * previous Vulkan operations on the surface have completed.
    * 
    * Set the GEM domain to CPU to force a wait for any pending GPU operations.
    * This ensures:
    * 1. Any previous render/texture operations on this surface complete
    * 2. Caches are flushed so VA-API sees the correct surface state
    */
   struct anv_image_binding *dst_binding = &((struct anv_image *)dst_image)->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];
   if (dst_binding->address.bo) {
      struct drm_i915_gem_set_domain set_domain = {
         .handle = dst_binding->address.bo->gem_handle,
         .read_domains = I915_GEM_DOMAIN_CPU,  /* Wait for GPU to finish */
         .write_domain = I915_GEM_DOMAIN_CPU,  /* VA-API will write */
      };
      
      int ret = intel_ioctl(device->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain);
      if (ret != 0) {
         if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
            fprintf(stderr, "Failed to set GEM domain before VA-API decode: %m\n");
         }
      } else if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "VA-API decode: Synchronized surface before decode\n");
      }
   }
   
   /* Get video session parameters - already a pointer, no need for FROM_HANDLE */
   struct anv_video_session_params *params = cmd_buffer->video.params;
   if (!params) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "No video session parameters bound\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Import reference frame surfaces and add to mapping for DPB */
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      const VkVideoReferenceSlotInfoKHR *ref_slot = &frame_info->pReferenceSlots[i];
      if (ref_slot->slotIndex < 0 || !ref_slot->pPictureResource)
         continue;
      
      ANV_FROM_HANDLE(anv_image_view, ref_image_view, ref_slot->pPictureResource->imageViewBinding);
      if (!ref_image_view || !ref_image_view->image)
         continue;
      
      const struct anv_image *ref_image = ref_image_view->image;
      
      /* Check if already mapped */
      VASurfaceID ref_surface = anv_vaapi_lookup_surface(session, ref_image);
      if (ref_surface == VA_INVALID_SURFACE) {
         /* Import and add to mapping */
         result = anv_vaapi_import_surface_from_image(device, (struct anv_image *)ref_image, &ref_surface);
         if (result == VK_SUCCESS) {
            anv_vaapi_add_surface_mapping(session, ref_image, ref_surface);
         }
      }
   }
   
   /* Translate picture parameters */
   VAPictureParameterBufferH264 va_pic_param;
   anv_vaapi_translate_h264_picture_params(device, frame_info, h264_pic_info,
                                           &params->vk, session, dst_surface, &va_pic_param);
   
   /* Create picture parameter buffer */
   VABufferID pic_param_buf;
   va_status = vaCreateBuffer(session->va_display, session->va_context,
                              VAPictureParameterBufferType,
                              sizeof(VAPictureParameterBufferH264), 1,
                              &va_pic_param, &pic_param_buf);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to create VA picture parameter buffer: %d\n", va_status);
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Get bitstream buffer */
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   if (!src_buffer || !src_buffer->address.bo) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Invalid source buffer for decode\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   /* Map the bitstream buffer to get its contents */
   void *bitstream_data = anv_gem_mmap(device, src_buffer->address.bo->gem_handle,
                                       0, frame_info->srcBufferRange, 0);
   if (!bitstream_data) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to map bitstream buffer\n");
      }
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   }
   
   /* Process multiple slices - H.264 frames typically have multiple slices
    * We need to create a slice parameter and slice data buffer for each slice.
    */
   uint32_t slice_count = h264_pic_info->sliceCount;
   if (slice_count == 0) {
      anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
      vaDestroyBuffer(session->va_display, pic_param_buf);
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "H.264 decode has no slices\n");
      }
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "VA-API H.264: Processing %u slices\n", slice_count);
   }
   
   /* Allocate arrays for slice buffers */
   VABufferID *slice_param_bufs = vk_alloc(&device->vk.alloc,
                                           slice_count * sizeof(VABufferID),
                                           8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   VABufferID *slice_data_bufs = vk_alloc(&device->vk.alloc,
                                          slice_count * sizeof(VABufferID),
                                          8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!slice_param_bufs || !slice_data_bufs) {
      anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
      vaDestroyBuffer(session->va_display, pic_param_buf);
      vk_free(&device->vk.alloc, slice_param_bufs);
      vk_free(&device->vk.alloc, slice_data_bufs);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   
   /* Process each slice */
   for (uint32_t s = 0; s < slice_count; s++) {
      bool last_slice = (s == slice_count - 1);
      uint32_t slice_offset = h264_pic_info->pSliceOffsets[s];
      uint32_t slice_size;
      
      if (last_slice) {
         /* Last slice goes to end of buffer */
         slice_size = frame_info->srcBufferRange - slice_offset;
      } else {
         /* Slice size = next slice offset - current offset */
         slice_size = h264_pic_info->pSliceOffsets[s + 1] - slice_offset;
      }
      
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "  Slice %u: offset=%u size=%u\n", s, slice_offset, slice_size);
      }
      
      /* Create slice parameter buffer for this slice */
      VASliceParameterBufferH264 va_slice_param;
      anv_vaapi_translate_h264_slice_params(device, frame_info, h264_pic_info,
                                            session, &va_pic_param,
                                            slice_offset, slice_size,
                                            &va_slice_param);
      
      va_status = vaCreateBuffer(session->va_display, session->va_context,
                                 VASliceParameterBufferType,
                                 sizeof(VASliceParameterBufferH264), 1,
                                 &va_slice_param, &slice_param_bufs[s]);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
            fprintf(stderr, "Failed to create VA slice parameter buffer %u: %d\n", s, va_status);
         }
         /* Clean up previously created buffers */
         for (uint32_t i = 0; i < s; i++) {
            vaDestroyBuffer(session->va_display, slice_param_bufs[i]);
            vaDestroyBuffer(session->va_display, slice_data_bufs[i]);
         }
         anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
         vaDestroyBuffer(session->va_display, pic_param_buf);
         vk_free(&device->vk.alloc, slice_param_bufs);
         vk_free(&device->vk.alloc, slice_data_bufs);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
      
      /* Create slice data buffer with the actual bitstream for this slice */
      va_status = vaCreateBuffer(session->va_display, session->va_context,
                                 VASliceDataBufferType,
                                 slice_size, 1,
                                 bitstream_data + frame_info->srcBufferOffset + slice_offset,
                                 &slice_data_bufs[s]);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
            fprintf(stderr, "Failed to create VA slice data buffer %u: %d\n", s, va_status);
         }
         vaDestroyBuffer(session->va_display, slice_param_bufs[s]);
         /* Clean up previously created buffers */
         for (uint32_t i = 0; i < s; i++) {
            vaDestroyBuffer(session->va_display, slice_param_bufs[i]);
            vaDestroyBuffer(session->va_display, slice_data_bufs[i]);
         }
         anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
         vaDestroyBuffer(session->va_display, pic_param_buf);
         vk_free(&device->vk.alloc, slice_param_bufs);
         vk_free(&device->vk.alloc, slice_data_bufs);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
   }
   
   /* Submit decode operation to VA-API */
   va_status = vaBeginPicture(session->va_display, session->va_context, dst_surface);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "vaBeginPicture failed: %d\n", va_status);
      }
      goto cleanup_buffers;
   }
   
   /* Render picture parameters */
   va_status = vaRenderPicture(session->va_display, session->va_context,
                               &pic_param_buf, 1);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "vaRenderPicture (picture params) failed: %d\n", va_status);
      }
      vaEndPicture(session->va_display, session->va_context);
      goto cleanup_buffers;
   }
   
   /* Render all slices (slice parameters and slice data) */
   for (uint32_t s = 0; s < slice_count; s++) {
      /* Render slice parameters for this slice */
      va_status = vaRenderPicture(session->va_display, session->va_context,
                                  &slice_param_bufs[s], 1);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
            fprintf(stderr, "vaRenderPicture (slice %u params) failed: %d\n", s, va_status);
         }
         vaEndPicture(session->va_display, session->va_context);
         goto cleanup_buffers;
      }
      
      /* Render slice data for this slice */
      va_status = vaRenderPicture(session->va_display, session->va_context,
                                  &slice_data_bufs[s], 1);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
            fprintf(stderr, "vaRenderPicture (slice %u data) failed: %d\n", s, va_status);
         }
         vaEndPicture(session->va_display, session->va_context);
         goto cleanup_buffers;
      }
   }
   
   /* End picture and execute decode */
   va_status = vaEndPicture(session->va_display, session->va_context);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "vaEndPicture failed: %d\n", va_status);
      }
      goto cleanup_buffers;
   }
   
   /* Sync - wait for decode to complete
    * This is critical for Phase 3, Part 4: ensures VA-API decode finishes
    * before Vulkan accesses the surface.
    */
   va_status = vaSyncSurface(session->va_display, dst_surface);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "vaSyncSurface failed: %d\n", va_status);
      }
   }
   
   /* CRITICAL SYNCHRONIZATION (Phase 3, Part 4):
    * After VA-API decode completes, we need to ensure proper cache coherency
    * between the VA-API driver (which used the video engine) and Vulkan.
    * 
    * On Gen7/7.5/8, the video engine and render engine have separate caches,
    * so we must ensure:
    * 1. VA-API decode has completed (done via vaSyncSurface above)
    * 2. Any pending writes from video engine are flushed
    * 3. Vulkan render cache will be invalidated when accessing the surface
    * 
    * We use GTT domain because:
    * - Modern kernels only support CPU, GTT, and WC domains for set_domain
    * - I915_GEM_DOMAIN_RENDER is NOT valid and returns EINVAL
    * - GTT ensures the surface is accessible for GPU operations (which Vulkan needs)
    * - The kernel handles necessary cache flushes when transitioning to GTT
    * - This matches the pattern used by crocus (Gen7/7.5/8 Gallium driver)
    */
   if (dst_binding->address.bo) {
      struct drm_i915_gem_set_domain set_domain = {
         .handle = dst_binding->address.bo->gem_handle,
         .read_domains = I915_GEM_DOMAIN_GTT,  /* GPU aperture access for Vulkan */
         .write_domain = 0,  /* No immediate write, just transitioning from VA-API */
      };
      
      int ret = intel_ioctl(device->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain);
      if (ret != 0) {
         if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
            fprintf(stderr, "Failed to set GEM domain after VA-API decode: %m\n");
         }
      } else if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "VA-API decode: Successfully transitioned BO to GTT domain\n");
      }
   }
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "Successfully decoded H.264 frame via VA-API (%u slices)\n", slice_count);
   }
   
cleanup_buffers:
   /* Clean up all slice buffers */
   for (uint32_t s = 0; s < slice_count; s++) {
      vaDestroyBuffer(session->va_display, slice_data_bufs[s]);
      vaDestroyBuffer(session->va_display, slice_param_bufs[s]);
   }
   vk_free(&device->vk.alloc, slice_data_bufs);
   vk_free(&device->vk.alloc, slice_param_bufs);
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
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Image has no backing memory\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   struct anv_bo *bo = binding->address.bo;
   
   /* For video images, we need to export them via DMA-buf for VA-API sharing.
    * If the BO is not marked as external, mark it now. This is safe for video
    * decode surfaces since they're not used in contexts where external flag
    * would cause issues.
    */
   if (!bo->is_external) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Marking video BO as external for DMA-buf export\n");
      }
      bo->is_external = true;
   }
   
   /* Export the BO as a DMA-buf file descriptor using GEM handle to fd */
   int fd = anv_gem_handle_to_fd(device, bo->gem_handle);
   if (fd < 0) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to export BO as DMA-buf: %m\n");
      }
      return vk_error(device, VK_ERROR_TOO_MANY_OBJECTS);
   }
   
   *fd_out = fd;
   return VK_SUCCESS;
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
   
   /* Get image layout information for stride and offsets
    * For NV12 (multi-planar format), we need both Y and UV plane information
    */
   uint32_t y_plane = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_0_BIT);
   uint32_t uv_plane = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_1_BIT);
   const struct anv_surface *y_surface = &image->planes[y_plane].primary_surface;
   const struct anv_surface *uv_surface = &image->planes[uv_plane].primary_surface;
   
   /* Set up DMA-buf descriptor for VA-API
    * For NV12 format (YUV 4:2:0), we have 2 planes:
    * - Plane 0: Y (luma)
    * - Plane 1: UV (chroma, interleaved)
    * 
    * CRITICAL: The UV plane offset must be calculated from the actual ISL
    * surface layout, NOT just height*stride, because ISL may add padding
    * for alignment requirements.
    * 
    * CRITICAL 2: Video surfaces on Gen7/7.5/8 MUST use Y-tiling per the PRM.
    * The tiling information is now set on the GEM BO via anv_device_set_bo_tiling()
    * in anv_device.c. When VA-API imports the DMA-buf, the i965/crocus driver
    * queries the kernel to get the tiling mode from the BO. This is the standard
    * mechanism for communicating tiling for legacy (non-modifier) DMA-buf imports.
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
   extbuf.pitches[0] = y_surface->isl.row_pitch_B;
   extbuf.pitches[1] = uv_surface->isl.row_pitch_B;
   
   /* Set offsets for each plane using actual memory layout from ISL
    * This is critical - we must use the ISL-calculated offset, not a
    * simple height*stride calculation, because ISL adds alignment padding.
    */
   extbuf.offsets[0] = 0;  /* Y plane starts at beginning */
   extbuf.offsets[1] = uv_surface->memory_range.offset;  /* UV plane from ISL */
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "VA-API surface import: %ux%u NV12\n", 
              image->vk.extent.width, image->vk.extent.height);
      fprintf(stderr, "  Y plane:  pitch=%u offset=%u\n", 
              extbuf.pitches[0], extbuf.offsets[0]);
      fprintf(stderr, "  UV plane: pitch=%u offset=%u\n", 
              extbuf.pitches[1], extbuf.offsets[1]);
      fprintf(stderr, "  Y surface:  row_pitch=%u size=%lu\n",
              y_surface->isl.row_pitch_B, y_surface->memory_range.size);
      fprintf(stderr, "  UV surface: row_pitch=%u offset=%lu size=%lu\n",
              uv_surface->isl.row_pitch_B, uv_surface->memory_range.offset,
              uv_surface->memory_range.size);
   }
   
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
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Failed to create VA surface from DMA-buf: %d\n", va_status);
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   
   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "Created VA surface %u from Vulkan image (DMA-buf sharing)\n",
              *surface_id);
   }
   
   return VK_SUCCESS;
}
