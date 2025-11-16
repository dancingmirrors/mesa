# VA-API Bridge Critical Fix: DMA-buf Plane Offset Calculation

**Date:** November 2024  
**Issue:** Green screen / old framebuffer content due to incorrect plane offsets  
**Status:** FIXED

## Problem Description

The VA-API bridge was displaying green screens or residual framebuffer content when decoding H.264 video via DMA-buf sharing. The issue occurred because plane offsets passed to VA-API were calculated incorrectly.

## Root Cause

When importing Vulkan video surfaces into VA-API via DMA-buf, the plane offsets must be relative to the start of the buffer object (BO), not relative to the start of the image binding.

The issue manifested as follows:

1. **Vulkan image creation**: Multi-planar NV12 images are created with Y and UV planes
2. **Memory layout**: Each plane has a `memory_range.offset` relative to its binding
3. **BO export**: The entire BO is exported via DMA-buf (not just the binding)
4. **Offset mismatch**: Code was using plane offsets relative to binding, not BO
5. **VA-API confusion**: VA-API interpreted offsets incorrectly, reading wrong memory locations
6. **Result**: Decoded data written to correct location, but VA-API reading from wrong location

### Example Scenario

Consider a video image with:
- Binding starts at offset 4096 within the BO
- Y plane at binding offset 0 → actual BO offset = 4096
- UV plane at binding offset 2088960 → actual BO offset = 4096 + 2088960 = 2093056

**Before fix:**
```c
extbuf.offsets[0] = 0;                              // Wrong! Should be 4096
extbuf.offsets[1] = uv_surface->memory_range.offset; // Wrong! Should be 2093056
```

**After fix:**
```c
extbuf.offsets[0] = binding->address.offset + y_surface->memory_range.offset;
extbuf.offsets[1] = binding->address.offset + uv_surface->memory_range.offset;
```

## The Fix

**File:** `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c`  
**Function:** `anv_vaapi_import_surface_from_image()`  
**Lines:** 838-839

### Before (BROKEN)
```c
extbuf.offsets[0] = 0;       /* Y plane starts at beginning */
extbuf.offsets[1] = uv_surface->memory_range.offset; /* UV plane from ISL */
```

### After (FIXED)
```c
extbuf.offsets[0] = binding->address.offset + y_surface->memory_range.offset;
extbuf.offsets[1] = binding->address.offset + uv_surface->memory_range.offset;
```

## Why This Was Needed

The DMA-buf sharing architecture has a subtle but critical detail:

1. **Vulkan side**: Images have memory bindings that can start at offsets within the BO
2. **Export**: When we export via DMA-buf, we export the entire BO (not just the binding portion)
3. **Import**: VA-API needs offsets relative to the BO start, not the binding start
4. **Memory layout**: `binding->address.offset` is the binding's offset within the BO
5. **Plane layout**: `surface->memory_range.offset` is the plane's offset within the binding

Therefore: **BO offset = binding offset + plane offset**

### When Binding Offset is Zero

For most video images created with dedicated allocations:
- `binding->address.offset` = 0 (binding starts at BO start)
- The bug was masked because adding 0 doesn't change the result
- However, this is not guaranteed for all allocation patterns

### When Binding Offset is Non-Zero

For video images using:
- Suballocations from larger memory blocks
- Shared memory with other resources
- Specific memory placement requirements

The binding can start at a non-zero offset within the BO, and the bug would cause visible corruption.

## Comparison with BO Export

Looking at `anv_vaapi_export_video_surface_dmabuf()`:

```c
struct anv_image_binding *binding = &image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN];
struct anv_bo *bo = binding->address.bo;
int fd = anv_gem_handle_to_fd(device, bo->gem_handle);
```

We export the entire `bo`, not just the portion starting at `binding->address.offset`. Therefore, when importing, we must account for this offset.

## Related Fixes

This fix is related to but distinct from the DPB tiling fix documented in `VA_API_BRIDGE_FIX_DPB_TILING.md`:

| Fix | Issue | Solution |
|-----|-------|----------|
| **DPB Tiling** | Reference frames not having tiling set | Set tiling for both DST and DPB images |
| **DMA-buf Offsets** (this fix) | Plane offsets calculated incorrectly | Add binding offset to plane offsets |

