#ifndef NVK_PERF_H
#define NVK_PERF_H 1

#include <stdbool.h>
#include <stdint.h>

#include "util/macros.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"

struct nvk_cmd_buffer;
struct nvk_device;

enum nvk_perf_kind {
   NVK_PERF_KIND_GFX,
   NVK_PERF_KIND_GFX_MISC,
   NVK_PERF_KIND_CS,
   NVK_PERF_KIND_BAR_WAIT,
   NVK_PERF_KIND_BAR_INVAL,
   NVK_PERF_KIND_COPY,
   NVK_PERF_KIND_END,
   NVK_PERF_KIND_COUNT,
};

enum nvk_perf_counter {
   NVK_PERF_CTR_SUBMITS,
   NVK_PERF_CTR_CMDBUFS,
   NVK_PERF_CTR_DRAWS,
   NVK_PERF_CTR_DISPATCHES,
   NVK_PERF_CTR_WORKGROUPS,
   NVK_PERF_CTR_BARRIER_CALLS,
   NVK_PERF_CTR_FLUSH_WFI,
   NVK_PERF_CTR_WFI,
   NVK_PERF_CTR_INVAL_TEX,
   NVK_PERF_CTR_INVAL_SHADER,
   NVK_PERF_CTR_INVAL_CONST,
   NVK_PERF_CTR_INVAL_MME,
   NVK_PERF_CTR_INVAL_QMD,
   NVK_PERF_CTR_HOST_SYNC,
   NVK_PERF_CTR_MARKS,
   NVK_PERF_CTR_BAD_MARKS,
   NVK_PERF_CTR_COUNT,
};

#define NVK_PERF_HIST_BUCKETS 6
#define NVK_PERF_TOP_SLOTS 512

struct nvk_perf_shader_snap {
   const void *key;
   uint32_t num_instrs;
   uint32_t num_static_cycles;
   uint32_t num_gprs;
   uint32_t max_warps_per_sm;
   uint32_t num_spills_to_mem;
   uint32_t slm_size;
};

struct nvk_perf_rt_snap {
   uint32_t w;
   uint32_t h;
   uint32_t color_format;
   uint32_t depth_format;
   uint8_t samples;
   uint8_t color_count;
   uint8_t compressed;
   uint8_t layers;
};

struct nvk_perf_top_entry {
   uint8_t used;
   uint8_t kind;
   int64_t ns;
   int64_t count;
   int64_t max_ns;
   int64_t wg;
   int64_t threads;
   struct nvk_perf_shader_snap a;
   struct nvk_perf_shader_snap b;
   struct nvk_perf_rt_snap rt;
};

struct nvk_perf_stats {
   bool enabled;
   int64_t last_dump_ns;
   int64_t counters[NVK_PERF_CTR_COUNT];
   int64_t gpu_ns[NVK_PERF_KIND_COUNT];
   int64_t gpu_segs[NVK_PERF_KIND_COUNT];
   int64_t hist_ns[NVK_PERF_KIND_COUNT][NVK_PERF_HIST_BUCKETS];
   int64_t hist_segs[NVK_PERF_KIND_COUNT][NVK_PERF_HIST_BUCKETS];
   simple_mtx_t mtx;
   struct nvk_perf_top_entry top[NVK_PERF_TOP_SLOTS];
   int64_t top_dropped;
};

static inline void
nvk_perf_add(struct nvk_perf_stats *perf, enum nvk_perf_counter ctr, int64_t n)
{
   if (unlikely(perf->enabled))
      p_atomic_add(&perf->counters[ctr], n);
}

void nvk_perf_maybe_dump(struct nvk_device *dev);
void nvk_perf_init_cmd_buffer(struct nvk_cmd_buffer *cmd);
void nvk_perf_mark_slow(struct nvk_cmd_buffer *cmd, enum nvk_perf_kind kind);
void nvk_perf_end_cmd_buffer(struct nvk_cmd_buffer *cmd);
void nvk_perf_harvest_cmd_buffer(struct nvk_cmd_buffer *cmd);

#endif
