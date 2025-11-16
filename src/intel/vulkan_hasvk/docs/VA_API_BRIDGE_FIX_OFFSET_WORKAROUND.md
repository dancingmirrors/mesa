# VA-API Bridge Fix: Plane Offset Workaround for intel-vaapi-driver Bug

**Date:** November 2024  
**Issue:** Incorrect y_cb_offset calculation in intel-vaapi-driver  
**Status:** FIXED (workaround implemented)

## Problem Description

The intel-vaapi-driver (i965 VA-API driver) has a bug where it incorrectly calculates the Y-to-Cb plane offset (`y_cb_offset`) from absolute buffer addresses instead of relative to the Y plane. This breaks video decoding compatibility when surfaces have non-zero binding offsets.

### The Bug in intel-vaapi-driver

When importing a surface via DMA-buf with external buffers, the VA-API driver receives an `offsets` array specifying where each plane (Y, UV) is located in the buffer:
- `offsets[0]` = offset of Y plane in the DMA-buf
- `offsets[1]` = offset of UV plane in the DMA-buf

The driver then needs to calculate `y_cb_offset`, which is the **row offset** from the Y plane to the Cb (U) plane. This should be:

```c
// CORRECT calculation
y_cb_offset = (offsets[1] - offsets[0]) / width;
```

However, the intel-vaapi-driver does:

```c
// BUGGY calculation
y_cb_offset = offsets[1] / width;
```

This only works when `offsets[0] == 0`. If the Y plane doesn't start at offset 0 of the DMA-buf, the calculation is wrong.

### Example

Consider a 1920x1088 NV12 image with:
- Binding offset within BO: 0x1000 (4096 bytes)
- Y plane at binding offset 0 → absolute BO offset = 0x1000
- UV plane at binding offset 0x1FE000 → absolute BO offset = 0x1000 + 0x1FE000 = 0x1FF000
- Row pitch: 512 bytes (example value from problem statement, though realistic pitch would be 1920)

If we pass absolute offsets:
- `offsets[0] = 0x1000` (4096)
- `offsets[1] = 0x1FF000` (2093056)

Then the driver calculates:
- **With bug**: `y_cb_offset = 0x1FF000 / 512 = 16368 rows` ❌ WRONG
- **Correct**: `y_cb_offset = (0x1FF000 - 0x1000) / 512 = 0x1FE000 / 512 = 16352 rows` ✓

## The Workaround

Rather than fixing the intel-vaapi-driver (which would require changes to a separate project), we work around the bug in hasvk by passing **relative offsets** instead of absolute offsets:

```c
extbuf.offsets[0] = 0;  // Y plane at offset 0 (relative to Y plane start)
extbuf.offsets[1] = uv_surface->memory_range.offset - y_surface->memory_range.offset;  // UV relative to Y
```

### Critical Assumption

This workaround **only works** if the Y plane actually starts at offset 0 of the DMA-buf (which represents the entire BO). This requires:

1. `binding->address.offset == 0` (image bound at start of device memory)
2. `y_surface->memory_range.offset == 0` (Y plane at start of binding)

For video images:
- **Non-disjoint images**: `y_surface->memory_range.offset` is always 0 (plane 0 is first) ✓
- **Dedicated allocations**: `binding->address.offset` is typically 0 ✓

The code now includes a validation check that warns if these assumptions are violated.

## Implementation

**File:** `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c`  
**Function:** `anv_vaapi_import_surface_from_image()`

### Changes Made

```c
// Calculate absolute Y plane offset for validation
uint64_t y_plane_abs_offset = binding->address.offset + y_surface->memory_range.offset;

// Warn if Y plane is not at BO offset 0
if (y_plane_abs_offset != 0) {
   fprintf(stderr, "WARNING: Video image Y plane not at BO offset 0\n");
   fprintf(stderr, "This may cause incorrect decoding due to intel-vaapi-driver offset bug.\n");
   fprintf(stderr, "Use dedicated memory allocation with offset=0 for video images.\n");
}

// Set offsets relative to Y plane (workaround for driver bug)
extbuf.offsets[0] = 0;
extbuf.offsets[1] = uv_surface->memory_range.offset - y_surface->memory_range.offset;
```

### What Changed from Previous Implementation

The previous implementation (documented in `VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md`) passed absolute BO offsets:

```c
// PREVIOUS: Absolute offsets
extbuf.offsets[0] = binding->address.offset + y_surface->memory_range.offset;
extbuf.offsets[1] = binding->address.offset + uv_surface->memory_range.offset;
```

This was correct for the DMA-buf sharing model (offsets are relative to the start of the exported BO), but incompatible with the intel-vaapi-driver bug.

The new implementation passes relative offsets to work around the driver bug, with the assumption that the Y plane is at BO offset 0.

## Impact and Limitations

### When It Works

The workaround works correctly when:
- Video images use dedicated memory allocations (recommended best practice)
- Images are bound at memory offset 0
- Images are non-disjoint (planes in the same allocation)

This covers the vast majority of real-world video decode use cases.

### When It Might Not Work

The workaround may fail if:
- Video image is bound at a non-zero offset within device memory
- Custom memory allocation strategies that don't use dedicated allocations
- Disjoint multi-planar images (rare for video decode)

In these cases, the validation warning will alert the user, and decoding may produce incorrect results.

### Recommendation

Applications using HasVK video decode should:
1. Use `VkMemoryDedicatedAllocateInfo` for video images
2. Bind images at offset 0 (`memoryOffset = 0` in `VkBindImageMemoryInfo`)
3. Use non-disjoint image format (default behavior)

## Testing

To verify the fix works correctly:

```bash
# Enable debugging
export INTEL_DEBUG=hasvk
export LIBVA_MESSAGING_LEVEL=2

# Test video decode
mpv --hwdec=vulkan test_video.mp4
```

Expected output:
```
VA-API surface import: 1920x1088 NV12
  Binding offset: 0
  Y plane:  pitch=1920 offset=0 (relative to Y plane start)
  UV plane: pitch=1920 offset=2088960 (relative to Y plane start)
```

No warning should appear if the Y plane is at BO offset 0.

## Relationship to Other Fixes

This fix is related to but distinct from the fixes documented in `VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md`:

| Fix | Issue | Solution |
|-----|-------|----------|
| **Previous** | Offsets were relative to binding, not BO | Added binding offset to plane offsets |
| **Current** | intel-vaapi-driver y_cb_offset bug | Made offsets relative to Y plane |
| **Data Size** | `data_size` field was 0 | Calculate as UV offset + UV size |
| **Cache Coherency** | Video/render engine cache mismatch | Two-step GEM domain transition |

All fixes work together to ensure correct VA-API video decode operation.

## Alternative Solutions Considered

1. **Fix intel-vaapi-driver**: The ideal solution, but requires changes to an external project and waiting for distribution updates.

2. **Export DMA-buf from specific offset**: Not possible - GEM handle export always exports the entire BO.

3. **Force Y plane to BO offset 0**: Would require changes to image allocation code; current workaround is simpler.

4. **Detect driver version and use different offsets**: Too fragile; workaround with validation is more reliable.

## References

- intel/intel-vaapi-driver issue: y_cb_offset calculation bug
- Mesa HasVK VA-API bridge architecture: `VA_API_BRIDGE_ARCHITECTURE.md`
- DMA-buf offset fixes: `VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md`
- Vulkan Video specification: VK_KHR_video_decode_queue
