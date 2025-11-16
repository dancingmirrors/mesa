# VA-API Bridge Critical Fixes: DMA-buf Surface Import and Synchronization

**Date:** November 2024  
**Issue:** Green screen / old framebuffer content due to incorrect surface import and cache coherency  
**Status:** FIXED

## Problem Description

The VA-API bridge was displaying green screens or residual framebuffer content when decoding H.264 video via DMA-buf sharing. The issue had FOUR root causes:
1. Plane offsets were calculated incorrectly
2. The `data_size` field was not being set
3. Cache coherency synchronization was insufficient
4. Uninitialized array elements in VASurfaceAttribExternalBuffers

## Root Causes

### Issue 1: Incorrect Plane Offsets

When importing Vulkan video surfaces into VA-API via DMA-buf, the plane offsets must be relative to the start of the buffer object (BO), not relative to the start of the image binding.

The issue manifested as follows:

1. **Vulkan image creation**: Multi-planar NV12 images are created with Y and UV planes
2. **Memory layout**: Each plane has a `memory_range.offset` relative to its binding
3. **BO export**: The entire BO is exported via DMA-buf (not just the binding)
4. **Offset mismatch**: Code was using plane offsets relative to binding, not BO
5. **VA-API confusion**: VA-API interpreted offsets incorrectly, reading wrong memory locations

### Issue 2: Missing data_size Field

The `VASurfaceAttribExternalBuffers` structure has a `data_size` field that must be set to the total size of the buffer data. This was being left uninitialized (0), causing VA-API to not know how much data is in the buffer.

Without `data_size` set:
- VA-API cannot validate the buffer size
- VA-API may not access the surface data correctly
- Surface import may succeed but data access fails

### Issue 3: Insufficient Cache Coherency ⚠️ **CRITICAL**

After VA-API decode completes, the decoded data written by the video engine must be properly flushed from caches so Vulkan (render engine) can see it.

The problem:
- Video engine and render engine have separate caches on Intel GPUs
- After VA-API decode, data is in video engine caches
- Without proper cache flush, Vulkan reads stale/uninitialized data (green screens)
- Previous code only set read domain, didn't explicitly flush writes

This is the most subtle and critical issue - even with correct surface import, cache coherency problems prevent the decoded data from being visible.

### Issue 4: Uninitialized Array Elements in VASurfaceAttribExternalBuffers

The `VASurfaceAttribExternalBuffers` structure contains `pitches[4]` and `offsets[4]` arrays to support up to 4 planes. For NV12 format, only 2 planes are used (Y and UV), but the structure was being initialized with a designated initializer that left indices 2 and 3 uninitialized.

The problem:
- Stack-allocated structure with designated initializer doesn't zero unused fields
- `pitches[2]`, `pitches[3]`, `offsets[2]`, `offsets[3]` contained garbage values
- Some VA-API drivers or validation layers may inspect all array elements
- Could cause validation failures or undefined behavior

Expected values:
```
pitches[4]=1920 1920 0 0
offsets[4]=0 2088960 0 0
```

Without proper initialization, the last two values in each array were undefined.

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

## The Fixes

**File:** `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c`  
**Function:** `anv_vaapi_import_surface_from_image()`  
**Lines:** 838-845

### Fix 1: Plane Offset Calculation

**Before (BROKEN):**
```c
extbuf.offsets[0] = 0;       /* Y plane starts at beginning */
extbuf.offsets[1] = uv_surface->memory_range.offset; /* UV plane from ISL */
```

**After (FIXED):**
```c
extbuf.offsets[0] = binding->address.offset + y_surface->memory_range.offset;
extbuf.offsets[1] = binding->address.offset + uv_surface->memory_range.offset;
```

### Fix 2: Data Size Field

**Before (BROKEN):**
```c
VASurfaceAttribExternalBuffers extbuf = {
   .pixel_format = VA_FOURCC_NV12,
   .width = image->vk.extent.width,
   .height = image->vk.extent.height,
   // ... other fields ...
   // data_size NOT SET - defaults to 0!
};
```

