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

#ifndef ANV_VIDEO_CROSS_DEVICE_H
#define ANV_VIDEO_CROSS_DEVICE_H

#include "anv_private.h"

/**
 * Cross-Device Resource Sharing for HasVK Video
 *
 * This module enables video surfaces decoded by one Vulkan device to be
 * efficiently shared with another Vulkan device for rendering or processing.
 *
 * Common Use Case:
 * - FFmpeg creates Vulkan Device A for video decode (using HasVK VA-API bridge)
 * - libplacebo creates Vulkan Device B for rendering
 * - Decoded frames need to be shared from A to B
 *
 * Solution:
 * - Export video surfaces from Device A as DMA-buf file descriptors
 * - Import these file descriptors into Device B as external memory
 * - Use explicit synchronization (semaphores) to coordinate access
 *
 * Supported Handle Types:
 * - VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT (preferred)
 * - VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT (fallback)
 */

/**
 * Get external memory handle types supported for video surfaces
 *
 * @param device  ANV device
 * @return Supported external memory handle type flags
 */
VkExternalMemoryHandleTypeFlags
anv_video_get_supported_external_handle_types(struct anv_device *device);

/**
 * Check if an image supports cross-device sharing
 *
 * @param image  Image to check
 * @return true if the image can be shared across devices
 */
bool
anv_video_image_supports_cross_device(struct anv_image *image);

/**
 * Export video surface for cross-device sharing
 *
 * Exports a video surface as a file descriptor that can be imported
 * by another Vulkan device.
 *
 * @param device       ANV device
 * @param image        Video surface to export
 * @param handle_type  Type of handle to export
 * @param fd_out       Output file descriptor
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_video_export_surface_for_cross_device(struct anv_device *device,
                                           struct anv_image *image,
                                           VkExternalMemoryHandleTypeFlagBits handle_type,
                                           int *fd_out);

/**
 * Get external memory properties for video surfaces
 *
 * @param device                     ANV device
 * @param format                     Surface format
 * @param type                       Image type
 * @param tiling                     Image tiling
 * @param usage                      Image usage flags
 * @param external_handle_type       External memory handle type
 * @param pExternalMemoryProperties  Output properties
 */
void
anv_video_get_external_memory_properties(
   struct anv_device *device,
   VkFormat format,
   VkImageType type,
   VkImageTiling tiling,
   VkImageUsageFlags usage,
   VkExternalMemoryHandleTypeFlagBits external_handle_type,
   VkExternalMemoryProperties *pExternalMemoryProperties);

/**
 * Create a sync object for cross-device synchronization
 *
 * @param device      ANV device
 * @param handle_type Type of sync object
 * @param fd_out      Output file descriptor
 * @return VK_SUCCESS on success, error code otherwise
 */
VkResult
anv_video_create_cross_device_sync(struct anv_device *device,
                                    VkExternalSemaphoreHandleTypeFlagBits handle_type,
                                    int *fd_out);

/**
 * Get format modifiers for video surfaces
 *
 * @param device         ANV device
 * @param format         Surface format
 * @param modifiers      Output array of modifiers
 * @param modifier_count Input: array size, Output: number of modifiers
 */
void
anv_video_get_format_modifiers(struct anv_device *device,
                                VkFormat format,
                                uint64_t *modifiers,
                                uint32_t *modifier_count);

#endif /* ANV_VIDEO_CROSS_DEVICE_H */
