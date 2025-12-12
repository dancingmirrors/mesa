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

/* *INDENT-OFF* */

#ifndef ANV_VIDEO_VAAPI_BRIDGE_H
#define ANV_VIDEO_VAAPI_BRIDGE_H

#include <va/va.h>
#include <va/va_drm.h>
#include <stdlib.h>
#include <string.h>

#include "anv_private.h"

/**
 * VA-API Bridge for HasVK Video Decode
 *
 * This module provides a bridge between Vulkan Video decode operations
 * and VA-API, leveraging the stable VA-API implementation on Gen7/7.5/8
 * hardware through the crocus driver.
 *
 * Architecture:
 *   Application → HasVK Vulkan Video API → anv_video.c
 *       → anv_video_vaapi_bridge.c → VA-API → Crocus → Hardware
 */

/**
 * Surface mapping entry for DPB management
 * Maps Vulkan images to VA-API surfaces
 */
struct anv_vaapi_surface_map {
   const struct anv_image *image;  /* Vulkan image */
   VASurfaceID va_surface;         /* Corresponding VA surface */
};

/**
 * Deferred VA-API decode command
 *
 * Stored in command buffer and executed at QueueSubmit time.
 * This implements Phase 4 of the VA-API integration plan.
 */
struct anv_vaapi_decode_cmd {
   VAContextID context;           /* VA context for this decode */
   VASurfaceID target_surface;    /* Target surface to decode into */
   struct anv_bo *target_bo;      /* BO for synchronization */
   uint32_t target_gem_handle;    /* GEM handle for set_domain */
   VABufferID pic_param_buf;      /* Picture parameter buffer */
   VABufferID *slice_param_bufs;  /* Array of slice parameter buffers */
   VABufferID *slice_data_bufs;   /* Array of slice data buffers */
   uint32_t slice_count;          /* Number of slices */
   int producer_syncfd;           /* Optional sync fd from producer (or -1) */

   /* Surface cleanup - destroy after decode completes (no caching) */
   VASurfaceID *ref_surfaces;     /* Array of reference surfaces to destroy */
   uint32_t ref_surface_count;    /* Number of reference surfaces */
   struct anv_vaapi_session *session;  /* Session for clearing surface mappings */
};

/**
 * VA-API session state
 *
 * Manages the VA-API objects associated with a Vulkan video session.
 */
struct anv_vaapi_session {
   VADisplay va_display;          /* VA display handle */
   VAContextID va_context;        /* VA decode context */
   VAConfigID va_config;          /* VA configuration */

   /* DPB (Decoded Picture Buffer) surfaces */
   VASurfaceID *va_surfaces;      /* Array of VA surfaces for reference frames */
   uint32_t num_surfaces;         /* Number of surfaces allocated */

   /* Surface mapping for reference frames */
   struct anv_vaapi_surface_map *surface_map;  /* Image to VA surface mapping */
   uint32_t surface_map_size;     /* Current number of mapped surfaces */
   uint32_t surface_map_capacity; /* Maximum capacity of surface map */

   /* Parameter buffers for decode operations */
   VABufferID va_picture_param;   /* Picture parameter buffer */
   VABufferID va_slice_param;     /* Slice parameter buffer */
   VABufferID va_slice_data;      /* Slice data buffer */

   /* Session properties */
   uint32_t width;                /* Video frame width */
   uint32_t height;               /* Video frame height */
   VAProfile va_profile;          /* VA-API profile (e.g., VAProfileH264Main) */
};

/**
 * Initialize VA-API bridge for a video session
 *
 * Creates VA-API display, config, and context for video decoding.
 *
 * @param device        ANV device
 * @param vid           Video session to initialize
 * @param pCreateInfo   Vulkan video session creation info
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vaapi_session_create(struct anv_device *device,
                         struct anv_video_session *vid,
                         const VkVideoSessionCreateInfoKHR *pCreateInfo);

/**
 * Destroy VA-API bridge session
 *
 * Releases all VA-API resources associated with the video session.
 *
 * @param device  ANV device
 * @param vid     Video session to destroy
 */
void
anv_vaapi_session_destroy(struct anv_device *device,
                          struct anv_video_session *vid);

/**
 * Decode a frame using VA-API
 *
 * Translates Vulkan video decode info to VA-API calls and submits
 * the decode operation.
 *
 * @param cmd_buffer   Command buffer
 * @param frame_info   Vulkan decode info
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vaapi_decode_frame(struct anv_cmd_buffer *cmd_buffer,
                       const VkVideoDecodeInfoKHR *frame_info);

/**
 * Get VA display from device
 *
 * Returns the VA display associated with the device, creating it
 * if necessary.
 *
 * @param device  ANV device
 * @return VA display handle or NULL on failure
 */
