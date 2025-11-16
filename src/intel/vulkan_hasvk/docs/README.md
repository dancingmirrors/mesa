# HasVK Documentation

This directory contains documentation for the HasVK Vulkan driver (for Intel Gen7-8 hardware: Ivy Bridge, Haswell, Broadwell).

## Quick Navigation

### Current/Active Documentation

**If you want to use or understand the current video decode implementation:**

- **[VA_API_BRIDGE_STATUS.md](VA_API_BRIDGE_STATUS.md)** - **START HERE** - Quick overview of what's implemented and what's missing
- **[VA_API_BRIDGE_ARCHITECTURE.md](VA_API_BRIDGE_ARCHITECTURE.md)** - Detailed architecture of the VA-API bridge
- **[VA_API_INTEGRATION_PLAN.md](VA_API_INTEGRATION_PLAN.md)** - Implementation plan and phase status
- **[INTEL_DEBUG.md](INTEL_DEBUG.md)** - Debug flags and environment variables (includes VA-API debugging)

### Historical Documentation

**If you're interested in the history of video decode attempts:**

- **[VIDEO_DECODING_ANALYSIS.md](VIDEO_DECODING_ANALYSIS.md)** - Analysis of legacy native implementations (now superseded)
- **[genX_video_long.c](genX_video_long.c)** - Legacy "long format" native implementation (caused GPU hangs)
- **[genX_video_short.c](genX_video_short.c)** - Legacy "short format" native implementation (caused GPU hangs)
- **[gen7_mfd.c](gen7_mfd.c)**, **[gen75_mfd.c](gen75_mfd.c)**, **[gen8_mfd.c](gen8_mfd.c)** - Hardware command references
- **[31517.patch](31517.patch)** - Zink VA-API patch that inspired the VA-API bridge approach

## Overview

### Current Implementation: VA-API Bridge ✅

**Status:** Functionally complete, awaiting real-world testing

The current video decode implementation uses a **VA-API bridge** that routes Vulkan Video decode operations through the stable VA-API implementation (crocus or i965 drivers). This approach provides stable, working video decode without the GPU hangs that plagued previous native implementations.

**Key features:**
- Zero-copy resource sharing via DMA-buf
- Complete H.264 decode support
- Proper synchronization with GEM domains
- Deferred command execution at queue submit
- Comprehensive error handling and logging

**What's implemented:**
- ✅ VA-API session management
- ✅ DMA-buf surface import/export
- ✅ H.264 parameter translation (VK → VA-API)
- ✅ DPB (reference frame) management
- ✅ Multi-slice frame support
- ✅ Command buffer integration

**What's missing:**
- Testing with real applications (mpv, ffmpeg)
- Additional codecs (H.265, VP9 - if hardware supports)
- Video encode support

See [VA_API_BRIDGE_STATUS.md](VA_API_BRIDGE_STATUS.md) for details.

### Why VA-API Bridge?

Previous attempts at native video decode implementations (genX_video_long.c, genX_video_short.c) caused **GPU hangs** on Gen7/7.5/8 hardware. Root causes were difficult to debug due to:
- Complex hardware state management
- Undocumented hardware quirks
- Synchronization issues between video and render engines
- Chroma plane offset alignment problems
- Deblocking filter state issues

The VA-API bridge solves these problems by:
1. **Leveraging proven code** - crocus/i965 VA-API drivers are stable and well-tested
2. **Avoiding hardware quirks** - VA-API driver handles hardware-specific workarounds
3. **Better maintenance** - Bug fixes in VA-API driver benefit all users
4. **Lower risk** - No direct hardware programming means fewer GPU hangs

### Historical Context

The journey to working video decode on hasvk involved several attempts:

1. **Initial attempt:** genX_video_long.c (long format mode)
   - More sophisticated, parsed slice headers
   - Had deblocking issues on Haswell
   - Chroma artifacts on Ivy Bridge

2. **Second attempt:** genX_video_short.c (short format mode)
   - Simpler, relied on hardware to parse
   - Still had GPU hangs
   - Hardcoded chroma offsets caused issues

3. **Analysis phase:** VIDEO_DECODING_ANALYSIS.md
   - Analyzed both implementations
   - Studied zink's VA-API approach
   - Identified potential fixes

4. **Final solution:** VA-API bridge (current)
   - Implemented all planned phases
   - Functionally complete
   - Awaiting testing

See [VIDEO_DECODING_ANALYSIS.md](VIDEO_DECODING_ANALYSIS.md) for detailed analysis of what went wrong with native implementations.

## File Guide

### Active Implementation Files

