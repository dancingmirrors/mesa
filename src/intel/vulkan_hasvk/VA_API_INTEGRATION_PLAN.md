# HasVK VA-API Integration Plan

## Updated Problem Context

**Critical Finding**: The long mode implementation causes GPU hangs on Gen7/7.5/8 hardware.

**Key Insight**: VA-API is rock solid on this hardware (through crocus/i965 drivers), so the best path forward is to leverage VA-API through Vulkan rather than trying to fix the native Vulkan video decode implementation.

## Strategy: VA-API Bridge for HasVK

Since VA-API works reliably on this hardware via the Gallium drivers (crocus for Gen7-8), we should create a bridge that allows Vulkan video decode operations to be serviced by VA-API underneath.

### Architecture Options

#### Option 1: Internal VA-API Bridge (RECOMMENDED)

Create a VA-API client inside hasvk that translates Vulkan Video decode operations to VA-API calls.

**Pros:**
- Leverages proven VA-API implementation
- No GPU hangs (uses stable VA-API code path)
- Can work with existing crocus driver
- Users get working video decode immediately

**Cons:**
- Requires both Vulkan and VA-API stack
- Additional complexity in hasvk
- Resource sharing between Vulkan and VA-API

#### Option 2: Zink-Style Layering (ALTERNATIVE)

Similar to what the 31517.patch does - use zink as an intermediary.

**Pros:**
- Leverages zink's existing VA-API integration
- Proven approach (patch exists)

**Cons:**
- Adds zink dependency to hasvk
- More complex stack: App → HasVK → Zink → VA-API → Crocus
- Higher overhead

#### Option 3: Fix GPU Hangs (NOT RECOMMENDED)

Try to debug and fix the long mode GPU hangs.

**Cons:**
- Unknown root cause
- Could take months to debug
- Hardware may have undocumented quirks
- Risk of never solving it

## Recommended Implementation: Option 1

### High-Level Design

```
Application
    ↓
HasVK Vulkan Video API
    ↓
anv_video.c (interface layer)
    ↓
anv_video_vaapi_bridge.c (NEW)
    ↓
VA-API (libva)
    ↓
Crocus driver (proven stable)
    ↓
Gen7/7.5/8 Hardware
```

### Key Components to Implement

#### 1. VA-API Bridge Module (`anv_video_vaapi_bridge.c`)

This module translates Vulkan Video operations to VA-API:

```c
struct anv_vaapi_session {
    VADisplay va_display;
    VAContextID va_context;
    VAConfigID va_config;
    VASurfaceID *va_surfaces;  // DPB surfaces
    VABufferID va_picture_param;
    VABufferID va_slice_param;
    VABufferID va_slice_data;
};

VkResult
anv_vaapi_session_create(struct anv_device *device,
                        struct anv_video_session *vid,
                        const VkVideoSessionCreateInfoKHR *pCreateInfo);

VkResult  
anv_vaapi_decode_frame(struct anv_cmd_buffer *cmd_buffer,
                      const VkVideoDecodeInfoKHR *frame_info);
```

#### 2. Resource Sharing (DMA-buf/PRIME)

Share video surfaces between Vulkan (hasvk) and VA-API (crocus):

```c
// Export Vulkan image as DMA-buf
VkResult anv_export_video_surface_dmabuf(
    struct anv_image *image,
    int *fd_out);

// Import into VA-API
VAStatus va_create_surface_from_dmabuf(
    VADisplay display,
    int fd,
    VASurfaceID *surface_id);
```

#### 3. Modified Video Entry Points

Update the video codec entry points to use VA-API bridge:

```c
void
genX(CmdDecodeVideoKHR)(VkCommandBuffer commandBuffer,
                       const VkVideoDecodeInfoKHR *frame_info)
{
    ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
    
    // Use VA-API bridge instead of direct hardware programming
    anv_vaapi_decode_frame(cmd_buffer, frame_info);
}
```

### Implementation Steps

#### Phase 1: Setup and Proof of Concept (3-5 days)

