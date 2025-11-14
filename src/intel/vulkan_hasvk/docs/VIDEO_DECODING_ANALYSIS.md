# HasVK H.264 Video Decoding Analysis and Recommendations

## Problem Statement

Both `genX_video_long.c` and `genX_video_short.c` have issues:
- **Haswell**: Major deblocking issues
- **Ivy Bridge**: Only chroma garbage visible

The question is whether ideas from the zink VA-API over Vulkan implementation (31517.patch) can be used to fix these issues.

## Current Implementation Analysis

### genX_video_long.c (Long Format Mode)

**Strengths:**
- More sophisticated chroma plane offset calculation using ISL surface layout
- Proper slice header parsing using vl_rbsp utilities
- Support for phantom slices when first MB doesn't start at (0,0)
- Comprehensive NAL unit type and reference handling

**Potential Issues:**
1. **Chroma Plane Offset Calculation (Lines 427-443)**
   ```c
   if (img->n_planes > 1) {
       ss.YOffsetforUCb =
           img->planes[1].primary_surface.memory_range.offset /
           img->planes[0].primary_surface.isl.row_pitch_B;
   } else {
       /* Fallback for single-plane layout */
       ss.YOffsetforUCb = align(img->vk.extent.height, 32);
   }
   ```
   This assumes ISL correctly laid out the planes, but may not account for hardware-specific alignment requirements.

2. **Deblocking Filter State**
   - Missing explicit deblocking filter control in MFX_AVC_SLICE_STATE
   - No disable_deblocking_filter_idc handling from slice header

3. **Reference Picture Setup**
   - Direct MV buffer management may have synchronization issues
   - POC list setup doesn't validate bounds

### genX_video_short.c (Short Format Mode)

**Strengths:**
- Simpler implementation
- Direct hardware control through BSD objects

**Issues:**
1. **Hardcoded Chroma Offset (Lines 98-99)**
   ```c
   ss.YOffsetforUCb = align(img->vk.extent.height, 32);
   ss.YOffsetforVCr = align(img->vk.extent.height, 32);
   ```
   This is **likely the source of chroma garbage on Ivy Bridge**. The actual chroma plane offset from ISL layout is ignored.

2. **MFD_AVC_SLICEADDR Usage**
   - Look-ahead slicing logic appears incorrect (lines 406-418)
   - Using `s+2` index could access beyond array bounds

## Insights from Zink Implementation (31517.patch)

### Key Patterns That Could Help HasVK

1. **Proper Resource Info Querying**
   From patch 12/27:
   ```c
   static void
   zink_resource_get_info(struct pipe_screen *pscreen, 
                         struct pipe_resource *pres,
                         unsigned *stride, unsigned *offset)
   {
       // Uses VkImageAspectFlags to query actual plane layouts
       VkImageSubresource isr = { aspect, };
       VkSubresourceLayout srl;
       VKSCR(GetImageSubresourceLayout)(screen->dev, obj->image, &isr, &srl);
       *offset = srl.offset;
       *stride = srl.rowPitch;
   }
   ```
   
   **Application to HasVK**: Instead of calculating offsets manually, we should query Vulkan directly for the actual plane layout.

2. **Video Queue Management**
   Patches 5-6 show proper video queue setup:
   - Separate video decode queue discovery
   - Proper queue family index handling
   - Video-specific command pool creation

   **Application to HasVK**: Currently uses graphics queue - should use dedicated video queue if available.

3. **Planar Image Creation**
   Patch 7/27 shows proper handling of multi-plane images with modifiers:
   ```c
   for (unsigned p = 1; p < num_planes; p++) {
       res_plane->plane = p;
       res_plane->aspect = plane_aspects[p];
       // Proper chaining of plane resources
   }
   ```

4. **Video Resource Binding**
   Patch 18/27 shows critical video-specific resource setup:
   - VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR
   - VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR
   - Proper format feature queries for video profiles

## Recommended Fixes

### Priority 1: Fix Chroma Plane Offsets (Addresses Ivy Bridge Chroma Garbage)

**For both long and short modes**, replace the hardcoded/calculated offsets with proper Vulkan queries:

```c
// In genX_video.c (common helper)
static uint32_t
anv_get_video_chroma_offset(const struct anv_image *img)
{
   if (img->n_planes < 2)
      return align(img->vk.extent.height, 32);
      
   // Query actual layout from Vulkan/ISL
   uint32_t offset_rows = img->planes[1].primary_surface.memory_range.offset /
                          img->planes[0].primary_surface.isl.row_pitch_B;
   
   // Validate and clamp to hardware limits
   uint32_t max_offset = (img->vk.extent.height * 3) / 2;  // Max for NV12
   if (offset_rows > max_offset) {
      // Fallback to safe value
      return align(img->vk.extent.height, 32);
   }
   
   return offset_rows;
}
```

**Note**: The actual offset must match what ISL/anv_image allocated. Gen7/7.5/8 hardware expects Y offset in rows, not bytes.

### Priority 2: Add Deblocking Filter Control (Addresses Haswell Deblocking Issues)

The MFX_AVC_IMG_STATE has deblocking control, but genX_video_long.c doesn't properly set it from the slice header.