**After (FIXED):**
```c
VASurfaceAttribExternalBuffers extbuf = {
   .pixel_format = VA_FOURCC_NV12,
   .width = image->vk.extent.width,
   .height = image->vk.extent.height,
   // ... other fields ...
};

// Set total data size for the DMA-buf
extbuf.data_size = extbuf.offsets[1] + uv_surface->memory_range.size;
```

For a 1920x1088 NV12 surface:
- UV offset: 2088960 bytes
- UV size: 1044480 bytes  
- **Total data_size: 3133440 bytes**

### Fix 3: Cache Coherency Synchronization

**File:** `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c`  
**Function:** `anv_vaapi_execute_deferred_decodes()`

**Before (BROKEN):**
```c
// After VA-API decode
vaSyncSurface(va_display, decode_cmd->target_surface);

// Only set read domain - doesn't flush writes!
struct drm_i915_gem_set_domain set_domain = {
   .handle = decode_cmd->target_gem_handle,
   .read_domains = I915_GEM_DOMAIN_GTT,
   .write_domain = 0,  // No flush!
};
intel_ioctl(device->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain);
```

**After (FIXED):**
```c
// After VA-API decode
vaSyncSurface(va_display, decode_cmd->target_surface);

// Step 1: Flush video engine writes
struct drm_i915_gem_set_domain set_domain_flush = {
   .handle = decode_cmd->target_gem_handle,
   .read_domains = I915_GEM_DOMAIN_GTT,
   .write_domain = I915_GEM_DOMAIN_GTT,  // Explicit flush!
};
intel_ioctl(device->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain_flush);

// Step 2: Transition to read-only for Vulkan
struct drm_i915_gem_set_domain set_domain_read = {
   .handle = decode_cmd->target_gem_handle,
   .read_domains = I915_GEM_DOMAIN_GTT,
   .write_domain = 0,
};
intel_ioctl(device->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &set_domain_read);
```

### Fix 4: Array Initialization

**File:** `src/intel/vulkan_hasvk/anv_video_vaapi_bridge.c`  
**Function:** `anv_vaapi_import_surface_from_image()`  
**Lines:** 812-820

**Before (BROKEN):**
```c
VASurfaceAttribExternalBuffers extbuf = {
   .pixel_format = VA_FOURCC_NV12,
   .width = image->vk.extent.width,
   .height = image->vk.extent.height,
   .num_buffers = 1,
   .buffers = (uintptr_t *) & fd,
   .flags = 0,
   .num_planes = 2,
};
// pitches[2], pitches[3], offsets[2], offsets[3] are UNINITIALIZED!
```

**After (FIXED):**
```c
VASurfaceAttribExternalBuffers extbuf;
memset(&extbuf, 0, sizeof(extbuf));  // Zero all fields including arrays
extbuf.pixel_format = VA_FOURCC_NV12;
extbuf.width = image->vk.extent.width;
extbuf.height = image->vk.extent.height;
extbuf.num_buffers = 1;
extbuf.buffers = (uintptr_t *) & fd;
extbuf.flags = 0;
extbuf.num_planes = 2;
// All unused array elements are now properly zeroed
```

## Why These Fixes Were Needed

### Fix 1: Plane Offsets (Binding vs BO)

The DMA-buf sharing architecture has a subtle but critical detail:

1. **Vulkan side**: Images have memory bindings that can start at offsets within the BO
2. **Export**: When we export via DMA-buf, we export the entire BO (not just the binding portion)
3. **Import**: VA-API needs offsets relative to the BO start, not the binding start
4. **Memory layout**: `binding->address.offset` is the binding's offset within the BO
5. **Plane layout**: `surface->memory_range.offset` is the plane's offset within the binding

Therefore: **BO offset = binding offset + plane offset**

### Fix 2: data_size Field (Buffer Size Validation)

The `VASurfaceAttribExternalBuffers` structure needs `data_size` to:

1. **Validate buffer size**: VA-API checks that the buffer is large enough
2. **Memory safety**: Ensures VA-API doesn't read beyond buffer bounds
3. **Driver requirements**: Some VA-API drivers (like i965) require this field

