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
 * - Synchronization handled by VA-API's vaSyncSurface and Vulkan barriers
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <errno.h>
#include <inttypes.h>

#include "vk_video/vulkan_video_codecs_common.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "drm-uapi/i915_drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/cache_ops.h"

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
 * WORKAROUND: Fix off-by-one alignment issues for video surface offsets on Gen7.
 * Similar to the depth/stencil buffer fix, ISL-computed surface offsets can sometimes
 * be off by exactly 1 byte from the required alignment.
 */
static inline uint64_t
fix_gen7_surface_offset_alignment(struct anv_device *device, uint64_t offset, uint32_t alignment)
{
   /* Only apply fix on Gen7 (Ivy Bridge/Haswell) */
   if (device->info->verx10 != 70)
      return offset;

   if (offset % alignment != 0) {
      uint64_t misalignment = offset % alignment;

      /* WORKAROUND: If off by exactly 1, fix it */
      if (misalignment == alignment - 1) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "VA-API 7/7.5 alignment fix: offset %llu -> %llu (alignment %u)\n",
                    (unsigned long long)offset, (unsigned long long)(offset + 1), alignment);
         }
         offset += 1;
      }
   }

   return offset;
}

/**
 * Map Vulkan video profile to VA-API profile
 */
static VAProfile
get_va_profile(const VkVideoProfileInfoKHR *profile)
{
   if (profile->videoCodecOperation ==
       VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
      /* Parse H.264 profile info to determine Baseline/Main/High */
      const VkVideoDecodeH264ProfileInfoKHR *h264_profile =
         vk_find_struct_const(profile->pNext, VIDEO_DECODE_H264_PROFILE_INFO_KHR);

      if (h264_profile) {
         switch (h264_profile->stdProfileIdc) {
         case STD_VIDEO_H264_PROFILE_IDC_BASELINE:
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VA-API: Parsed H.264 profile: Baseline (IDC=%d) -> VAProfileH264ConstrainedBaseline\n",
                       h264_profile->stdProfileIdc);
            }
            return VAProfileH264ConstrainedBaseline;
         case STD_VIDEO_H264_PROFILE_IDC_MAIN:
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VA-API: Parsed H.264 profile: Main (IDC=%d) -> VAProfileH264Main\n",
                       h264_profile->stdProfileIdc);
            }
            return VAProfileH264Main;
         case STD_VIDEO_H264_PROFILE_IDC_HIGH:
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VA-API: Parsed H.264 profile: High (IDC=%d) -> VAProfileH264High\n",
                       h264_profile->stdProfileIdc);
            }
            return VAProfileH264High;
         default:
            /* Unsupported H.264 profile, default to Main */
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VA-API: Unsupported H.264 profile (IDC=%d), defaulting to VAProfileH264Main\n",
                       h264_profile->stdProfileIdc);
            }
            return VAProfileH264Main;
         }
      }

      /* No profile info provided, default to Main */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VA-API: No H.264 profile info provided, defaulting to VAProfileH264Main\n");
      }
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
   if (profile->videoCodecOperation ==
       VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
      return VAEntrypointVLD;
   }

   return 0;                    /* Invalid */
}

/**
 * Get or create VA display from device
 *
 * CRITICAL FIX: Opens a separate DRM file descriptor for VA-API operations.
 * This prevents conflicts when FFmpeg's Vulkan hwdec and libplacebo each
 * create their own Vulkan instances with separate DRM fds, all accessing
 * the same i915 hardware.
 *
 * Without this fix, sharing the Vulkan device's DRM fd causes hangs with
 * "Using 39-bit DMA addresses" in dmesg when multiple Vulkan contexts
 * (FFmpeg's hwdec + libplacebo's renderer) try to use the same fd.
 */