1. **Add VA-API dependency to hasvk**
   ```python
   # In meson.build
   dep_va = dependency('libva', required: true)
   dep_va_drm = dependency('libva-drm', required: true)
   ```

2. **Create basic VA-API bridge skeleton**
   - Initialize VA display using DRM fd
   - Create VA config for H.264 decode
   - Test basic VA-API connectivity

3. **Implement simple decode test**
   - Single frame decode without resource sharing
   - Verify VA-API path works

#### Phase 2: Resource Sharing (5-7 days)

1. **Implement DMA-buf export from Vulkan images**
   ```c
   // In anv_image.c
   VkResult anv_GetMemoryFdKHR(...) {
       // Export memory as DMA-buf fd
   }
   ```

2. **Import DMA-buf into VA-API surfaces**
   ```c
   VASurfaceAttrib attribs[] = {
       { VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME },
       { VASurfaceAttribExternalBufferDescriptor, &desc },
   };
   vaCreateSurfaces(va_display, VA_RT_FORMAT_YUV420, width, height,
                   &surface_id, 1, attribs, 2);
   ```

3. **Implement DPB (Decoded Picture Buffer) sharing**
   - Share all reference frames via DMA-buf
   - Handle lifetime correctly

#### Phase 3: H.264 Decode Integration (7-10 days)

1. **Translate VkVideoDecodeH264PictureInfoKHR to VAPictureParameterBufferH264**
   ```c
   void translate_h264_picture_params(
       const VkVideoDecodeH264PictureInfoKHR *vk_params,
       VAPictureParameterBufferH264 *va_params);
   ```

2. **Translate slice data**
   ```c
   void translate_h264_slice_params(
       const VkVideoDecodeH264PictureInfoKHR *vk_params,
       VASliceParameterBufferH264 *va_slice_params);
   ```

3. **Handle bitstream buffer**
   - Map Vulkan buffer to CPU
   - Pass to VA-API

4. **Synchronization**
   - Ensure VA-API operations complete before Vulkan uses results
   - Use fences/semaphores

#### Phase 4: Command Buffer Integration (3-5 days)

1. **Defer VA-API operations to queue submit**
   ```c
   struct anv_vaapi_decode_cmd {
       VAContextID context;
       VASurfaceID target_surface;
       VABufferID *buffers;
       int num_buffers;
   };
   
   // Store in command buffer
   util_dynarray_append(&cmd_buffer->vaapi_decodes, 
                       struct anv_vaapi_decode_cmd, cmd);
   ```

2. **Execute at QueueSubmit time**
   ```c
   // In anv_queue.c
   for (each vaapi_decode_cmd) {
       vaBeginPicture(display, context, target_surface);
       vaRenderPicture(display, context, buffers, num_buffers);
       vaEndPicture(display, context);
   }
   ```

#### Phase 5: Testing and Validation (5-7 days)

1. **Test with mpv --hwdec=vulkan**
2. **Test with various H.264 content**
   - Different profiles (Baseline, Main, High)
   - Different resolutions
   - Different frame types (I, P, B)
3. **Performance validation**
4. **Memory leak checks**
5. **Multi-threaded stress testing**

### Total Estimated Effort: 23-34 days (4-7 weeks)

## Code Structure

### New Files to Create

```
src/intel/vulkan_hasvk/
├── anv_video_vaapi_bridge.c    (NEW - Main VA-API bridge)
├── anv_video_vaapi_bridge.h    (NEW - Bridge interface)
├── anv_video_vaapi_h264.c      (NEW - H.264 specific translations)
└── anv_video.c                  (MODIFY - Use bridge)
```

### Modified Files

```
src/intel/vulkan_hasvk/
├── meson.build                  (Add VA-API dependency)
├── anv_device.c                 (Initialize VA display)
├── anv_queue.c                  (Execute VA-API commands)
├── anv_private.h                (Add VA-API session structures)
├── anv_image.c                  (DMA-buf export support)
└── genX_video.c                 (Remove - deprecated by bridge)
```

## Example Implementation Snippets

### 1. VA Display Initialization

