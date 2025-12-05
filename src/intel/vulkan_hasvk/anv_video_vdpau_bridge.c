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
 * VDPAU Bridge Module for HasVK
 *
 * This module implements a bridge between Vulkan Video decode operations
 * and VDPAU. It uses libvdpau-va-gl to leverage the stable VA-API/OpenGL
 * implementation on Gen7/7.5/8 hardware through the crocus driver, avoiding
 * the complexity of direct VA-API interfacing.
 *
 * Key Benefits:
 * - VDPAU has simpler slice data handling than VA-API
 * - libvdpau-va-gl handles complex VA-API parameter translation
 * - Better tested path for H.264 decode
 * - DMA-buf complexity is hidden by the VDPAU backend
 */

#include "anv_video_vdpau_bridge.h"
#include "anv_private.h"

#include <vdpau/vdpau.h>
#include <vdpau/vdpau_x11.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include <dlfcn.h>
#include <libgen.h>

#include "vk_video/vulkan_video_codecs_common.h"
#include "vk_video/vulkan_video_codec_h264std.h"
#include "drm-uapi/i915_drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/u_dynarray.h"
#include "isl/isl.h"

/* VDPAU constants - use system definitions if available */
#ifndef VDP_INVALID_HANDLE
#define VDP_INVALID_HANDLE ((VdpVideoSurface)-1)
#endif

/* VDPAU chroma type for NV12/YUV 4:2:0 */
#ifndef VDP_CHROMA_TYPE_420
#define VDP_CHROMA_TYPE_420 0
#endif

/* VDPAU YCbCr format for NV12 */
#ifndef VDP_YCBCR_FORMAT_NV12
#define VDP_YCBCR_FORMAT_NV12 0
#endif

/* Y-tile dimensions */
#define YTILE_WIDTH 128
#define YTILE_HEIGHT 32
#define YTILE_SPAN 16

/**
 * Custom linear-to-Y-tiled copy with configurable swizzle mode
 *
 * Unlike ISL's generic implementation, this supports both:
 * - Bit 9 only swizzle (I915_BIT_6_SWIZZLE_9)
 * - Bit 9 and 10 swizzle (I915_BIT_6_SWIZZLE_9_10)
 *
 * Gen7 systems may use different swizzle modes depending on memory configuration.
 *
 * Y-tile layout:
 * - Tile is 128 bytes wide x 32 rows = 4096 bytes
 * - Within tile: data is stored column-major in 16-byte "OWord" units
 * - Each OWord column stores 32 rows (column = 16*32 = 512 bytes)
 * - There are 8 OWord columns per tile (8 * 512 = 4096 bytes)
 *
 * For a surface with row_pitch_B, the number of tiles per row is:
 *   tiles_per_row = row_pitch_B / YTILE_WIDTH = row_pitch_B / 128
 *
 * The address calculation is:
 *   tile_base = tile_row * tiles_per_row * 4096 + tile_col * 4096
 *   byte_in_tile = oword_col * 512 + row_in_tile * 16 + byte_in_oword
 *   address = tile_base + byte_in_tile
 */
static void
linear_to_ytiled_custom(char *dst, const char *src,
                        uint32_t width, uint32_t height,
                        uint32_t dst_pitch, uint32_t src_pitch,
                        int swizzle_mode)
{
   /* Calculate number of tiles per row based on destination pitch */
   uint32_t tiles_per_row = dst_pitch / YTILE_WIDTH;
   if (tiles_per_row == 0) tiles_per_row = 1;  /* Safety check */

   /* Size of one complete row of tiles in bytes */
   uint64_t tile_row_stride = (uint64_t)tiles_per_row * (YTILE_WIDTH * YTILE_HEIGHT);

   for (uint32_t y = 0; y < height; y++) {
      uint32_t tile_row = y / YTILE_HEIGHT;
      uint32_t row_in_tile = y % YTILE_HEIGHT;

      for (uint32_t x = 0; x < width; x++) {
         uint32_t tile_col = x / YTILE_WIDTH;
         uint32_t x_in_tile = x % YTILE_WIDTH;
         uint32_t oword = x_in_tile / YTILE_SPAN;
         uint32_t byte_in_oword = x_in_tile % YTILE_SPAN;

         /* Offset within tile (0-4095) */
         uint32_t tile_offset = oword * (YTILE_SPAN * YTILE_HEIGHT) + row_in_tile * YTILE_SPAN + byte_in_oword;

         /* Apply swizzle to within-tile offset ONLY (not full address)
          * This matches how ISL applies swizzle - only based on OWord column within tile
          */
         uint32_t swizzled_offset = tile_offset;
         if (swizzle_mode == 1) {
            /* I915_BIT_6_SWIZZLE_9: XOR bit 6 with bit 9 of tile_offset */
            if (tile_offset & (1 << 9))
               swizzled_offset ^= (1 << 6);
         } else if (swizzle_mode == 3) {
            /* I915_BIT_6_SWIZZLE_9_10: XOR bit 6 with (bit 9 XOR bit 10) of tile_offset */
            uint32_t swizzle = ((tile_offset >> 9) & 1) ^ ((tile_offset >> 10) & 1);
            if (swizzle)
               swizzled_offset ^= (1 << 6);
         }
         /* swizzle_mode 0: no swizzle applied */

         /* Tile address in the surface */
         uint64_t tile_base = tile_row * tile_row_stride + (uint64_t)tile_col * (YTILE_WIDTH * YTILE_HEIGHT);

         dst[tile_base + swizzled_offset] = src[y * src_pitch + x];
      }
   }
}

/**
 * Map Vulkan video profile to VDPAU decoder profile
 */
static VdpDecoderProfile
get_vdp_profile(const VkVideoProfileInfoKHR *profile)
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
               fprintf(stderr, "VDPAU: Parsed H.264 profile: Baseline (IDC=%d) -> VDP_DECODER_PROFILE_H264_BASELINE\n",
                       h264_profile->stdProfileIdc);
            }
            return VDP_DECODER_PROFILE_H264_BASELINE;
         case STD_VIDEO_H264_PROFILE_IDC_MAIN:
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VDPAU: Parsed H.264 profile: Main (IDC=%d) -> VDP_DECODER_PROFILE_H264_MAIN\n",
                       h264_profile->stdProfileIdc);
            }
            return VDP_DECODER_PROFILE_H264_MAIN;
         case STD_VIDEO_H264_PROFILE_IDC_HIGH:
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VDPAU: Parsed H.264 profile: High (IDC=%d) -> VDP_DECODER_PROFILE_H264_HIGH\n",
                       h264_profile->stdProfileIdc);
            }
            return VDP_DECODER_PROFILE_H264_HIGH;
         default:
            /* Unsupported H.264 profile, default to Main */
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VDPAU: Unsupported H.264 profile (IDC=%d), defaulting to VDP_DECODER_PROFILE_H264_MAIN\n",
                       h264_profile->stdProfileIdc);
            }
            return VDP_DECODER_PROFILE_H264_MAIN;
         }
      }

      /* No profile info provided, default to High (most compatible) */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: No H.264 profile info provided, defaulting to VDP_DECODER_PROFILE_H264_HIGH\n");
      }
      return VDP_DECODER_PROFILE_H264_HIGH;
   }

   /* Unsupported codec */
   return (VdpDecoderProfile)-1;
}

/**
 * Set up VDPAU environment to use Mesa's bundled libvdpau_va_gl
 *
 * This function ensures that when libvdpau loads a VDPAU backend driver,
 * it uses Mesa's bundled libvdpau_va_gl instead of any system-installed
 * version. This prevents confusion when the system has its own version
 * of libvdpau_va_gl installed via package manager.
 *
 * The VDPAU library searches for drivers in:
 * 1. Directory specified by VDPAU_DRIVER_PATH environment variable
 * 2. Default system locations (e.g., /usr/lib/vdpau)
 *
 * By setting VDPAU_DRIVER_PATH to point to Mesa's install directory,
 * we ensure the bundled version is found first.
 */
