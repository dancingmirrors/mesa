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

#ifndef ANV_VIDEO_VDPAU_BRIDGE_H
#define ANV_VIDEO_VDPAU_BRIDGE_H

#include <vdpau/vdpau.h>
#include <vdpau/vdpau_x11.h>
#include <stdlib.h>
#include <string.h>

#include "anv_private.h"
#include "util/os_misc.h"

/**
 * VDPAU Bridge for HasVK Video Decode
 *
 * This module provides a bridge between Vulkan Video decode operations
 * and VDPAU, leveraging the stable VDPAU implementation for H.264 decode
 * through libvdpau-va-gl which translates VDPAU to VA-API/OpenGL.
 *
 * Architecture:
 *   Application → HasVK Vulkan Video API → anv_video.c
 *       → anv_video_vdpau_bridge.c → VDPAU → libvdpau-va-gl → VA-API/GL → Hardware
 *
 * Benefits over direct VA-API:
 * - VDPAU has a simpler interface for H.264 decode
 * - libvdpau-va-gl handles the complex VA-API interactions
 * - Better tested and more stable code path
 * - Simpler slice header handling
 */

/**
 * Surface mapping entry for DPB management
 * Maps Vulkan images to VDPAU surfaces
 */
struct anv_vdpau_surface_map {
   const struct anv_image *image;  /* Vulkan image */
   VdpVideoSurface vdp_surface;    /* Corresponding VDPAU video surface */
};

/**
 * Deferred VDPAU decode command
 *
 * Stored in command buffer and executed at QueueSubmit time.
 * Ownership model:
 * - bitstream_buffers: allocated by decode_frame, freed by execute_deferred_decodes
 * - bitstream_data/bitstream_data_size: mapped by decode_frame, unmapped by execute_deferred_decodes
 * - ref_surfaces: allocated by decode_frame, freed by execute_deferred_decodes (surfaces NOT destroyed)
 */
struct anv_vdpau_decode_cmd {
   VdpDecoder decoder;             /* VDPAU decoder for this decode */
   VdpVideoSurface target_surface; /* Target surface to decode into */
   struct anv_bo *target_bo;       /* BO for cache flush after decode */

   /* H.264 specific decode info */
   VdpPictureInfoH264 pic_info;    /* Picture info structure */

   /* Bitstream data */
   uint32_t bitstream_buffer_count;
   VdpBitstreamBuffer *bitstream_buffers;  /* Array of bitstream buffers */
   void *bitstream_data;           /* Mapped bitstream data (for unmapping) */
   uint64_t bitstream_data_size;   /* Size of mapped data (for unmapping) */

   /* Surface cleanup - destroy after decode completes (no caching) */
   VdpVideoSurface *ref_surfaces;  /* Array of reference surfaces to destroy */
   uint32_t ref_surface_count;     /* Number of reference surfaces */
   struct anv_vdpau_session *session;  /* Session for clearing surface mappings */
};

/**
 * VDPAU session state
 *
 * Manages the VDPAU objects associated with a Vulkan video session.
 */
struct anv_vdpau_session {
   VdpDevice vdp_device;           /* VDPAU device handle */
   VdpDecoder vdp_decoder;         /* VDPAU decoder */

   /* VDPAU function pointers obtained via vdp_get_proc_address */
   VdpGetProcAddress *vdp_get_proc_address;
   VdpDeviceDestroy *vdp_device_destroy;
   VdpDecoderCreate *vdp_decoder_create;
   VdpDecoderDestroy *vdp_decoder_destroy;
   VdpDecoderRender *vdp_decoder_render;
   VdpVideoSurfaceCreate *vdp_video_surface_create;
   VdpVideoSurfaceDestroy *vdp_video_surface_destroy;
   VdpVideoSurfaceGetBitsYCbCr *vdp_video_surface_get_bits_ycbcr;
   VdpVideoSurfacePutBitsYCbCr *vdp_video_surface_put_bits_ycbcr;
   VdpVideoSurfaceGetParameters *vdp_video_surface_get_parameters;
   VdpGetErrorString *vdp_get_error_string;

   /* DPB (Decoded Picture Buffer) surfaces */
   VdpVideoSurface *vdp_surfaces;  /* Array of VDPAU surfaces for reference frames */
   uint32_t num_surfaces;          /* Number of surfaces allocated */

   /* Surface mapping for reference frames */
   struct anv_vdpau_surface_map *surface_map;  /* Image to VDPAU surface mapping */
   uint32_t surface_map_size;      /* Current number of mapped surfaces */
   uint32_t surface_map_capacity;  /* Maximum capacity of surface map */

   /* Session properties */
   uint32_t width;                 /* Video frame width */
   uint32_t height;                /* Video frame height */
   VdpDecoderProfile vdp_profile;  /* VDPAU profile (e.g., VDP_DECODER_PROFILE_H264_HIGH) */

