/*
 * Copyright © 2025 dancingmirrors@icloud.com
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
 * VDPAU Bridge Module for hasvk
 *
 * This module implements a bridge between Vulkan Video decode operations
 * and VDPAU. It uses libvdpau-va-gl to leverage the stable VA-API/OpenGL
 * implementation on generations 7-8 hardware through the crocus driver,
 * avoiding the complexity of direct VA-API interfacing.
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
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <dlfcn.h>
#include <libgen.h>
#include <pthread.h>
#include <va/va.h>
#include <va/va_drm.h>

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

/* Maximum frames to process per submit for video decode.
 * Set to 0 for unlimited - process all queued frames.
 *
 * Historical note: This was previously limited to work around FD leaks
 * causing VK_ERROR_OUT_OF_HOST_MEMORY. The real fix is proper FD management.
 */
#ifndef HASVK_MAX_FRAMES_PER_SUBMIT
#define HASVK_MAX_FRAMES_PER_SUBMIT 0
#endif

/* Maximum surface cache size for video decode. */
#ifndef HASVK_MAX_SURFACE_CACHE_SIZE
#define HASVK_MAX_SURFACE_CACHE_SIZE 32
#endif

/**
 * Custom linear-to-Y-tiled copy with configurable swizzle mode
 *
 * Unlike ISL's generic implementation, this supports both:
 * - Bit 9 only swizzle (I915_BIT_6_SWIZZLE_9)
 * - Bit 9 and 10 swizzle (I915_BIT_6_SWIZZLE_9_10)
 *
 * The hardware may use different swizzle modes depending on memory configuration.
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
            return VDP_DECODER_PROFILE_H264_BASELINE;
         case STD_VIDEO_H264_PROFILE_IDC_MAIN:
            return VDP_DECODER_PROFILE_H264_MAIN;
         case STD_VIDEO_H264_PROFILE_IDC_HIGH:
            return VDP_DECODER_PROFILE_H264_HIGH;
         default:
            /* Unsupported H.264 profile, default to Main */
            return VDP_DECODER_PROFILE_H264_MAIN;
         }
      }

      /* No profile info provided, default to High (most compatible) */
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
   const char *existing_path = os_get_option("VDPAU_DRIVER_PATH");
   if (existing_path && existing_path[0] != '\0') {
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
                  os_set_option("VDPAU_DRIVER_PATH", vdpau_path, 0);
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
   const char *existing_driver = os_get_option("VDPAU_DRIVER");
   if (!existing_driver || existing_driver[0] == '\0') {
      os_set_option("VDPAU_DRIVER", "va_gl", 0);
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

   /* Try to get hasvk DMA-buf export extension (required at build time)
    * This is loaded via dlsym since it's not a standard VDPAU function.
    * We look it up in the global namespace which will find it in libvdpau_va_gl.
    */
   session->vdp_video_surface_export_dmabuf = NULL;
   session->dmabuf_supported = false;

   void *export_fn = dlsym(RTLD_DEFAULT, "vdpVideoSurfaceExportDmaBufhasvk");
   if (export_fn) {
      session->vdp_video_surface_export_dmabuf =
         (VdpVideoSurfaceExportDmaBufhasvk_fn)export_fn;
      session->dmabuf_supported = true;
   }

   return VK_SUCCESS;
}

/**
 * Get or create VDPAU device from ANV device
 *
 * VDPAU requires an X11 display for initialization when using libvdpau-va-gl.
 * We attempt to open a connection to the default X11 display.
 *
 * Thread-safe: Uses device mutex to prevent race conditions when multiple
 * decode threads try to create the VDPAU device simultaneously.
 */
VdpDevice
anv_vdpau_get_device(struct anv_device *device)
{
   /* Fast path: Check if VDPAU device already exists without locking */
   if (device->vdp_device != VDP_INVALID_HANDLE)
      return device->vdp_device;

   /* Slow path: Need to create the device, acquire lock to prevent races */
   pthread_mutex_lock(&device->mutex);

   /* Double-check after acquiring lock - another thread might have created it */
   if (device->vdp_device != VDP_INVALID_HANDLE) {
      pthread_mutex_unlock(&device->mutex);
      return device->vdp_device;
   }

   /* Try to open X11 display for VDPAU */
   void *libX11 = dlopen("libX11.so.6", RTLD_LAZY);
   if (!libX11) {
      pthread_mutex_unlock(&device->mutex);
      return VDP_INVALID_HANDLE;
   }

   typedef void *(*XOpenDisplay_fn)(const char *);
   XOpenDisplay_fn XOpenDisplay_ptr = (XOpenDisplay_fn)dlsym(libX11, "XOpenDisplay");
   if (!XOpenDisplay_ptr) {
      dlclose(libX11);
      pthread_mutex_unlock(&device->mutex);
      return VDP_INVALID_HANDLE;
   }

   /* Open connection to default X11 display */
   void *x11_display = XOpenDisplay_ptr(NULL);
   if (!x11_display) {
      dlclose(libX11);
      pthread_mutex_unlock(&device->mutex);
      return VDP_INVALID_HANDLE;
   }

   /* Set up VDPAU environment to prefer Mesa's bundled libvdpau_va_gl
    * over any system-installed version. Must be done before loading libvdpau.
    */
   setup_vdpau_driver_path();

   /* Load VDPAU library and create device */
   void *libvdpau = dlopen("libvdpau.so.1", RTLD_LAZY);
   if (!libvdpau) {
      /* Close X11 display */
      typedef int (*XCloseDisplay_fn)(void *);
      XCloseDisplay_fn XCloseDisplay_ptr = (XCloseDisplay_fn)dlsym(libX11, "XCloseDisplay");
      if (XCloseDisplay_ptr)
         XCloseDisplay_ptr(x11_display);
      dlclose(libX11);
      pthread_mutex_unlock(&device->mutex);
      return VDP_INVALID_HANDLE;
   }

   typedef VdpStatus (*vdp_device_create_x11_fn)(void *, int, VdpDevice *, VdpGetProcAddress **);
   vdp_device_create_x11_fn vdp_device_create_x11 =
      (vdp_device_create_x11_fn)dlsym(libvdpau, "vdp_device_create_x11");
   if (!vdp_device_create_x11) {
      dlclose(libvdpau);
      typedef int (*XCloseDisplay_fn)(void *);
      XCloseDisplay_fn XCloseDisplay_ptr = (XCloseDisplay_fn)dlsym(libX11, "XCloseDisplay");
      if (XCloseDisplay_ptr)
         XCloseDisplay_ptr(x11_display);
      dlclose(libX11);
      pthread_mutex_unlock(&device->mutex);
      return VDP_INVALID_HANDLE;
   }

   VdpDevice vdp_device;
   VdpGetProcAddress *vdp_get_proc_address;
   VdpStatus status = vdp_device_create_x11(x11_display, 0, &vdp_device, &vdp_get_proc_address);
   if (status != VDP_STATUS_OK) {
      dlclose(libvdpau);
      typedef int (*XCloseDisplay_fn)(void *);
      XCloseDisplay_fn XCloseDisplay_ptr = (XCloseDisplay_fn)dlsym(libX11, "XCloseDisplay");
      if (XCloseDisplay_ptr)
         XCloseDisplay_ptr(x11_display);
      dlclose(libX11);
      pthread_mutex_unlock(&device->mutex);
      return VDP_INVALID_HANDLE;
   }

   /* Store device and handles in ANV device */
   device->vdp_device = vdp_device;
   device->vdp_get_proc_address = vdp_get_proc_address;
   device->x11_display = x11_display;
   device->libX11 = libX11;
   device->libvdpau = libvdpau;

   pthread_mutex_unlock(&device->mutex);
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

   /* Allocate surface mapping for DPB management
    * Use a reasonable cache size to prevent GPU memory exhaustion:
    * - Video surfaces can be ~17.7MB each for 4K NV12
    * - 32 surfaces = ~566MB for 4K, ~96MB for 1080p
    *
    * hasvk hardware typically shares system RAM but has limited GPU address
    * space (1.7GB GTT aperture). Cache size balances memory usage across
    * all resolutions.
    */
   uint32_t requested_capacity = pCreateInfo->maxDpbSlots + 1;
   uint32_t max_cache_size = HASVK_MAX_SURFACE_CACHE_SIZE;

   session->surface_map_capacity = MIN2(requested_capacity, max_cache_size);
   session->surface_map_size = 0;
   session->frame_counter = 0;

   /* Initialize cached linear buffers (allocated on first use) */
   session->linear_y_buffer = NULL;
   session->linear_uv_buffer = NULL;
   session->linear_y_buffer_size = 0;
   session->linear_uv_buffer_size = 0;

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
            session->vdp_video_surface_destroy(session->surface_map[i].vdp_surface);
         }
      }
      vk_free(&device->vk.alloc, session->surface_map);
   }

   if (session->vdp_surfaces)
      vk_free(&device->vk.alloc, session->vdp_surfaces);

   /* Free cached linear buffers */
   if (session->linear_y_buffer)
      free(session->linear_y_buffer);
   if (session->linear_uv_buffer)
      free(session->linear_uv_buffer);

   /* Destroy decoder */
   if (session->vdp_decoder && session->vdp_decoder_destroy)
      session->vdp_decoder_destroy(session->vdp_decoder);

   vk_free(&device->vk.alloc, vid->vdpau_session);
   vid->vdpau_session = NULL;
}