```c
// In anv_device.c
VkResult anv_CreateDevice(...)
{
    ...
    
    // Initialize VA-API for video decode
    if (enabled_extensions.KHR_video_decode_queue) {
        device->va_display = vaGetDisplayDRM(device->fd);
        if (!device->va_display)
            return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
            
        int major, minor;
        VAStatus va_status = vaInitialize(device->va_display, &major, &minor);
        if (va_status != VA_STATUS_SUCCESS) {
            vaTerminate(device->va_display);
            return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
        }
    }
    
    ...
}
```

### 2. H.264 Picture Parameter Translation

```c
static void
translate_h264_picture_params(
    const VkVideoDecodeH264PictureInfoKHR *vk_pic,
    const StdVideoH264PictureParameterSet *pps,
    const StdVideoH264SequenceParameterSet *sps,
    VAPictureParameterBufferH264 *va_pic)
{
    memset(va_pic, 0, sizeof(*va_pic));
    
    // Current picture
    va_pic->CurrPic.picture_id = /* surface ID */;
    va_pic->CurrPic.frame_idx = vk_pic->pStdPictureInfo->frame_num;
    va_pic->CurrPic.flags = 0;
    va_pic->CurrPic.TopFieldOrderCnt = vk_pic->pStdPictureInfo->PicOrderCnt[0];
    va_pic->CurrPic.BottomFieldOrderCnt = vk_pic->pStdPictureInfo->PicOrderCnt[1];
    
    // Picture dimensions
    va_pic->picture_width_in_mbs_minus1 = sps->pic_width_in_mbs_minus1;
    va_pic->picture_height_in_mbs_minus1 = sps->pic_height_in_map_units_minus1;
    va_pic->bit_depth_luma_minus8 = sps->bit_depth_luma_minus8;
    va_pic->bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8;
    
    // SPS fields
    va_pic->seq_fields.bits.chroma_format_idc = sps->chroma_format_idc;
    va_pic->seq_fields.bits.residual_colour_transform_flag = 0;
    va_pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = 
        sps->flags.gaps_in_frame_num_value_allowed_flag;
    va_pic->seq_fields.bits.frame_mbs_only_flag = sps->flags.frame_mbs_only_flag;
    va_pic->seq_fields.bits.mb_adaptive_frame_field_flag = 
        sps->flags.mb_adaptive_frame_field_flag;
    va_pic->seq_fields.bits.direct_8x8_inference_flag = 
        sps->flags.direct_8x8_inference_flag;
    
    // PPS fields
    va_pic->pic_fields.bits.entropy_coding_mode_flag = pps->flags.entropy_coding_mode_flag;
    va_pic->pic_fields.bits.weighted_pred_flag = pps->flags.weighted_pred_flag;
    va_pic->pic_fields.bits.weighted_bipred_idc = pps->weighted_bipred_idc;
    va_pic->pic_fields.bits.transform_8x8_mode_flag = pps->flags.transform_8x8_mode_flag;
    va_pic->pic_fields.bits.constrained_intra_pred_flag = 
        pps->flags.constrained_intra_pred_flag;
    va_pic->pic_fields.bits.pic_order_present_flag = 
        pps->flags.bottom_field_pic_order_in_frame_present_flag;
    va_pic->pic_fields.bits.deblocking_filter_control_present_flag = 
        pps->flags.deblocking_filter_control_present_flag;
    va_pic->pic_fields.bits.redundant_pic_cnt_present_flag = 
        pps->flags.redundant_pic_cnt_present_flag;
    
    // Quantization
    va_pic->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
    va_pic->chroma_qp_index_offset = pps->chroma_qp_index_offset;
    va_pic->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
    
    // Reference frames
    va_pic->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
    va_pic->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
}
```

### 3. DMA-buf Surface Sharing

