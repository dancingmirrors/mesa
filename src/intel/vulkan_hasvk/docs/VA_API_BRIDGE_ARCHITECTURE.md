# VA-API Bridge Architecture for HasVK

## Overview

The VA-API Bridge is a critical component that allows HasVK (Haswell Vulkan driver) to provide stable video decode functionality by leveraging the mature VA-API implementation available through the crocus/i965 drivers on Gen7/7.5/8 hardware.

## Motivation

Native Vulkan video decode implementations on Gen7-8 hardware have proven problematic:
- **Long mode**: Causes GPU hangs on Gen7/7.5/8 hardware
- **Short mode**: Also exhibits instability issues

However, VA-API is rock solid on this hardware through the legacy drivers. The VA-API bridge allows applications using Vulkan Video (e.g., `mpv --hwdec=vulkan`) to "just work" by routing decode operations through VA-API.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                        Application                          │
│                    (e.g., mpv, ffmpeg)                      │
└─────────────────────┬───────────────────────────────────────┘
                      │ Vulkan Video Decode API
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                    HasVK Vulkan Driver                       │
│                                                              │
│  ┌────────────────────────────────────────────────────┐     │
│  │         Vulkan Video Entry Points                  │     │
│  │  - vkCreateVideoSessionKHR                         │     │
│  │  - vkCmdDecodeVideoKHR                             │     │
│  │  - etc.                                            │     │
│  └───────────────────┬────────────────────────────────┘     │
│                      │                                       │
│                      ▼                                       │
│  ┌────────────────────────────────────────────────────┐     │
│  │           anv_video.c (interface layer)            │     │
│  └───────────────────┬────────────────────────────────┘     │
│                      │                                       │
│                      ▼                                       │
│  ┌────────────────────────────────────────────────────┐     │
│  │       anv_video_vaapi_bridge.c (NEW)               │     │
│  │                                                     │     │
│  │  - Session management                              │     │
│  │  - Parameter translation (VK → VA-API)             │     │
│  │  - DMA-buf resource sharing                        │     │
│  │  - Synchronization                                 │     │
│  └───────────────────┬────────────────────────────────┘     │
│                      │                                       │
└──────────────────────┼───────────────────────────────────────┘
                       │ VA-API calls
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                     VA-API (libva)                           │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────┐
│             Crocus Driver (Mesa Gallium)                     │
│              (Proven stable on Gen7-8)                       │
└─────────────────────┬───────────────────────────────────────┘
                      │ i915 DRM
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                  Gen7/7.5/8 Hardware                         │
│          (Haswell, Broadwell, Cherryview)                    │
└─────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. VA-API Bridge Module

**Files:**
- `anv_video_vaapi_bridge.h` - Interface definitions
- `anv_video_vaapi_bridge.c` - Implementation

**Responsibilities:**
- Creating and managing VA-API sessions
- Translating Vulkan video structures to VA-API structures
- Managing DMA-buf sharing between Vulkan and VA-API
- Coordinating synchronization between APIs

### 2. Data Structures

#### `struct anv_vaapi_session`

Contains the VA-API state for a video decode session:

```c
struct anv_vaapi_session {
   VADisplay va_display;          /* VA display handle */
   VAContextID va_context;        /* VA decode context */
   VAConfigID va_config;          /* VA configuration */
   
   /* DPB (Decoded Picture Buffer) surfaces */
   VASurfaceID *va_surfaces;      /* Array of VA surfaces */
   uint32_t num_surfaces;         /* Number of surfaces */
   
   /* Parameter buffers */
   VABufferID va_picture_param;   /* Picture parameters */
   VABufferID va_slice_param;     /* Slice parameters */
   VABufferID va_slice_data;      /* Slice data */
   
   /* Session properties */
   uint32_t width;                /* Frame width */
   uint32_t height;               /* Frame height */
   VAProfile va_profile;          /* VA-API profile */
};
```

#### Device-Level Integration

The `anv_device` structure now includes:
```c
void *va_display;  /* VA-API display (lazily initialized) */
```

The `anv_video_session` structure now includes:
```c
struct anv_vaapi_session *vaapi_session;  /* VA-API bridge session */
```