   /* X11 Display for VDPAU initialization */
   void *x11_display;              /* X11 Display pointer (NULL if not using X11) */
};

/**
 * Initialize VDPAU bridge for a video session
 *
 * Creates VDPAU device and decoder for video decoding.
 *
 * @param device        ANV device
 * @param vid           Video session to initialize
 * @param pCreateInfo   Vulkan video session creation info
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vdpau_session_create(struct anv_device *device,
                         struct anv_video_session *vid,
                         const VkVideoSessionCreateInfoKHR *pCreateInfo);

/**
 * Destroy VDPAU bridge session
 *
 * Releases all VDPAU resources associated with the video session.
 *
 * @param device  ANV device
 * @param vid     Video session to destroy
 */
void
anv_vdpau_session_destroy(struct anv_device *device,
                          struct anv_video_session *vid);

/**
 * Decode a frame using VDPAU
 *
 * Translates Vulkan video decode info to VDPAU calls and submits
 * the decode operation.
 *
 * @param cmd_buffer   Command buffer
 * @param frame_info   Vulkan decode info
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vdpau_decode_frame(struct anv_cmd_buffer *cmd_buffer,
                       const VkVideoDecodeInfoKHR *frame_info);

/**
 * Get VDPAU device from ANV device
 *
 * Returns the VDPAU device associated with the device, creating it
 * if necessary.
 *
 * @param device  ANV device
 * @return VdpDevice handle or VDP_INVALID_HANDLE on failure
 */
VdpDevice
anv_vdpau_get_device(struct anv_device *device);

/**
 * Create VDPAU surface from Vulkan image
 *
 * Creates a VDPAU video surface that corresponds to a Vulkan image.
 * The surface data will be copied from/to the Vulkan image.
 *
 * @param device      ANV device
 * @param session     VDPAU session
 * @param image       Source Vulkan image
 * @param surface_id  Output VDPAU surface ID
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vdpau_create_surface_from_image(struct anv_device *device,
                                    struct anv_vdpau_session *session,
                                    struct anv_image *image,
                                    VdpVideoSurface *surface_id);

/**
 * Copy VDPAU surface to Vulkan image
 *
 * Copies decoded data from VDPAU surface back to the Vulkan image.
 *
 * @param device      ANV device
 * @param session     VDPAU session
 * @param surface     VDPAU surface with decoded data
 * @param image       Destination Vulkan image
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vdpau_copy_surface_to_image(struct anv_device *device,
                                struct anv_vdpau_session *session,
                                VdpVideoSurface surface,
                                struct anv_image *image);

/**
 * Add surface mapping entry
 *
 * Maps a Vulkan image to a VDPAU surface for DPB management.
 *
 * @param session     VDPAU session
 * @param image       Vulkan image
 * @param vdp_surface VDPAU surface
 */
void
anv_vdpau_add_surface_mapping(struct anv_vdpau_session *session,
                              const struct anv_image *image,
                              VdpVideoSurface vdp_surface);

/**
 * Lookup VDPAU surface from Vulkan image
 *
 * Searches the surface mapping for a VDPAU surface associated with
 * the given Vulkan image.
 *
 * @param session  VDPAU session
 * @param image    Vulkan image to lookup
 * @return VDPAU surface or VDP_INVALID_HANDLE if not found
 */
VdpVideoSurface
anv_vdpau_lookup_surface(struct anv_vdpau_session *session,
                         const struct anv_image *image);

/**
 * H.264-specific parameter translation functions
 */

/**
 * Translate Vulkan H.264 picture parameters to VDPAU format
 *
 * @param device         ANV device
 * @param decode_info    Vulkan decode info
 * @param h264_pic_info  H.264-specific picture info
 * @param params         Video session parameters (contains SPS/PPS)
 * @param session        VDPAU session (for DPB surface lookup)
 * @param dst_surface    VDPAU surface for decode destination
 * @param vdp_pic        Output VDPAU picture info structure
 */
void
anv_vdpau_translate_h264_picture_params(
   struct anv_device *device,
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   const struct vk_video_session_parameters *params,
   struct anv_vdpau_session *session,
   VdpVideoSurface dst_surface,
   VdpPictureInfoH264 *vdp_pic);

/**
 * Execute deferred VDPAU decode commands
 *
 * Called at QueueSubmit time to execute all VDPAU decode operations
 * that were recorded in the command buffer.
 *
 * @param device      ANV device
 * @param cmd_buffer  Command buffer containing deferred commands
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_vdpau_execute_deferred_decodes(struct anv_device *device,
                                   struct anv_cmd_buffer *cmd_buffer);

/* *INDENT-ON* */
#endif /* ANV_VIDEO_VDPAU_BRIDGE_H */