VADisplay
anv_vaapi_get_display(struct anv_device *device)
{
   /* Check if VA display already exists */
   if (device->va_display)
      return device->va_display;

   /* CRITICAL FIX: Open a separate DRM file descriptor for VA-API
    * to avoid conflicts with Vulkan's DRM fd.
    *
    * Problem: When FFmpeg's Vulkan hwdec creates its own Vulkan instance
    * and libplacebo also has a Vulkan context, they each have separate
    * DRM file descriptors. If VA-API uses the Vulkan device's fd, it can
    * conflict with operations from other Vulkan instances on the same
    * i915 hardware, causing hangs.
    *
    * Solution: VA-API gets its own dedicated DRM fd by opening the same
    * device path that Vulkan is using. This isolates VA-API operations from
    * Vulkan's operations, preventing the "39-bit DMA addresses" hang.
    *
    * We use the physical device path (e.g., /dev/dri/renderD128) which is
    * the same device the Vulkan driver opened, but we open a separate fd
    * to it. The kernel will handle any necessary coordination between the
    * two file descriptors accessing the same device.
    */

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API: Opening separate DRM fd for VA-API operations\n");
      fprintf(stderr, "VA-API: Using device path: %s\n", device->physical->path);
      fprintf(stderr, "VA-API: This prevents conflicts with multiple Vulkan instances\n");
   }

   /* Open a dedicated DRM file descriptor for VA-API
    * We use the same device path as Vulkan, but open a separate fd
    */
   int va_drm_fd = open(device->physical->path, O_RDWR | O_CLOEXEC);
   if (va_drm_fd < 0) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VA-API: Failed to open device %s: %s\n",
                 device->physical->path, strerror(errno));
      }
      return NULL;
   }

   /* Create VA display from the dedicated DRM file descriptor */
   VADisplay va_display = vaGetDisplayDRM(va_drm_fd);
   if (!va_display) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Failed to get VA display from DRM fd\n");
      }
      close(va_drm_fd);
      return NULL;
   }

   /* Initialize VA-API */
   int major, minor;
   VAStatus va_status = vaInitialize(va_display, &major, &minor);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Failed to initialize VA-API: %d\n", va_status);
      }
      close(va_drm_fd);
      return NULL;
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API initialized: version %d.%d\n", major, minor);
      fprintf(stderr, "VA-API: Using dedicated DRM fd %d (separate from Vulkan fd %d)\n",
              va_drm_fd, device->fd);
   }

   /* Store both the display and the fd for cleanup */
   device->va_display = va_display;
   device->va_drm_fd = va_drm_fd;
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
   vid->vaapi_session =
      vk_alloc(&device->vk.alloc, sizeof(struct anv_vaapi_session), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
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
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Unsupported video codec profile\n");
      }
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device,
                      VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
   }

   /* Create VA config */
   va_status = vaCreateConfig(session->va_display,
                              session->va_profile,
                              va_entrypoint, NULL, 0, &session->va_config);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Failed to create VA config: %d\n", va_status);
      }
      vk_free(&device->vk.alloc, vid->vaapi_session);
      vid->vaapi_session = NULL;
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Allocate DPB surfaces array
    * maxDpbSlots indicates the maximum number of reference frames
    */
   session->num_surfaces = pCreateInfo->maxDpbSlots + 1;        /* +1 for current frame */
   session->va_surfaces = vk_alloc(&device->vk.alloc,
                                   session->num_surfaces *
                                   sizeof(VASurfaceID), 8,
                                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
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
                                   session->surface_map_capacity *
                                   sizeof(struct anv_vaapi_surface_map), 8,
                                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
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
   va_status = vaCreateContext(session->va_display, session->va_config, session->width, session->height, VA_PROGRESSIVE, NULL,  /* render_targets - populated dynamically */
                               0,       /* num_render_targets */
                               &session->va_context);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
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

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
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

   /* Destroy surfaces from the surface mapping (Decoded Picture Buffer / DPB)
    * These are the surfaces we created during decodes and kept alive for references
    */
   if (session->surface_map) {
      for (uint32_t i = 0; i < session->surface_map_size; i++) {
         if (session->surface_map[i].va_surface != VA_INVALID_SURFACE) {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VA-API: Destroying DPB surface %u for image %p (session cleanup)\n",
                       session->surface_map[i].va_surface,
                       (void *)session->surface_map[i].image);
            }
            vaDestroySurfaces(session->va_display, &session->surface_map[i].va_surface, 1);
         }
      }
      vk_free(&device->vk.alloc, session->surface_map);
   }

   /* Note: va_surfaces array is allocated but unused - surfaces are tracked in surface_map instead */
   if (session->va_surfaces) {
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

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
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
 * Clear all surface mappings
 *
 * Used after decode completes to remove stale mappings since we don't cache surfaces.
 *
 * Note: This only resets the count, not the underlying array. The array stays allocated
 * and is reused for the next frame. This is intentional to avoid repeated allocations.
 */
static void
anv_vaapi_clear_surface_mappings(struct anv_vaapi_session *session)
{
   session->surface_map_size = 0;
}

/**
 * Helper function to destroy VA surfaces with error logging
 *
 * Destroys VA surfaces and logs any failures for debugging.
 * Used both in error cleanup paths and normal post-decode cleanup.
 */
static void
anv_vaapi_destroy_surfaces_with_logging(VADisplay va_display,
                                        VASurfaceID *surfaces,
                                        uint32_t count,
                                        const char *surface_type)
{
   for (uint32_t i = 0; i < count; i++) {
      if (surfaces[i] != VA_INVALID_SURFACE) {
         VAStatus cleanup_status = vaDestroySurfaces(va_display, &surfaces[i], 1);
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            if (cleanup_status == VA_STATUS_SUCCESS) {
               fprintf(stderr, "VA-API: Destroyed %s surface %u (no caching)\n",
                       surface_type, surfaces[i]);
            } else {
               fprintf(stderr, "VA-API: Warning - failed to destroy %s surface %u: %d\n",
                       surface_type, surfaces[i], cleanup_status);
            }
         }
      }
   }
}