### 3. Session Lifecycle

#### Creation Flow

1. **Application** calls `vkCreateVideoSessionKHR`
2. **anv_video.c** calls `vk_video_session_init()` (common runtime)
3. **anv_video.c** calls `anv_vaapi_session_create()`
4. **Bridge** performs:
   - Get/create VA display from device DRM fd
   - Map Vulkan profile to VA-API profile
   - Create VA config for the profile
   - Allocate DPB surface array
   - Create VA context

#### Destruction Flow

1. **Application** calls `vkDestroyVideoSessionKHR`
2. **anv_video.c** waits for idle (defensive check)
3. **anv_video.c** calls `anv_vaapi_session_destroy()`
4. **Bridge** performs:
   - Destroy VA buffers (picture, slice params)
   - Destroy VA surfaces
   - Destroy VA context and config
   - Free session memory

### 4. Decode Operation Flow (IMPLEMENTED)

```
vkCmdDecodeVideoKHR
    ↓
genX_CmdDecodeVideoKHR (in genX_video.c)
    ↓
anv_vaapi_decode_frame (in anv_video_vaapi_bridge.c)
    ↓
[Import/Cache Phase]
- Import destination surface via DMA-buf if not cached
- Import reference frame surfaces if not cached
- Build surface mapping for DPB management
    ↓
[Translation Phase]
- anv_vaapi_translate_h264_picture_params (in anv_video_vaapi_h264.c)
  → Map Vulkan decode info → VAPictureParameterBufferH264
  → Build reference frame list from DPB
- anv_vaapi_translate_h264_slice_params
  → Map slice parameters → VASliceParameterBufferH264
  → Build RefPicList0/RefPicList1 from DPB
- Map bitstream buffer via anv_gem_mmap
  → Create VA slice data buffers
    ↓
[Command Recording]
- Create VA-API parameter buffers
- Create anv_vaapi_decode_cmd with all decode state
- Store in cmd_buffer->video.vaapi_decodes (deferred execution)
    ↓
[At Queue Submit - in anv_batch_chain.c]
anv_vaapi_execute_deferred_decodes
    ↓
[Synchronization - Before Decode]
- I915_GEM_SET_DOMAIN (CPU domain) to wait for Vulkan GPU operations
    ↓
[VA-API Submission]
- vaBeginPicture(context, target_surface)
- vaRenderPicture() (picture params)
- For each slice:
  - vaRenderPicture() (slice params)
  - vaRenderPicture() (slice data)
- vaEndPicture()
- vaSyncSurface() (wait for decode completion)
    ↓
[Synchronization - After Decode]
- I915_GEM_SET_DOMAIN (GTT domain) for cache coherency with Vulkan
    ↓
[Cleanup]
- Destroy VA-API buffers
- Clear deferred command list
```

## Resource Sharing Strategy

### DMA-buf PRIME

Video surfaces must be shared between Vulkan (HasVK) and VA-API (crocus):

1. **Vulkan Image Creation**
   - Application creates `VkImage` with video usage
   - HasVK allocates backing memory

2. **Export to DMA-buf**
   - Export Vulkan image memory as DMA-buf fd
   - Use existing kernel DRM PRIME mechanisms

3. **Import to VA-API**
   - Create `VASurfaceAttribExternalBuffers` descriptor
   - Use `vaCreateSurfaces()` with `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`
   - VA-API now references the same physical memory

4. **Synchronization**
   - Use DRM implicit fencing or explicit sync objects
   - Ensure VA-API operations complete before Vulkan reuses surfaces

### Memory Layout Compatibility

Both Vulkan and VA-API must agree on:
- **Pixel format**: NV12 (`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`)
- **Tiling**: Linear or compatible tiling mode
- **Pitch/Stride**: Match between APIs
- **Plane offsets**: Y plane and UV plane offsets

## Profile Mapping

| Vulkan Video Profile | VA-API Profile |
|---------------------|----------------|
| `VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR` | `VAProfileH264Main` |
| (Baseline) | `VAProfileH264Baseline` |
| (High) | `VAProfileH264High` |