Add to slice header parsing in `anv_h264_parse_slice_header_long()`:
```c
// After parsing cabac_init_idc, parse deblocking filter fields
if (pps->flags.deblocking_filter_control_present_flag) {
    uint32_t disable_deblocking_filter_idc = vl_rbsp_ue(&rbsp);
    if (disable_deblocking_filter_idc != 1) {
        int32_t slice_alpha_c0_offset_div2 = vl_rbsp_se(&rbsp);
        int32_t slice_beta_offset_div2 = vl_rbsp_se(&rbsp);
    }
    *disable_deblocking = disable_deblocking_filter_idc;
}
```

Then use this in MFX_AVC_SLICE_STATE emission.

### Priority 3: Fix Short Mode Slice Addressing

The look-ahead logic in genX_video_short.c (lines 406-418) has an off-by-one error:
```c
// Current broken code:
if (!last_slice) {
    uint32_t next_offset = h264_pic_info->pSliceOffsets[s + 1];
    uint32_t next_end = h264_pic_info->pSliceOffsets[s + 2];  // BUG: can overflow
    if (s == h264_pic_info->sliceCount - 2)
        next_end = frame_info->srcBufferRange;
```

Should be:
```c
if (!last_slice) {
    uint32_t next_offset = h264_pic_info->pSliceOffsets[s + 1];
    uint32_t next_end;
    if (s + 2 < h264_pic_info->sliceCount)
        next_end = h264_pic_info->pSliceOffsets[s + 2];
    else
        next_end = frame_info->srcBufferRange;
```

## Feasibility Assessment

### Is it Feasible to Use Zink Ideas? **YES**

1. **Chroma Offset Fix**: **High Feasibility**
   - Can be implemented in 1-2 hours
   - Low risk - just using proper offset calculation
   - Should fix Ivy Bridge chroma issues immediately

2. **Deblocking Filter Fix**: **Medium Feasibility**
   - Requires slice header parsing extension
   - Medium complexity (2-4 hours)
   - Should address Haswell deblocking issues

3. **Video Queue Support**: **Low Feasibility for HasVK**
   - HasVK targets legacy hardware (Gen7-8)
   - These platforms don't have dedicated video queues in the same way newer hardware does
   - Graphics queue works fine, this is not the root cause

### Should We Stub Zink Code for HasVK? **NO**

**Reasons:**
1. **Different Driver Models**: Zink is Gallium-over-Vulkan, HasVK is native Vulkan
2. **Different Goals**: Zink translates OpenGL/Gallium to Vulkan; HasVK implements Vulkan directly
3. **Code Complexity**: Stubbing would create maintenance burden
4. **Better Approach**: Learn patterns from Zink, implement natively in HasVK

**What TO borrow from Zink:**
- Pattern of querying actual plane layouts
- Video resource setup patterns
- Synchronization patterns for video operations

**What NOT to borrow:**
- Gallium-specific pipe interfaces
- Stream uploader patterns (not needed in Vulkan)
- Zink's batch management (HasVK has its own)

## Recommended Implementation Path

### Phase 1: Quick Fixes (1-3 days)
1. Fix chroma plane offset calculation in both modes
2. Fix slice addressing bug in short mode
3. Test on available hardware

### Phase 2: Deblocking Fix (3-5 days)
1. Extend slice header parsing for deblocking parameters
2. Apply deblocking control in MFX_AVC_IMG_STATE
3. Test deblocking on Haswell

### Phase 3: Validation (2-3 days)
1. Test with mpv --hwdec=vulkan
2. Test with vainfo
3. Test various H.264 streams (different profiles, levels, resolutions)

### Total Estimated Effort: 6-11 days

## Hardware Considerations

### Crocus Driver Interaction

The problem statement mentions hasvk hardware also uses crocus. This is important:

1. **Shared Hardware Access**: Both drivers may access video hardware
2. **Resource Sharing**: Might need PRIME/DMA-buf sharing between Gallium (crocus) and Vulkan (hasvk)
3. **Testing**: Should test hasvk video independent of crocus to isolate issues

### Known Hardware Limitations

**Gen7 (Ivy Bridge):**
- Limited to H.264 Baseline/Main/High profiles up to level 4.1
- Maximum 1920x1088 resolution
- Chroma format 4:2:0 only
- May have stricter alignment requirements

**Gen7.5 (Haswell):**
- Same profile support as Gen7
- Improved deblocking filter
- Better error concealment

**Gen8 (Broadwell):**
- Improved video engine
- Better performance
- Same format limitations

## Conclusion

**Yes, it is highly feasible to use ideas from the zink patch to fix hasvk video decoding.**

The key insights are:
1. **Proper chroma plane offset calculation** - should fix Ivy Bridge issues
2. **Deblocking filter control** - should fix Haswell issues  
3. **Don't stub zink code** - learn patterns, implement natively

The fixes are relatively straightforward and don't require major architectural changes. The estimated effort is reasonable (1-2 weeks) and the risk is low since the changes are localized to video decoding paths.

## Next Steps

1. Create a feature branch for video fixes
2. Implement chroma offset fix first (highest impact, lowest risk)
3. Test on hardware if available, or request testing from community
4. Implement deblocking fix
5. Document any remaining issues for future work

## References

- 31517.patch: Zink VA-API over Vulkan implementation
- genX_video_long.c: HasVK long format H.264 decoder
- genX_video_short.c: HasVK short format H.264 decoder
- Intel® HD Graphics Programmer's Reference Manual for Gen7/7.5/8