static void
setup_vdpau_driver_path(void)
{
   /* Only set the path if not already explicitly configured by user */
   const char *existing_path = getenv("VDPAU_DRIVER_PATH");
   if (existing_path && existing_path[0] != '\0') {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Using user-specified VDPAU_DRIVER_PATH: %s\n",
                 existing_path);
      }
      return;
   }

   /* Use dladdr to find where this library (hasvk) is installed.
    * The bundled libvdpau_va_gl.so is installed in the same libdir
    * under the 'vdpau' subdirectory.
    *
    * Example: if hasvk is at /usr/local/lib/libvulkan_intel_hasvk.so
    * then libvdpau_va_gl.so is at /usr/local/lib/vdpau/libvdpau_va_gl.so
    */
   Dl_info info;
   if (dladdr((void *)setup_vdpau_driver_path, &info) && info.dli_fname) {
      /* Make a copy since dirname() may modify its argument */
      char *lib_path = strdup(info.dli_fname);
      if (lib_path) {
         char *lib_dir = dirname(lib_path);
         if (lib_dir) {
            /* Construct path to vdpau subdirectory */
            size_t vdpau_path_len = strlen(lib_dir) + strlen("/vdpau") + 1;
            char *vdpau_path = malloc(vdpau_path_len);
            if (vdpau_path) {
               snprintf(vdpau_path, vdpau_path_len, "%s/vdpau", lib_dir);

               /* Check if the directory exists and is accessible.
                * Note: This has a theoretical TOCTOU race, but the consequence
                * is benign - if the directory disappears between this check and
                * libvdpau's driver load, libvdpau will simply fail to find the
                * driver and use system defaults. The directory is part of Mesa's
                * installation and shouldn't change during runtime.
                */
               struct stat st;
               if (stat(vdpau_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                  setenv("VDPAU_DRIVER_PATH", vdpau_path, 0);

                  if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
                     fprintf(stderr, "VDPAU: Set VDPAU_DRIVER_PATH to Mesa's bundled driver: %s\n",
                             vdpau_path);
                  }
               } else if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
                  fprintf(stderr, "VDPAU: Mesa vdpau directory not found: %s (using system default)\n",
                          vdpau_path);
               }

               free(vdpau_path);
            }
         }
         free(lib_path);
      }
   }

   /* Also set VDPAU_DRIVER to va_gl if not already set, to ensure the
    * correct driver is loaded even if other drivers are present.
    */
   const char *existing_driver = getenv("VDPAU_DRIVER");
   if (!existing_driver || existing_driver[0] == '\0') {
      setenv("VDPAU_DRIVER", "va_gl", 0);

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Set VDPAU_DRIVER to va_gl\n");
      }
   } else if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Using user-specified VDPAU_DRIVER: %s\n",
              existing_driver);
   }
}

/**
 * Get VDPAU function pointers
 */
static VkResult
get_vdpau_procs(struct anv_vdpau_session *session)
{
   VdpStatus status;

#define GET_PROC(id, func) \
   status = session->vdp_get_proc_address(session->vdp_device, id, (void**)&session->func); \
   if (status != VDP_STATUS_OK) { \
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) { \
         fprintf(stderr, "VDPAU: Failed to get proc " #func ": %d\n", status); \
      } \
      return VK_ERROR_INITIALIZATION_FAILED; \
   }

   GET_PROC(VDP_FUNC_ID_DEVICE_DESTROY, vdp_device_destroy);
   GET_PROC(VDP_FUNC_ID_DECODER_CREATE, vdp_decoder_create);
   GET_PROC(VDP_FUNC_ID_DECODER_DESTROY, vdp_decoder_destroy);
   GET_PROC(VDP_FUNC_ID_DECODER_RENDER, vdp_decoder_render);
   GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_CREATE, vdp_video_surface_create);
   GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY, vdp_video_surface_destroy);
   GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR, vdp_video_surface_get_bits_ycbcr);
   GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR, vdp_video_surface_put_bits_ycbcr);
   GET_PROC(VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS, vdp_video_surface_get_parameters);
   GET_PROC(VDP_FUNC_ID_GET_ERROR_STRING, vdp_get_error_string);

#undef GET_PROC

   return VK_SUCCESS;
}

/**
 * Get or create VDPAU device from ANV device
 *
 * VDPAU requires an X11 display for initialization when using libvdpau-va-gl.
 * We attempt to open a connection to the default X11 display.
 */
VdpDevice
anv_vdpau_get_device(struct anv_device *device)
{
   /* Check if VDPAU device already exists */
   if (device->vdp_device != VDP_INVALID_HANDLE)
      return device->vdp_device;

   /* Try to open X11 display for VDPAU */
   void *libX11 = dlopen("libX11.so.6", RTLD_LAZY);
   if (!libX11) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Cannot open libX11.so.6: %s\n", dlerror());
      }
      return VDP_INVALID_HANDLE;
   }

   typedef void *(*XOpenDisplay_fn)(const char *);
   XOpenDisplay_fn XOpenDisplay_ptr = (XOpenDisplay_fn)dlsym(libX11, "XOpenDisplay");
   if (!XOpenDisplay_ptr) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Cannot find XOpenDisplay\n");
      }
      dlclose(libX11);
      return VDP_INVALID_HANDLE;
   }

   /* Open connection to default X11 display */
   void *x11_display = XOpenDisplay_ptr(NULL);
   if (!x11_display) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Cannot open X11 display (is DISPLAY set?)\n");
      }
      dlclose(libX11);
      return VDP_INVALID_HANDLE;
   }

   /* Set up VDPAU environment to prefer Mesa's bundled libvdpau_va_gl
    * over any system-installed version. Must be done before loading libvdpau.
    */
   setup_vdpau_driver_path();

   /* Load VDPAU library and create device */
   void *libvdpau = dlopen("libvdpau.so.1", RTLD_LAZY);
   if (!libvdpau) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Cannot open libvdpau.so.1: %s\n", dlerror());
      }
      /* Close X11 display */
      typedef int (*XCloseDisplay_fn)(void *);
      XCloseDisplay_fn XCloseDisplay_ptr = (XCloseDisplay_fn)dlsym(libX11, "XCloseDisplay");
      if (XCloseDisplay_ptr)
         XCloseDisplay_ptr(x11_display);
      dlclose(libX11);
      return VDP_INVALID_HANDLE;
   }

   typedef VdpStatus (*vdp_device_create_x11_fn)(void *, int, VdpDevice *, VdpGetProcAddress **);
   vdp_device_create_x11_fn vdp_device_create_x11 =
      (vdp_device_create_x11_fn)dlsym(libvdpau, "vdp_device_create_x11");
   if (!vdp_device_create_x11) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Cannot find vdp_device_create_x11\n");
      }
      dlclose(libvdpau);
      typedef int (*XCloseDisplay_fn)(void *);
      XCloseDisplay_fn XCloseDisplay_ptr = (XCloseDisplay_fn)dlsym(libX11, "XCloseDisplay");
      if (XCloseDisplay_ptr)
         XCloseDisplay_ptr(x11_display);
      dlclose(libX11);
      return VDP_INVALID_HANDLE;
   }

   VdpDevice vdp_device;
   VdpGetProcAddress *vdp_get_proc_address;
   VdpStatus status = vdp_device_create_x11(x11_display, 0, &vdp_device, &vdp_get_proc_address);
   if (status != VDP_STATUS_OK) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: vdp_device_create_x11 failed: %d\n", status);
      }
      dlclose(libvdpau);
      typedef int (*XCloseDisplay_fn)(void *);
      XCloseDisplay_fn XCloseDisplay_ptr = (XCloseDisplay_fn)dlsym(libX11, "XCloseDisplay");
      if (XCloseDisplay_ptr)
         XCloseDisplay_ptr(x11_display);
      dlclose(libX11);
      return VDP_INVALID_HANDLE;
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Device created successfully (device=%u)\n", vdp_device);
   }

   /* Store device and handles in ANV device */
   device->vdp_device = vdp_device;
   device->vdp_get_proc_address = vdp_get_proc_address;
   device->x11_display = x11_display;
   device->libX11 = libX11;
   device->libvdpau = libvdpau;

   return vdp_device;
}