/**
 * Add or update a surface mapping in the session
 *
 * Implements LRU eviction when the cache is full to prevent unbounded memory growth.
 * This is critical for long video playback sessions.
 */
void
anv_vdpau_add_surface_mapping(struct anv_vdpau_session *session,
                              const struct anv_image *image,
                              VdpVideoSurface vdp_surface)
{
   /* Increment frame counter for LRU tracking */
   session->frame_counter++;

   /* Check if already mapped - update timestamp and return */
   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      if (session->surface_map[i].image == image) {
         session->surface_map[i].vdp_surface = vdp_surface;
         session->surface_map[i].last_used_frame = session->frame_counter;
         return;
      }
   }

   /* Add new mapping if space available */
   if (session->surface_map_size < session->surface_map_capacity) {
      session->surface_map[session->surface_map_size].image = image;
      session->surface_map[session->surface_map_size].vdp_surface = vdp_surface;
      session->surface_map[session->surface_map_size].last_used_frame = session->frame_counter;
      session->surface_map_size++;
      return;
   }

   /* Cache is full - evict least recently used surface
    * This prevents VK_ERROR_OUT_OF_DEVICE_MEMORY during long video playback
    */
   if (!session->surface_map) {
      /* Defensive check - should never happen if initialization succeeded */
      return;
   }

   uint32_t lru_index = 0;
   uint64_t oldest_frame = session->surface_map[0].last_used_frame;

   for (uint32_t i = 1; i < session->surface_map_size; i++) {
      if (session->surface_map[i].last_used_frame < oldest_frame) {
         oldest_frame = session->surface_map[i].last_used_frame;
         lru_index = i;
      }
   }

   /* Destroy the LRU surface before replacing it */
   if (session->surface_map[lru_index].vdp_surface != VDP_INVALID_HANDLE &&
       session->vdp_video_surface_destroy) {
      session->vdp_video_surface_destroy(session->surface_map[lru_index].vdp_surface);
   }

   /* Replace with new mapping */
   session->surface_map[lru_index].image = image;
   session->surface_map[lru_index].vdp_surface = vdp_surface;
   session->surface_map[lru_index].last_used_frame = session->frame_counter;
}

/**
 * Comparison function for qsort - sorts uint64_t in descending order
 */
static int
compare_uint64_desc(const void *a, const void *b)
{
   uint64_t val_a = *(const uint64_t *)a;
   uint64_t val_b = *(const uint64_t *)b;
   if (val_a > val_b) return -1;
   if (val_a < val_b) return 1;
   return 0;
}

/**
 * Aggressively evict old surfaces to free GPU memory
 *
 * Called when we detect memory pressure (e.g., DMA-buf export failures).
 * Evicts surfaces that haven't been used recently, keeping only the most
 * recent ones needed for reference frames.
 *
 * @param session       VDPAU session
 * @param keep_count    Number of most recently used surfaces to keep (minimum 3)
 */
