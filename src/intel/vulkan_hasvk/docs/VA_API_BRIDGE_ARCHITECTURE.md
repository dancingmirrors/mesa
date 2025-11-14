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

### 4. Decode Operation Flow (Planned)

```
vkCmdDecodeVideoKHR
    ↓
genX_CmdDecodeVideoKHR (dispatcher)
    ↓
anv_vaapi_decode_frame
    ↓
[Translation Phase]
- Map Vulkan decode info → VAPictureParameterBufferH264
- Map slice parameters → VASliceParameterBufferH264
- Map bitstream buffer → VA slice data
    ↓
[VA-API Submission]
- vaBeginPicture()
- vaRenderPicture() (picture params)
- vaRenderPicture() (slice params)
- vaRenderPicture() (slice data)
- vaEndPicture()
    ↓
[Synchronization]
- Wait for VA-API completion
- Signal Vulkan timeline semaphores
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

- [x] Basic VA-API bridge structure
- [x] Session lifecycle management
- [x] VA display initialization
- [x] Integration with video session create/destroy

### 📋 Phase 2: Resource Sharing (TODO)

- [ ] DMA-buf export from Vulkan images
- [ ] VA-API surface import from DMA-buf
- [ ] DPB (Decoded Picture Buffer) management
- [ ] Memory lifetime tracking

### 📋 Phase 3: H.264 Decode (TODO)

- [ ] Translate `VkVideoDecodeH264PictureInfoKHR` → `VAPictureParameterBufferH264`
- [ ] Translate slice parameters
- [ ] Map bitstream buffers
- [ ] Implement decode submission

### 📋 Phase 4: Synchronization (TODO)

- [ ] Fence/semaphore coordination
- [ ] Queue submit integration
- [ ] Command buffer recording
- [ ] Timeline synchronization

### 📋 Phase 5: Testing & Validation (TODO)

- [ ] Test with mpv `--hwdec=vulkan`
- [ ] Test with ffmpeg hwaccel
- [ ] Validate different H.264 profiles
- [ ] Performance benchmarking
- [ ] Memory leak detection

## Benefits

1. **Stability**: Leverages proven VA-API code path
2. **Hardware Access**: VA-API has proper I915_GEM_DOMAIN access
3. **Immediate Value**: Works today without fixing GPU hangs
4. **Compatibility**: Existing VA-API applications continue to work
5. **Future-Proof**: Can be replaced with native implementation when stable

## Limitations

1. **Dependency**: Requires both Vulkan and VA-API stacks
2. **Overhead**: Additional translation layer
3. **Complexity**: More moving parts to maintain
4. **H.264 Only**: Initially limited to H.264 decode (can be extended)

## Performance Considerations

- **Translation Overhead**: Minimal, just struct mapping
- **Memory Copies**: Zero-copy via DMA-buf sharing
- **Synchronization**: Explicit fencing adds latency but is necessary
- **Expected Impact**: < 5% overhead vs native VA-API

## Future Extensions

### Additional Codecs

- **H.265/HEVC**: Map to `VAProfileHEVCMain`
- **VP9**: Map to `VAProfileVP9Profile0`
- **AV1**: Map to `VAProfileAV1Profile0` (if supported)

### Encode Support

The same architecture can support video encode:
- Map `VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR` to `VAEntrypointEncSlice`
- Similar parameter translation for encode

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