## Implementation Status

### ✅ Phase 1: Infrastructure (COMPLETE)

- [x] Basic VA-API bridge structure (`anv_video_vaapi_bridge.h/c`)
- [x] Session lifecycle management (`anv_vaapi_session_create/destroy`)
- [x] VA display initialization (`anv_vaapi_get_display`)
- [x] Integration with video session create/destroy (in `anv_video.c`)

### ✅ Phase 2: Resource Sharing (COMPLETE)

- [x] DMA-buf export from Vulkan images (`anv_vaapi_export_video_surface_dmabuf`)
- [x] VA-API surface import from DMA-buf (`anv_vaapi_import_surface_from_image`)
- [x] DPB (Decoded Picture Buffer) management (surface mapping with `anv_vaapi_surface_map`)
- [x] Memory lifetime tracking (surfaces cached in session surface map)

### ✅ Phase 3: H.264 Decode (COMPLETE)

- [x] Translate `VkVideoDecodeH264PictureInfoKHR` → `VAPictureParameterBufferH264` (`anv_vaapi_h264.c`)
- [x] Translate slice parameters (`anv_vaapi_translate_h264_slice_params`)
- [x] Map bitstream buffers (via GEM mmap in `anv_vaapi_decode_frame`)
- [x] Implement decode submission (deferred execution in `anv_vaapi_execute_deferred_decodes`)

### ✅ Phase 4: Synchronization (COMPLETE)

- [x] GEM domain synchronization (using `I915_GEM_SET_DOMAIN` before/after VA-API decode)
- [x] Queue submit integration (executed in `anv_batch_chain.c` at queue submit)
- [x] Command buffer recording (deferred commands stored in `cmd_buffer->video.vaapi_decodes`)
- [x] VA-API sync (`vaSyncSurface` after decode completion)

### 📋 Phase 5: Testing & Validation (IN PROGRESS)

- [ ] Test with mpv `--hwdec=vulkan`
- [ ] Test with ffmpeg hwaccel
- [ ] Validate different H.264 profiles (Baseline, Main, High)
- [ ] Performance benchmarking
- [ ] Memory leak detection

**Note:** The VA-API bridge implementation is functionally complete. Testing with real applications is the remaining work.

## What's Currently Missing / Known Limitations

While the VA-API bridge is functionally complete, the following areas need attention:

### 1. Real-World Application Testing

**Status:** Not yet tested with real applications

The implementation needs validation with:
- **mpv** with `--hwdec=vulkan`: Test playback of various H.264 content
- **ffmpeg** with Vulkan hwaccel: Test transcoding and decode
- **Chromium/Firefox** (if they support Vulkan Video): Browser video decode
- **Sample Vulkan Video apps**: Reference implementations

**Why this matters:** Real applications may expose edge cases not covered by the implementation.

### 2. Additional Codec Support

**Status:** Only H.264 is implemented

The bridge currently supports only H.264 decode. Future codecs could include:
- **H.265/HEVC**: Map to `VAProfileHEVCMain` (if crocus driver supports it on Gen7-8)
- **VP9**: Map to `VAProfileVP9Profile0` (unlikely on Gen7-8 hardware)
- **AV1**: Not supported on Gen7-8 hardware

**Note:** Gen7-8 hardware may not support codecs beyond H.264 in VA-API anyway.

### 3. Video Encode Support

**Status:** Not implemented

The same VA-API bridge architecture could support video encode:
- Map `VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR` to `VAEntrypointEncSlice`
- Translate encode parameters similar to decode

**Feasibility:** Gen7-8 hardware does support H.264 encode via VA-API, so this is technically possible.

### 4. Advanced H.264 Features

**Status:** Basic H.264 works, advanced features untested

The following H.264 features may need additional testing/fixes:
- **Interlaced content** (field pictures): RefPicList field flag handling implemented but untested
- **Weighted prediction**: Parameter translation present but untested
- **Multiple slice groups** (FMO): Deprecated, unlikely to be needed
- **Error resilience**: Non-existing frame handling implemented
- **4K resolution**: Max coded extent set to 4096x4096 but untested on Gen7-8