static void
anv_vdpau_evict_old_surfaces(struct anv_vdpau_session *session,
                             uint32_t keep_count)
{
   if (!session || !session->surface_map || session->surface_map_size == 0)
      return;

   /* Keep at least 3 surfaces for basic H.264 decode (I, P, B frames) */
   if (keep_count < 3)
      keep_count = 3;

   /* If we already have fewer surfaces than keep_count, nothing to evict */
   if (session->surface_map_size <= keep_count)
      return;

   /* Find the Nth most recent surface (where N = keep_count)
    * All surfaces older than this will be evicted
    */
   uint64_t eviction_threshold = 0;

   /* Create a sorted list of last_used_frame values
    * Check for potential integer overflow in allocation size
    */
   size_t alloc_size = session->surface_map_size * sizeof(uint64_t);
   if (session->surface_map_size > 0 &&
       alloc_size / sizeof(uint64_t) != session->surface_map_size) {
      /* Integer overflow detected */
      return;
   }

   uint64_t *sorted_frames = malloc(alloc_size);
   if (!sorted_frames) {
      /* Memory allocation failed - can't evict surfaces properly.
       * Log error and return since we can't free memory without sorting.
       */
      return;
   }

   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      sorted_frames[i] = session->surface_map[i].last_used_frame;
   }

   /* Sort in descending order (most recent first) using qsort for efficiency */
   qsort(sorted_frames, session->surface_map_size, sizeof(uint64_t), compare_uint64_desc);

   /* Determine eviction threshold:
    * - Threshold is the timestamp of the Nth most recent surface
    * - Surfaces with timestamp < threshold will be evicted
    * - Surfaces with timestamp >= threshold will be kept
    *
    * Example with 5 surfaces, keep_count=3:
    * - Sorted descending: [100, 90, 80, 70, 60]
    *   indices:            [0,   1,  2,  3,  4]
    * - sorted_frames[2] = 80 (the 3rd most recent)
    * - Keep: timestamps >= 80 (100, 90, 80)
    * - Evict: timestamps < 80 (70, 60)
    * - Threshold = sorted_frames[keep_count-1] = sorted_frames[2] = 80
    *
    * Edge case: keep_count >= surface_map_size means keep all surfaces.
    * Set threshold to 0 so all surfaces (frame_counter >= 1) are kept.
    */
   if (keep_count < session->surface_map_size) {
      /* Normal case: evict old surfaces beyond keep_count
       * keep_count is guaranteed to be >= 3 from earlier check
       */
      eviction_threshold = sorted_frames[keep_count - 1];
   } else {
      /* Edge case: keep all surfaces (keep_count >= cache size) */
      eviction_threshold = 0;
   }
   free(sorted_frames);

   /* Evict all surfaces older than the threshold */
   uint32_t evicted_count = 0;
   for (uint32_t i = 0; i < session->surface_map_size; ) {
      if (session->surface_map[i].last_used_frame < eviction_threshold) {
         /* Destroy this surface */
         if (session->surface_map[i].vdp_surface != VDP_INVALID_HANDLE &&
             session->vdp_video_surface_destroy) {
            session->vdp_video_surface_destroy(session->surface_map[i].vdp_surface);
            evicted_count++;
         }

         /* Remove from array by shifting remaining elements */
         for (uint32_t j = i; j < session->surface_map_size - 1; j++) {
            session->surface_map[j] = session->surface_map[j + 1];
         }
         session->surface_map_size--;
         /* Don't increment i since we shifted elements */
      } else {
         i++;
      }
   }
}

/**
 * Lookup VDPAU surface for a given image
 *
 * Updates LRU timestamp on access to prevent premature eviction of active surfaces.
 *
 * Design note: We update the timestamp to the CURRENT frame_counter value without
 * incrementing it. This is correct because frame_counter represents a logical clock
 * that only advances on surface additions. When we lookup a surface, we're marking
 * it as "accessed at the current time" (the current frame_counter value).
 *
 * Example:
 *   frame_counter=10: Add surface A (A.timestamp = 10)
 *   frame_counter=11: Add surface B (B.timestamp = 11)
 *   Lookup A: A.timestamp = 11 (current frame_counter, A is now "newer" than B)
 *   frame_counter=12: Add surface C (C.timestamp = 12)
 *   LRU eviction: B has oldest timestamp (11), so B is evicted
 */