The `data_size` is the total size of all data in the buffer:
- **Formula**: `UV plane offset + UV plane size`
- **Why**: This gives the end of the last plane's data

Without this field set, VA-API trace shows `data_size=0`, which can cause:
- Import to succeed but surface usage to fail
- Incorrect memory validation
- Driver-specific issues

### Fix 3: Cache Coherency (Video ↔ Render Engine)

Intel GPUs have separate cache hierarchies for different engines:
- **Video engine**: Used by VA-API for decode operations
- **Render engine**: Used by Vulkan for rendering/sampling

After VA-API decode:
1. **Decoded data**: Written by video engine to BO
2. **Cache state**: Data in video engine caches, not yet visible to render engine
3. **Without flush**: Vulkan reads stale data from render engine caches (green screens!)
4. **With flush**: Explicit GEM domain transition flushes caches properly

The two-step synchronization:
- **Step 1**: Set GTT write domain → Forces kernel to flush video engine writes
- **Step 2**: Set GTT read domain → Prepares BO for Vulkan read access

This is critical on Intel Gen7-8 where cache coherency between engines is not automatic.

### Fix 4: Array Initialization (Uninitialized Memory)

C designated initializers only initialize the fields that are explicitly listed. For stack-allocated structures, uninitialized fields contain whatever garbage was previously on the stack.

The `VASurfaceAttribExternalBuffers` structure has:
- `uint32_t pitches[4]` - Row pitch for each plane
- `uint32_t offsets[4]` - Offset for each plane

For NV12 (2 planes), we only set indices 0 and 1:
- `pitches[0]` = Y plane pitch (1920 for 1920x1088)
- `pitches[1]` = UV plane pitch (1920)
- `pitches[2]` = **UNINITIALIZED** (could be any value!)
- `pitches[3]` = **UNINITIALIZED** (could be any value!)

Similarly for offsets. While `num_planes=2` tells VA-API to ignore indices 2 and 3, some VA-API implementations or validation layers may still:
- Read all array elements for logging/debugging
- Validate that unused elements are zero
- Have undefined behavior when encountering garbage values

Using `memset(&extbuf, 0, sizeof(extbuf))` before setting fields ensures:
- All unused array elements are zero
- All padding bytes are zero
- Predictable behavior across all VA-API implementations
- Clean LIBVA_TRACE output showing `pitches[4]=1920 1920 0 0` instead of garbage

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
| **DMA-buf Offsets** | Plane offsets calculated incorrectly | Add binding offset to plane offsets |
| **Data Size** | `data_size` field was 0 | Calculate as UV offset + UV size |
| **Cache Coherency** | Video/render engine cache mismatch | Two-step GEM domain transition |
| **Array Initialization** | Uninitialized array elements | Use memset to zero entire structure |

All fixes are necessary for correct VA-API decode operation.

## Debugging the Issue

To detect these issues:

1. Enable debugging:
   ```bash
   export INTEL_DEBUG=hasvk
   export LIBVA_MESSAGING_LEVEL=2
   export LIBVA_TRACE=/tmp/va.log
   ```

2. Check for binding offset in hasvk logs:
   ```
   Binding offset: 0  (or non-zero if using suballocations)
   Total data size: 3133440  (should NOT be 0!)
   Y plane:  pitch=1920 offset=0 (binding_offset=0 + plane_offset=0)
   UV plane: pitch=1920 offset=2088960 (binding_offset=0 + plane_offset=2088960)
   ```

3. Check VA-API trace for data_size:
   ```
   --VASurfaceAttribExternalBufferDescriptor
     pixel_format=0x3231564e
     width=1920
     height=1088
     data_size=3133440  <-- Should be NON-ZERO!
     num_planes=2
     pitches[4]=1920 1920 0 0
     offsets[4]=0 2088960 0 0
   ```

### Signs of the Bug

**Before fixes:**
- LIBVA_TRACE shows `data_size=0`
- Video shows green screens or old framebuffer content
- No errors reported, but decode output is wrong

**After fixes:**
- LIBVA_TRACE shows correct `data_size` (e.g., 3133440 for 1920x1088)
- Video decodes correctly
- Both I-frames and P/B-frames work properly

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