/**
 * Create VDPAU session for video decoding
 */
VkResult
anv_vdpau_session_create(struct anv_device *device,
                         struct anv_video_session *vid,
                         const VkVideoSessionCreateInfoKHR *pCreateInfo)
{
   /* Allocate VDPAU session structure */
   vid->vdpau_session =
      vk_alloc(&device->vk.alloc, sizeof(struct anv_vdpau_session), 8,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!vid->vdpau_session)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(vid->vdpau_session, 0, sizeof(struct anv_vdpau_session));

   struct anv_vdpau_session *session = vid->vdpau_session;

   /* Get or create VDPAU device */
   session->vdp_device = anv_vdpau_get_device(device);
   if (session->vdp_device == VDP_INVALID_HANDLE) {
      vk_free(&device->vk.alloc, vid->vdpau_session);
      vid->vdpau_session = NULL;
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Store get_proc_address for getting other functions */
   session->vdp_get_proc_address = device->vdp_get_proc_address;

   /* Get all VDPAU function pointers */
   VkResult result = get_vdpau_procs(session);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, vid->vdpau_session);
      vid->vdpau_session = NULL;
      return result;
   }

   /* Store video dimensions from maxCodedExtent (will be overridden on first decode) */
   session->width = pCreateInfo->maxCodedExtent.width;
   session->height = pCreateInfo->maxCodedExtent.height;

   /* Get VDPAU profile from Vulkan profile */
   session->vdp_profile = get_vdp_profile(pCreateInfo->pVideoProfile);
   if (session->vdp_profile == (VdpDecoderProfile)-1) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Unsupported video codec profile\n");
      }
      vk_free(&device->vk.alloc, vid->vdpau_session);
      vid->vdpau_session = NULL;
      return vk_error(device, VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
   }

   /* Store maxDpbSlots for later decoder creation */
   session->max_dpb_slots = pCreateInfo->maxDpbSlots;
   session->decoder_created = false;
   session->vdp_decoder = 0;  /* Will be created lazily on first decode (0 is invalid for VdpDecoder) */

   /* VDPAU decoder will be created lazily on first decode operation with actual
    * video dimensions instead of using maxCodedExtent. This prevents pitch mismatch
    * issues where VA-API surfaces are created at 4096x4096 (maxCodedExtent) but
    * actual video is much smaller (e.g., 1920x1080), causing incorrect pitch
    * (4096 vs 2048) and video corruption.
    */

   /* Allocate DPB surfaces array */
   session->num_surfaces = pCreateInfo->maxDpbSlots + 1;  /* +1 for current frame */
   session->vdp_surfaces = vk_alloc(&device->vk.alloc,
                                    session->num_surfaces * sizeof(VdpVideoSurface),
                                    8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!session->vdp_surfaces) {
      vk_free(&device->vk.alloc, vid->vdpau_session);
      vid->vdpau_session = NULL;
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Initialize surface IDs to invalid */
   for (uint32_t i = 0; i < session->num_surfaces; i++) {
      session->vdp_surfaces[i] = VDP_INVALID_HANDLE;
   }

   /* Allocate surface mapping for DPB management */
   session->surface_map_capacity = session->num_surfaces;
   session->surface_map_size = 0;
   session->surface_map = vk_alloc(&device->vk.alloc,
                                   session->surface_map_capacity *
                                   sizeof(struct anv_vdpau_surface_map),
                                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!session->surface_map) {
      vk_free(&device->vk.alloc, session->vdp_surfaces);
      vk_free(&device->vk.alloc, vid->vdpau_session);
      vid->vdpau_session = NULL;
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU session created: max %ux%u, profile=%d (decoder will be created on first decode)\n",
              session->width, session->height, session->vdp_profile);
   }

   return VK_SUCCESS;
}

/**
 * Destroy VDPAU session
 */
void
anv_vdpau_session_destroy(struct anv_device *device,
                          struct anv_video_session *vid)
{
   if (!vid->vdpau_session)
      return;

   struct anv_vdpau_session *session = vid->vdpau_session;

   /* Destroy surfaces from the surface mapping */
   if (session->surface_map && session->vdp_video_surface_destroy) {
      for (uint32_t i = 0; i < session->surface_map_size; i++) {
         if (session->surface_map[i].vdp_surface != VDP_INVALID_HANDLE) {
            if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
               fprintf(stderr, "VDPAU: Destroying DPB surface %u for image %p\n",
                       session->surface_map[i].vdp_surface,
                       (void *)session->surface_map[i].image);
            }
            session->vdp_video_surface_destroy(session->surface_map[i].vdp_surface);
         }
      }
      vk_free(&device->vk.alloc, session->surface_map);
   }

   if (session->vdp_surfaces)
      vk_free(&device->vk.alloc, session->vdp_surfaces);

   /* Destroy decoder */
   if (session->vdp_decoder && session->vdp_decoder_destroy)
      session->vdp_decoder_destroy(session->vdp_decoder);

   vk_free(&device->vk.alloc, vid->vdpau_session);
   vid->vdpau_session = NULL;

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU session destroyed\n");
   }
}

/**
 * Add or update a surface mapping in the session
 */
void
anv_vdpau_add_surface_mapping(struct anv_vdpau_session *session,
                              const struct anv_image *image,
                              VdpVideoSurface vdp_surface)
{
   /* Check if already mapped */
   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      if (session->surface_map[i].image == image) {
         session->surface_map[i].vdp_surface = vdp_surface;
         return;
      }
   }

   /* Add new mapping if space available */
   if (session->surface_map_size < session->surface_map_capacity) {
      session->surface_map[session->surface_map_size].image = image;
      session->surface_map[session->surface_map_size].vdp_surface = vdp_surface;
      session->surface_map_size++;
   }
}

/**
 * Lookup VDPAU surface for a given image
 */
VdpVideoSurface
anv_vdpau_lookup_surface(struct anv_vdpau_session *session,
                         const struct anv_image *image)
{
   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      if (session->surface_map[i].image == image) {
         return session->surface_map[i].vdp_surface;
      }
   }
   return VDP_INVALID_HANDLE;
}

/**
 * Create VDPAU surface from Vulkan image
 */
VkResult
anv_vdpau_create_surface_from_image(struct anv_device *device,
                                    struct anv_vdpau_session *session,
                                    struct anv_image *image,
                                    VdpVideoSurface *surface_id)
{
   VdpStatus vdp_status;

   /* Create a VDPAU video surface with matching dimensions */
   vdp_status = session->vdp_video_surface_create(session->vdp_device,
                                                  VDP_CHROMA_TYPE_420,
                                                  image->vk.extent.width,
                                                  image->vk.extent.height,
                                                  surface_id);
   if (vdp_status != VDP_STATUS_OK) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         const char *err_str = session->vdp_get_error_string ?
            session->vdp_get_error_string(vdp_status) : "unknown";
         fprintf(stderr, "VDPAU: Failed to create video surface: %s (%d)\n",
                 err_str, vdp_status);
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Created video surface %u (%ux%u)\n",
              *surface_id, image->vk.extent.width, image->vk.extent.height);
   }

   return VK_SUCCESS;
}

