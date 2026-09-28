#include "nvk_perf.h"

#include "nvk_cmd_buffer.h"
#include "nvk_cmd_pool.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvkmd/nvkmd.h"
#include "nvk_image.h"
#include "nvk_image_view.h"
#include "vk_enum_to_str.h"

#include "cl/nvk_query.h"
#include "util/os_time.h"

#include "nv_push_cl9097.h"
#include "nv_push_cl90c0.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nvk_perf_mark_rec {
   struct nvk_query_report *report;
   enum nvk_perf_kind kind;
   struct nvk_perf_shader_snap a;
   struct nvk_perf_shader_snap b;
   struct nvk_perf_rt_snap rt;
   uint32_t wg;
   uint32_t threads_per_wg;
};

static const char *const nvk_perf_kind_names[NVK_PERF_KIND_COUNT] = {
   [NVK_PERF_KIND_GFX] = "gfx",
   [NVK_PERF_KIND_GFX_MISC] = "gfx_misc",
   [NVK_PERF_KIND_CS] = "cs",
   [NVK_PERF_KIND_BAR_WAIT] = "bar_wait",
   [NVK_PERF_KIND_BAR_INVAL] = "bar_inval",
   [NVK_PERF_KIND_COPY] = "copy",
   [NVK_PERF_KIND_END] = "end",
};

static const char *const nvk_perf_counter_names[NVK_PERF_CTR_COUNT] = {
   [NVK_PERF_CTR_SUBMITS] = "submits",
   [NVK_PERF_CTR_CMDBUFS] = "cmdbufs",
   [NVK_PERF_CTR_DRAWS] = "draws",
   [NVK_PERF_CTR_DISPATCHES] = "dispatches",
   [NVK_PERF_CTR_WORKGROUPS] = "workgroups",
   [NVK_PERF_CTR_BARRIER_CALLS] = "barrier_calls",
   [NVK_PERF_CTR_FLUSH_WFI] = "flush_wfi",
   [NVK_PERF_CTR_WFI] = "plain_wfi",
   [NVK_PERF_CTR_INVAL_TEX] = "tex_inval",
   [NVK_PERF_CTR_INVAL_SHADER] = "shader_inval",
   [NVK_PERF_CTR_INVAL_CONST] = "const_inval",
   [NVK_PERF_CTR_INVAL_MME] = "mme_inval",
   [NVK_PERF_CTR_INVAL_QMD] = "qmd_inval",
   [NVK_PERF_CTR_HOST_SYNC] = "host_sync",
   [NVK_PERF_CTR_MARKS] = "marks",
   [NVK_PERF_CTR_BAD_MARKS] = "bad_marks",
};

static const uint64_t nvk_perf_hist_limit_ns[NVK_PERF_HIST_BUCKETS - 1] = {
   4000, 16000, 64000, 256000, 1000000,
};

static const char *const nvk_perf_hist_names[NVK_PERF_HIST_BUCKETS] = {
   "<4us", "<16us", "<64us", "<256us", "<1ms", ">=1ms",
};

static void
nvk_perf_snap(struct nvk_perf_shader_snap *s, const struct nvk_shader *sh)
{
   memset(s, 0, sizeof(*s));
   if (sh == NULL)
      return;

   s->key = sh;
   s->num_instrs = sh->info.num_instrs;
   s->num_static_cycles = MIN2(sh->info.num_static_cycles, UINT32_MAX);
   s->num_gprs = sh->info.num_gprs;
   s->max_warps_per_sm = sh->info.max_warps_per_sm;
   s->num_spills_to_mem = sh->info.num_spills_to_mem;
   s->slm_size = sh->info.slm_size;
}

static uint8_t
nvk_perf_iview_compressed(const struct nvk_image_view *iview)
{
   if (iview == NULL || iview->vk.image == NULL)
      return 0;
   const struct nvk_image *image =
      container_of(iview->vk.image, struct nvk_image, vk);
   return image->is_compressed;
}

