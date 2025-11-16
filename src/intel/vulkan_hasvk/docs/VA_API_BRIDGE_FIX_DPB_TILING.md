# VA-API Bridge Critical Fix: DPB Reference Frame Tiling

**Date:** November 2024  
**Issue:** Green screen / old framebuffer content instead of decoded video  
**Status:** FIXED

## Problem Description

The VA-API bridge was displaying green screens or residual framebuffer content instead of properly decoded H.264 video, despite no suspicious errors in logs. This occurred when using applications like `mpv --hwdec=vulkan`.

## Root Cause

Reference frames (Decoded Picture Buffer / DPB) were not having their tiling mode set on the GEM BO when allocated. The issue manifested as follows:

1. **Destination frames** (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) had tiling set correctly
2. **Reference frames** (VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR) did NOT have tiling set
3. When VA-API imported reference frames via DMA-buf, it queried the kernel for tiling
4. Without tiling set, kernel returned **LINEAR** instead of **Y-TILED**
5. VA-API misinterpreted Y-tiled surface data as linear layout
6. Motion compensation used corrupted reference frame data
7. Result: Green blocks, artifacts, or old framebuffer content

## The Fix

**File:** `src/intel/vulkan_hasvk/anv_device.c`  
**Function:** `anv_AllocateMemory()`  
**Lines:** 3349-3351

### Before (BROKEN)
```c
if (image->vk.wsi_legacy_scanout ||
    (image->vk.usage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR)) {
```

### After (FIXED)
```c
if (image->vk.wsi_legacy_scanout ||
    (image->vk.usage & (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                        VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR))) {
```

## Why This Was Needed

The VA-API bridge architecture relies on **DMA-buf sharing** between Vulkan (hasvk) and VA-API (crocus/i965):

1. Vulkan allocates video surfaces with Y-tiling via ISL
2. Surfaces are exported as DMA-buf file descriptors
3. VA-API imports these DMA-bufs as VASurfaces
4. For **legacy (non-modifier) imports**, tiling must be queryable from kernel
5. `anv_device_set_bo_tiling()` tells kernel about BO tiling via `DRM_IOCTL_I915_GEM_SET_TILING`
6. VA-API uses `DRM_IOCTL_I915_GEM_GET_TILING` to query tiling when importing

Without step 5 for DPB images, step 6 fails, and VA-API assumes linear tiling.

## Comparison with Old Native Implementation

The old native implementation in `docs/genX_video_short.c` used `anv_mocs()` extensively. Investigation revealed:

| Aspect | Native Implementation | VA-API Bridge | Verdict |
|--------|----------------------|---------------|---------|
| **MOCS** | Set explicitly with `anv_mocs()` | Delegated to VA-API driver | ✅ Correct |
| **Stream-Out** | Relied on zero-init (disabled) | Delegated to VA-API driver | ✅ Correct (encoder-only anyway) |
| **DPB State** | Explicit MFD_AVC_DPB_STATE | Delegated to VA-API driver | ✅ Correct |
| **Slice Handling** | Explicit MFD commands | Delegated to VA-API driver | ✅ Correct |
| **Surface Tiling** | ISL direct, no export | **MISSING for DPB** | ❌ **BUG - NOW FIXED** |
| **Synchronization** | Implicit batch order | GEM domain transitions | ✅ Correct |

## Testing Recommendations

After this fix, test with:

```bash
# Enable debugging
export INTEL_DEBUG=hasvk
export LIBVA_MESSAGING_LEVEL=2

# Test with mpv
mpv --hwdec=vulkan video.mp4

# Test with ffmpeg
ffmpeg -hwaccel vulkan -i input.mp4 -f null -
```

Expected results:
- Video decodes correctly (no green screens)
- Reference frames properly used for motion compensation
- P-frames and B-frames decode correctly (they depend on reference frames)
- No tiling-related warnings in VA-API logs

## Technical Details

### Y-Tiling on Gen7/7.5/8

Video surfaces on Intel Gen7/7.5/8 hardware MUST use Y-tiling per the PRMs:
- More efficient for video decode engine access patterns
- Required by MFD (Media Fixed Decode) hardware
- Different layout than linear or X-tiling

### DMA-buf Tiling Communication

For legacy (non-modifier) DMA-buf imports:
1. Exporter sets tiling: `ioctl(fd, DRM_IOCTL_I915_GEM_SET_TILING, &args)`
2. Importer gets tiling: `ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING, &args)`

For modern modifier-based imports, tiling is in DRM format modifiers instead.
VA-API on Gen7-8 uses legacy path, hence this fix was critical.

### Why Only DST Was Set Initially

The original code assumed only destination (output) frames needed tiling set for display/WSI.
It didn't account for reference frames also being shared with VA-API for decode.

## Lessons Learned

1. **Cross-API resource sharing requires complete metadata**
   - Not just the buffer itself, but all layout information
   
2. **Reference frames are first-class surfaces**
   - They undergo the same import/export as destination frames
   
3. **Legacy paths need explicit setup**
   - Modern modifiers would make this automatic
   - But Gen7-8 VA-API uses legacy DMA-buf import
   
4. **Silent failures are the worst**
   - No errors logged, just green screens
   - Kernel silently returned linear for unknown tiling

## Related Files

- `src/intel/vulkan_hasvk/anv_device.c` - The fix
- `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c` - DMA-buf export/import
- `src/intel/vulkan_hasvk/anv_allocator.c` - `anv_device_set_bo_tiling()` implementation
- `docs/genX_video_short.c` - Old native implementation for comparison
- `docs/VA_API_BRIDGE_ARCHITECTURE.md` - Overall bridge architecture
- `docs/VA_API_BRIDGE_STATUS.md` - Implementation status
- `docs/VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md` - Related fix for plane offset calculation

## See Also

**Other critical fixes for VA-API bridge:**
- [VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md](VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md) - Fix for DMA-buf plane offset calculation (must account for binding offset)

Both this fix and the DMA-buf offset fix are required for correct video decode operation.

## References

- Intel HD Graphics Programmer's Reference Manual (PRM) - Gen7/7.5/8
- DRM/i915 kernel documentation - GEM tiling ioctls
- VA-API specification - Surface attributes and DMA-buf import
- Vulkan Video specification - VK_KHR_video_decode_queue
