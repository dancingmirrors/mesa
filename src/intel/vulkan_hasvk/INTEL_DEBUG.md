# INTEL_DEBUG Options Working in hasvk

This document summarizes which `INTEL_DEBUG` environment variable options now work with the hasvk driver (for Ivy Bridge, Haswell, and Broadwell GPUs).

## Usage
Set the `INTEL_DEBUG` environment variable to one or more comma-separated options:
```bash
INTEL_DEBUG=bat,perf vkcube
INTEL_DEBUG=fs,vs vkcube
```

## Shader Compilation & Dumping (✅ NOW WORKING)

These options dump shader information during compilation:

- **`vs`** - Dump vertex shader assembly and NIR
- **`tcs`** / **`hs`** - Dump tessellation control shader assembly and NIR
- **`tes`** / **`ds`** - Dump tessellation evaluation shader assembly and NIR
- **`gs`** - Dump geometry shader assembly and NIR
- **`fs`** / **`wm`** - Dump fragment shader assembly and NIR
- **`cs`** - Dump compute shader assembly and NIR

**What you'll see:** When these flags are set, the elk compiler will print:
- NIR shader IR after various optimization passes
- Final assembly code with annotations (if `ann` is also set)
- Shader statistics

**Example:**
```bash
INTEL_DEBUG=fs vkcube
# Will print fragment shader assembly to stderr
```

## Batch Processing & Statistics

- **`bat`** / **`batch`** - ✅ ALREADY WORKED - Dump batch buffer contents
  - Decodes and prints all GPU commands in the batch buffer
  - Shows addresses, command names, and parameters
  
- **`bat-stats`** - ✅ NOW WORKING - Collect and print batch buffer statistics
  - Counts different command types
  - Shows batch buffer usage patterns
  - Prints summary on device destruction

**Example:**
```bash
INTEL_DEBUG=bat vkcube
# Prints decoded batch buffers for every submission

INTEL_DEBUG=bat-stats vkcube
# Collects statistics and prints summary at exit
```

## Performance & Debugging

- **`perf`** - ✅ ALREADY WORKED (custom hasvk addition) - Print performance warnings
  - Shows warnings about suboptimal usage patterns
  - Helps identify performance issues
  
- **`submit`** - ✅ ALREADY WORKED - Print batch buffer submission statistics
  - Shows information about each execbuf2 call
  
- **`sync`** - ✅ ALREADY WORKED - Wait for each batch to complete before continuing
  - Useful for debugging GPU hangs
  - Helps isolate which command buffer causes problems

- **`stall`** - ✅ ALREADY WORKED - Insert GPU stalls after each draw/dispatch
  - Forces GPU to finish each command before starting the next
  - Useful for debugging race conditions

## Image Compression Control (✅ NOW WORKING)

- **`noccs`** / **`no-ccs`** - Disable CCS (Color Compression Surface)
  - Disables lossless color compression (CCS_D)
  - Useful for debugging compression-related issues
  
- **`nohiz`** - Disable HiZ (Hierarchical Depth Buffer)
  - Disables hierarchical Z-buffer optimization
  - Note: HiZ is already limited on Gen7 (Ivy Bridge) in hasvk
  
- **`nofc`** - ✅ ALREADY WORKED - Disable fast clears
  - Disables fast clear optimization for render targets

## Hardware State

- **`l3`** - ✅ ALREADY WORKED - Print L3 cache configuration changes
  - Shows when L3 partitioning changes between different workloads
  
- **`pc`** - ✅ ALREADY WORKED - Print PIPE_CONTROL commands
  - Shows all pipeline synchronization commands
  - Useful for debugging synchronization issues

## Other Options

- **`color`** - ✅ ALREADY WORKED - Use ANSI color codes in output
  - Makes batch buffer dumps easier to read
  
- **`no-oaconfig`** - ✅ ALREADY WORKED - Disable hardware performance counters
  - Disables i915-perf integration
  - Useful when running on simulation or when perf is not available

## Shader Annotation

- **`ann`** - Print assembly with annotations
  - Works with shader dump flags (vs, fs, etc.)
  - Shows source-level information in assembly
  - Requires shader debug flags to be set

**Example:**
```bash
INTEL_DEBUG=fs,ann vkcube
# Dumps fragment shader assembly with source annotations
```

## Options NOT Applicable to hasvk

These options exist in the regular Intel Vulkan driver but are NOT applicable to Ivy Bridge/Haswell/Broadwell hardware:

- **Ray Tracing:** `rt`, `rt_notrace`, `bvh_*` - Requires Gen12+
- **Mesh Shaders:** `task`, `mesh` - Requires Gen12+
- **Modern Features:** `no-vrt`, `sparse`, `heaps` - Not applicable to Gen7/8
- **Optimizer/MDA:** `optimizer`, `mda` - Different code paths in elk vs brw compiler
- **Reemit:** `reemit` - Infrastructure not present in hasvk

## Combined Examples

**Debug a shader issue:**
```bash
INTEL_DEBUG=vs,fs,ann,color vkcube
```

**Debug batch buffer issues:**
```bash
INTEL_DEBUG=bat,pc,sync vkcube
```

**Performance analysis:**
```bash
INTEL_DEBUG=perf,bat-stats,l3 vkcube
```

**Disable all compression:**
```bash
INTEL_DEBUG=noccs,nohiz,nofc vkcube
```

## Technical Details

The hasvk driver now properly:
1. Sets `debug_flag` in `elk_compile_params` for all shader stages
2. Initializes batch decoder for both `DEBUG_BATCH` and `DEBUG_BATCH_STATS`
3. Collects batch statistics with `intel_batch_stats()` during submission
4. Prints statistics with `intel_batch_print_stats()` on device destruction
5. Checks `DEBUG_NO_CCS` and `DEBUG_NO_HIZ` in image creation code

## Related Environment Variables

- **`HASVK_USERSPACE_RELOCS`** - Control relocation behavior (default: true)
  - `true` (default) - Userspace performs relocations
  - `false` - Kernel performs relocations
  - Only affects Ivy Bridge, Haswell, Cherryview (no soft-pinning)

- **`INTEL_SIMD_DEBUG`** - Control SIMD width selection (separate from INTEL_DEBUG)