| File | Description |
|------|-------------|
| `VA_API_BRIDGE_STATUS.md` | Quick status overview - start here |
| `VA_API_BRIDGE_ARCHITECTURE.md` | Detailed architecture and design |
| `VA_API_INTEGRATION_PLAN.md` | Implementation plan and phase tracking |
| `VA_API_BRIDGE_FIX_DPB_TILING.md` | Fix for DPB reference frame tiling issue |
| `VA_API_BRIDGE_FIX_DMABUF_OFFSETS.md` | Fix for DMA-buf plane offset calculation |
| `INTEL_DEBUG.md` | Debug flags and environment variables |

### Historical/Reference Files

| File | Description |
|------|-------------|
| `VIDEO_DECODING_ANALYSIS.md` | Analysis of legacy implementations |
| `genX_video_long.c` | Legacy long format implementation (GPU hangs) |
| `genX_video_short.c` | Legacy short format implementation (GPU hangs) |
| `gen7_mfd.c` | Gen7 (Ivy Bridge) MFD commands reference |
| `gen75_mfd.c` | Gen7.5 (Haswell) MFD commands reference |
| `gen8_mfd.c` | Gen8 (Broadwell) MFD commands reference |
| `31517.patch` | Zink VA-API patch (inspiration) |

## For Developers

### If you want to understand the current implementation:

1. Read [VA_API_BRIDGE_STATUS.md](VA_API_BRIDGE_STATUS.md) for high-level overview
2. Read [VA_API_BRIDGE_ARCHITECTURE.md](VA_API_BRIDGE_ARCHITECTURE.md) for details
3. Look at actual source code:
   - `../anv_video_vaapi_bridge.h/c` - Core bridge
   - `../anv_video_vaapi_h264.c` - H.264 translation
   - `../genX_video.c` - Command buffer entry point
   - `../anv_video.c` - Session management

### If you want to test the implementation:

1. Read the "How to Test" section in [VA_API_BRIDGE_STATUS.md](VA_API_BRIDGE_STATUS.md)
2. Set up Gen7/Gen8 hardware with VA-API drivers
3. Enable debugging: `export INTEL_DEBUG=perf LIBVA_MESSAGING_LEVEL=2`
4. Test with: `mpv --hwdec=vulkan video.mp4`
5. Report results!

### If you want to debug video decode:

1. Read the "VA-API Bridge Debugging" section in [INTEL_DEBUG.md](INTEL_DEBUG.md)
2. Enable logging with `INTEL_DEBUG=perf`
3. Enable VA-API logging with `LIBVA_MESSAGING_LEVEL=2`
4. Optionally trace VA-API: `export LIBVA_TRACE=/tmp/vaapi.log`

### If you want to understand why native implementations failed:

1. Read [VIDEO_DECODING_ANALYSIS.md](VIDEO_DECODING_ANALYSIS.md)
2. Compare genX_video_long.c and genX_video_short.c
3. Understand the GPU hang issues and why they were hard to fix

## Common Questions

**Q: Does video decode work on hasvk?**  
A: Implementation is complete, but **not yet tested** with real applications. Theoretically it should work.

**Q: Which codecs are supported?**  
A: Currently only H.264 decode. H.265/VP9 not implemented (and may not be supported by Gen7-8 hardware anyway).

**Q: Does video encode work?**  
A: No, only decode is implemented. Encode is technically feasible with the same VA-API bridge approach.

**Q: Why not fix the native implementations?**  
A: GPU hangs were difficult to debug, and VA-API is proven stable. VA-API bridge was faster and lower risk.

**Q: Is there performance overhead?**  
A: Minimal. DMA-buf sharing is zero-copy, and parameter translation is just struct field mapping. Should be nearly identical to native VA-API performance.

**Q: What if I don't have VA-API drivers?**  
A: You need either crocus or i965 VA-API driver. Without VA-API, video decode won't work.

**Q: Can I still see the old native implementations?**  
A: Yes, they're preserved in this docs/ directory (genX_video_long.c, genX_video_short.c) for reference.

**Q: Where's the actual source code?**  
A: In `../` (parent directory):
- `anv_video_vaapi_bridge.h/c` - Main bridge
- `anv_video_vaapi_h264.c` - H.264 support
- `genX_video.c` - Command buffer integration
- `anv_video.c` - Session management

## Contributing

If you test the VA-API bridge and find issues:

1. Enable debugging: `INTEL_DEBUG=perf LIBVA_MESSAGING_LEVEL=2`
2. Capture the output
3. Note the specific video file, application, and hardware
4. File an issue or submit a patch

If you want to add features:

1. Review the architecture in [VA_API_BRIDGE_ARCHITECTURE.md](VA_API_BRIDGE_ARCHITECTURE.md)
2. Follow the existing code patterns in `anv_video_vaapi_*.c`
3. Add appropriate debug logging with `INTEL_DEBUG(DEBUG_PERF)`
4. Update documentation

## License

All documentation and code in this directory is under the MIT license (see individual file headers).