/**
 * Decode a frame using VA-API (Phase 4: Deferred Execution)
 *
 * Records VA-API decode command for later execution at QueueSubmit time.
 * This implements the command buffer pattern correctly - operations are
 * deferred until queue submission.
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

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "anv_vaapi_decode_frame: ENTRY (vid=%p, session=%p)\n",
              (void *)vid, (void *)session);
   }

   if (!vid || !session) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "anv_vaapi_decode_frame: ERROR - vid=%p session=%p (one is NULL!)\n",
                 (void *)vid, (void *)session);
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Get H.264-specific picture info */
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext,
                           VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   if (!h264_pic_info) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Missing H.264 picture info in decode\n");
      }
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   /* Get destination image view and extract image */
   ANV_FROM_HANDLE(anv_image_view, dst_image_view,
                   frame_info->dstPictureResource.imageViewBinding);
   if (!dst_image_view || !dst_image_view->image) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Invalid destination image view for decode\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   const struct anv_image *dst_image = dst_image_view->image;

   /* Import or reuse destination surface for VA-API
    *
    * CRITICAL: VA surfaces must persist as long as they're in the DPB (Decoded Picture Buffer).
    * When we decode frame N, that surface becomes a reference for future frames.
    * We CANNOT destroy it immediately - it must stay alive until it's no longer referenced.
    *
    * Strategy:
    * - Check if we already have a VA surface for this Vulkan image
    * - If yes, reuse it (previous decode into this image)
    * - If no, create a new one (first time using this image)
    * - Keep surface alive for use as reference in future frames
    */
   VASurfaceID dst_surface;
   bool dst_surface_created = false;

   /* Check if we already have a surface for this image */
   dst_surface = anv_vaapi_lookup_surface(session, dst_image);
   if (dst_surface != VA_INVALID_SURFACE) {
      /* Reuse existing surface */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr,
                 "VA-API decode: Reusing existing surface %u for image %p (DPB management)\n",
                 dst_surface, (void *)dst_image);
      }
   } else {
      /* Create new surface and add to mapping */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr,
                 "VA-API decode: Importing new destination surface for image %p (will keep for DPB)\n",
                 (void *)dst_image);
      }
      result =
         anv_vaapi_import_surface_from_image(device,
                                             (struct anv_image *) dst_image,
                                             &dst_surface);
      if (result != VK_SUCCESS) {
         return result;
      }

      /* Add to surface mapping for future reference */
      anv_vaapi_add_surface_mapping(session, dst_image, dst_surface);
      dst_surface_created = true;

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr,
                 "VA-API decode: Created new surface %u for image %p (kept for DPB)\n",
                 dst_surface, (void *)dst_image);
      }
   }

   /* Get video session parameters - already a pointer, no need for FROM_HANDLE */
   struct anv_video_session_params *params = cmd_buffer->video.params;
   if (!params) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "No video session parameters bound\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Import or reuse reference frame surfaces
    *
    * Reference frames should already exist in the surface mapping from when they were
    * decoded as destination surfaces. We look them up first and only create new surfaces
    * if they don't exist (which would be an error condition, but we handle it gracefully).
    *
    * NOTE: Reference surfaces need mapping because the H.264 parameter translation code
    * needs to lookup references to build the DPB.
    */
   VASurfaceID *ref_surfaces = NULL;
   uint32_t ref_surface_count = 0;

   /* Allocate array for reference surfaces if we have any */
   if (frame_info->referenceSlotCount > 0) {
      size_t alloc_size = (size_t)frame_info->referenceSlotCount * sizeof(VASurfaceID);
      ref_surfaces = vk_alloc(&device->vk.alloc, alloc_size, 8,
                              VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!ref_surfaces) {
         /* Clean up destination surface if we created it */
         if (dst_surface_created) {
            vaDestroySurfaces(session->va_display, &dst_surface, 1);
         }
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      const VkVideoReferenceSlotInfoKHR *ref_slot =
         &frame_info->pReferenceSlots[i];
      if (ref_slot->slotIndex < 0 || !ref_slot->pPictureResource)
         continue;

      ANV_FROM_HANDLE(anv_image_view, ref_image_view,
                      ref_slot->pPictureResource->imageViewBinding);
      if (!ref_image_view || !ref_image_view->image)
         continue;

      const struct anv_image *ref_image = ref_image_view->image;

      /* Look up existing surface first - it should exist from when this image was a decode destination */
      VASurfaceID ref_surface = anv_vaapi_lookup_surface(session, ref_image);

      if (ref_surface == VA_INVALID_SURFACE) {
         /* Reference surface doesn't exist - this shouldn't happen in normal operation
          * but we handle it gracefully by creating a new one */
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr,
                    "VA-API decode: WARNING - Reference image %p not in surface mapping, importing\n",
                    (void *)ref_image);
         }

         result =
            anv_vaapi_import_surface_from_image(device,
                                                (struct anv_image *)
                                                ref_image, &ref_surface);
         if (result != VK_SUCCESS) {
            /* Failed to import reference surface - clean up and return error */
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr,
                       "VA-API decode: Failed to import reference surface for ref image %p, cleaning up\n",
                       (void *)ref_image);
            }

            /* Destroy any successfully created reference surfaces */
            anv_vaapi_destroy_surfaces_with_logging(session->va_display, ref_surfaces,
                                                    ref_surface_count, "ref");

            /* Free the ref surfaces array */
            vk_free(&device->vk.alloc, ref_surfaces);

            /* Destroy destination surface if we created it */
            if (dst_surface_created) {
               anv_vaapi_destroy_surfaces_with_logging(session->va_display, &dst_surface,
                                                       1, "target");
            }

            return result;
         }

         /* Add new reference to mapping */
         anv_vaapi_add_surface_mapping(session, ref_image, ref_surface);

         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr,
                    "VA-API decode: Created new ref surface %u for ref image %p (added to DPB)\n",
                    ref_surface, (void *)ref_image);
         }
      } else {
         /* Found existing surface */
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr,
                    "VA-API decode: Using existing ref surface %u for ref image %p (from DPB)\n",
                    ref_surface, (void *)ref_image);
         }
      }

      /* Store reference surface (but we won't destroy it after decode) */
      ref_surfaces[ref_surface_count++] = ref_surface;
   }

   /* Translate picture parameters */
   VAPictureParameterBufferH264 va_pic_param;
   anv_vaapi_translate_h264_picture_params(device, frame_info, h264_pic_info,
                                           &params->vk, session, dst_surface,
                                           &va_pic_param);

   /* Validate that critical picture parameters were set */
   if (va_pic_param.CurrPic.picture_id == VA_INVALID_SURFACE ||
       va_pic_param.CurrPic.flags & VA_PICTURE_H264_INVALID) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr,
                 "VA-API: Picture parameter translation failed - invalid current picture\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Create picture parameter buffer */
   VABufferID pic_param_buf;
   va_status = vaCreateBuffer(session->va_display, session->va_context,
                              VAPictureParameterBufferType,
                              sizeof(VAPictureParameterBufferH264), 1,
                              &va_pic_param, &pic_param_buf);
   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Failed to create VA picture parameter buffer: %d\n",
                 va_status);
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Get bitstream buffer */
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   if (!src_buffer || !src_buffer->address.bo) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Invalid source buffer for decode\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Map the bitstream buffer to get its contents */
   void *bitstream_data =
      anv_gem_mmap(device, src_buffer->address.bo->gem_handle,
                   0, frame_info->srcBufferRange, 0);
   if (!bitstream_data) {
      vaDestroyBuffer(session->va_display, pic_param_buf);
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
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
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "H.264 decode has no slices\n");
      }
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API H.264: Processing %u slices\n", slice_count);
   }

   /* Allocate arrays for slice buffers */
   VABufferID *slice_param_bufs = vk_alloc(&device->vk.alloc,
                                           slice_count * sizeof(VABufferID),
                                           8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   VABufferID *slice_data_bufs = vk_alloc(&device->vk.alloc,
                                          slice_count * sizeof(VABufferID),
                                          8,
                                          VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
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
      }
      else {
         /* Slice size = next slice offset - current offset */
         slice_size = h264_pic_info->pSliceOffsets[s + 1] - slice_offset;
      }

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "  Slice %u: offset=%u size=%u\n", s, slice_offset,
                 slice_size);
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
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr,
                    "Failed to create VA slice parameter buffer %u: %d\n", s,
                    va_status);
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
                                 bitstream_data +
                                 frame_info->srcBufferOffset + slice_offset,
                                 &slice_data_bufs[s]);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "Failed to create VA slice data buffer %u: %d\n",
                    s, va_status);
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

   /* PHASE 4: Record decode command for deferred execution at QueueSubmit time
    * Instead of executing VA-API decode immediately, we store the command
    * in the command buffer's vaapi_decodes array.
    */

   /* Get target BO for synchronization */
   struct anv_image_binding *dst_binding =
      &((struct anv_image *) dst_image)->
      bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];

   /* Create deferred decode command */
   struct anv_vaapi_decode_cmd decode_cmd = {
      .context = session->va_context,
      .target_surface = dst_surface,
      .target_bo = dst_binding->address.bo,
      .target_gem_handle =
         dst_binding->address.bo ? dst_binding->address.bo->gem_handle : 0,
      .pic_param_buf = pic_param_buf,
      .slice_param_bufs = slice_param_bufs,
      .slice_data_bufs = slice_data_bufs,
      .slice_count = slice_count,
      .producer_syncfd = -1,  /* No sync fd by default */
      .ref_surfaces = ref_surfaces,
      .ref_surface_count = ref_surface_count,
      .session = session,
   };

   /* Add command to the deferred execution queue
    * Note: util_dynarray_append is a macro that takes 2 args: buffer and value
    */
   util_dynarray_append(&cmd_buffer->video.vaapi_decodes, decode_cmd);

   /* Unmap the bitstream buffer - the VA-API slice data buffers have copied the data */
   anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr,
              "VA-API decode: Recorded deferred decode command (%u slices)\n",
              slice_count);
   }

   return VK_SUCCESS;
}