/**
 * Copy VDPAU surface to Vulkan image
 *
 * After VDPAU decode completes, copy the decoded data from VDPAU surface
 * to the Vulkan image memory. VDPAU returns linear data, but the Vulkan
 * image may be tiled (Y-tiled on Gen7/7.5/8), so we need to use ISL's
 * tiled memcpy functions for the conversion.
 */
VkResult
anv_vdpau_copy_surface_to_image(struct anv_device *device,
                                struct anv_vdpau_session *session,
                                VdpVideoSurface surface,
                                struct anv_image *image)
{
   VdpStatus vdp_status;

   /* Get the main memory binding for the image */
   struct anv_image_binding *binding =
      &image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];

   if (!binding->address.bo) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Image has no backing memory\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   struct anv_bo *bo = binding->address.bo;

   /* Map the BO to get CPU access
    * Use Write-Combine (WC) mapping for tiled surfaces - this bypasses
    * CPU cache and writes directly to memory, which is required for
    * tiled surfaces that the GPU will read.
    */
   void *tiled_ptr = anv_gem_mmap(device, bo->gem_handle, 0, bo->size, I915_MMAP_WC);
   if (tiled_ptr == MAP_FAILED || tiled_ptr == NULL) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Failed to map BO for surface copy\n");
      }
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   }

   /* Get image plane info for NV12 format */
   uint32_t y_plane_idx = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_0_BIT);
   uint32_t uv_plane_idx = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_1_BIT);
   const struct anv_surface *y_surface = &image->planes[y_plane_idx].primary_surface;
   const struct anv_surface *uv_surface = &image->planes[uv_plane_idx].primary_surface;

   uint32_t width = image->vk.extent.width;
   uint32_t height = image->vk.extent.height;

   /* Query actual VDPAU surface parameters - the surface may be larger than requested */
   VdpChromaType surface_chroma;
   uint32_t surface_width, surface_height;
   vdp_status = session->vdp_video_surface_get_parameters(surface,
                                                          &surface_chroma,
                                                          &surface_width,
                                                          &surface_height);
   if (vdp_status != VDP_STATUS_OK) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Failed to get surface parameters\n");
      }
      /* Fall back to image dimensions */
      surface_width = width;
      surface_height = height;
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Surface %u actual size: %ux%u (image: %ux%u)\n",
              surface, surface_width, surface_height, width, height);
   }

   /* Use the larger of surface size and image size for allocation
    * Also consider that libvdpau-va-gl may use the session's max size internally.
    */
   uint32_t alloc_width = MAX2(surface_width, width);
   uint32_t alloc_height = MAX2(surface_height, height);

   /* WORKAROUND: libvdpau-va-gl may use VA-API surfaces sized to the decoder's
    * max dimensions (4096x4096 in our case). The i965 driver uses internal
    * pitch alignment that can be much larger than the visible width.
    * Allocate very generously to avoid buffer overflows.
    *
    * For 1280x720 video:
    * - VA-API might use 2048-byte pitch (power of 2 alignment)
    * - Height might be padded to 736 or 768
    *
    * DEBUG: INTEL_HASVK_VIDEO_PITCH env var can override pitch calculation:
    * - 0 (default): generous 2KB alignment
    * - 1: align to ISL row pitch (128 bytes for Y-tile)
    * - 2: use exact width (no alignment)
    */
   static int force_pitch = -1;
   if (force_pitch < 0) {
      const char *env = getenv("INTEL_HASVK_VIDEO_PITCH");
      force_pitch = env ? atoi(env) : 0;
   }

   uint32_t linear_y_pitch, linear_uv_pitch;
   if (force_pitch == 1) {
      /* Use ISL-style 128-byte alignment (Y-tile width) */
      linear_y_pitch = align(alloc_width, 128);
      linear_uv_pitch = align(alloc_width, 128);
   } else if (force_pitch == 2) {
      /* Use exact width (no alignment) - risky but tests if VDPAU uses this */
      linear_y_pitch = alloc_width;
      linear_uv_pitch = alloc_width;
   } else {
      /* Default: generous 2KB alignment */
      linear_y_pitch = MAX2(align(alloc_width, 2048), 2048);
      linear_uv_pitch = MAX2(align(alloc_width, 2048), 2048);
   }

   /* Use very generous height - could be padded to power of 2 or macroblock aligned */
   uint32_t aligned_height = MAX2(align(alloc_height, 64) + 64, 1024);
   size_t y_size = (size_t)linear_y_pitch * aligned_height;
   size_t uv_size = (size_t)linear_uv_pitch * (aligned_height / 2);

   /* Use page-aligned allocation for better compatibility with DMA operations
    * Note: Use ALIGN_POT macro which preserves type (works on 32-bit and 64-bit)
    */
   size_t y_alloc_size = ALIGN_POT(y_size, 4096);
   size_t uv_alloc_size = ALIGN_POT(uv_size, 4096);
   void *linear_y = aligned_alloc(4096, y_alloc_size);
   void *linear_uv = aligned_alloc(4096, uv_alloc_size);

   if (!linear_y || !linear_uv) {
      free(linear_y);
      free(linear_uv);
      anv_gem_munmap(device, tiled_ptr, bo->size);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Zero the entire allocated buffers to avoid uninitialized data issues */
   memset(linear_y, 0, y_alloc_size);
   memset(linear_uv, 128, uv_alloc_size);  /* 128 = neutral UV for NV12 */

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Allocated linear buffers: Y=%zu bytes (pitch=%u, h=%u, alloc=%zu), UV=%zu bytes (alloc=%zu)\n",
              y_size, linear_y_pitch, aligned_height, y_alloc_size, uv_size, uv_alloc_size);
   }

   /* Get decoded data from VDPAU surface into linear buffers */
   void *linear_data[2] = { linear_y, linear_uv };
   uint32_t linear_pitches[2] = { linear_y_pitch, linear_uv_pitch };

   vdp_status = session->vdp_video_surface_get_bits_ycbcr(surface,
                                                          VDP_YCBCR_FORMAT_NV12,
                                                          linear_data,
                                                          linear_pitches);
   if (vdp_status != VDP_STATUS_OK) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         const char *err_str = session->vdp_get_error_string ?
            session->vdp_get_error_string(vdp_status) : "unknown";
         fprintf(stderr, "VDPAU: Failed to get surface bits: %s (%d)\n",
                 err_str, vdp_status);
      }
      free(linear_y);
      free(linear_uv);
      anv_gem_munmap(device, tiled_ptr, bo->size);
      return vk_error(device, VK_ERROR_UNKNOWN);
   }

   /* Debug: dump some sample data to verify VDPAU output */
   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      uint8_t *y_data = (uint8_t *)linear_y;
      fprintf(stderr, "VDPAU: Y data sample (first 8 bytes of rows 0,1,16,17) with pitch=%u:\n", linear_y_pitch);
      fprintf(stderr, "  Row 0: %02x %02x %02x %02x %02x %02x %02x %02x\n",
              y_data[0], y_data[1], y_data[2], y_data[3],
              y_data[4], y_data[5], y_data[6], y_data[7]);
      fprintf(stderr, "  Row 1 (offset %u): %02x %02x %02x %02x %02x %02x %02x %02x\n",
              linear_y_pitch,
              y_data[linear_y_pitch], y_data[linear_y_pitch+1],
              y_data[linear_y_pitch+2], y_data[linear_y_pitch+3],
              y_data[linear_y_pitch+4], y_data[linear_y_pitch+5],
              y_data[linear_y_pitch+6], y_data[linear_y_pitch+7]);
      /* Also check if VDPAU might be using width as pitch instead */
      fprintf(stderr, "  Alt Row 1 (offset %u): %02x %02x %02x %02x %02x %02x %02x %02x\n",
              width,
              y_data[width], y_data[width+1],
              y_data[width+2], y_data[width+3],
              y_data[width+4], y_data[width+5],
              y_data[width+6], y_data[width+7]);
      if (height > 16) {
         fprintf(stderr, "  Row 16: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 y_data[16*linear_y_pitch], y_data[16*linear_y_pitch+1],
                 y_data[16*linear_y_pitch+2], y_data[16*linear_y_pitch+3],
                 y_data[16*linear_y_pitch+4], y_data[16*linear_y_pitch+5],
                 y_data[16*linear_y_pitch+6], y_data[16*linear_y_pitch+7]);
         fprintf(stderr, "  Alt Row 16 (offset %u): %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 16*width,
                 y_data[16*width], y_data[16*width+1],
                 y_data[16*width+2], y_data[16*width+3],
                 y_data[16*width+4], y_data[16*width+5],
                 y_data[16*width+6], y_data[16*width+7]);
         fprintf(stderr, "  Row 17: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 y_data[17*linear_y_pitch], y_data[17*linear_y_pitch+1],
                 y_data[17*linear_y_pitch+2], y_data[17*linear_y_pitch+3],
                 y_data[17*linear_y_pitch+4], y_data[17*linear_y_pitch+5],
                 y_data[17*linear_y_pitch+6], y_data[17*linear_y_pitch+7]);
      }
      /* Sample more rows to see where corruption starts */
      uint32_t sample_rows[] = {32, 64, 128, 256, 360, 512, 640};
      for (unsigned i = 0; i < sizeof(sample_rows)/sizeof(sample_rows[0]); i++) {
         uint32_t row = sample_rows[i];
         if (row < height) {
            size_t offset = (size_t)row * linear_y_pitch;
            fprintf(stderr, "  Row %u (offset %zu): %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    row, offset,
                    y_data[offset], y_data[offset+1],
                    y_data[offset+2], y_data[offset+3],
                    y_data[offset+4], y_data[offset+5],
                    y_data[offset+6], y_data[offset+7]);
         }
      }
   }

   /* Get destination pointers in the tiled buffer */
   uint64_t y_offset = binding->address.offset + y_surface->memory_range.offset;
   uint64_t uv_offset = binding->address.offset + uv_surface->memory_range.offset;

   /* WORKAROUND: Gen7 (Ivy Bridge) has off-by-one alignment issues similar to
    * depth/stencil surfaces. Check and fix alignment if needed.
    */
   uint32_t y_alignment = y_surface->isl.alignment_B;
   uint32_t uv_alignment = uv_surface->isl.alignment_B;

   if (y_offset % y_alignment != 0) {
      uint64_t misalignment = y_offset % y_alignment;
      if (misalignment == y_alignment - 1) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "VDPAU: Fixing Y plane off-by-one alignment (offset %lu -> %lu)\n",
                    (unsigned long)y_offset, (unsigned long)(y_offset + 1));
         }
         y_offset += 1;
      }
   }

   if (uv_offset % uv_alignment != 0) {
      uint64_t misalignment = uv_offset % uv_alignment;
      if (misalignment == uv_alignment - 1) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "VDPAU: Fixing UV plane off-by-one alignment (offset %lu -> %lu)\n",
                    (unsigned long)uv_offset, (unsigned long)(uv_offset + 1));
         }
         uv_offset += 1;
      }
   }

   char *tiled_y = (char *)tiled_ptr + y_offset;
   char *tiled_uv = (char *)tiled_ptr + uv_offset;

   enum isl_tiling tiling = y_surface->isl.tiling;
   bool has_swizzling = device->isl_dev.has_bit6_swizzling;

   /* DEBUG: Allow forcing swizzle mode via INTEL_HASVK_VIDEO_SWIZZLE env var
    * 0 = no swizzle (custom Y-tile copy)
    * 1 = bit 9 only (custom Y-tile copy, should match ISL)
    * 2 = use ISL default (may not work correctly for all swizzle modes)
    * 3 = bit 9 and 10 swizzle (custom Y-tile copy, some IVB systems)
    * 4 = simple row-by-row copy (ignore tiling, for comparison)
    * 5 = test pattern: horizontal gradient (Y value = x % 256)
    * 6 = test pattern: vertical gradient (Y value = y % 256)
    *
    * Also use INTEL_HASVK_VIDEO_PITCH to control VDPAU pitch:
    * 0 = generous 2KB alignment (default, safe)
    * 1 = 128-byte alignment (Y-tile width)
    * 2 = exact width (no alignment)
    */
   static int force_swizzle = -1;
   if (force_swizzle < 0) {
      const char *env = getenv("INTEL_HASVK_VIDEO_SWIZZLE");
      if (env) {
         force_swizzle = atoi(env);
      } else {
         force_swizzle = 2; /* Use default (device setting) */
      }
   }
   if (force_swizzle == 0) {
      has_swizzling = false;
   } else if (force_swizzle == 1 || force_swizzle == 3) {
      has_swizzling = true;
   }

   /* Generate test pattern if requested */
   if (force_swizzle == 5 || force_swizzle == 6) {
      uint8_t *y_data = (uint8_t *)linear_y;
      uint8_t *uv_data = (uint8_t *)linear_uv;

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Generating test pattern (mode %d)\n", force_swizzle);
      }

      for (uint32_t row = 0; row < height; row++) {
         for (uint32_t col = 0; col < width; col++) {
            if (force_swizzle == 5) {
               /* Horizontal gradient: Y = x % 256 */
               y_data[row * linear_y_pitch + col] = col % 256;
            } else {
               /* Vertical gradient: Y = y % 256 */
               y_data[row * linear_y_pitch + col] = row % 256;
            }
         }
      }

      /* UV plane: neutral gray (128, 128) */
      for (uint32_t row = 0; row < height / 2; row++) {
         for (uint32_t col = 0; col < width; col++) {
            uv_data[row * linear_uv_pitch + col] = 128;
         }
      }
   }

   /* Mode 7: Dump linear VDPAU data to files for inspection */
   if (force_swizzle == 7) {
      static int dump_count = 0;
      if (dump_count < 5) {  /* Only dump first 5 frames */
         char filename[256];
         snprintf(filename, sizeof(filename), "/tmp/vdpau_y_%d_%ux%u_p%u.raw",
                  dump_count, width, height, linear_y_pitch);
         FILE *f = fopen(filename, "wb");
         if (f) {
            /* Write Y plane row by row with actual pitch */
            for (uint32_t row = 0; row < height; row++) {
               fwrite((uint8_t*)linear_y + row * linear_y_pitch, 1, width, f);
            }
            fclose(f);
            fprintf(stderr, "VDPAU: Dumped Y plane to %s (%ux%u)\n", filename, width, height);
         }

         snprintf(filename, sizeof(filename), "/tmp/vdpau_uv_%d_%ux%u_p%u.raw",
                  dump_count, width, height/2, linear_uv_pitch);
         f = fopen(filename, "wb");
         if (f) {
            for (uint32_t row = 0; row < height/2; row++) {
               fwrite((uint8_t*)linear_uv + row * linear_uv_pitch, 1, width, f);
            }
            fclose(f);
            fprintf(stderr, "VDPAU: Dumped UV plane to %s (%ux%u)\n", filename, width, height/2);
         }
         dump_count++;
      }
   }

   /* Bounds checking to prevent buffer overflow */
   size_t y_end_offset = y_offset + y_surface->memory_range.size;
   size_t uv_end_offset = uv_offset + uv_surface->memory_range.size;
   if (y_end_offset > bo->size || uv_end_offset > bo->size) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: ERROR - surface would overflow BO!\n");
         fprintf(stderr, "  BO size=%lu, Y end=%lu, UV end=%lu\n",
                 (unsigned long)bo->size, (unsigned long)y_end_offset,
                 (unsigned long)uv_end_offset);
      }
      free(linear_y);
      free(linear_uv);
      anv_gem_munmap(device, tiled_ptr, bo->size);
      return vk_error(device, VK_ERROR_UNKNOWN);
   }

   /* Debug option: force linear copy to test if VDPAU decode is working */
   static int force_linear = -1;
   if (force_linear < 0) {
      const char *env = getenv("INTEL_HASVK_VIDEO_LINEAR");
      force_linear = env && atoi(env);
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Y plane: offset=%lu, align=%u, pitch=%u, size=%lu\n",
              (unsigned long)y_offset, y_alignment, y_surface->isl.row_pitch_B,
              (unsigned long)y_surface->memory_range.size);
      fprintf(stderr, "VDPAU: UV plane: offset=%lu, align=%u, pitch=%u, size=%lu\n",
              (unsigned long)uv_offset, uv_alignment, uv_surface->isl.row_pitch_B,
              (unsigned long)uv_surface->memory_range.size);
      fprintf(stderr, "VDPAU: BO size=%lu\n", (unsigned long)bo->size);
      /* Show ISL surface dimensions vs image dimensions */
      fprintf(stderr, "VDPAU: ISL Y surf: %ux%u (logical), phys array pitch=%u\n",
              y_surface->isl.logical_level0_px.width,
              y_surface->isl.logical_level0_px.height,
              y_surface->isl.array_pitch_el_rows);
      fprintf(stderr, "VDPAU: Image dims: %ux%u, copy dims: %ux%u\n",
              image->vk.extent.width, image->vk.extent.height, width, height);
   }

   if (tiling == ISL_TILING_LINEAR || force_linear) {
      /* Linear tiling - just memcpy row by row */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK)) && force_linear && tiling != ISL_TILING_LINEAR) {
         fprintf(stderr, "VDPAU: WARNING - forcing linear copy to tiled surface (debug mode)\n");
      }
      for (uint32_t row = 0; row < height; row++) {
         memcpy(tiled_y + row * y_surface->isl.row_pitch_B,
                (char *)linear_y + row * linear_y_pitch,
                width);
      }
      for (uint32_t row = 0; row < height / 2; row++) {
         memcpy(tiled_uv + row * uv_surface->isl.row_pitch_B,
                (char *)linear_uv + row * linear_uv_pitch,
                width);
      }
   } else if (force_swizzle == 4) {
      /* Mode 4: Simple row-by-row copy ignoring tiling
       * This writes data as if the destination were linear, even though it's tiled.
       * Useful for diagnosing if the issue is in tiling logic vs GPU sampling.
       */
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Using simple row copy (ignoring Y-tile layout) - mode 4\n");
      }
      for (uint32_t row = 0; row < height; row++) {
         memcpy(tiled_y + row * y_surface->isl.row_pitch_B,
                (char *)linear_y + row * linear_y_pitch,
                width);
      }
      for (uint32_t row = 0; row < height / 2; row++) {
         memcpy(tiled_uv + row * uv_surface->isl.row_pitch_B,
                (char *)linear_uv + row * linear_uv_pitch,
                width);
      }
   } else if (tiling == ISL_TILING_Y0 && (force_swizzle == 0 || force_swizzle == 1 || force_swizzle == 3 || force_swizzle == 5 || force_swizzle == 6)) {
      /* Use custom Y-tile copy for explicit swizzle modes:
       * - force_swizzle=0: no swizzle (for testing)
       * - force_swizzle=1: bit 9 swizzle only (should match ISL)
       * - force_swizzle=3: bit 9+10 swizzle (some IVB systems)
       * - force_swizzle=5,6: test patterns (use bit 9 swizzle)
       */
      int actual_swizzle = (force_swizzle == 5 || force_swizzle == 6) ? 1 : force_swizzle;

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Using custom Y-tile copy with swizzle mode %d\n", actual_swizzle);
      }

      linear_to_ytiled_custom(tiled_y, (const char *)linear_y,
                              width, height,
                              y_surface->isl.row_pitch_B, linear_y_pitch,
                              actual_swizzle);

      linear_to_ytiled_custom(tiled_uv, (const char *)linear_uv,
                              width, height / 2,
                              uv_surface->isl.row_pitch_B, linear_uv_pitch,
                              actual_swizzle);
   } else {
      /* Y-tiled or X-tiled - use ISL tiled memcpy
       * isl_memcpy_linear_to_tiled copies a rectangular region from linear to tiled
       * Parameters: xt1, xt2 (x range in bytes), yt1, yt2 (y range in rows)
       */

      /* Copy Y plane (1 byte per pixel) */
      isl_memcpy_linear_to_tiled(0, width,       /* x range in bytes */
                                 0, height,      /* y range in rows */
                                 tiled_y,        /* destination (tiled) */
                                 linear_y,       /* source (linear) */
                                 y_surface->isl.row_pitch_B,  /* dst pitch */
                                 linear_y_pitch,              /* src pitch */
                                 has_swizzling,  /* bit6 swizzling on Gen7 */
                                 tiling,
                                 ISL_MEMCPY);

      /* Copy UV plane (2 bytes per pixel pair, half height) */
      isl_memcpy_linear_to_tiled(0, width,           /* x range in bytes (UV is interleaved) */
                                 0, height / 2,      /* y range in rows (half height for 4:2:0) */
                                 tiled_uv,           /* destination (tiled) */
                                 linear_uv,          /* source (linear) */
                                 uv_surface->isl.row_pitch_B,  /* dst pitch */
                                 linear_uv_pitch,              /* src pitch */
                                 has_swizzling,      /* bit6 swizzling on Gen7 */
                                 tiling,
                                 ISL_MEMCPY);
   }

   /* Debug: dump some tiled data to verify copy worked */
   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      uint8_t *tiled_data = (uint8_t *)tiled_y;
      uint8_t *linear_data = (uint8_t *)linear_y;
      uint32_t tiles_per_row = y_surface->isl.row_pitch_B / 128;
      fprintf(stderr, "VDPAU: Tiled Y data sample (tiles_per_row=%u):\n", tiles_per_row);

      /* Show first OWord of each row in the first tile (rows 0-31) */
      fprintf(stderr, "VDPAU: First tile OWord0 (first 16 bytes of tile):\n");
      for (int row = 0; row < 32; row++) {
         /* In Y-tile, row N of OWord 0 is at offset row*16 within the tile */
         size_t tile_row_offset = row * 16;
         fprintf(stderr, "  Tile row %2d (linear row %2d): %02x %02x %02x %02x  %02x %02x %02x %02x  ",
                 row, row,
                 tiled_data[tile_row_offset], tiled_data[tile_row_offset+1],
                 tiled_data[tile_row_offset+2], tiled_data[tile_row_offset+3],
                 tiled_data[tile_row_offset+4], tiled_data[tile_row_offset+5],
                 tiled_data[tile_row_offset+6], tiled_data[tile_row_offset+7]);
         /* Compare with linear source */
         size_t linear_offset = row * linear_y_pitch;
         fprintf(stderr, "| linear: %02x %02x %02x %02x\n",
                 linear_data[linear_offset], linear_data[linear_offset+1],
                 linear_data[linear_offset+2], linear_data[linear_offset+3]);
      }

      /* Tile row 1 (image row 32): offset = tiles_per_row * 4096 */
      size_t tile_row1_offset = (size_t)tiles_per_row * 4096;
      fprintf(stderr, "  Tile[1,0] row 0 (img row 32, offset=%zu): %02x %02x %02x %02x %02x %02x %02x %02x\n",
              tile_row1_offset,
              tiled_data[tile_row1_offset], tiled_data[tile_row1_offset+1],
              tiled_data[tile_row1_offset+2], tiled_data[tile_row1_offset+3],
              tiled_data[tile_row1_offset+4], tiled_data[tile_row1_offset+5],
              tiled_data[tile_row1_offset+6], tiled_data[tile_row1_offset+7]);
      /* Tile row 2 (image row 64): offset = 2 * tiles_per_row * 4096 */
      size_t tile_row2_offset = 2 * (size_t)tiles_per_row * 4096;
      fprintf(stderr, "  Tile[2,0] row 0 (img row 64, offset=%zu): %02x %02x %02x %02x %02x %02x %02x %02x\n",
              tile_row2_offset,
              tiled_data[tile_row2_offset], tiled_data[tile_row2_offset+1],
              tiled_data[tile_row2_offset+2], tiled_data[tile_row2_offset+3],
              tiled_data[tile_row2_offset+4], tiled_data[tile_row2_offset+5],
              tiled_data[tile_row2_offset+6], tiled_data[tile_row2_offset+7]);
      /* Also sample tile column 1 (x=128-255) */
      size_t tile_col1_offset = 4096; /* Second tile in row */
      fprintf(stderr, "  Tile[0,1] row 0 (img col 128, offset=%zu): %02x %02x %02x %02x %02x %02x %02x %02x\n",
              tile_col1_offset,
              tiled_data[tile_col1_offset], tiled_data[tile_col1_offset+1],
              tiled_data[tile_col1_offset+2], tiled_data[tile_col1_offset+3],
              tiled_data[tile_col1_offset+4], tiled_data[tile_col1_offset+5],
              tiled_data[tile_col1_offset+6], tiled_data[tile_col1_offset+7]);
      /* Sample at image row 360 (middle of frame) - tile row 11 */
      size_t tile_row11_offset = 11 * (size_t)tiles_per_row * 4096;
      fprintf(stderr, "  Tile[11,0] row 0 (img row 352, offset=%zu): %02x %02x %02x %02x %02x %02x %02x %02x\n",
              tile_row11_offset,
              tiled_data[tile_row11_offset], tiled_data[tile_row11_offset+1],
              tiled_data[tile_row11_offset+2], tiled_data[tile_row11_offset+3],
              tiled_data[tile_row11_offset+4], tiled_data[tile_row11_offset+5],
              tiled_data[tile_row11_offset+6], tiled_data[tile_row11_offset+7]);
   }

   /* Ensure all writes are visible to the GPU by flushing write-combine buffers */
   __builtin_ia32_mfence();

   /* Clean up */
   free(linear_y);
   free(linear_uv);
   anv_gem_munmap(device, tiled_ptr, bo->size);

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Copied surface %u to image:\n", surface);
      fprintf(stderr, "  Image: %ux%u, Y pitch=%u, UV pitch=%u\n",
              width, height, y_surface->isl.row_pitch_B, uv_surface->isl.row_pitch_B);
      fprintf(stderr, "  Tiling=%d, swizzle=%d\n", tiling, has_swizzling);
      fprintf(stderr, "  Y offset=%lu, UV offset=%lu\n",
              (unsigned long)(binding->address.offset + y_surface->memory_range.offset),
              (unsigned long)(binding->address.offset + uv_surface->memory_range.offset));
      fprintf(stderr, "  Y surface: size=%lu, UV surface: size=%lu\n",
              (unsigned long)y_surface->memory_range.size,
              (unsigned long)uv_surface->memory_range.size);
   }

   return VK_SUCCESS;
}