static void
nvk_perf_rt_snap(struct nvk_perf_rt_snap *r,
                 const struct nvk_rendering_state *render)
{
   memset(r, 0, sizeof(*r));
   r->w = render->area.extent.width;
   r->h = render->area.extent.height;
   r->samples = MIN2(render->samples, 255);
   r->color_count = MIN2(render->color_att_count, 255);
   r->layers = MIN2(render->layer_count, 255);
   if (render->color_att_count > 0) {
      r->color_format = render->color_att[0].vk_format;
      r->compressed = nvk_perf_iview_compressed(render->color_att[0].iview);
   }
   if (render->depth_att.iview != NULL) {
      r->depth_format = render->depth_att.vk_format;
      if (render->color_att_count == 0)
         r->compressed = nvk_perf_iview_compressed(render->depth_att.iview);
   }
}

static bool
nvk_perf_rt_equal(const struct nvk_perf_rt_snap *x,
                  const struct nvk_perf_rt_snap *y)
{
   return x->w == y->w && x->h == y->h &&
          x->color_format == y->color_format &&
          x->depth_format == y->depth_format &&
          x->samples == y->samples && x->compressed == y->compressed;
}

static void
nvk_perf_top_add(struct nvk_perf_stats *perf, enum nvk_perf_kind kind,
                 const struct nvk_perf_shader_snap *a,
                 const struct nvk_perf_shader_snap *b,
                 const struct nvk_perf_rt_snap *rt,
                 uint64_t dt, bool count_it,
                 uint64_t wg, uint64_t threads)
{
   static const struct nvk_perf_rt_snap no_rt = { 0 };
   if (rt == NULL)
      rt = &no_rt;

   const uintptr_t h = (((uintptr_t)a->key >> 4) * 0x9E3779B1u) ^
                       (((uintptr_t)b->key >> 4) * 0x85EBCA77u) ^
                       ((uintptr_t)rt->w * 0x27D4EB2Fu) ^
                       ((uintptr_t)rt->h * 0x165667B1u) ^
                       ((uintptr_t)rt->color_format * 0x9E3779B1u) ^
                       ((uintptr_t)rt->samples << 20) ^
                       ((uintptr_t)rt->compressed << 28) ^ kind;

   for (unsigned probe = 0; probe < NVK_PERF_TOP_SLOTS; probe++) {
      struct nvk_perf_top_entry *e =
         &perf->top[(h + probe) % NVK_PERF_TOP_SLOTS];
      if (!e->used) {
         e->used = 1;
         e->kind = kind;
         e->a = *a;
         e->b = *b;
         e->rt = *rt;
         e->ns = dt;
         e->count = count_it ? 1 : 0;
         e->max_ns = dt;
         e->wg = wg;
         e->threads = threads;
         return;
      }
      if (e->kind == kind && e->a.key == a->key && e->b.key == b->key &&
          nvk_perf_rt_equal(&e->rt, rt)) {
         e->ns += dt;
         if (count_it)
            e->count++;
         if ((int64_t)dt > e->max_ns)
            e->max_ns = dt;
         e->wg += wg;
         e->threads += threads;
         return;
      }
   }
   perf->top_dropped++;
}

static const char *
nvk_perf_format_name(uint32_t format)
{
   const char *name = vk_Format_to_str((VkFormat)format);
   if (name != NULL && strncmp(name, "VK_FORMAT_", 10) == 0)
      return name + 10;
   return name != NULL ? name : "?";
}

static int
nvk_perf_top_cmp(const void *pa, const void *pb)
{
   const struct nvk_perf_top_entry *a =
      *(const struct nvk_perf_top_entry *const *)pa;
   const struct nvk_perf_top_entry *b =
      *(const struct nvk_perf_top_entry *const *)pb;
   return (a->ns < b->ns) - (a->ns > b->ns);
}