Both fixes are necessary for correct VA-API decode operation.

## Debugging the Issue

To detect this issue:

1. Enable debugging:
   ```bash
   export INTEL_DEBUG=perf
   export LIBVA_MESSAGING_LEVEL=2
   ```

2. Look for offset values in logs:
   ```
   Binding offset: 4096
   Y plane:  pitch=1920 offset=4096 (binding_offset=4096 + plane_offset=0)
   UV plane: pitch=1920 offset=2093056 (binding_offset=4096 + plane_offset=2088960)
   ```

3. If binding offset is non-zero and not reflected in final offsets, this bug is present

## Testing Recommendations

After this fix, test with:

```bash
# Enable debugging
export INTEL_DEBUG=perf
export LIBVA_MESSAGING_LEVEL=2

# Test with mpv
mpv --hwdec=vulkan video.mp4

# Test with ffmpeg
ffmpeg -hwaccel vulkan -i input.mp4 -f null -
```

Expected results:
- Video decodes correctly (no green screens)
- Plane offsets in logs show binding offset correctly added
- Both I-frames (intra) and P/B-frames (inter) decode correctly

## Technical Details

### Memory Layout for Non-Disjoint NV12 Images

```
BO Memory Layout:
├─ [0 ... binding_offset-1]:        Other data or padding
├─ [binding_offset]:                 Binding start (image data begins here)
│  ├─ [0]:                           Y plane start (relative to binding)
│  │  └─ [0 ... Y_size-1]:          Y plane data
│  └─ [Y_size_aligned]:              UV plane start (relative to binding)
│     └─ [0 ... UV_size-1]:          UV plane data
└─ [...]:                            End of BO
```

### Offset Calculations

For DMA-buf import, VA-API needs:
- **Y plane BO offset**: `binding_offset + 0`
- **UV plane BO offset**: `binding_offset + Y_size_aligned`

Where `Y_size_aligned` is the ISL-calculated offset that includes:
- Y plane data size
- Alignment padding for Y-tiling (tile boundaries)
- Any other ISL-imposed alignment requirements

### Why ISL Offsets Are Needed

Simply calculating `height * pitch` is insufficient because:
1. **Y-tiling alignment**: Tiles are 128 bytes × 32 rows, requiring alignment
2. **ISL padding**: ISL may add extra padding for hardware requirements
3. **Multi-level images**: For mipmapped surfaces, ISL handles level offsets
4. **Array layers**: For array images, ISL handles layer offsets

Therefore, we must use `surface->memory_range.offset` from ISL, then add the binding offset.

## Lessons Learned

1. **Understand coordinate systems**
   - BO-relative offsets for DMA-buf export/import
   - Binding-relative offsets for internal Vulkan usage
   - Plane-relative offsets for sub-regions

2. **Export/import symmetry**
   - Export exports the entire BO
   - Import offsets must be relative to the entire BO
   - Can't assume binding starts at BO offset 0

3. **Test with non-standard allocations**
   - Dedicated allocations often mask bugs
   - Suballocations reveal offset issues
   - Shared memory exposes coordination problems

4. **Trust ISL layout calculations**
   - Don't recalculate offsets manually
   - ISL knows about hardware quirks and alignment
   - Always use `memory_range.offset` from ISL

## Related Files

- `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c` - The fix (import function)
- `src/intel/vulkan_hasvk/anv_image.c` - Image creation and layout
- `src/intel/vulkan_hasvk/anv_device.c` - Memory allocation
- `src/intel/isl/isl.c` - Surface layout calculations
- `docs/VA_API_BRIDGE_FIX_DPB_TILING.md` - Related tiling fix
- `docs/VA_API_BRIDGE_ARCHITECTURE.md` - Overall bridge architecture

## References

- VA-API specification - VASurfaceAttribExternalBuffers structure
- DRM/i915 kernel documentation - DMA-buf prime import/export
- Intel HD Graphics PRM - Surface layout and tiling
- Vulkan specification - VkBindImageMemory and memory bindings
