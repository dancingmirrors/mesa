# VA-API Bridge Fix: Y Plane Offset Requirement for intel-vaapi-driver

**Date:** November 2024  
**Issue:** intel-vaapi-driver assumes Y plane at BO offset 0 for NV12  
**Status:** FIXED (validation check added)

## Problem Description

The intel-vaapi-driver (i965 VA-API driver) for NV12 format has a limitation where it ignores the `offsets[]` array in `VASurfaceAttribExternalBuffers` and instead hardcodes the assumption that:
- Y plane starts at buffer object (BO) offset 0
- UV plane starts at offset `height * pitch` from BO start

This breaks video decoding when surfaces have non-zero binding offsets.

### The Driver Behavior for NV12

When importing a surface via DMA-buf with external buffers for NV12 format, the VA-API driver:

1. Receives the `offsets[]` array: `offsets[0]` for Y plane, `offsets[1]` for UV plane
2. **Ignores these offsets** for NV12
3. Hardcodes: `obj_surface->y_cb_offset = obj_surface->height;`
4. Assumes UV plane is at `height * pitch` bytes from the **start of the BO**

From `intel-vaapi-driver/src/i965_drv_video.c`:
```c
case VA_FOURCC_NV12:
case VA_FOURCC_P010:
    // ...
    obj_surface->y_cb_offset = obj_surface->height;  // HARDCODED, ignores offsets[1]!
    obj_surface->y_cr_offset = obj_surface->height;
    // ...
```

### When It Breaks

If the Y plane doesn't start at BO offset 0 (e.g., due to non-zero binding offset or suballocation), the driver will:
- Read Y plane data from the wrong location
- Read UV plane data from the wrong location
- Produce corrupted/garbage video output

### Example

Consider a video image with:
- Binding offset within BO: 0x1000 (4096 bytes)
- Y plane at binding offset 0 → **absolute BO offset = 0x1000** ❌
- UV plane follows Y → absolute BO offset = 0x1000 + (height * pitch)

The driver will:
- Look for Y at BO offset 0 (wrong - actual is at 0x1000)
- Look for UV at BO offset (height * pitch) (wrong - actual is at 0x1000 + height * pitch)
- Result: Corrupted video

## The Fix

Since the intel-vaapi-driver ignores our `offsets[]` values for NV12, we cannot work around this in hasvk by changing what we pass. Instead, we:

1. **Keep passing correct absolute BO offsets** (as per DMA-buf API specification)
2. **Add validation check** that warns if Y plane is not at BO offset 0
3. **Document the requirement** that video images must use dedicated allocations

### Implementation

**File:** `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c`  
**Function:** `anv_vaapi_import_surface_from_image()`

```c
// Calculate absolute Y plane offset for validation
uint64_t y_plane_abs_offset = binding->address.offset + y_surface->memory_range.offset;

// Warn if Y plane is not at BO offset 0
if (y_plane_abs_offset != 0) {
   fprintf(stderr, "WARNING: Video image Y plane not at BO offset 0\n");
   fprintf(stderr, "This will cause incorrect decoding - intel-vaapi-driver assumes Y plane at BO offset 0.\n");
   fprintf(stderr, "Use dedicated memory allocation with offset=0 for video images.\n");
}

// Pass correct absolute BO offsets (even though driver ignores them for NV12)
extbuf.offsets[0] = binding->address.offset + y_surface->memory_range.offset;
extbuf.offsets[1] = binding->address.offset + uv_surface->memory_range.offset;
```

### Why Pass Correct Offsets If They're Ignored?

Even though intel-vaapi-driver ignores the offsets for NV12, we still pass correct values because:
1. **API compliance**: DMA-buf spec requires absolute BO offsets
2. **Other drivers**: crocus or future drivers might use the offsets correctly
3. **Future-proofing**: If intel-vaapi-driver is fixed, our code will work correctly
4. **Debugging**: Correct values in debug logs help troubleshooting

## Impact and Limitations

### When It Works

The code works correctly when:
- Video images use dedicated memory allocations (recommended best practice)
- Images are bound at memory offset 0 (`binding->address.offset == 0`)
- Images are non-disjoint (planes in the same allocation)
- Y plane starts at binding offset 0 (`y_surface->memory_range.offset == 0`)

This covers the vast majority of real-world video decode use cases.

### When It Might Not Work

If any of these is violated, the validation warning alerts the user:
- Video image bound at non-zero offset within device memory
- Custom memory allocation strategies without dedicated allocations
- Suballocations from larger memory blocks

In these cases, decoding will produce corrupted output.

### Recommendation

Applications using HasVK video decode should:
1. Use `VkMemoryDedicatedAllocateInfo` for video images
2. Bind images at offset 0 (`memoryOffset = 0` in `VkBindImageMemoryInfo`)
3. Use non-disjoint image format (default behavior)

## Relationship to Referenced Patch

The user referenced [this intel-vaapi-driver patch](https://github.com/intel/intel-vaapi-driver/commit/3aa8fbe690e33e2026002f7af713d52faf1bd617) which is for a **different issue**:
- That patch fixes surface **export** (`i965_ExportSurfaceHandle`) for 3-plane formats
- Our issue is surface **import** (`i965_CreateSurfaces2`) for 2-plane NV12
- The patch is unrelated to our NV12 import path

## Comparison with 3-Plane Formats

For 3-plane formats (I420, YV12), the driver DOES use offsets, but incorrectly:

```c
case VA_FOURCC_I420:
    // ...
    obj_surface->y_cb_offset = obj_surface->height;  // Hardcoded for Cb
    obj_surface->y_cr_offset = memory_attibute->offsets[2] / obj_surface->width;  // BUG!
```

The bug: `offsets[2] / width` assumes `offsets[0] == 0`. Should be `(offsets[2] - offsets[0]) / width`.

However, hasvk only uses NV12 (2-plane), so this 3-plane bug doesn't affect us.

## Testing

To verify the fix works correctly:

```bash
# Enable debugging
export INTEL_DEBUG=hasvk
export LIBVA_MESSAGING_LEVEL=2

# Test video decode
mpv --hwdec=vulkan test_video.mp4
```

Expected output (if Y plane is at BO offset 0):
```
VA-API surface import: 1920x1088 NV12
  Binding offset: 0
  Y plane:  pitch=1920 offset=0 (BO absolute)
  UV plane: pitch=1920 offset=2088960 (BO absolute)
  NOTE: intel-vaapi-driver ignores offsets[] for NV12, assumes UV at height*pitch
```

If Y plane is NOT at offset 0, you'll see:
```
WARNING: Video image Y plane not at BO offset 0 (binding_offset=4096 + y_offset=0)
This will cause incorrect decoding - intel-vaapi-driver assumes Y plane at BO offset 0.
Use dedicated memory allocation with offset=0 for video images.
```

## Alternative Solutions Considered

1. **Fix intel-vaapi-driver**: The ideal solution, but requires changes to an external project and waiting for distribution updates. Also, the project is unmaintained.

2. **Force Y plane to BO offset 0**: Would require changes to image allocation code; validation check is simpler.

3. **Use a different VA-API driver**: crocus might handle this better, but users may have i965 installed.

## References

- intel/intel-vaapi-driver source: `src/i965_drv_video.c` (NV12 import code)
- Mesa HasVK VA-API bridge architecture: `VA_API_BRIDGE_ARCHITECTURE.md`
- DMA-buf offset fixes: `VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md`
- Vulkan Video specification: VK_KHR_video_decode_queue