/**
 * Export Vulkan image as DMA-buf
 *
 * Exports the memory backing a Vulkan video image as a DMA-buf
 * file descriptor for sharing with VA-API.
 */
VkResult
anv_vaapi_export_video_surface_dmabuf(struct anv_device *device,
                                      struct anv_image *image, int *fd_out)
{
   /* Get the main memory binding for the image */
   struct anv_image_binding *binding =
      &image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];

   if (!binding->address.bo) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
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
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Marking video BO (gem_handle=%u) as external for DMA-buf export\n",
                 bo->gem_handle);
      }
      bo->is_external = true;
   }

   /* Export the BO as a DMA-buf file descriptor using GEM handle to fd */
   int fd = anv_gem_handle_to_fd(device, bo->gem_handle);
   if (fd < 0) {
      int export_errno = errno;  /* Save errno before any other calls */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Failed to export BO (gem_handle=%u) as DMA-buf: %s (errno=%d)\n",
                 bo->gem_handle, strerror(export_errno), export_errno);
         fprintf(stderr, "  Common causes:\n");
         if (export_errno == EINVAL) {
            fprintf(stderr, "  - EINVAL: Invalid gem_handle or BO was already freed\n");
            fprintf(stderr, "  - EINVAL: BO created with no_export flag\n");
            fprintf(stderr, "  - EINVAL: BO is a userptr (cannot be exported)\n");
         } else if (export_errno == EMFILE || export_errno == ENFILE) {
            fprintf(stderr, "  - %s: Too many open file descriptors\n",
                    export_errno == EMFILE ? "EMFILE" : "ENFILE");
         } else if (export_errno == EBADF) {
            fprintf(stderr, "  - EBADF: DRM device fd is invalid\n");
         }
         fprintf(stderr, "  See docs/KERNEL_COMPATIBILITY.md for troubleshooting\n");
      }
      return vk_error(device, VK_ERROR_TOO_MANY_OBJECTS);
   }

   /* Log fstat info for the exported fd */
   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      struct stat st;
      if (fstat(fd, &st) == 0) {
         fprintf(stderr, "Exported BO (gem_handle=%u) as DMA-buf fd=%d (inode=%lu, size=%ld)\n",
                 bo->gem_handle, fd, (unsigned long)st.st_ino, (long)st.st_size);
      } else {
         fprintf(stderr, "Exported BO (gem_handle=%u) as DMA-buf fd=%d (fstat failed: %m)\n",
                 bo->gem_handle, fd);
      }
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
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "anv_vaapi_import_surface_from_image: ERROR - no VA display\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Export image memory as DMA-buf */
   VkResult result =
      anv_vaapi_export_video_surface_dmabuf(device, image, &fd);
   if (result != VK_SUCCESS) {
      return result;
   }

   /* Get the main memory binding for the image to determine base offset */
   struct anv_image_binding *binding =
      &image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];

   /* Get image layout information for stride and offsets
    * For NV12 (multi-planar format), we need both Y and UV plane information
    */
   uint32_t y_plane =
      anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_0_BIT);
   uint32_t uv_plane =
      anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_1_BIT);
   const struct anv_surface *y_surface =
      &image->planes[y_plane].primary_surface;
   const struct anv_surface *uv_surface =
      &image->planes[uv_plane].primary_surface;

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

   VASurfaceAttribExternalBuffers extbuf;
   memset(&extbuf, 0, sizeof(extbuf));
   extbuf.pixel_format = VA_FOURCC_NV12;
   extbuf.width = image->vk.extent.width;
   extbuf.height = image->vk.extent.height;
   extbuf.num_buffers = 1;
   /* Use explicit fd array as recommended */
   int fds[1] = { fd };
   extbuf.buffers = (uintptr_t *)fds;
   /* Set flags for DRM PRIME memory type
    * For legacy (non-modifier) imports, the tiling information is retrieved
    * by the VA-API driver via gem/get_tiling from the kernel.
    * The VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME flag is set in the attribs array below.
    */
   extbuf.flags = 0;
   extbuf.num_planes = 2;          /* Y and UV for NV12 */

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "[vo/default/vaapi] DMA-buf: Mapping %u planes via libplacebo\n", extbuf.num_planes);
   }

   /* Set stride (row pitch) for both planes
    * For NV12:
    * - Y plane has full width stride
    * - UV plane has same stride (U and V are interleaved)
    */
   extbuf.pitches[0] = y_surface->isl.row_pitch_B;
   extbuf.pitches[1] = uv_surface->isl.row_pitch_B;

   /* CRITICAL UNDERSTANDING OF INTEL-VAAPI-DRIVER NV12 HANDLING:
    * Based on analysis of intel-vaapi-driver source code (i965_drv_video.c),
    * when importing external NV12 surfaces via DMA-buf, the driver:
    *
    * 1. Sets obj_surface->width = pitches[0] (Y plane pitch in bytes)
    * 2. Calculates obj_surface->height = offsets[1] / width
    *    This computes: height = UV_plane_offset / Y_plane_pitch (in rows)
    * 3. Sets y_cb_offset = obj_surface->height (UV offset in rows)
    * 4. When programming hardware, it uses: byte_offset = y_cb_offset * pitch
    *
    * So the driver DOES respect our offsets, just indirectly:
    * - We pass offsets[1] = actual UV plane byte offset from ISL layout
    * - Driver calculates: obj_surface->height = offsets[1] / pitches[0]
    * - Driver sets: y_cb_offset = obj_surface->height
    * - Hardware uses: UV_byte_offset = y_cb_offset * pitch = offsets[1]
    *
    * CRITICAL REQUIREMENT: For this to work correctly:
    * - Y plane MUST start at BO offset 0 (binding->address.offset must be 0)
    * - offsets[1] MUST be exactly where ISL placed the UV plane
    * - offsets[1] / pitches[0] MUST be aligned to 32 (for tiled surfaces)
    *
    * This means we should NOT try to "fix" the layout to match what we think
    * the driver wants - we should pass the exact ISL layout and the driver
    * will interpret it correctly.
    */
   uint64_t y_plane_abs_offset = binding->address.offset + y_surface->memory_range.offset;
   if (y_plane_abs_offset != 0) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "WARNING: Video image Y plane not at BO offset 0 (binding_offset=%" PRId64 " + y_offset=%" PRIu64 ")\n",
                 binding->address.offset, y_surface->memory_range.offset);
         fprintf(stderr, "This will cause incorrect decoding - intel-vaapi-driver requires Y plane at BO offset 0.\n");
         fprintf(stderr, "The driver calculates height = UV_offset / Y_pitch, which will be wrong if Y doesn't start at 0.\n");
      }
   }

   /* Set offsets for each plane using actual memory layout from ISL
    * This is critical - we must use the ISL-calculated offset, not a
    * simple height*stride calculation, because ISL adds alignment padding.
    *
    * CRITICAL: For DMA-buf sharing, offsets must be relative to the start of the BO,
    * not the start of the binding. If the binding has a non-zero offset within the BO
    * (binding->address.offset), we must add that to each plane's offset.
    *
    * The intel-vaapi-driver uses these offsets indirectly:
    * - It calculates obj_surface->height = offsets[1] / pitches[0]
    * - Then sets y_cb_offset = obj_surface->height (in rows)
    * - Hardware computes byte offset as: y_cb_offset * pitch = offsets[1]
    * So the UV plane offset we pass IS respected, just via this calculation.
    */
   uint64_t y_offset = binding->address.offset + y_surface->memory_range.offset;
   uint64_t uv_offset = binding->address.offset + uv_surface->memory_range.offset;

   /* WORKAROUND: Fix off-by-one alignment errors on Gen7 for video surface offsets */
   y_offset = fix_gen7_surface_offset_alignment(device, y_offset, y_surface->isl.alignment_B);
   uv_offset = fix_gen7_surface_offset_alignment(device, uv_offset, uv_surface->isl.alignment_B);

   extbuf.offsets[0] = y_offset;
   extbuf.offsets[1] = uv_offset;

   /* Set total data size for the DMA-buf
    * CRITICAL: VA-API needs to know the total size of data in the buffer.
    * This is the UV plane offset plus the UV plane size.
    */
   extbuf.data_size = extbuf.offsets[1] + uv_surface->memory_range.size;

   /* Validate critical alignment requirement for Y-tiled surfaces:
    * The UV plane offset must be aligned such that (UV_offset / Y_pitch) is a multiple of 32 rows.
    * This is required because Y-tiles are 128B x 32 rows, and the hardware/driver expects
    * plane offsets to be tile-aligned.
    */
   if (y_surface->isl.tiling != ISL_TILING_LINEAR) {
      uint32_t uv_offset_in_rows = extbuf.offsets[1] / extbuf.pitches[0];
      if (uv_offset_in_rows % 32 != 0) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "WARNING: UV plane offset not aligned to 32-row tile boundary!\n");
            fprintf(stderr, "  UV offset: %u bytes, Y pitch: %u bytes\n",
                    extbuf.offsets[1], extbuf.pitches[0]);
            fprintf(stderr, "  UV offset in rows: %u (should be multiple of 32)\n",
                    uv_offset_in_rows);
            fprintf(stderr, "  This may cause chroma corruption on Gen7 hardware.\n");
         }
      }
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      /* Plane 0: Y plane details */
      fprintf(stderr, "[vo/default/vaapi] DMA-buf: Plane 0: object=0 fd=%d size=%u offset=%u pitch=%u\n",
              fd, extbuf.data_size, extbuf.offsets[0], extbuf.pitches[0]);

      /* Get tiling modifier for logging purposes
       * For Gen7/8 video surfaces, we use legacy (non-modifier) DMA-buf import where
       * tiling information is communicated via the kernel's gem/get_tiling ioctl.
       * The actual tiling mode (Y-tiling) is set on the BO via anv_device_set_bo_tiling().
       *
       * For logging compatibility with mpv's output, we show:
       * - DRM_FORMAT_MOD_INVALID for non-linear (tiled) surfaces (legacy import path)
       * - 0 (DRM_FORMAT_MOD_LINEAR) for linear surfaces
       */
      uint64_t modifier;
      if (y_surface->isl.tiling != ISL_TILING_LINEAR) {
         /* Tiled surface - modifier is implicit via kernel, not explicit in DMA-buf */
         modifier = DRM_FORMAT_MOD_INVALID;
      } else {
         /* Linear surface - explicit modifier */
         modifier = 0;  /* DRM_FORMAT_MOD_LINEAR */
      }

      /* For NV12, Y plane dimensions are full size, format is r8 (single channel, 8-bit) */
      fprintf(stderr, "[vo/default/vaapi] DMA-buf: Creating texture %ux%u format=r8 modifier=0x%llx\n",
              image->vk.extent.width, image->vk.extent.height,
              (unsigned long long)modifier);
      fprintf(stderr, "[vo/default/vulkan] libplacebo: Wrapping pl_tex %ux%u format=r8\n",
              image->vk.extent.width, image->vk.extent.height);

      /* Plane 1: UV plane details */
      fprintf(stderr, "[vo/default/vaapi] DMA-buf: Plane 1: object=0 fd=%d size=%u offset=%u pitch=%u\n",
              fd, extbuf.data_size, extbuf.offsets[1], extbuf.pitches[1]);

      /* For NV12, UV plane is half size (4:2:0 subsampling), format is rg8 (two channels, 8-bit each)
       * Use proper subsampling that handles odd dimensions: (width + 1) / 2, (height + 1) / 2
       */
      uint32_t uv_width = (image->vk.extent.width + 1) / 2;
      uint32_t uv_height = (image->vk.extent.height + 1) / 2;
      fprintf(stderr, "[vo/default/vaapi] DMA-buf: Creating texture %ux%u format=rg8 modifier=0x%llx\n",
              uv_width, uv_height,
              (unsigned long long)modifier);
      fprintf(stderr, "[vo/default/vulkan] libplacebo: Wrapping pl_tex %ux%u format=rg8\n",
              uv_width, uv_height);

      fprintf(stderr, "[vo/default/vaapi] DMA-buf: Successfully mapped all 2 planes\n");
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
   va_status = vaCreateSurfaces(va_display, VA_RT_FORMAT_YUV420,        /* NV12 is YUV 4:2:0 */
                                image->vk.extent.width, image->vk.extent.height, surface_id, 1, /* num_surfaces */
                                attribs, 2      /* num_attribs */
      );

   /* Close the fd - VA-API will duplicate it internally if needed */
   close(fd);

   if (va_status != VA_STATUS_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Failed to create VA surface from DMA-buf: status=%d\n",
                 va_status);
         fprintf(stderr, "  This may be caused by:\n");
         fprintf(stderr, "  1. Incompatible surface parameters (size, format, tiling)\n");
         fprintf(stderr, "  2. DMA-buf fd already closed or invalid\n");
         fprintf(stderr, "  3. VA-API driver doesn't support DRM PRIME import\n");
         fprintf(stderr, "  4. Y plane not at BO offset 0 (binding offset=%ld, y_offset=%lu)\n",
                 (long)binding->address.offset, (unsigned long)y_surface->memory_range.offset);
         fprintf(stderr, "  5. Incorrect plane offsets or pitches\n");
         fprintf(stderr, "  Run 'vainfo' to check VA-API driver compatibility\n");
         fprintf(stderr, "  See docs/KERNEL_COMPATIBILITY.md for troubleshooting\n");
      }
      return vk_error(device, VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR);
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr,
              "Created VA surface %u from Vulkan image (DMA-buf sharing)\n",
              *surface_id);
   }

   return VK_SUCCESS;
}