VADisplay
anv_vaapi_get_display(struct anv_device *device);

/**
 * Export Vulkan image as DMA-buf
 *
 * Exports a Vulkan video surface as a DMA-buf file descriptor
 * for sharing with VA-API.
 *
 * @param device   ANV device
 * @param image    Image to export
 * @param fd_out   Output file descriptor
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vaapi_export_video_surface_dmabuf(struct anv_device *device,
                                      struct anv_image *image,
                                      int *fd_out);

/**
 * Import DMA-buf into VA-API surface
 *
 * Creates a VA-API surface from a DMA-buf file descriptor exported
 * from a Vulkan image.
 *
 * @param device      ANV device
 * @param image       Source Vulkan image
 * @param surface_id  Output VA surface ID
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vaapi_import_surface_from_image(struct anv_device *device,
                                    struct anv_image *image,
                                    VASurfaceID *surface_id);

/**
 * Add surface mapping entry
 *
 * Maps a Vulkan image to a VA-API surface for DPB management.
 *
 * @param session    VA-API session
 * @param image      Vulkan image
 * @param va_surface VA surface ID
 */
void
anv_vaapi_add_surface_mapping(struct anv_vaapi_session *session,
                               const struct anv_image *image,
                               VASurfaceID va_surface);

/**
 * Lookup VA surface from Vulkan image
 *
 * Searches the surface mapping for a VA-API surface associated with
 * the given Vulkan image.
 *
 * @param session  VA-API session
 * @param image    Vulkan image to lookup
 * @return VA surface ID or VA_INVALID_SURFACE if not found
 */
VASurfaceID
anv_vaapi_lookup_surface(struct anv_vaapi_session *session,
                         const struct anv_image *image);

/**
 * H.264-specific parameter translation functions
 */

/**
 * Translate Vulkan H.264 picture parameters to VA-API format
 *
 * @param device         ANV device
 * @param decode_info    Vulkan decode info
 * @param h264_pic_info  H.264-specific picture info
 * @param params         Video session parameters (contains SPS/PPS)
 * @param session        VA-API session (for DPB surface lookup)
 * @param dst_surface_id VA surface ID for decode destination
 * @param va_pic         Output VA-API picture parameter buffer
 */
void
anv_vaapi_translate_h264_picture_params(
   struct anv_device *device,
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   const struct vk_video_session_parameters *params,
   struct anv_vaapi_session *session,
   VASurfaceID dst_surface_id,
   VAPictureParameterBufferH264 *va_pic);

/**
 * Translate Vulkan H.264 slice parameters to VA-API format
 *
 * @param device         ANV device
 * @param decode_info    Vulkan decode info
 * @param h264_pic_info  H.264-specific picture info
 * @param session        VA-API session (for DPB to RefPicList mapping)
 * @param va_pic         VA-API picture parameters (contains ReferenceFrames DPB)
 * @param slice_offset   Offset of slice data in bitstream buffer
 * @param slice_size     Size of slice data
 * @param va_slice       Output VA-API slice parameter buffer
 */
void
anv_vaapi_translate_h264_slice_params(
   struct anv_device *device,
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   struct anv_vaapi_session *session,
   const VAPictureParameterBufferH264 *va_pic,
   uint32_t slice_offset,
   uint32_t slice_size,
   VASliceParameterBufferH264 *va_slice);

/**
 * Phase 4: Command Buffer Integration Functions
 */

/**
 * Execute deferred VA-API decode commands
 *
 * Called at QueueSubmit time to execute all VA-API decode operations
 * that were recorded in the command buffer.
 *
 * @param device      ANV device
 * @param cmd_buffer  Command buffer containing deferred commands
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vaapi_execute_deferred_decodes(struct anv_device *device,
                                    struct anv_cmd_buffer *cmd_buffer);

/**
 * Check if VA-API bridge should be used
 *
 * The VA-API bridge is now always enabled for hasvk video decode.
 * Native H.264 decode is not feasible on Ivy Bridge and earlier hardware,
 * so the VA-API bridge (using the crocus driver) is the only supported path.
 *
 * @return true (VA-API bridge is always used)
 */
static inline bool
anv_use_vaapi_bridge(void)
{
   static bool logged = false;

   if (!logged && unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API bridge: ENABLED (default)\n");
      fprintf(stderr, "  Video decode will use crocus driver via VA-API\n");
      fprintf(stderr, "  DPB and decode logging requires INTEL_DEBUG=hasvk to be set\n");
      logged = true;
   }

   return true;
}

/* *INDENT-ON* */
#endif /* ANV_VIDEO_VAAPI_BRIDGE_H */
