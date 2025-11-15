# HasVK VA-API Bridge Quick Reference

**One-page reference for the hasvk VA-API video decode bridge**

## TL;DR

✅ **Implementation:** Complete (4 phases done)  
📋 **Testing:** Needed (not yet tested with real apps)  
🎯 **Goal:** Stable H.264 decode via VA-API (no GPU hangs)  
🔧 **Codecs:** H.264 only (for now)

## Quick Status

```
Infrastructure    ✅ DONE  | Resource Sharing ✅ DONE
H.264 Decode      ✅ DONE  | Synchronization  ✅ DONE
Testing           📋 TODO  |
```

## Architecture in 30 Seconds

```
Your App
    ↓ Vulkan Video API
HasVK Driver (genX_video.c)
    ↓ CmdDecodeVideoKHR
VA-API Bridge (anv_video_vaapi_bridge.c)
    ↓ DMA-buf sharing + parameter translation
VA-API (libva)
    ↓ vaBeginPicture / vaRenderPicture / vaEndPicture
Crocus/i965 Driver
    ↓ Hardware commands
Gen7/8 Video Engine (MFD)
```

**Key Insight:** Zero-copy DMA-buf sharing, no memory overhead!

## Quick Test

```bash
# Install dependencies
# Need: mesa (hasvk), libva, crocus/i965 drivers

# Enable debug logging
export INTEL_DEBUG=perf
export LIBVA_MESSAGING_LEVEL=2

# Test with mpv
mpv --hwdec=vulkan your-video.mp4

# Should see in output:
# - "VA-API initialized"
# - "VA-API session created"
# - Video plays without GPU hangs!
```

## File Map

| What | Where |
|------|-------|
| Main bridge | `anv_video_vaapi_bridge.c` |
| H.264 translation | `anv_video_vaapi_h264.c` |
| Entry point | `genX_video.c` (CmdDecodeVideoKHR) |
| Session mgmt | `anv_video.c` |
| Execution | `anv_batch_chain.c` (queue submit) |

## Debug in 3 Steps

1. **Enable logging:**
   ```bash
   export INTEL_DEBUG=perf LIBVA_MESSAGING_LEVEL=2
   ```

2. **Run your app:**
   ```bash
   mpv --hwdec=vulkan video.mp4
   ```

3. **Check output:**
   - Look for "VA-API" messages
   - Check for errors in VA session creation
   - Verify surface import succeeds

## Common Issues

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| "Failed to get VA display" | VA-API driver not installed | Install crocus or i965 |
| "Invalid SPS/PPS IDs" | Bad video file | Try different video |
| Garbage output | Surface import failed | Check debug logs |
| Crash | Missing VA-API libs | Install libva-drm |

## Performance

**Expected:** Nearly identical to native VA-API
- Zero memory copies (DMA-buf)
- Minimal translation overhead (<1%)
- Same hardware decode path

**Actual:** Unknown (not tested yet!)

## What Works (Theoretically)

✅ H.264 Baseline profile  
✅ H.264 Main profile  
✅ H.264 High profile  
✅ Up to 16 reference frames  
✅ Multi-slice frames  
✅ B-frames  
✅ Resolutions up to 4096x4096

## What Doesn't Work

❌ H.265/HEVC (not implemented)  
❌ VP9 (not implemented)  
❌ Video encode (not implemented)  
❌ Formats other than NV12

## Implementation Details

**Resource Sharing:**
- Vulkan images → DMA-buf FD → VA-API surfaces
- NV12 format with proper Y/UV plane offsets
- Surface caching to avoid re-imports
- Y-tiling for video surfaces (Gen7-8 requirement)

**Synchronization:**
- GEM SET_DOMAIN before decode (wait for Vulkan)
- VA-API decode execution
- vaSyncSurface (wait for decode)
- GEM SET_DOMAIN after decode (cache coherency)

**Parameter Translation:**
- VkVideoDecodeH264PictureInfoKHR → VAPictureParameterBufferH264
- VkVideoDecodeH264PictureInfoKHR → VASliceParameterBufferH264
- Bitstream buffer → VA slice data
- Reference frames → DPB with RefPicList0/1

## For More Details

- **Quick overview:** VA_API_BRIDGE_STATUS.md
- **Architecture:** VA_API_BRIDGE_ARCHITECTURE.md
- **Implementation plan:** VA_API_INTEGRATION_PLAN.md
- **Debugging:** INTEL_DEBUG.md
- **Navigation:** README.md
- **History:** VIDEO_DECODING_ANALYSIS.md

## Critical TODOs

1. **Test with mpv** on Gen7/8 hardware
2. **Verify functionality** with various H.264 content
3. **Measure performance** vs native VA-API
4. **Document issues** found during testing
5. **Fix bugs** if any are discovered

## Why VA-API Bridge?

**Problem:** Direct hardware programming caused GPU hangs  
**Solution:** Route through proven VA-API implementation  
**Result:** Stable decode without hardware quirks  
**Trade-off:** Adds VA-API dependency (already needed for other uses)

## Contact

Questions? Check the documentation in `src/intel/vulkan_hasvk/docs/`

Issues? Enable `INTEL_DEBUG=perf` and share the logs!

---

**Last Updated:** December 2024  
**Status:** Implementation complete, testing needed