/**
 * Wait for a sync fd to be signaled using poll
 *
 * This is a simple implementation using poll() instead of libsync.
 * Returns 0 on success, -1 on error.
 */
static int
sync_wait(int fd, int timeout_ms)
{
   struct pollfd fds = {
      .fd = fd,
      .events = POLLIN,
   };

   int ret;
   do {
      ret = poll(&fds, 1, timeout_ms);
   } while (ret == -1 && errno == EINTR);

   if (ret < 0) {
      return -1;
   }

   if (ret == 0) {
      /* Timeout */
      errno = ETIME;
      return -1;
   }

   return 0;
}

/**
 * Execute deferred VA-API decode commands (Phase 4)
 *
 * Called at QueueSubmit time to execute all VA-API decode operations
 * that were recorded in the command buffer during CmdDecodeVideoKHR.
 *
 * This implements the proper command buffer pattern where operations
 * are deferred until submission.
 */
VkResult
anv_vaapi_execute_deferred_decodes(struct anv_device *device,
                                   struct anv_cmd_buffer *cmd_buffer)
{
   VAStatus va_status = VA_STATUS_SUCCESS;
   VkResult result = VK_SUCCESS;

   /* Get VA display */
   VADisplay va_display = anv_vaapi_get_display(device);
   if (!va_display) {
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Execute each deferred decode command */
   util_dynarray_foreach(&cmd_buffer->video.vaapi_decodes,
                         struct anv_vaapi_decode_cmd, decode_cmd)
   {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr,
                 "Executing deferred VA-API decode: surface=%u, %u slices\n",
                 decode_cmd->target_surface, decode_cmd->slice_count);
      }