```c
static VkResult
create_va_surface_from_anv_image(struct anv_device *device,
                                struct anv_image *image,
                                VASurfaceID *surface_id_out)
{
    // Export Vulkan image as DMA-buf
    int fd;
    VkResult result = anv_bo_export_dma_buf(device, image->planes[0].address.bo, &fd);
    if (result != VK_SUCCESS)
        return result;
    
    // Setup DMA-buf descriptor for VA-API
    VASurfaceAttribExternalBuffers extbuf = {
        .pixel_format = VA_FOURCC_NV12,
        .width = image->vk.extent.width,
        .height = image->vk.extent.height,
        .num_buffers = 1,
        .buffers = (uintptr_t *)&fd,
        .flags = 0,
        .num_planes = 2,  // Y and UV for NV12
    };
    
    // Get actual plane offsets
    extbuf.pitches[0] = image->planes[0].primary_surface.isl.row_pitch_B;
    extbuf.pitches[1] = image->planes[0].primary_surface.isl.row_pitch_B;
    extbuf.offsets[0] = 0;
    extbuf.offsets[1] = image->planes[1].primary_surface.memory_range.offset;
    
    VASurfaceAttrib attribs[2] = {
        {
            .type = VASurfaceAttribMemoryType,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value.type = VAGenericValueTypeInteger,
            .value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
        },
        {
            .type = VASurfaceAttribExternalBufferDescriptor,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value.type = VAGenericValueTypePointer,
            .value.value.p = &extbuf,
        },
    };
    
    VAStatus va_status = vaCreateSurfaces(
        device->va_display,
        VA_RT_FORMAT_YUV420,
        image->vk.extent.width,
        image->vk.extent.height,
        surface_id_out,
        1,  // num_surfaces
        attribs,
        2   // num_attribs
    );
    
    close(fd);  // VA-API dups the fd
    
    if (va_status != VA_STATUS_SUCCESS)
        return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
        
    return VK_SUCCESS;
}
```

## Benefits of This Approach

1. **Immediate Results**: Leverages working VA-API implementation
2. **No GPU Hangs**: Avoids buggy direct hardware programming
3. **Proven Stability**: VA-API on crocus is rock solid
4. **Incremental Development**: Can develop and test in stages
5. **Backward Compatible**: Doesn't break existing (non-video) hasvk functionality
6. **Performance**: VA-API is well-optimized for this hardware

## Potential Challenges

1. **Resource Sharing Complexity**: DMA-buf sharing between drivers
   - **Mitigation**: Well-documented, many examples exist

2. **Synchronization**: Ensuring VA-API and Vulkan stay in sync
   - **Mitigation**: Use explicit fences, careful ordering

3. **Memory Management**: Tracking lifetimes across two APIs
   - **Mitigation**: Reference counting, careful cleanup

4. **Format Mismatches**: Vulkan vs VA-API format differences
   - **Mitigation**: Only support NV12 (G8_B8R8_2PLANE_420_UNORM)

## Testing Plan

### Unit Tests
- VA-API session creation/destruction
- Surface import/export
- Parameter translation correctness

### Integration Tests  
- Single frame decode
- Multi-frame decode with references
- Different H.264 profiles
- Resolution changes
- DPB wraparound

### Application Tests
- mpv --hwdec=vulkan
- ffmpeg with hwaccel
- Any Vulkan app using video decode

## Comparison with Zink Approach (31517.patch)

### Similarities
- Both use VA-API underneath
- Both handle resource sharing
- Both translate between APIs

### Differences
- **Zink**: Gallium → VA-API (full driver translation)
- **HasVK**: Vulkan Video → VA-API (video-only bridge)
  
### Why Not Use Zink Directly?

1. HasVK is a native Vulkan driver, doesn't need Gallium layer
2. Video-only bridge is simpler and lower overhead
3. Maintains HasVK's architecture
4. Easier to maintain

## Conclusion

**This approach is highly feasible and recommended.**

Given that:
- Long mode causes GPU hangs
- VA-API is proven stable
- Resource sharing is well-understood
- Timeline is reasonable (4-7 weeks)

The VA-API bridge is the pragmatic solution that will deliver working video decode on hasvk/Gen7-8 hardware.

## Next Steps

1. Get approval for VA-API dependency in hasvk
2. Start with Phase 1 (proof of concept)
3. Validate resource sharing works
4. Implement full H.264 decode
5. Test with real applications
6. Document and merge

This avoids the GPU hang issue entirely while leveraging proven technology.