/**
 * Decode a frame using VDPAU (Deferred Execution)
 *
 * Records VDPAU decode command for later execution at QueueSubmit time.
 */
VkResult
anv_vdpau_decode_frame(struct anv_cmd_buffer *cmd_buffer,
                       const VkVideoDecodeInfoKHR *frame_info)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_video_session *vid = cmd_buffer->video.vid;
   struct anv_vdpau_session *session = vid->vdpau_session;
   VkResult result;

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "anv_vdpau_decode_frame: ENTRY (vid=%p, session=%p)\n",
              (void *)vid, (void *)session);
   }

   if (!vid || !session) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "anv_vdpau_decode_frame: ERROR - vid=%p session=%p\n",
                 (void *)vid, (void *)session);
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Get H.264-specific picture info */
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   if (!h264_pic_info) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Missing H.264 picture info in decode\n");
      }
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   /* Get destination image view and extract image */
   ANV_FROM_HANDLE(anv_image_view, dst_image_view,
                   frame_info->dstPictureResource.imageViewBinding);
   if (!dst_image_view || !dst_image_view->image) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Invalid destination image view\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }
   struct anv_image *dst_image = (struct anv_image *)dst_image_view->image;

   /* Lazy decoder creation: Create VDPAU decoder on first decode with actual dimensions
    * This fixes pitch mismatch issues where maxCodedExtent (4096x4096) would cause
    * VA-API surfaces to be created with 4096-byte pitch, but actual video is smaller
    * (e.g., 1920x1080 needs 2048-byte pitch). Creating decoder with actual dimensions
    * ensures VA-API surfaces match the video size and pitch.
    */
   uint32_t actual_width = dst_image->vk.extent.width;
   uint32_t actual_height = dst_image->vk.extent.height;

   if (!session->decoder_created ||
       session->width != actual_width ||
       session->height != actual_height) {

      /* If decoder exists but dimensions changed, recreate it */
      if (session->decoder_created) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "VDPAU: Video dimensions changed from %ux%u to %ux%u, recreating decoder\n",
                    session->width, session->height, actual_width, actual_height);
         }

         /* Destroy old decoder */
         if (session->vdp_decoder && session->vdp_decoder_destroy) {
            session->vdp_decoder_destroy(session->vdp_decoder);
            session->vdp_decoder = 0;  /* 0 is invalid for VdpDecoder */
            session->decoder_created = false;
         }

         /* Clear surface mappings as they're tied to old decoder */
         for (uint32_t i = 0; i < session->surface_map_size; i++) {
            if (session->surface_map[i].vdp_surface != VDP_INVALID_HANDLE &&
                session->vdp_video_surface_destroy) {
               session->vdp_video_surface_destroy(session->surface_map[i].vdp_surface);
               session->surface_map[i].vdp_surface = VDP_INVALID_HANDLE;
            }
         }
         session->surface_map_size = 0;
      } else {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "VDPAU: Creating decoder with actual dimensions %ux%u (was max %ux%u)\n",
                    actual_width, actual_height, session->width, session->height);
         }
      }

      /* Update session dimensions to actual video size */
      session->width = actual_width;
      session->height = actual_height;

      /* Create VDPAU decoder with actual dimensions */
      VdpStatus vdp_status = session->vdp_decoder_create(session->vdp_device,
                                                         session->vdp_profile,
                                                         session->width,
                                                         session->height,
                                                         session->max_dpb_slots,
                                                         &session->vdp_decoder);
      if (vdp_status != VDP_STATUS_OK) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            const char *err_str = session->vdp_get_error_string ?
               session->vdp_get_error_string(vdp_status) : "unknown";
            fprintf(stderr, "VDPAU: Failed to create decoder: %s (%d)\n", err_str, vdp_status);
         }
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }

      session->decoder_created = true;

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Decoder created successfully: %ux%u, profile=%d, decoder=%u\n",
                 session->width, session->height, session->vdp_profile,
                 session->vdp_decoder);
      }
   }

   /* Create or reuse destination surface */
   VdpVideoSurface dst_surface;
   dst_surface = anv_vdpau_lookup_surface(session, dst_image);
   if (dst_surface == VDP_INVALID_HANDLE) {
      result = anv_vdpau_create_surface_from_image(device, session, dst_image, &dst_surface);
      if (result != VK_SUCCESS) {
         return result;
      }
      anv_vdpau_add_surface_mapping(session, dst_image, dst_surface);
   }

   /* Get video session parameters */
   struct anv_video_session_params *params = cmd_buffer->video.params;
   if (!params) {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: No video session parameters bound\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Allocate array for reference surfaces */
   VdpVideoSurface *ref_surfaces = NULL;
   uint32_t ref_surface_count = 0;

   if (frame_info->referenceSlotCount > 0) {
      ref_surfaces = vk_alloc(&device->vk.alloc,
                              frame_info->referenceSlotCount * sizeof(VdpVideoSurface),
                              8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!ref_surfaces) {
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   /* Import or reuse reference frame surfaces */
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      const VkVideoReferenceSlotInfoKHR *ref_slot = &frame_info->pReferenceSlots[i];
      if (ref_slot->slotIndex < 0 || !ref_slot->pPictureResource)
         continue;

      ANV_FROM_HANDLE(anv_image_view, ref_image_view,
                      ref_slot->pPictureResource->imageViewBinding);
      if (!ref_image_view || !ref_image_view->image)
         continue;

      const struct anv_image *ref_image = ref_image_view->image;
      VdpVideoSurface ref_surface = anv_vdpau_lookup_surface(session, ref_image);

      if (ref_surface == VDP_INVALID_HANDLE) {
         result = anv_vdpau_create_surface_from_image(device, session,
                                                       (struct anv_image *)ref_image,
                                                       &ref_surface);
         if (result != VK_SUCCESS) {
            vk_free(&device->vk.alloc, ref_surfaces);
            return result;
         }
         anv_vdpau_add_surface_mapping(session, ref_image, ref_surface);
      }

      ref_surfaces[ref_surface_count++] = ref_surface;
   }

   /* Translate picture parameters to VDPAU format */
   VdpPictureInfoH264 vdp_pic;
   anv_vdpau_translate_h264_picture_params(device, frame_info, h264_pic_info,
                                           &params->vk, session, dst_surface,
                                           &vdp_pic);

   /* Get bitstream buffer */
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   if (!src_buffer || !src_buffer->address.bo) {
      vk_free(&device->vk.alloc, ref_surfaces);
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Invalid source buffer\n");
      }
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Map the bitstream buffer */
   void *bitstream_data = anv_gem_mmap(device, src_buffer->address.bo->gem_handle,
                                       0, frame_info->srcBufferRange, 0);
   if (!bitstream_data) {
      vk_free(&device->vk.alloc, ref_surfaces);
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Failed to map bitstream buffer\n");
      }
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   }

   /* Create bitstream buffer array for VDPAU
    * VDPAU takes an array of VdpBitstreamBuffer structures, one per slice
    */
   uint32_t slice_count = h264_pic_info->sliceCount;
   if (slice_count == 0) {
      anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
      vk_free(&device->vk.alloc, ref_surfaces);
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: H.264 decode has no slices\n");
      }
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   VdpBitstreamBuffer *bitstream_buffers = vk_alloc(&device->vk.alloc,
                                                    slice_count * sizeof(VdpBitstreamBuffer),
                                                    8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!bitstream_buffers) {
      anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
      vk_free(&device->vk.alloc, ref_surfaces);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Set up bitstream buffer for each slice */
   for (uint32_t s = 0; s < slice_count; s++) {
      uint32_t slice_offset = h264_pic_info->pSliceOffsets[s];
      uint32_t slice_size;

      if (s == slice_count - 1) {
         slice_size = frame_info->srcBufferRange - slice_offset;
      } else {
         slice_size = h264_pic_info->pSliceOffsets[s + 1] - slice_offset;
      }

      bitstream_buffers[s].struct_version = VDP_BITSTREAM_BUFFER_VERSION;
      bitstream_buffers[s].bitstream = (uint8_t *)bitstream_data +
                                       frame_info->srcBufferOffset + slice_offset;
      bitstream_buffers[s].bitstream_bytes = slice_size;
   }

   /* Get target BO for synchronization */
   struct anv_image_binding *dst_binding = &dst_image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];

   /* Create deferred decode command */
   struct anv_vdpau_decode_cmd decode_cmd = {
      .decoder = session->vdp_decoder,
      .target_surface = dst_surface,
      .target_bo = dst_binding->address.bo,
      .pic_info = vdp_pic,
      .bitstream_buffer_count = slice_count,
      .bitstream_buffers = bitstream_buffers,
      .bitstream_data = bitstream_data,
      .bitstream_data_size = frame_info->srcBufferRange,
      .ref_surfaces = ref_surfaces,
      .ref_surface_count = ref_surface_count,
      .session = session,
   };

   /* Add command to the deferred execution queue */
   util_dynarray_append(&cmd_buffer->video.vdpau_decodes, decode_cmd);

   /* Add texture cache invalidate for coherency */
   anv_add_pending_pipe_bits(cmd_buffer,
                             ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT,
                             "VDPAU decode texture cache invalidate");

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VDPAU: Recorded deferred decode command (%u slices)\n",
              slice_count);
   }

   return VK_SUCCESS;
}