VdpVideoSurface
anv_vdpau_lookup_surface(struct anv_vdpau_session *session,
                         const struct anv_image *image)
{
   for (uint32_t i = 0; i < session->surface_map_size; i++) {
      if (session->surface_map[i].image == image) {
         /* Update LRU timestamp on access to current logical time */
         session->surface_map[i].last_used_frame = session->frame_counter;
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
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   return VK_SUCCESS;
}

/**
 * Copy VDPAU surface to Vulkan image using DMA-buf (optimized path)
 *
 * This function implements an optimized copy path using DMA-buf to avoid
 * the overhead of vdpVideoSurfaceGetBitsYCbCr which does:
 * - VA-API → CPU readback
 * - Pitch conversion/padding handling
 *
 * Instead, we:
 * 1. Export VA-API surface as DMA-buf FD (vaExportSurfaceHandle)
 * 2. Import DMA-buf into Vulkan as external memory (anv_device_import_bo)
 * 3. CPU copy from imported BO to destination image
 *
 * Falls back to CPU copy if any step fails (e.g., DMA-buf not supported).
 */
VkResult
anv_vdpau_copy_surface_to_image_dmabuf(struct anv_device *device,
                                       struct anv_vdpau_session *session,
                                       VdpVideoSurface surface,
                                       struct anv_image *image,
                                       struct anv_cmd_buffer *cmd_buffer)
{
   /* Check if DMA-buf export function is available */
   if (!session->vdp_video_surface_export_dmabuf) {
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* Export VDPAU surface as DMA-buf */
   int dmabuf_fd = -1;
   uint32_t width, height, fourcc, num_planes;
   uint32_t pitches[3], offsets[3];
   uint64_t modifier;

   VdpStatus vdp_status = session->vdp_video_surface_export_dmabuf(
      surface, &dmabuf_fd, &width, &height, &fourcc,
      &num_planes, pitches, offsets, &modifier);

   if (vdp_status != VDP_STATUS_OK || dmabuf_fd < 0) {
      if (dmabuf_fd >= 0)
         close(dmabuf_fd);

      /* Note: DMA-buf export failures can happen for various reasons
       * (e.g., VDP_STATUS_INVALID_HANDLE, VDP_STATUS_NO_IMPLEMENTATION).
       * These are NOT necessarily memory pressure issues, so we should
       * NOT aggressively evict surfaces here as it can destroy surfaces
       * that are still needed for decoding reference frames.
       *
       * Memory pressure handling is done at the BO import level below,
       * where we can detect VK_ERROR_OUT_OF_DEVICE_MEMORY specifically.
       */

      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* Validate dimensions match */
   if (width != image->vk.extent.width || height != image->vk.extent.height) {
      close(dmabuf_fd);
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* Validate format (NV12 = 0x3231564E in little endian) */
   if (fourcc != 0x3231564E) {
      close(dmabuf_fd);
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* Import the DMA-buf FD as a BO using ANV's device import function
    * Note: DRM_IOCTL_PRIME_FD_TO_HANDLE duplicates the FD internally but
    * does NOT take ownership. We must close dmabuf_fd ourselves.
    */
   struct anv_bo *imported_bo = NULL;
   VkResult result = anv_device_import_bo(device, dmabuf_fd,
                                          ANV_BO_ALLOC_EXTERNAL,
                                          0 /* client_address */,
                                          &imported_bo);

   /* Close the DMA-buf FD - the kernel has already duplicated it if needed */
   close(dmabuf_fd);

   if (result != VK_SUCCESS || !imported_bo) {
      /* BO import failure usually means GPU is out of memory or address space.
       * Evict old surfaces to free memory, but keep enough for reference frames.
       * For H.264, we need ~4-5 reference frames, so we keep cache_capacity - 2
       * to ensure we don't break decoding while still freeing some memory.
       */
      if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
         uint32_t keep_count = session->surface_map_capacity > 2 ?
                               session->surface_map_capacity - 2 : 3;
         anv_vdpau_evict_old_surfaces(session, keep_count);
      }

      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* CRITICAL FOR CACHE COHERENCY: Wait for GPU operations to complete.
    *
    * The imported DMA-buf was written by the VA-API video decoder (GPU render domain).
    *
    * We use DRM_IOCTL_I915_GEM_WAIT to:
    * 1. Wait for any pending GPU operations to complete
    * 2. Allow the kernel to handle cache coherency implicitly
    *
    * Without this wait, we see corruption in the top rows of the video frame because
    * the CPU may try to read before GPU writes complete.
    *
    * This is especially critical under heavy system load when cache pressure is high.
    */
   int64_t timeout_ns = INT64_MAX; /* Wait indefinitely */
   int ret = anv_gem_wait(device, imported_bo->gem_handle, &timeout_ns);
   if (ret != 0) {
      /* Continue anyway - better to have potential corruption than fail completely */
   }

   /* Now perform GPU copy from imported BO to destination image using genX_gpu_memcpy
    * We need to copy both Y and UV planes.
    *
    * For NV12 format:
    * - Plane 0 (Y): Full resolution, 1 byte per pixel
    * - Plane 1 (UV): Half resolution, 2 bytes per pixel (interleaved U and V)
    */
   uint32_t y_plane_idx = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_0_BIT);
   uint32_t uv_plane_idx = anv_image_aspect_to_plane(image, VK_IMAGE_ASPECT_PLANE_1_BIT);
   const struct anv_surface *y_surface = &image->planes[y_plane_idx].primary_surface;
   const struct anv_surface *uv_surface = &image->planes[uv_plane_idx].primary_surface;

   struct anv_image_binding *dst_binding = &image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];
   if (!dst_binding->address.bo) {
      anv_device_release_bo(device, imported_bo);
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* Calculate destination addresses in the target image */
   uint64_t y_offset = dst_binding->address.offset + y_surface->memory_range.offset;
   uint64_t uv_offset = dst_binding->address.offset + uv_surface->memory_range.offset;

   /* Fix off-by-one alignment if needed */
   if (device->info->verx10 == 70 || device->info->verx10 == 75) {
      uint32_t y_alignment = y_surface->isl.alignment_B;
      uint32_t uv_alignment = uv_surface->isl.alignment_B;

      if (y_offset % y_alignment != 0) {
         uint64_t misalignment = y_offset % y_alignment;
         if (misalignment == y_alignment - 1) {
            y_offset += 1;
         }
      }

      if (uv_offset % uv_alignment != 0) {
         uint64_t misalignment = uv_offset % uv_alignment;
         if (misalignment == uv_alignment - 1) {
            uv_offset += 1;
         }
      }
   }

   /* Determine source tiling from DRM modifier
    * VA-API usually exports surfaces as Y-tiled.
    */
   enum isl_tiling src_tiling = ISL_TILING_LINEAR;
   if (modifier == I915_FORMAT_MOD_Y_TILED || modifier == DRM_FORMAT_MOD_LINEAR) {
      /* Explicitly specified tiling */
      if (modifier == I915_FORMAT_MOD_Y_TILED) {
         src_tiling = ISL_TILING_Y0;
      }
   } else if (modifier != DRM_FORMAT_MOD_INVALID) {
      /* Unknown modifier - fall back to CPU copy for safety */
      anv_device_release_bo(device, imported_bo);
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   /* Copy data from imported DMA-buf BO to destination image
    * Handle both tiled and linear sources from VA-API.
    */
   enum isl_tiling dst_tiling = y_surface->isl.tiling;

   /* CACHE COHERENCY: Wait for GPU operations on destination BO to complete.
    * The destination image may have been used by GPU previously, so we need to
    * ensure it's idle before CPU write access. Modern kernels handle cache
    * coherency implicitly through the mapping and synchronization primitives.
    */
   timeout_ns = INT64_MAX; /* Wait indefinitely */
   ret = anv_gem_wait(device, dst_binding->address.bo->gem_handle, &timeout_ns);
   if (ret != 0) {
      /* Continue anyway */
   }

   /* Map both source (imported DMA-buf) and destination BOs for CPU access.
    *
    * For same-tiling direct copies (Y-tiled → Y-tiled), use GTT mapping (cached)
    * instead of WC for much better memcpy performance. When tiling matches and we're
    * just doing bulk memcpy, CPU cache helps significantly.
    *
    * For tiling conversions (tiled ↔ linear), use WC mapping as ISL functions
    * are optimized for WC and avoid cache pollution from temporary data.
    */
   bool use_gtt_for_copy = (src_tiling == dst_tiling &&
                            src_tiling != ISL_TILING_LINEAR &&
                            pitches[0] == y_surface->isl.row_pitch_B &&
                            pitches[1] == uv_surface->isl.row_pitch_B);

   void *src_ptr = anv_gem_mmap(device, imported_bo->gem_handle, 0, imported_bo->size,
                                use_gtt_for_copy ? 0 :
                                (src_tiling == ISL_TILING_LINEAR ? 0 : I915_MMAP_WC));
   void *dst_ptr = anv_gem_mmap(device, dst_binding->address.bo->gem_handle, 0,
                                dst_binding->address.bo->size,
                                use_gtt_for_copy ? 0 :
                                (dst_tiling == ISL_TILING_LINEAR ? 0 : I915_MMAP_WC));

   if (src_ptr == MAP_FAILED || src_ptr == NULL || dst_ptr == MAP_FAILED || dst_ptr == NULL) {
      if (src_ptr && src_ptr != MAP_FAILED)
         anv_gem_munmap(device, src_ptr, imported_bo->size);
      if (dst_ptr && dst_ptr != MAP_FAILED)
         anv_gem_munmap(device, dst_ptr, dst_binding->address.bo->size);
      anv_device_release_bo(device, imported_bo);
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   }

   char *src_y = (char *)src_ptr + offsets[0];
   char *src_uv = (char *)src_ptr + offsets[1];
   char *dst_y = (char *)dst_ptr + y_offset;
   char *dst_uv = (char *)dst_ptr + uv_offset;

   /* For cached (GTT) mapped memory, ensure CPU cache coherency.
    * For WC-mapped memory, ensure all previous writes are visible.
    * This is critical for imported DMA-buf memory that may have been written
    * by the GPU or video decoder.
    */
   if (!use_gtt_for_copy && src_tiling != ISL_TILING_LINEAR) {
      __builtin_ia32_mfence();
   }

   bool has_swizzling = device->isl_dev.has_bit6_swizzling;

   /* Handle different source/destination tiling combinations */
   if (src_tiling == dst_tiling && src_tiling != ISL_TILING_LINEAR) {
      /* Both tiled with same format.
       * If pitches match, we can use direct memcpy which is much faster.
       * Otherwise, fall back to tiled->linear->tiled for pitch conversion.
       */
      if (pitches[0] == y_surface->isl.row_pitch_B &&
          pitches[1] == uv_surface->isl.row_pitch_B) {
         /* FAST PATH: Pitches match - use direct tile-aligned memcpy
          * This avoids the expensive tiled->linear->tiled conversion
          * and is safe when pitch/tiling are identical.
          *
          * Using GTT (cached) mapping for much better memcpy performance
          * compared to WC (write-combine) mapping. CPU cache makes a huge
          * difference for bulk memcpy operations on tiled memory.
          *
          * IMPORTANT: For Y-tiled surfaces, we must copy the full pitch
          * worth of data per row because the tiling layout stores data in
          * a specific pattern (128x32 tiles). Copying only the width would
          * break the tile structure and cause corruption.
          */

         /* For Y-tiled surfaces, we must copy the FULL allocated surface size
          * (memory_range.size), not just pitch × height. ISL allocates surfaces
          * with tile alignment padding, so memory_range.size is larger than
          * pitch × height.
          *
          * Example for 4K (3840x2160):
          * - pitch × height = 3840 × 2160 = 8,294,400 bytes
          * - memory_range.size = 8,355,840 bytes (tile-aligned)
          * - Must copy full 8,355,840 bytes to avoid green artifacts
          *
          * For 1080p (1920x1088):
          * - pitch × height = 1920 × 1088 = 2,088,960 bytes
          * - memory_range.size = 2,088,960 bytes (already tile-aligned)
          * - Both match, so no issue
          */
         size_t y_copy_size = y_surface->memory_range.size;
         size_t uv_copy_size = uv_surface->memory_range.size;

         /* Direct bulk copy - fastest path for matching pitch/tiling */
         memcpy(dst_y, src_y, y_copy_size);
         memcpy(dst_uv, src_uv, uv_copy_size);
      } else {
         /* SLOW PATH: Pitch mismatch - use tiled->linear->tiled conversion
          * This handles cases where VA-API and Vulkan use different pitches
          */

         /* Allocate temporary linear buffers for Y and UV planes
          * Use tight packing (width as pitch) to avoid pitch mismatch issues
          */
         size_t y_linear_pitch = width;
         size_t uv_linear_pitch = width;
         size_t y_linear_size = height * y_linear_pitch;
         size_t uv_linear_size = (height / 2) * uv_linear_pitch;
         char *y_linear = malloc(y_linear_size);
         char *uv_linear = malloc(uv_linear_size);

         if (!y_linear || !uv_linear) {
            free(y_linear);
            free(uv_linear);
            anv_gem_munmap(device, src_ptr, imported_bo->size);
            anv_gem_munmap(device, dst_ptr, dst_binding->address.bo->size);
            anv_device_release_bo(device, imported_bo);
            return anv_vdpau_copy_surface_to_image(device, session, surface, image);
         }

         /* Y plane: tiled -> linear (using source pitch) */
         isl_memcpy_tiled_to_linear(0, width,
                                    0, height,
                                    y_linear,
                                    src_y,
                                    y_linear_pitch,
                                    pitches[0],
                                    has_swizzling,
                                    src_tiling,
                                    ISL_MEMCPY);

         /* Y plane: linear -> tiled (using dest pitch) */
         isl_memcpy_linear_to_tiled(0, width,
                                    0, height,
                                    dst_y,
                                    y_linear,
                                    y_surface->isl.row_pitch_B,
                                    y_linear_pitch,
                                    has_swizzling,
                                    dst_tiling,
                                    ISL_MEMCPY);

         /* UV plane: tiled -> linear (using source pitch) */
         isl_memcpy_tiled_to_linear(0, width,
                                    0, height / 2,
                                    uv_linear,
                                    src_uv,
                                    uv_linear_pitch,
                                    pitches[1],
                                    has_swizzling,
                                    src_tiling,
                                    ISL_MEMCPY);

         /* UV plane: linear -> tiled (using dest pitch) */
         isl_memcpy_linear_to_tiled(0, width,
                                    0, height / 2,
                                    dst_uv,
                                    uv_linear,
                                    uv_surface->isl.row_pitch_B,
                                    uv_linear_pitch,
                                    has_swizzling,
                                    dst_tiling,
                                    ISL_MEMCPY);

         free(y_linear);
         free(uv_linear);
      }
   } else if (src_tiling != ISL_TILING_LINEAR && dst_tiling != ISL_TILING_LINEAR) {
      /* Both tiled but different formats - need tiled-to-tiled conversion
       * This is rare, fall back to CPU copy for safety
       */
      anv_gem_munmap(device, src_ptr, imported_bo->size);
      anv_gem_munmap(device, dst_ptr, dst_binding->address.bo->size);
      anv_device_release_bo(device, imported_bo);
      return anv_vdpau_copy_surface_to_image(device, session, surface, image);
   } else if (src_tiling == ISL_TILING_LINEAR && dst_tiling != ISL_TILING_LINEAR) {
      /* Linear source, tiled dest - use linear_to_tiled */

      /* Copy Y plane using ISL tiled memcpy
       * For NV12, Y plane is width×height, 1 byte per pixel = width bytes per row
       */
      isl_memcpy_linear_to_tiled(0, width,
                                 0, height,
                                 dst_y,
                                 src_y,
                                 y_surface->isl.row_pitch_B,
                                 pitches[0],
                                 has_swizzling,
                                 dst_tiling,
                                 ISL_MEMCPY);

      /* Copy UV plane using ISL tiled memcpy
       * For NV12, UV plane is (width/2)×(height/2) in pixels, but 2 bytes per pixel
       * (U and V interleaved), so row width in bytes is: (width/2 pixels) × (2 bytes/pixel) = width bytes
       */
      isl_memcpy_linear_to_tiled(0, width,
                                 0, height / 2,
                                 dst_uv,
                                 src_uv,
                                 uv_surface->isl.row_pitch_B,
                                 pitches[1],
                                 has_swizzling,
                                 dst_tiling,
                                 ISL_MEMCPY);
   } else if (src_tiling != ISL_TILING_LINEAR && dst_tiling == ISL_TILING_LINEAR) {
      /* Tiled source, linear dest - use tiled_to_linear */

      /* Copy Y plane */
      isl_memcpy_tiled_to_linear(0, width,
                                 0, height,
                                 dst_y,
                                 src_y,
                                 y_surface->isl.row_pitch_B,
                                 pitches[0],
                                 has_swizzling,
                                 src_tiling,
                                 ISL_MEMCPY);

      /* Copy UV plane */
      isl_memcpy_tiled_to_linear(0, width,
                                 0, height / 2,
                                 dst_uv,
                                 src_uv,
                                 uv_surface->isl.row_pitch_B,
                                 pitches[1],
                                 has_swizzling,
                                 src_tiling,
                                 ISL_MEMCPY);
   } else {
      /* Both linear - use direct memcpy or row-by-row copy */
      if (pitches[0] == y_surface->isl.row_pitch_B && pitches[1] == uv_surface->isl.row_pitch_B) {
         /* Pitch matches - use bulk memcpy for maximum performance */

         memcpy(dst_y, src_y, height * pitches[0]);
         memcpy(dst_uv, src_uv, (height / 2) * pitches[1]);
      } else {
         /* Pitch mismatch - copy row by row
          * Use width as the copy width since both surfaces are linear and we're
          * copying the actual video data (not padding).
          */

         /* For NV12:
          * Y plane: width bytes per row (1 byte per pixel)
          * UV plane: width bytes per row (2 bytes per 2 pixels, interleaved)
          */
         uint32_t y_row_bytes = width;
         uint32_t uv_row_bytes = width;

         /* Copy Y plane row by row */
         for (uint32_t row = 0; row < height; row++) {
            memcpy(dst_y + row * y_surface->isl.row_pitch_B,
                   src_y + row * pitches[0],
                   y_row_bytes);
         }

         /* Copy UV plane row by row */
         for (uint32_t row = 0; row < height / 2; row++) {
            memcpy(dst_uv + row * uv_surface->isl.row_pitch_B,
                   src_uv + row * pitches[1],
                   uv_row_bytes);
         }
      }
   }

   /* Ensure all CPU writes are visible to GPU before unmapping.
    *
    * Memory ordering requirements:
    * - For GTT (cached) mappings: mfence + kernel handles cache flushing
    * - For WC-mapped (tiled) surfaces: mfence ensures WC buffer flush
    * - For cached (linear) surfaces: mfence ensures store ordering
    *
    * Modern kernels handle cache coherency implicitly on unmap, but we still
    * need mfence to ensure all CPU stores complete before releasing the mapping.
    */
   __builtin_ia32_mfence();

   /* Unmap BOs and release imported BO */
   anv_gem_munmap(device, src_ptr, imported_bo->size);
   anv_gem_munmap(device, dst_ptr, dst_binding->address.bo->size);
   anv_device_release_bo(device, imported_bo);



   return VK_SUCCESS;
}

/**
 * Copy VDPAU surface to Vulkan image
 *
 * After VDPAU decode completes, copy the decoded data from VDPAU surface
 * to the Vulkan image memory. VDPAU returns linear data, but the Vulkan
 * image may be tiled (Y-tiled usually), so we need to use ISL's
 * tiled memcpy functions for the conversion.
 *
 * This is the CPU copy path - slow but always works.
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
      /* Fall back to image dimensions */
      surface_width = width;
      surface_height = height;
   }

   /* Use the larger of surface size and image size for allocation
    * Also consider that libvdpau-va-gl may use the session's max size internally.
    */
   uint32_t alloc_width = MAX2(surface_width, width);
   uint32_t alloc_height = MAX2(surface_height, height);

   /* PERFORMANCE FIX: Use width directly as pitch to match VA-API's pitch.
    * VA-API typically uses pitch == width for NV12 surfaces, and we need to
    * match this exactly to enable the fast bulk copy path in vdpVideoSurfaceGetBitsYCbCr.
    *
    * Previously, we aligned pitch to 2048 bytes (e.g., 3840 -> 4096), which caused
    * pitch mismatch and forced slow row-by-row copy (3240 memcpy calls per 4K frame).
    *
    * Now we use the actual width, which matches VA-API pitch and enables fast bulk copy
    * (2 memcpy calls per frame).
    */
   uint32_t linear_y_pitch = alloc_width;
   uint32_t linear_uv_pitch = alloc_width;

   /* Use very generous height - could be padded to power of 2 or macroblock aligned */
   uint32_t aligned_height = MAX2(align(alloc_height, 64) + 64, 1024);
   size_t y_size = (size_t)linear_y_pitch * aligned_height;
   size_t uv_size = (size_t)linear_uv_pitch * (aligned_height / 2);

   /* Use page-aligned allocation for better compatibility with DMA operations
    * Note: Use ALIGN_POT macro which preserves type (works on 32-bit and 64-bit)
    */
   size_t y_alloc_size = ALIGN_POT(y_size, 4096);
   size_t uv_alloc_size = ALIGN_POT(uv_size, 4096);

   /* PERFORMANCE: Reuse cached linear buffers instead of allocating per frame.
    * For 4K video, these buffers are ~16MB each. Reusing them eliminates
    * malloc/free overhead (~200 μs per frame) and reduces memory fragmentation.
    * Buffers grow as needed but never shrink (acceptable for decode session lifetime).
    */
   void *linear_y = NULL;
   void *linear_uv = NULL;

   if (session->linear_y_buffer_size < y_alloc_size) {
      /* Need larger Y buffer - grow it */
      void *new_buffer = realloc(session->linear_y_buffer, y_alloc_size);
      if (!new_buffer) {
         /* realloc failed but old buffer is still valid */
         anv_gem_munmap(device, tiled_ptr, bo->size);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      session->linear_y_buffer = new_buffer;
      session->linear_y_buffer_size = y_alloc_size;
   }
   linear_y = session->linear_y_buffer;

   if (session->linear_uv_buffer_size < uv_alloc_size) {
      /* Need larger UV buffer - grow it */
      void *new_buffer = realloc(session->linear_uv_buffer, uv_alloc_size);
      if (!new_buffer) {
         /* realloc failed but old buffer is still valid */
         anv_gem_munmap(device, tiled_ptr, bo->size);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
      session->linear_uv_buffer = new_buffer;
      session->linear_uv_buffer_size = uv_alloc_size;
   }
   linear_uv = session->linear_uv_buffer;

   /* Get decoded data from VDPAU surface into linear buffers
    * NOTE: We don't need to zero the buffers beforehand since
    * vdp_video_surface_get_bits_ycbcr will fill them completely
    */
   void *linear_data[2] = { linear_y, linear_uv };
   uint32_t linear_pitches[2] = { linear_y_pitch, linear_uv_pitch };

   vdp_status = session->vdp_video_surface_get_bits_ycbcr(surface,
                                                          VDP_YCBCR_FORMAT_NV12,
                                                          linear_data,
                                                          linear_pitches);
   if (vdp_status != VDP_STATUS_OK) {
      /* Don't free buffers - they're cached in session */
      anv_gem_munmap(device, tiled_ptr, bo->size);
      return vk_error(device, VK_ERROR_UNKNOWN);
   }

   /* Get destination pointers in the tiled buffer */
   uint64_t y_offset = binding->address.offset + y_surface->memory_range.offset;
   uint64_t uv_offset = binding->address.offset + uv_surface->memory_range.offset;

    /* Fix off-by-one alignment if needed */
   if (device->info->verx10 == 70 || device->info->verx10 == 75) {
      uint32_t y_alignment = y_surface->isl.alignment_B;
      uint32_t uv_alignment = uv_surface->isl.alignment_B;

      if (y_offset % y_alignment != 0) {
         uint64_t misalignment = y_offset % y_alignment;
         if (misalignment == y_alignment - 1) {
            y_offset += 1;
         }
      }

      if (uv_offset % uv_alignment != 0) {
         uint64_t misalignment = uv_offset % uv_alignment;
         if (misalignment == uv_alignment - 1) {
            uv_offset += 1;
         }
      }
   }

   char *tiled_y = (char *)tiled_ptr + y_offset;
   char *tiled_uv = (char *)tiled_ptr + uv_offset;

   enum isl_tiling tiling = y_surface->isl.tiling;
   bool has_swizzling = device->isl_dev.has_bit6_swizzling;

   /* Bounds checking to prevent buffer overflow */
   size_t y_end_offset = y_offset + y_surface->memory_range.size;
   size_t uv_end_offset = uv_offset + uv_surface->memory_range.size;
   if (y_end_offset > bo->size || uv_end_offset > bo->size) {
      free(linear_y);
      free(linear_uv);
      anv_gem_munmap(device, tiled_ptr, bo->size);
      return vk_error(device, VK_ERROR_UNKNOWN);
   }

   if (tiling == ISL_TILING_LINEAR) {
      /* Linear tiling - just memcpy row by row */
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
                                 has_swizzling,  /* bit6 swizzling on Ivy Bridge */
                                 tiling,
                                 ISL_MEMCPY);

      /* Copy UV plane (2 bytes per pixel pair, half height) */
      isl_memcpy_linear_to_tiled(0, width,           /* x range in bytes (UV is interleaved) */
                                 0, height / 2,      /* y range in rows (half height for 4:2:0) */
                                 tiled_uv,           /* destination (tiled) */
                                 linear_uv,          /* source (linear) */
                                 uv_surface->isl.row_pitch_B,  /* dst pitch */
                                 linear_uv_pitch,              /* src pitch */
                                 has_swizzling,      /* bit6 swizzling on Ivy Bridge */
                                 tiling,
                                 ISL_MEMCPY);
   }

   /* Ensure all writes are visible to the GPU by flushing write-combine buffers */
   __builtin_ia32_mfence();

   /* Clean up - note: linear buffers are cached in session, not freed here */
   anv_gem_munmap(device, tiled_ptr, bo->size);

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

   if (!vid || !session) {
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Get H.264-specific picture info */
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   if (!h264_pic_info) {
      return vk_error(device, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   /* Get destination image view and extract image */
   ANV_FROM_HANDLE(anv_image_view, dst_image_view,
                   frame_info->dstPictureResource.imageViewBinding);
   if (!dst_image_view || !dst_image_view->image) {
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
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }

      session->decoder_created = true;
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
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   /* Map the bitstream buffer */
   void *bitstream_data = anv_gem_mmap(device, src_buffer->address.bo->gem_handle,
                                       0, frame_info->srcBufferRange, 0);
   if (!bitstream_data) {
      vk_free(&device->vk.alloc, ref_surfaces);
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   }

   /* Create bitstream buffer array for VDPAU
    * VDPAU takes an array of VdpBitstreamBuffer structures, one per slice
    */
   uint32_t slice_count = h264_pic_info->sliceCount;
   if (slice_count == 0) {
      anv_gem_munmap(device, bitstream_data, frame_info->srcBufferRange);
      vk_free(&device->vk.alloc, ref_surfaces);
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

   return VK_SUCCESS;
}

/**
 * Execute deferred VDPAU decode commands
 *
 * Called at QueueSubmit time to execute VDPAU decode operations.
 *
 * Performance optimization: Submit all decode operations first to allow
 * VA-API/GPU pipelining, then copy the results. This avoids serializing
 * decodes where decode N+1 can't start until decode N is fully copied.
 *
 * Since we bundle our own libvdpau, we can override the traditional
 * expectation that all queued frames must be processed, enabling
 * aggressive frame dropping.
 */
VkResult
anv_vdpau_execute_deferred_decodes(struct anv_device *device,
                                   struct anv_cmd_buffer *cmd_buffer)
{
   VdpStatus vdp_status;
   VkResult result = VK_SUCCESS;

   uint32_t total_decode_count = util_dynarray_num_elements(
      &cmd_buffer->video.vdpau_decodes, struct anv_vdpau_decode_cmd);

   if (total_decode_count == 0) {
      return VK_SUCCESS;
   }

   /* Determine how many frames to process based on queue size and limit.
    * When frames accumulate (e.g., 4K video on slow hardware with --video-sync=display-vdrop),
    * we drop older frames and only process the most recent ones to prevent freezing.
    *
    * Strategy:
    * - If queue <= limit or limit is 0 (unlimited): process all frames
    * - If queue > limit: skip old frames, process only the most recent 'limit' frames
    *
    * This prevents the "freeze then catch-up" behavior by maintaining real-time
    * playback at the cost of dropping frames that are already too old to display.
    */
   uint32_t frames_to_process;
   uint32_t skip_count = 0;  /* Number of old frames to drop */
   const uint32_t max_frames = HASVK_MAX_FRAMES_PER_SUBMIT;

   if (max_frames == 0 || total_decode_count <= max_frames) {
      /* Process all frames - queue is manageable or no limit set */
      frames_to_process = total_decode_count;
   } else {
      /* Queue overflow - drop oldest frames, keep newest ones */
      frames_to_process = max_frames;
      skip_count = total_decode_count - frames_to_process;
   }

   /* Acquire VDPAU mutex to serialize decode operations across all sessions.
    * This is necessary because:
    * 1. VDPAU decoder operations are not thread-safe
    * 2. libvdpau-va-gl has internal state that can race
    * 3. VA-API (used by libvdpau-va-gl) serializes operations internally anyway
    */
   if (INTEL_DEBUG(DEBUG_PERF))
      fprintf(stderr, "anv_vdpau_execute_deferred_decodes: Acquiring vdpau_mutex (frames=%u)\n", frames_to_process);
   pthread_mutex_lock(&device->vdpau_mutex);
   if (INTEL_DEBUG(DEBUG_PERF))
      fprintf(stderr, "anv_vdpau_execute_deferred_decodes: Acquired vdpau_mutex\n");

   /* Phase 1: Submit decode operations to VA-API
    * This allows VA-API to pipeline multiple decodes on the GPU instead of
    * serializing them. Each vdp_decoder_render submits work to VA-API but
    * doesn't wait for completion.
    *
    * When dropping frames, we skip the oldest frames and only process the
    * most recent ones to maintain real-time playback.
    */
   uint32_t frame_index = 0;
   util_dynarray_foreach(&cmd_buffer->video.vdpau_decodes,
                         struct anv_vdpau_decode_cmd, decode_cmd)
   {
      /* Skip old frames if we're dropping */
      if (frame_index < skip_count) {
         frame_index++;
         continue;
      }

      /* Stop after processing the desired number of frames */
      if (frame_index >= skip_count + frames_to_process) {
         break;
      }

      vdp_status = decode_cmd->session->vdp_decoder_render(
         decode_cmd->decoder,
         decode_cmd->target_surface,
         (VdpPictureInfo *)&decode_cmd->pic_info,
         decode_cmd->bitstream_buffer_count,
         decode_cmd->bitstream_buffers);

      if (vdp_status != VDP_STATUS_OK) {
         result = vk_error(device, VK_ERROR_UNKNOWN);
         break;  /* Exit loop on error, will unlock mutex below */
      }

      frame_index++;
   }

   /* Release VDPAU mutex after decode submission (always, success or failure).
    * Phase 1 (decode submission) needs serialization to prevent VDPAU races.
    * Phase 2 (copy) can proceed in parallel across threads since each thread
    * operates on different surfaces/images. This is critical for 4K video
    * performance where the copy operation is expensive.
    */
   if (INTEL_DEBUG(DEBUG_PERF))
      fprintf(stderr, "anv_vdpau_execute_deferred_decodes: Releasing vdpau_mutex\n");
   pthread_mutex_unlock(&device->vdpau_mutex);
   if (INTEL_DEBUG(DEBUG_PERF))
      fprintf(stderr, "anv_vdpau_execute_deferred_decodes: Released vdpau_mutex, starting copy phase\n");

   /* Early exit if decode submission failed */
   if (result != VK_SUCCESS) {
      if (INTEL_DEBUG(DEBUG_PERF))
         fprintf(stderr, "anv_vdpau_execute_deferred_decodes: Early exit due to decode error\n");
      goto cleanup;
   }

   /* Phase 2: Copy decoded results from VDPAU surfaces to Vulkan images
    * The vdp_video_surface_get_bits_ycbcr call will sync and wait for
    * GPU decode completion, but at this point all decodes have been
    * submitted and can execute in parallel on the GPU.
    *
    * Try DMA-buf zero-copy path first, fall back to CPU copy if unavailable.
    * Only copy frames that were actually decoded (skip old dropped frames).
    */
   frame_index = 0;
   util_dynarray_foreach(&cmd_buffer->video.vdpau_decodes,
                         struct anv_vdpau_decode_cmd, decode_cmd)
   {
      /* Skip old frames that were not decoded */
      if (frame_index < skip_count) {
         frame_index++;
         continue;
      }

      /* Stop after processing the desired number of frames */
      if (frame_index >= skip_count + frames_to_process) {
         break;
      }

      /* Find the Vulkan image for this surface */
      struct anv_image *target_image = NULL;
      for (uint32_t i = 0; i < decode_cmd->session->surface_map_size; i++) {
         if (decode_cmd->session->surface_map[i].vdp_surface == decode_cmd->target_surface) {
            target_image = (struct anv_image *)decode_cmd->session->surface_map[i].image;
            break;
         }
      }

      if (target_image) {
         /* Try DMA-buf path first (zero-copy GPU-to-GPU)
          * Falls back to CPU copy automatically if DMA-buf not available
          */
         VkResult copy_result = anv_vdpau_copy_surface_to_image_dmabuf(
            device, decode_cmd->session, decode_cmd->target_surface,
            target_image, cmd_buffer);
         if (copy_result != VK_SUCCESS && result == VK_SUCCESS) {
            result = copy_result;
         }
      }

      frame_index++;
   }

cleanup:
   /* Phase 3: Clean up resources for ALL frames (even those not decoded)
    * Frames beyond the limit are dropped to prevent resource leaks
    */
   util_dynarray_foreach(&cmd_buffer->video.vdpau_decodes,
                         struct anv_vdpau_decode_cmd, decode_cmd)
   {
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
   }

   /* Clear all commands from the queue */
   util_dynarray_clear(&cmd_buffer->video.vdpau_decodes);

   /* Add cache invalidation for coherency after decode and copy operations.
    *
    * We need both texture cache invalidate and data cache flush because:
    * 1. TEXTURE_CACHE_INVALIDATE: Ensures GPU sampler sees fresh data when
    *    reading the decoded video frames as textures
    * 2. DATA_CACHE_FLUSH: Ensures any CPU writes during the copy are visible
    *    to GPU before it starts using the surfaces
    *
    * This is done once per batch (not per frame) to reduce overhead while
    * still ensuring proper cache coherency. Under heavy system load, cache
    * pressure is high and these flushes are critical to prevent flickering.
    *
    * We always reach here with total_decode_count > 0 due to early return above.
    */
   anv_add_pending_pipe_bits(cmd_buffer,
                             ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT |
                             ANV_PIPE_DATA_CACHE_FLUSH_BIT,
                             "VDPAU decode batch cache coherency");

   if (INTEL_DEBUG(DEBUG_PERF))
      fprintf(stderr, "anv_vdpau_execute_deferred_decodes: Completed successfully\n");

   return result;
}