void
nvk_perf_maybe_dump(struct nvk_device *dev)
{
   struct nvk_perf_stats *perf = &dev->perf;
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   if (!perf->enabled)
      return;

   const int64_t now = os_time_get_nano();
   const int64_t last = p_atomic_read(&perf->last_dump_ns);
   if (last == 0) {
      p_atomic_cmpxchg(&perf->last_dump_ns, 0, now);
      return;
   }
   if (now - last < 1000000000ll)
      return;
   if (p_atomic_cmpxchg(&perf->last_dump_ns, last, now) != last)
      return;

   const double secs = (double)(now - last) * 1e-9;

   int64_t ctr[NVK_PERF_CTR_COUNT];
   for (unsigned i = 0; i < NVK_PERF_CTR_COUNT; i++)
      ctr[i] = p_atomic_xchg(&perf->counters[i], 0);

   fprintf(stderr, "NVK_PERF %.2fs:", secs);
   for (unsigned i = 0; i < NVK_PERF_CTR_COUNT; i++) {
      if (i == NVK_PERF_CTR_WORKGROUPS) {
         const double per = ctr[NVK_PERF_CTR_DISPATCHES] > 0 ?
            (double)ctr[i] / (double)ctr[NVK_PERF_CTR_DISPATCHES] : 0.0;
         fprintf(stderr, " wg_per_dispatch=%.1f", per);
      } else {
         fprintf(stderr, " %s=%" PRId64, nvk_perf_counter_names[i], ctr[i]);
      }
   }
   fprintf(stderr, "\n");

   if (!(pdev->debug_flags & NVK_DEBUG_PERF_GPU_TIME))
      return;

   int64_t ns[NVK_PERF_KIND_COUNT], segs[NVK_PERF_KIND_COUNT], total = 0;
   for (unsigned i = 0; i < NVK_PERF_KIND_COUNT; i++) {
      ns[i] = p_atomic_xchg(&perf->gpu_ns[i], 0);
      segs[i] = p_atomic_xchg(&perf->gpu_segs[i], 0);
      total += ns[i];
   }

   fprintf(stderr, "NVK_PERF_GPU total=%.1fms", (double)total * 1e-6);
   for (unsigned i = 0; i < NVK_PERF_KIND_END; i++) {
      fprintf(stderr, " %s=%.1fms/%" PRId64 "(%.1fus)",
              nvk_perf_kind_names[i], (double)ns[i] * 1e-6, segs[i],
              segs[i] > 0 ? (double)ns[i] * 1e-3 / (double)segs[i] : 0.0);
   }
   fprintf(stderr, "\n");

   for (unsigned i = 0; i < NVK_PERF_KIND_END; i++) {
      if (segs[i] == 0)
         continue;
      fprintf(stderr, "NVK_PERF_HIST %s:", nvk_perf_kind_names[i]);
      for (unsigned b = 0; b < NVK_PERF_HIST_BUCKETS; b++) {
         const int64_t hns = p_atomic_xchg(&perf->hist_ns[i][b], 0);
         const int64_t hsegs = p_atomic_xchg(&perf->hist_segs[i][b], 0);
         fprintf(stderr, " %s=%.1fms/%" PRId64, nvk_perf_hist_names[b],
                 (double)hns * 1e-6, hsegs);
      }
      fprintf(stderr, "\n");
   }

   simple_mtx_lock(&perf->mtx);
   const struct nvk_perf_top_entry *sorted[NVK_PERF_TOP_SLOTS];
   unsigned num = 0;
   for (unsigned s = 0; s < NVK_PERF_TOP_SLOTS; s++) {
      if (perf->top[s].used)
         sorted[num++] = &perf->top[s];
   }
   qsort(sorted, num, sizeof(sorted[0]), nvk_perf_top_cmp);

   unsigned shown[NVK_PERF_KIND_COUNT] = { 0 };
   for (unsigned s = 0; s < num; s++) {
      const struct nvk_perf_top_entry *e = sorted[s];
      if (shown[e->kind]++ >= 10)
         continue;
      if (e->kind == NVK_PERF_KIND_GFX) {
         fprintf(stderr, "NVK_PERF_TOP gfx %.2fms/%" PRId64 " max=%.0fus"
                 " rt=%ux%u x%u %s%s%s c=%u"
                 " vs[%p i=%u c=%u g=%u w=%u s=%u]"
                 " fs[%p i=%u c=%u g=%u w=%u s=%u]\n",
                 (double)e->ns * 1e-6, e->count, (double)e->max_ns * 1e-3,
                 e->rt.w, e->rt.h, e->rt.samples,
                 e->rt.color_count > 0 ?
                    nvk_perf_format_name(e->rt.color_format) : "nocolor",
                 e->rt.depth_format != 0 ? " +" : "",
                 e->rt.depth_format != 0 ?
                    nvk_perf_format_name(e->rt.depth_format) : "",
                 e->rt.compressed,
                 e->a.key, e->a.num_instrs, e->a.num_static_cycles,
                 e->a.num_gprs, e->a.max_warps_per_sm, e->a.num_spills_to_mem,
                 e->b.key, e->b.num_instrs, e->b.num_static_cycles,
                 e->b.num_gprs, e->b.max_warps_per_sm, e->b.num_spills_to_mem);
      } else {
         fprintf(stderr, "NVK_PERF_TOP cs %.2fms/%" PRId64 " max=%.0fus"
                 " wg/disp=%.0f thr/us=%.0f"
                 " cs[%p i=%u c=%u g=%u w=%u s=%u slm=%u]\n",
                 (double)e->ns * 1e-6, e->count, (double)e->max_ns * 1e-3,
                 e->count > 0 ? (double)e->wg / (double)e->count : 0.0,
                 e->ns > 0 ? (double)e->threads * 1e3 / (double)e->ns : 0.0,
                 e->a.key, e->a.num_instrs, e->a.num_static_cycles,
                 e->a.num_gprs, e->a.max_warps_per_sm, e->a.num_spills_to_mem,
                 e->a.slm_size);
      }
   }
   if (perf->top_dropped > 0)
      fprintf(stderr, "NVK_PERF_TOP dropped=%" PRId64 "\n", perf->top_dropped);
   memset(perf->top, 0, sizeof(perf->top));
   perf->top_dropped = 0;
   simple_mtx_unlock(&perf->mtx);
}