/**
 * Execute deferred VDPAU decode commands
 *
 * Called at QueueSubmit time to execute all VDPAU decode operations.
 */
VkResult
anv_vdpau_execute_deferred_decodes(struct anv_device *device,
                                   struct anv_cmd_buffer *cmd_buffer)
{
   VdpStatus vdp_status;
   VkResult result = VK_SUCCESS;

   /* Execute each deferred decode command */
   util_dynarray_foreach(&cmd_buffer->video.vdpau_decodes,
                         struct anv_vdpau_decode_cmd, decode_cmd)
   {
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "VDPAU: Executing decode: surface=%u, %u bitstream buffers\n",
                 decode_cmd->target_surface, decode_cmd->bitstream_buffer_count);
      }

      /* Call VDPAU decoder render */
      vdp_status = decode_cmd->session->vdp_decoder_render(
         decode_cmd->decoder,
         decode_cmd->target_surface,
         (VdpPictureInfo *)&decode_cmd->pic_info,
         decode_cmd->bitstream_buffer_count,
         decode_cmd->bitstream_buffers);

      if (vdp_status != VDP_STATUS_OK) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            const char *err_str = decode_cmd->session->vdp_get_error_string ?
               decode_cmd->session->vdp_get_error_string(vdp_status) : "unknown";
            fprintf(stderr, "VDPAU: Decoder render failed: %s (%d)\n", err_str, vdp_status);
         }
         result = vk_error(device, VK_ERROR_UNKNOWN);
      } else {
         /* Copy decoded data from VDPAU surface to Vulkan image */
         struct anv_image *target_image = NULL;

         /* Find the Vulkan image for this surface */
         for (uint32_t i = 0; i < decode_cmd->session->surface_map_size; i++) {
            if (decode_cmd->session->surface_map[i].vdp_surface == decode_cmd->target_surface) {
               target_image = (struct anv_image *)decode_cmd->session->surface_map[i].image;
               break;
            }
         }

         if (target_image) {
            VkResult copy_result = anv_vdpau_copy_surface_to_image(
               device, decode_cmd->session, decode_cmd->target_surface, target_image);
            if (copy_result != VK_SUCCESS && result == VK_SUCCESS) {
               result = copy_result;
            }
         }
      }

      /* Clean up bitstream buffers */
      vk_free(&device->vk.alloc, decode_cmd->bitstream_buffers);

      /* Unmap the bitstream data */
      if (decode_cmd->bitstream_data) {
         anv_gem_munmap(device, decode_cmd->bitstream_data, decode_cmd->bitstream_data_size);
      }

      /* Free reference surface array (but not the surfaces themselves) */
      if (decode_cmd->ref_surfaces) {
         vk_free(&device->vk.alloc, decode_cmd->ref_surfaces);
      }

      if (result != VK_SUCCESS) {
         break;
      }
   }

   /* Clear the deferred commands after execution */
   util_dynarray_clear(&cmd_buffer->video.vdpau_decodes);

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK)) && result == VK_SUCCESS) {
      fprintf(stderr, "VDPAU: All deferred decodes executed successfully\n");
   }

   return result;
}