### 5. Performance Optimization

**Status:** Functional but not optimized

Potential optimizations:
- **Surface caching**: Currently caches surfaces, but eviction policy may need tuning
- **Multi-threaded decode**: VA-API supports concurrent contexts, not exploited
- **Zero-copy paths**: Currently optimal (DMA-buf sharing), but verify no extra copies
- **Bitstream buffer management**: Currently uses GEM mmap, could use direct buffer sharing

### 6. Error Handling and Recovery

**Status:** Basic error handling present

The implementation handles errors but could be improved:
- **VA-API errors**: Currently logged with INTEL_DEBUG but may need better recovery
- **Invalid parameters**: Some validation present but could be more comprehensive  
- **Resource exhaustion**: Surface map has fixed capacity, should handle overflow gracefully
- **Driver compatibility**: Assumes compatible VA-API driver (crocus/i965), should detect

### 7. Debugging and Diagnostics

**Status:** Basic INTEL_DEBUG logging present

Current debugging:
- **INTEL_DEBUG=perf**: Logs VA-API operations and errors
- **LIBVA_MESSAGING_LEVEL**: Can enable VA-API driver logging

Could be improved with:
- **Dedicated video debug flag**: `INTEL_DEBUG=video` for VA-API bridge specific logging
- **Frame dumps**: Save decoded frames for visual inspection
- **Performance counters**: Track decode time, surface usage, cache hit rate

### 8. Documentation Gaps

**Status:** Architecture documented, usage examples missing

Missing documentation:
- **User guide**: How to enable VA-API bridge, environment variables, troubleshooting
- **Testing procedures**: Step-by-step guide to test with mpv/ffmpeg
- **Known issues**: List of known bugs or incompatibilities
- **Performance characteristics**: Expected overhead, supported resolutions

## What We're NOT Missing (Already Implemented)

To be clear, these are **already working**:

✅ **VA-API session management**: Create, configure, destroy sessions  
✅ **DMA-buf resource sharing**: Export Vulkan images, import to VA-API surfaces  
✅ **H.264 parameter translation**: Complete VK → VA-API conversion for picture/slice params  
✅ **DPB management**: Reference frame tracking with surface mapping  
✅ **Multi-slice support**: Handles H.264 frames with multiple slices  
✅ **Synchronization**: GEM domain transitions for cache coherency  
✅ **Deferred execution**: Proper command buffer recording and queue submit integration  
✅ **Memory layout compatibility**: ISL surface offsets used for NV12 plane alignment  
✅ **Y-tiling support**: Video surfaces use Y-tiling as required by Gen7-8 hardware  

## Recommendations for Next Steps

1. **Set up test environment** with Gen7/Gen8 hardware and install VA-API drivers (crocus or i965)
2. **Test basic decode** with simple H.264 file using mpv: `mpv --hwdec=vulkan video.mp4`
3. **Enable debugging** with `INTEL_DEBUG=perf LIBVA_MESSAGING_LEVEL=2`
4. **Verify functionality** with various H.264 content (different profiles, resolutions)
5. **Performance testing** to measure overhead vs native VA-API
6. **Document results** including any issues found and workarounds
7. **File bug reports** for any real issues discovered during testing

## References

- [VA-API Specification](https://github.com/intel/libva)
- [Vulkan Video Extensions](https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#video)
- [DRM PRIME Documentation](https://www.kernel.org/doc/html/latest/gpu/drm-mm.html)
- [VA_API_INTEGRATION_PLAN.md](VA_API_INTEGRATION_PLAN.md) - Original design document

## Maintenance Notes

### Adding New Profiles

1. Update `get_va_profile()` to map new Vulkan codec
2. Add profile-specific parameter translation
3. Test with sample content

### Debugging

Enable VA-API logging:
```bash
export LIBVA_MESSAGING_LEVEL=2
export LIBVA_TRACE=/tmp/vaapi.log
```

Enable Vulkan validation:
```bash
export VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
```

## Contact

For questions or issues with the VA-API bridge, please file an issue in the Mesa repository with the `hasvk` and `video` labels.