void
nvk_perf_init_cmd_buffer(struct nvk_cmd_buffer *cmd)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const struct nvk_physical_device *pdev = nvk_device_physical(dev);

   cmd->perf_mode = 0;
   cmd->perf_last_kind = NVK_PERF_KIND_END;

   if (!(pdev->debug_flags & NVK_DEBUG_PERF_GPU_TIME))
      return;

   if (cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      return;

   if (!(nvk_cmd_buffer_queue_flags(cmd) &
         (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
      return;

   cmd->perf_mode = (pdev->debug_flags & NVK_DEBUG_PERF_GPU_TIME_FINE) ? 2 : 1;
}

static struct nvk_query_report *
nvk_perf_alloc_report(struct nvk_cmd_buffer *cmd, uint64_t *addr_out)
{
   const uint32_t size = sizeof(struct nvk_query_report);

   if (cmd->perf_mem == NULL ||
       cmd->perf_mem_offset + size > NVK_CMD_MEM_SIZE) {
      struct nvk_cmd_mem *mem;
      if (nvk_cmd_buffer_alloc_mem(cmd, false, &mem) != VK_SUCCESS)
         return NULL;

      util_dynarray_append(&cmd->perf_mems, mem);
      cmd->perf_mem = mem;
      cmd->perf_mem_offset = 0;
   }

   const uint32_t offset = cmd->perf_mem_offset;
   cmd->perf_mem_offset = offset + size;

   *addr_out = cmd->perf_mem->mem->va->addr + offset;
   return (struct nvk_query_report *)((char *)cmd->perf_mem->mem->map + offset);
}

void
nvk_perf_mark_slow(struct nvk_cmd_buffer *cmd, enum nvk_perf_kind kind)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const bool fine = cmd->perf_mode >= 2;

   if (kind == NVK_PERF_KIND_END) {
      if (util_dynarray_num_elements(&cmd->perf_marks,
                                     struct nvk_perf_mark_rec) == 0)
         return;
   } else {
      const bool bar = kind == NVK_PERF_KIND_BAR_WAIT ||
                       kind == NVK_PERF_KIND_BAR_INVAL;
      if (bar && !fine)
         return;
      if (kind == cmd->perf_last_kind &&
          !(fine && (bar || kind == NVK_PERF_KIND_CS ||
                     kind == NVK_PERF_KIND_GFX)))
         return;
   }

   uint64_t addr;
   struct nvk_query_report *report = nvk_perf_alloc_report(cmd, &addr);
   if (report == NULL)
      return;

   report->value = 0;
   report->timestamp = 0;

   uint8_t subc = nvk_cmd_buffer_last_subchannel(cmd);
   if (subc != SUBC_NV9097 && subc != SUBC_NV90C0) {
      subc = (nvk_cmd_buffer_queue_flags(cmd) & VK_QUEUE_GRAPHICS_BIT) ?
             SUBC_NV9097 : SUBC_NV90C0;
   }

   if (subc == SUBC_NV9097) {
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
      P_MTHD(p, NV9097, SET_REPORT_SEMAPHORE_A);
      P_NV9097_SET_REPORT_SEMAPHORE_A(p, addr >> 32);
      P_NV9097_SET_REPORT_SEMAPHORE_B(p, addr);
      P_NV9097_SET_REPORT_SEMAPHORE_C(p, 1);
      P_NV9097_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .release = RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE,
         .pipeline_location = PIPELINE_LOCATION_ALL,
         .structure_size = STRUCTURE_SIZE_FOUR_WORDS,
      });
   } else {
      struct nv_push *p = nvk_cmd_buffer_push(cmd, 5);
      P_MTHD(p, NV90C0, SET_REPORT_SEMAPHORE_A);
      P_NV90C0_SET_REPORT_SEMAPHORE_A(p, addr >> 32);
      P_NV90C0_SET_REPORT_SEMAPHORE_B(p, addr);
      P_NV90C0_SET_REPORT_SEMAPHORE_C(p, 1);
      P_NV90C0_SET_REPORT_SEMAPHORE_D(p, {
         .operation = OPERATION_RELEASE,
         .structure_size = STRUCTURE_SIZE_FOUR_WORDS,
      });
   }

   struct nvk_perf_mark_rec rec = { .report = report, .kind = kind };
   if (fine) {
      if (kind == NVK_PERF_KIND_GFX) {
         const struct nvk_shader *vs = cmd->state.gfx.shaders[MESA_SHADER_VERTEX];
         if (vs == NULL)
            vs = cmd->state.gfx.shaders[MESA_SHADER_MESH];
         nvk_perf_snap(&rec.a, vs);
         nvk_perf_snap(&rec.b, cmd->state.gfx.shaders[MESA_SHADER_FRAGMENT]);
         nvk_perf_rt_snap(&rec.rt, &cmd->state.gfx.render);
      } else if (kind == NVK_PERF_KIND_CS) {
         const struct nvk_shader *cs = cmd->state.cs.shader;
         nvk_perf_snap(&rec.a, cs);
         rec.wg = cmd->perf_wg;
         cmd->perf_wg = 0;
         if (cs != NULL) {
            rec.threads_per_wg = (uint32_t)cs->info.cs.local_size[0] *
                                 cs->info.cs.local_size[1] *
                                 cs->info.cs.local_size[2];
         }
      }
   }
   util_dynarray_append(&cmd->perf_marks, rec);
   cmd->perf_last_kind = kind;
   nvk_perf_add(&dev->perf, NVK_PERF_CTR_MARKS, 1);
}