      /* Optional: Wait for producer sync fd if provided
       * This allows waiting for upstream producers to finish before decoding.
       */
      if (decode_cmd->producer_syncfd >= 0) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "Waiting for producer sync fd %d...\n",
                    decode_cmd->producer_syncfd);
         }

         /* Wait for sync fd with 5 second timeout */
         int wait_ret = sync_wait(decode_cmd->producer_syncfd, 5000);
         if (wait_ret != 0) {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "Failed to wait for producer sync fd: %m\n");
            }
            /* Continue anyway - the decode might still work */
         } else {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "Producer sync fd signaled successfully\n");
            }
         }

         /* Close the sync fd after waiting */
         close(decode_cmd->producer_syncfd);
      }

      /* CRITICAL SYNCHRONIZATION (Before VA-API decode):
       * Vulkan command buffer synchronization ensures that any previous
       * operations on the surface have completed before we reach this point.
       *
       * The application is responsible for proper synchronization through:
       * 1. Vulkan pipeline barriers
       * 2. Semaphores between queue submissions
       * 3. Fences for CPU-GPU synchronization
       *
       * The I915_GEM_SET_DOMAIN ioctl was removed from the Linux kernel
       * starting with version 6.2 and is no longer available. Modern
       * synchronization relies on DMA-buf implicit fencing which is
       * automatically handled by the kernel when sharing buffers between
       * Vulkan and VA-API.
       *
       * See docs/KERNEL_COMPATIBILITY.md for details on kernel API changes.
       */

      /* Begin picture */
      va_status =
         vaBeginPicture(va_display, decode_cmd->context,
                        decode_cmd->target_surface);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "vaBeginPicture failed: %d\n", va_status);
         }
         result = vk_error(device, VK_ERROR_UNKNOWN);
         goto cleanup_cmd;
      }

      /* Render picture parameters */
      va_status = vaRenderPicture(va_display, decode_cmd->context,
                                  &decode_cmd->pic_param_buf, 1);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "vaRenderPicture (picture params) failed: %d\n",
                    va_status);
         }
         vaEndPicture(va_display, decode_cmd->context);
         result = vk_error(device, VK_ERROR_UNKNOWN);
         goto cleanup_cmd;
      }

      /* Render all slices */
      for (uint32_t s = 0; s < decode_cmd->slice_count; s++) {
         /* Render slice parameters */
         va_status = vaRenderPicture(va_display, decode_cmd->context,
                                     &decode_cmd->slice_param_bufs[s], 1);
         if (va_status != VA_STATUS_SUCCESS) {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr,
                       "vaRenderPicture (slice %u params) failed: %d\n", s,
                       va_status);
            }
            vaEndPicture(va_display, decode_cmd->context);
            result = vk_error(device, VK_ERROR_UNKNOWN);
            goto cleanup_cmd;
         }

         /* Render slice data */
         va_status = vaRenderPicture(va_display, decode_cmd->context,
                                     &decode_cmd->slice_data_bufs[s], 1);
         if (va_status != VA_STATUS_SUCCESS) {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "vaRenderPicture (slice %u data) failed: %d\n",
                       s, va_status);
            }
            vaEndPicture(va_display, decode_cmd->context);
            result = vk_error(device, VK_ERROR_UNKNOWN);
            goto cleanup_cmd;
         }
      }

      /* End picture and execute decode */
      va_status = vaEndPicture(va_display, decode_cmd->context);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "vaEndPicture failed: %d\n", va_status);
         }
         result = vk_error(device, VK_ERROR_UNKNOWN);
         goto cleanup_cmd;
      }

      /* Sync - wait for decode to complete */
      va_status = vaSyncSurface(va_display, decode_cmd->target_surface);
      if (va_status != VA_STATUS_SUCCESS) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "vaSyncSurface failed: %d\n", va_status);
         }
      }

      /*
       * vaSyncSurface waits for the video decode to complete, but we need
       * additional cache flushing to make VA-API writes visible to Vulkan.
       *
       * We use proper cache flush/invalidate operations to ensure that:
       * 1. Video engine writes are flushed from GPU caches to main memory
       * 2. CPU caches are invalidated so Vulkan sees fresh data
       * 3. Vulkan command buffers can correctly read the decoded data
       */
      if (decode_cmd->target_bo && decode_cmd->target_bo->gem_handle) {
         /* Map the BO to get a CPU pointer for cache operations */
         void *ptr = anv_gem_mmap(device, decode_cmd->target_bo->gem_handle,
                                 0, decode_cmd->target_bo->size, 0);
         if (ptr != MAP_FAILED && ptr != NULL) {
            /* Flush and invalidate the entire buffer to ensure cache coherency.
             * This forces:
             * - Write-back of any dirty cache lines to main memory
             * - Invalidation of cache lines so subsequent reads fetch from memory
             */
            if (util_has_cache_ops()) {
               util_flush_inval_range(ptr, decode_cmd->target_bo->size);

               if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
                  fprintf(stderr, "Cache flush/invalidate completed for BO gem_handle=%u (size=%" PRIu64 ")\n",
                         decode_cmd->target_bo->gem_handle, decode_cmd->target_bo->size);
               }
            } else {
               /* Fallback: Touch bytes to trigger cache involvement */
               volatile uint8_t dummy;
               dummy = ((uint8_t*)ptr)[0];
               dummy = ((uint8_t*)ptr)[decode_cmd->target_bo->size - 1];
               (void)dummy;  /* Prevent compiler optimization */

               if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
                  fprintf(stderr, "Cache flush via byte access for BO gem_handle=%u (no cache_ops available)\n",
                         decode_cmd->target_bo->gem_handle);
               }
            }

            /* Unmap after cache operations complete */
            anv_gem_munmap(device, ptr, decode_cmd->target_bo->size);
         } else {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "WARNING: Failed to mmap BO for cache flush (gem_handle=%u)\n",
                      decode_cmd->target_bo->gem_handle);
            }
         }
      }

    cleanup_cmd:
      /* Clean up VA-API buffers for this decode command */
      for (uint32_t s = 0; s < decode_cmd->slice_count; s++) {
         vaDestroyBuffer(va_display, decode_cmd->slice_data_bufs[s]);
         vaDestroyBuffer(va_display, decode_cmd->slice_param_bufs[s]);
      }
      vk_free(&device->vk.alloc, decode_cmd->slice_data_bufs);
      vk_free(&device->vk.alloc, decode_cmd->slice_param_bufs);
      vaDestroyBuffer(va_display, decode_cmd->pic_param_buf);

      /* CRITICAL: DO NOT destroy surfaces here!
       *
       * VA surfaces must persist in the DPB (Decoded Picture Buffer) for use
       * as references in future frames. They are kept alive in the session's
       * surface_map and only destroyed when:
       * 1. They're no longer referenced (DPB management - future work)
       * 2. The session is destroyed (anv_vaapi_session_destroy)
       *
       * This matches the behavior of intel-vaapi-driver and zink video:
       * - Surfaces allocated for max_dpb_slots
       * - Kept alive throughout session lifetime
       * - Used as references across multiple frames
       *
       * The ref_surfaces array in decode_cmd just tracks which surfaces were
       * used as references for this frame, but we don't destroy them.
       */

      /* Free the ref_surfaces tracking array (but not the surfaces themselves) */
      if (decode_cmd->ref_surfaces) {
         vk_free(&device->vk.alloc, decode_cmd->ref_surfaces);
      }

      if (result != VK_SUCCESS) {
         break;                 /* Stop on first error */
      }
   }

   /* Clear the deferred commands after execution */
   util_dynarray_clear(&cmd_buffer->video.vaapi_decodes);

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK)) && result == VK_SUCCESS) {
      fprintf(stderr, "All deferred VA-API decodes executed successfully\n");
   }

   return result;
}