void
nvk_perf_end_cmd_buffer(struct nvk_cmd_buffer *cmd)
{
   if (cmd->perf_mode == 0)
      return;

   nvk_perf_mark_slow(cmd, NVK_PERF_KIND_END);
}

void
nvk_perf_harvest_cmd_buffer(struct nvk_cmd_buffer *cmd)
{
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);
   const uint32_t n = util_dynarray_num_elements(&cmd->perf_marks,
                                                 struct nvk_perf_mark_rec);

   if (n >= 2) {
      util_dynarray_foreach(&cmd->perf_mems, struct nvk_cmd_mem *, mem)
         nvkmd_mem_sync_map_from_gpu((*mem)->mem, 0, (*mem)->mem->size_B);

      const struct nvk_perf_mark_rec *recs = cmd->perf_marks.data;
      int64_t ns[NVK_PERF_KIND_COUNT] = { 0 };
      int64_t segs[NVK_PERF_KIND_COUNT] = { 0 };
      int64_t hns[NVK_PERF_KIND_COUNT][NVK_PERF_HIST_BUCKETS] = { 0 };
      int64_t hsegs[NVK_PERF_KIND_COUNT][NVK_PERF_HIST_BUCKETS] = { 0 };
      int64_t bad = 0;
      const bool attribute = cmd->perf_mode >= 2;
      const struct nvk_perf_mark_rec *last_cs = NULL;

      if (attribute)
         simple_mtx_lock(&dev->perf.mtx);

      for (uint32_t i = 0; i + 1 < n; i++) {
         const struct nvk_query_report *r0 = recs[i].report;
         const struct nvk_query_report *r1 = recs[i + 1].report;
         if (r0->value == 0 || r1->value == 0 ||
             r1->timestamp < r0->timestamp ||
             r1->timestamp - r0->timestamp > 10000000000ull) {
            bad++;
            continue;
         }
         const uint64_t dt = r1->timestamp - r0->timestamp;
         unsigned b = 0;
         while (b < NVK_PERF_HIST_BUCKETS - 1 && dt >= nvk_perf_hist_limit_ns[b])
            b++;
         ns[recs[i].kind] += dt;
         segs[recs[i].kind]++;
         hns[recs[i].kind][b] += dt;
         hsegs[recs[i].kind][b]++;

         if (!attribute)
            continue;

         switch (recs[i].kind) {
         case NVK_PERF_KIND_GFX:
            last_cs = NULL;
            nvk_perf_top_add(&dev->perf, NVK_PERF_KIND_GFX,
                             &recs[i].a, &recs[i].b, &recs[i].rt, dt, true,
                             0, 0);
            break;
         case NVK_PERF_KIND_CS:
            last_cs = &recs[i];
            nvk_perf_top_add(&dev->perf, NVK_PERF_KIND_CS,
                             &recs[i].a, &recs[i].b, NULL, dt, true,
                             recs[i].wg,
                             (uint64_t)recs[i].wg * recs[i].threads_per_wg);
            break;
         case NVK_PERF_KIND_BAR_WAIT:
            if (last_cs != NULL)
               nvk_perf_top_add(&dev->perf, NVK_PERF_KIND_CS,
                                &last_cs->a, &last_cs->b, NULL, dt, false,
                                0, 0);
            break;
         case NVK_PERF_KIND_BAR_INVAL:
            break;
         default:
            last_cs = NULL;
            break;
         }
      }

      if (attribute)
         simple_mtx_unlock(&dev->perf.mtx);

      for (unsigned k = 0; k < NVK_PERF_KIND_COUNT; k++) {
         if (segs[k] == 0)
            continue;
         p_atomic_add(&dev->perf.gpu_ns[k], ns[k]);
         p_atomic_add(&dev->perf.gpu_segs[k], segs[k]);
         for (unsigned b = 0; b < NVK_PERF_HIST_BUCKETS; b++) {
            if (hsegs[k][b] == 0)
               continue;
            p_atomic_add(&dev->perf.hist_ns[k][b], hns[k][b]);
            p_atomic_add(&dev->perf.hist_segs[k][b], hsegs[k][b]);
         }
      }
      if (bad > 0)
         nvk_perf_add(&dev->perf, NVK_PERF_CTR_BAD_MARKS, bad);
   }

   util_dynarray_clear(&cmd->perf_marks);
   util_dynarray_clear(&cmd->perf_mems);
   cmd->perf_mem = NULL;
   cmd->perf_mem_offset = 0;
   cmd->perf_last_kind = NVK_PERF_KIND_END;
}
