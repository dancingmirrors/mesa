/*
 * Copyright © 2021 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_private.h"

#include <inttypes.h>
#include "genxml/gen_macros.h"
#include "genxml/hasvk_genX_pack.h"
#include "genxml/hasvk_genX_video_pack.h"
#include "util/vl_rbsp.h"

void
genX(CmdBeginVideoCodingKHR) (VkCommandBuffer commandBuffer,
                              const VkVideoBeginCodingInfoKHR * pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_video_session, vid, pBeginInfo->videoSession);
   ANV_FROM_HANDLE(anv_video_session_params, params,
                   pBeginInfo->videoSessionParameters);

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;
}

void
genX(CmdControlVideoCodingKHR) (VkCommandBuffer commandBuffer,
                                const VkVideoCodingControlInfoKHR *
                                pCodingControlInfo)
{

}

void genX(CmdEndVideoCodingKHR) (VkCommandBuffer commandBuffer,
                                 const VkVideoEndCodingInfoKHR *
                                 pEndCodingInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

#if GFX_VER == 8
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.PostSyncOperation = NoWrite;
   }
#elif GFX_VERx10 <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DCFlushEnable = 1;
      pc.RenderTargetCacheFlushEnable = 1;
      pc.VFCacheInvalidationEnable = 1;
      pc.StateCacheInvalidationEnable = 1;
      pc.CommandStreamerStallEnable = 1;
      pc.StallAtPixelScoreboard = 1;
      pc.DepthStallEnable = true;
   };
#endif
   cmd_buffer->video.vid = NULL;
   cmd_buffer->video.params = NULL;
}

/* Parse H.264 slice header to extract slice type and QP delta for long mode */
static void
anv_h264_parse_slice_header(const uint8_t *slice_data, size_t slice_size,
                            const StdVideoH264SequenceParameterSet *sps,
                            const StdVideoH264PictureParameterSet *pps,
                            uint8_t nal_unit_type,
                            uint8_t nal_ref_idc,
                            uint32_t *slice_type,
                            int32_t *slice_qp_delta)
{
   struct vl_vlc vlc;
   struct vl_rbsp rbsp;

   /* Set defaults in case parsing fails */
   *slice_type = 2; /* I slice */
   *slice_qp_delta = 0;

   if (!slice_data || slice_size < 2 || !sps || !pps)
      return;

   /* Initialize VLC reader */
   unsigned vlc_size = (unsigned)slice_size;
   vl_vlc_init(&vlc, 1, (const void *const *)&slice_data, &vlc_size);

   /* Skip NAL unit header (1 byte) */
   if (vl_vlc_valid_bits(&vlc) < 8)
      return;
   vl_vlc_eatbits(&vlc, 8);

   /* Initialize RBSP reader with emulation prevention byte removal */
   vl_rbsp_init(&rbsp, &vlc, ~0, true);

   /* Check if we have enough data before parsing */
   if (vl_vlc_bits_left(&rbsp.nal) < 16)
      return;

   /* Helper macro to check if we have enough bits before reading */
   #define CHECK_BITS(n) do { if (vl_vlc_bits_left(&rbsp.nal) < (n)) return; } while(0)

   /* Parse first_mb_in_slice - ue(v) */
   CHECK_BITS(1);
   vl_rbsp_ue(&rbsp);

   /* Parse slice_type - ue(v) */
   CHECK_BITS(1);
   uint32_t slice_type_raw = vl_rbsp_ue(&rbsp);

   /* Normalize slice type (values 5-9 are the same as 0-4 but indicate all
    * slices in the picture are the same type */
   *slice_type = slice_type_raw % 5;

   /* Parse pic_parameter_set_id - ue(v) */
   CHECK_BITS(1);
   vl_rbsp_ue(&rbsp);

   /* Parse frame_num - u(v) */
   if (sps->log2_max_frame_num_minus4 < 28) {
      uint32_t bits_needed = sps->log2_max_frame_num_minus4 + 4;
      CHECK_BITS(bits_needed);
      vl_rbsp_u(&rbsp, bits_needed);
   }

   /* Handle field pictures */
   if (!sps->flags.frame_mbs_only_flag) {
      CHECK_BITS(1);
      if (vl_rbsp_u(&rbsp, 1)) { /* field_pic_flag */
         CHECK_BITS(1);
         vl_rbsp_u(&rbsp, 1); /* bottom_field_flag */
      }
   }

   /* Parse idr_pic_id for IDR slices */
   if (nal_unit_type == 5) { /* IDR slice */
      CHECK_BITS(1);
      vl_rbsp_ue(&rbsp);
   }

   /* Parse picture order count fields */
   if (sps->pic_order_cnt_type == 0) {
      if (sps->log2_max_pic_order_cnt_lsb_minus4 < 28) {
         uint32_t bits_needed = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
         CHECK_BITS(bits_needed);
         vl_rbsp_u(&rbsp, bits_needed);
      }

      if (pps->flags.bottom_field_pic_order_in_frame_present_flag &&
          !vl_vlc_bits_left(&rbsp.nal))
         return; /* Not enough data */

      if (pps->flags.bottom_field_pic_order_in_frame_present_flag) {
         CHECK_BITS(1);
         vl_rbsp_se(&rbsp); /* delta_pic_order_cnt_bottom */
      }
   }

   if (sps->pic_order_cnt_type == 1 && !sps->flags.delta_pic_order_always_zero_flag) {
      CHECK_BITS(1);
      vl_rbsp_se(&rbsp); /* delta_pic_order_cnt[0] */
      if (pps->flags.bottom_field_pic_order_in_frame_present_flag) {
         CHECK_BITS(1);
         vl_rbsp_se(&rbsp); /* delta_pic_order_cnt[1] */
      }
   }

   /* Parse redundant_pic_cnt if present */
   if (pps->flags.redundant_pic_cnt_present_flag) {
      CHECK_BITS(1);
      vl_rbsp_ue(&rbsp);
   }

   /* Parse direct_spatial_mv_pred_flag for B slices */
   if (*slice_type == 1) { /* B slice */
      CHECK_BITS(1);
      vl_rbsp_u(&rbsp, 1);
   }

   /* Parse num_ref_idx_active_override_flag and related fields */
   if (*slice_type == 0 || *slice_type == 3 || *slice_type == 1) {
      /* P, SP, or B slice */
      CHECK_BITS(1);
      uint32_t num_ref_idx_active_override_flag = vl_rbsp_u(&rbsp, 1);
      if (num_ref_idx_active_override_flag) {
         CHECK_BITS(1);
         vl_rbsp_ue(&rbsp); /* num_ref_idx_l0_active_minus1 */
         if (*slice_type == 1) { /* B slice */
            CHECK_BITS(1);
            vl_rbsp_ue(&rbsp); /* num_ref_idx_l1_active_minus1 */
         }
      }
   }

   /* Parse ref_pic_list_modification for non-I/SI slices */
   if (*slice_type != 2 && *slice_type != 4) {
      /* ref_pic_list_modification_flag_l0 */
      uint32_t ref_pic_list_modification_flag_l0 = vl_rbsp_u(&rbsp, 1);
      if (ref_pic_list_modification_flag_l0) {
         while (true) {
            uint32_t modification_of_pic_nums_idc = vl_rbsp_ue(&rbsp);
            if (modification_of_pic_nums_idc == 3)
               break;
            if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
               vl_rbsp_ue(&rbsp); /* abs_diff_pic_num_minus1 */
            else if (modification_of_pic_nums_idc == 2)
               vl_rbsp_ue(&rbsp); /* long_term_pic_num */
         }
      }
   }

   /* Parse ref_pic_list_modification_flag_l1 for B slices */
   if (*slice_type == 1) {
      uint32_t ref_pic_list_modification_flag_l1 = vl_rbsp_u(&rbsp, 1);
      if (ref_pic_list_modification_flag_l1) {
         while (true) {
            uint32_t modification_of_pic_nums_idc = vl_rbsp_ue(&rbsp);
            if (modification_of_pic_nums_idc == 3)
               break;
            if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
               vl_rbsp_ue(&rbsp); /* abs_diff_pic_num_minus1 */
            else if (modification_of_pic_nums_idc == 2)
               vl_rbsp_ue(&rbsp); /* long_term_pic_num */
         }
      }
   }

   /* Parse pred_weight_table if needed */
   if ((pps->flags.weighted_pred_flag && (*slice_type == 0 || *slice_type == 3)) ||
       (pps->weighted_bipred_idc == 1 && *slice_type == 1)) {
      /* Parse luma_log2_weight_denom */
      vl_rbsp_ue(&rbsp);
      
      /* Parse chroma_log2_weight_denom if chroma is present */
      if (sps->chroma_format_idc != 0)
         vl_rbsp_ue(&rbsp);

      /* Get number of reference pictures for L0 */
      uint32_t num_ref_idx_l0 = pps->num_ref_idx_l0_default_active_minus1 + 1;
      
      /* Parse weights for L0 references */
      for (uint32_t i = 0; i < num_ref_idx_l0; i++) {
         uint32_t luma_weight_flag = vl_rbsp_u(&rbsp, 1);
         if (luma_weight_flag) {
            vl_rbsp_se(&rbsp); /* luma_weight */
            vl_rbsp_se(&rbsp); /* luma_offset */
         }
         if (sps->chroma_format_idc != 0) {
            uint32_t chroma_weight_flag = vl_rbsp_u(&rbsp, 1);
            if (chroma_weight_flag) {
               for (int j = 0; j < 2; j++) {
                  vl_rbsp_se(&rbsp); /* chroma_weight */
                  vl_rbsp_se(&rbsp); /* chroma_offset */
               }
            }
         }
      }

      /* Parse weights for L1 references if B slice */
      if (*slice_type == 1) {
         uint32_t num_ref_idx_l1 = pps->num_ref_idx_l1_default_active_minus1 + 1;
         for (uint32_t i = 0; i < num_ref_idx_l1; i++) {
            uint32_t luma_weight_flag = vl_rbsp_u(&rbsp, 1);
            if (luma_weight_flag) {
               vl_rbsp_se(&rbsp); /* luma_weight */
               vl_rbsp_se(&rbsp); /* luma_offset */
            }
            if (sps->chroma_format_idc != 0) {
               uint32_t chroma_weight_flag = vl_rbsp_u(&rbsp, 1);
               if (chroma_weight_flag) {
                  for (int j = 0; j < 2; j++) {
                     vl_rbsp_se(&rbsp); /* chroma_weight */
                     vl_rbsp_se(&rbsp); /* chroma_offset */
                  }
               }
            }
         }
      }
   }

   /* Parse dec_ref_pic_marking */
   if (nal_ref_idc != 0) {
      if (nal_unit_type == 5) { /* IDR slice */
         vl_rbsp_u(&rbsp, 1); /* no_output_of_prior_pics_flag */
         vl_rbsp_u(&rbsp, 1); /* long_term_reference_flag */
      } else {
         uint32_t adaptive_ref_pic_marking_mode_flag = vl_rbsp_u(&rbsp, 1);
         if (adaptive_ref_pic_marking_mode_flag) {
            while (true) {
               uint32_t memory_management_control_operation = vl_rbsp_ue(&rbsp);
               if (memory_management_control_operation == 0)
                  break;
               if (memory_management_control_operation == 1 ||
                   memory_management_control_operation == 3)
                  vl_rbsp_ue(&rbsp); /* difference_of_pic_nums_minus1 */
               if (memory_management_control_operation == 2)
                  vl_rbsp_ue(&rbsp); /* long_term_pic_num */
               if (memory_management_control_operation == 3 ||
                   memory_management_control_operation == 6)
                  vl_rbsp_ue(&rbsp); /* long_term_frame_idx */
               if (memory_management_control_operation == 4)
                  vl_rbsp_ue(&rbsp); /* max_long_term_frame_idx_plus1 */
            }
         }
      }
   }

   /* Parse cabac_init_idc for CABAC mode */
   if (pps->flags.entropy_coding_mode_flag &&
       *slice_type != 2 && *slice_type != 4) { /* Not I or SI slice */
      CHECK_BITS(1);
      vl_rbsp_ue(&rbsp); /* cabac_init_idc */
   }

   /* Finally, parse slice_qp_delta */
   CHECK_BITS(1);
   *slice_qp_delta = vl_rbsp_se(&rbsp);
   
   #undef CHECK_BITS
}

static uint32_t
anv_h264_get_slice_type(uint32_t type)
{
   /* Mapping from H.264 spec slice types to hardware values */
   switch(type % 5) {
      case 0: /* P_SLICE */
         return 0;
      case 1: /* B_SLICE */
         return 1;
      case 2: /* I_SLICE */
         return 2;
      case 3: /* SP_SLICE */
         return 0; /* Treat as P */
      case 4: /* SI_SLICE */
         return 2; /* Treat as I */
      default:
         return 0;
   }
}

static void
anv_h264_decode_video(struct anv_cmd_buffer *cmd_buffer,
                      const VkVideoDecodeInfoKHR *frame_info)
{
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   struct anv_video_session *vid = cmd_buffer->video.vid;
   struct anv_video_session_params *params = cmd_buffer->video.params;
   const struct VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext,
                           VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   const StdVideoH264SequenceParameterSet *sps =
      vk_video_find_h264_dec_std_sps(&params->vk,
                                     h264_pic_info->
                                     pStdPictureInfo->seq_parameter_set_id);
   const StdVideoH264PictureParameterSet *pps =
      vk_video_find_h264_dec_std_pps(&params->vk,
                                     h264_pic_info->
                                     pStdPictureInfo->pic_parameter_set_id);

   uint8_t dpb_slots[ANV_VIDEO_H264_MAX_DPB_SLOTS] = { 0, };

   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      int idx = frame_info->pReferenceSlots[i].slotIndex;
      if (idx < 0)
         continue;

      assert(idx < ANV_VIDEO_H264_MAX_DPB_SLOTS);
      dpb_slots[idx] = i;
   }

#if GFX_VER == 8
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   }
#elif GFX_VERx10 <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.CommandStreamerStallEnable = 1;
   }
#endif

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_MODE_SELECT), sel) {
      sel.StandardSelect = SS_AVC;
      sel.CodecSelect = Decode;
      sel.DecoderShortFormatMode = LongFormatDriverInterface;
      sel.DecoderModeSelect = VLDMode;
      /* One or the other *must* be set on HSW. */
      sel.PreDeblockingOutputEnable = 0;
      sel.PostDeblockingOutputEnable = 1;
      sel.StreamOutEnable = 0;
   }

   const struct anv_image_view *iv =
      anv_image_view_from_handle(frame_info->
                                 dstPictureResource.imageViewBinding);
   const struct anv_image *img = iv->image;

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_SURFACE_STATE), ss) {
      ss.Width = img->vk.extent.width - 1;

      ss.Height = img->vk.extent.height - 1;
      ss.SurfaceFormat = PLANAR_420_8;
      ss.InterleaveChroma = 1;
      ss.SurfacePitch = img->planes[0].primary_surface.isl.row_pitch_B - 1;
      ss.TiledSurface = img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      ss.TileWalk = TW_YMAJOR;
      
      /* Calculate chroma plane Y offset from ISL surface layout.
       * For NV12 format, planes[1] contains the interleaved U/V chroma plane.
       * The Y offset must be in units of rows, calculated from the plane's
       * memory offset divided by the luma plane's row pitch.
       */
      if (img->n_planes > 1) {
         ss.YOffsetforUCb =
            img->planes[1].primary_surface.memory_range.offset /
            img->planes[0].primary_surface.isl.row_pitch_B;
#if GFX_VERx10 >= 75
         ss.YOffsetforVCr = ss.YOffsetforUCb;
#endif
      } else {
         /* Fallback for single-plane layout (shouldn't normally happen for NV12) */
         ss.YOffsetforUCb = align(img->vk.extent.height, 32);
#if GFX_VERx10 >= 75
         ss.YOffsetforVCr = align(img->vk.extent.height, 32);
#endif
      }
#if GFX_VERx10 == 70
      ss.XOffsetforVCr = 0;
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_BUF_ADDR_STATE), buf) {
      struct anv_address dest_addr = anv_image_address(img,
                                                       &img->planes
                                                       [0].
                                                       primary_surface.
                                                       memory_range);
      buf.PreDeblockingDestinationAddress = dest_addr;
      buf.PostDeblockingDestinationAddress = dest_addr;
#if GFX_VERx10 >= 75
      buf.PreDeblockingDestinationMOCS =
         anv_mocs(cmd_buffer->device, buf.PreDeblockingDestinationAddress.bo,
                  0);
      buf.PostDeblockingDestinationMOCS =
         anv_mocs(cmd_buffer->device, buf.PostDeblockingDestinationAddress.bo,
                  0);
      buf.OriginalUncompressedPictureSourceMOCS =
         anv_mocs(cmd_buffer->device, NULL, 0);
      buf.StreamOutDataDestinationMOCS =
         anv_mocs(cmd_buffer->device, NULL, 0);
#endif

#if GFX_VER == 8
      buf.IntraRowStoreScratchBufferAddressHigh = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].offset
      };
      buf.IntraRowStoreScratchBufferMOCS =
         anv_mocs(cmd_buffer->device,
                  buf.IntraRowStoreScratchBufferAddressHigh.bo, 0);
      buf.DeblockingFilterRowStoreScratchAddressHigh = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset
      };
#else
      buf.IntraRowStoreScratchBufferAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].offset
      };
#if GFX_VERx10 >= 75
      buf.IntraRowStoreScratchBufferMOCS =
         anv_mocs(cmd_buffer->device,
                  vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo, 0);
#elif GFX_VERx10 == 70
      buf.PostDeblockingDestinationCacheabilityControl = 1;
      buf.PostDeblockingDestinationGraphicsDataType = 1;

      uint32_t intra_mocs = anv_mocs(cmd_buffer->device,
                                     vid->vid_mem
                                     [ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->
                                     bo, 0);
      buf.IntraRowStoreScratchBufferCacheabilityControl = intra_mocs & 0x3;
      buf.IntraRowStoreScratchBufferGraphicsDataType =
         (intra_mocs >> 2) & 0x1;
#endif
#if GFX_VERx10 == 70
      buf.DeblockingFilterRowStoreScratchBufferAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset
      };
#else
      buf.DeblockingFilterRowStoreScratchAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset
      };
#endif
#endif
#if GFX_VERx10 == 75
      buf.DeblockingFilterRowStoreScratchMOCS =
         anv_mocs(cmd_buffer->device,
                  vid->
                  vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo,
                  0);
      buf.MBStatusBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.MBILDBStreamOutBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif
#if GFX_VERx10 == 80
      struct anv_bo *ref_bo = NULL;
#endif
      struct anv_address ref0_addr = { 0 };
      if (frame_info->referenceSlotCount > 0) {
         const struct anv_image_view *ref_iv =
            anv_image_view_from_handle(frame_info->
                                       pReferenceSlots[0].pPictureResource->
                                       imageViewBinding);
         ref0_addr =
            anv_image_address(ref_iv->image,
                              &ref_iv->image->planes[0].primary_surface.
                              memory_range);
#if GFX_VERx10 == 80
         ref_bo = ref_iv->image->bindings[0].address.bo;
#endif
      }

      unsigned i = 0;
      for (i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct anv_image_view *ref_iv =
            anv_image_view_from_handle(frame_info->
                                       pReferenceSlots[i].pPictureResource->
                                       imageViewBinding);
         buf.ReferencePictureAddress[i] =
            anv_image_address(ref_iv->image,
                              &ref_iv->
                              image->planes[0].primary_surface.memory_range);
      }

      for (; i < ANV_VIDEO_H264_MAX_DPB_SLOTS; i++)
         buf.ReferencePictureAddress[i] = ref0_addr;
#if GFX_VERx10 == 80
      buf.ReferencePictureMOCS = anv_mocs(cmd_buffer->device, ref_bo, 0);
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_IND_OBJ_BASE_ADDR_STATE),
                  index_obj) {
      index_obj.MFXIndirectBitstreamObjectAddress =
         anv_address_add(src_buffer->address,
                         frame_info->srcBufferOffset & ~4095);
#if GFX_VERx10 == 75
      index_obj.MFXIndirectBitstreamObjectMOCS =
         anv_mocs(cmd_buffer->device, src_buffer->address.bo, 0);
      index_obj.MFXIndirectMVObjectMOCS =
         anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFDIndirectITCOEFFObjectMOCS =
         anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFDIndirectITDBLKObjectMOCS =
         anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFCIndirectPAKBSEObjectMOCS =
         anv_mocs(cmd_buffer->device, NULL, 0);
#endif
#if GFX_VER == 7
      index_obj.MFXIndirectBitstreamObjectAccessUpperBound =
         (struct anv_address) { NULL, 0x80000000 };
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_BSP_BUF_BASE_ADDR_STATE), bsp) {
      bsp.BSDMPCRowStoreScratchBufferAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].offset
      };
#if GFX_VERx10 == 75
      bsp.BSDMPCRowStoreScratchBufferMOCS =
         anv_mocs(cmd_buffer->device,
                  vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo,
                  0);
#endif
      bsp.MPRRowStoreScratchBufferAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].offset
      };
#if GFX_VERx10 == 75
      bsp.MPRRowStoreScratchBufferMOCS =
         anv_mocs(cmd_buffer->device,
                  vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo, 0);
      bsp.BitplaneReadBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif
   }

   uint32_t pic_height = sps->pic_height_in_map_units_minus1 + 1;
   if (!sps->flags.frame_mbs_only_flag)
      pic_height *= 2;

   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "  Frame: %ux%u MBs (width_minus1=%u, height=%u)\n",
              sps->pic_width_in_mbs_minus1 + 1, pic_height,
              sps->pic_width_in_mbs_minus1, pic_height - 1);
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_IMG_STATE), avc_img) {
      avc_img.FrameWidth = sps->pic_width_in_mbs_minus1;
      avc_img.FrameHeight = pic_height - 1;
      avc_img.FrameSize = (sps->pic_width_in_mbs_minus1 + 1) * pic_height;

      if (!h264_pic_info->pStdPictureInfo->flags.field_pic_flag)
         avc_img.ImageStructure = FramePicture;
      else if (h264_pic_info->pStdPictureInfo->flags.bottom_field_flag)
         avc_img.ImageStructure = BottomFieldPicture;
      else
         avc_img.ImageStructure = TopFieldPicture;

      bool is_intra_picture = h264_pic_info->pStdPictureInfo->flags.is_intra;

      if (is_intra_picture) {
         avc_img.WeightedBiPredictionIDC = 0;
         avc_img.WeightedPredictionEnable = 0;
      }
      else {
         avc_img.WeightedBiPredictionIDC = pps->weighted_bipred_idc;
         avc_img.WeightedPredictionEnable = pps->flags.weighted_pred_flag;
      }
      avc_img.FirstChromaQPOffset = pps->chroma_qp_index_offset;
      avc_img.SecondChromaQPOffset = pps->second_chroma_qp_index_offset;
      avc_img.FieldPicture =
         h264_pic_info->pStdPictureInfo->flags.field_pic_flag;
      avc_img.MBAFFMode = (sps->flags.mb_adaptive_frame_field_flag
                           && !h264_pic_info->pStdPictureInfo->
                           flags.field_pic_flag);
      avc_img.FrameMBOnly = sps->flags.frame_mbs_only_flag;
      avc_img._8x8IDCTTransformMode = pps->flags.transform_8x8_mode_flag;
      avc_img.Direct8x8Inference = sps->flags.direct_8x8_inference_flag;
      avc_img.ConstrainedIntraPrediction =
         pps->flags.constrained_intra_pred_flag;
      avc_img.NonReferencePicture =
         !h264_pic_info->pStdPictureInfo->flags.is_reference;
      avc_img.EntropyCodingSyncEnable = pps->flags.entropy_coding_mode_flag;
      avc_img.ChromaFormatIDC = sps->chroma_format_idc;
      avc_img.TrellisQuantizationChromaDisable = true;
      avc_img.NumberofReferenceFrames = frame_info->referenceSlotCount;
      avc_img.NumberofActiveReferencePicturesfromL0 =
         pps->num_ref_idx_l0_default_active_minus1 + 1;
      avc_img.NumberofActiveReferencePicturesfromL1 =
         pps->num_ref_idx_l1_default_active_minus1 + 1;
      avc_img.InitialQPValue = pps->pic_init_qp_minus26;
      avc_img.PicOrderPresent =
         pps->flags.bottom_field_pic_order_in_frame_present_flag;
      avc_img.DeltaPicOrderAlwaysZero =
         sps->flags.delta_pic_order_always_zero_flag;
      avc_img.PicOrderCountType = sps->pic_order_cnt_type;
      avc_img.DeblockingFilterControlPresent =
         pps->flags.deblocking_filter_control_present_flag;
      avc_img.RedundantPicCountPresent =
         pps->flags.redundant_pic_cnt_present_flag;
      avc_img.Log2MaxFrameNumber = sps->log2_max_frame_num_minus4;
      avc_img.Log2MaxPicOrderCountLSB =
         sps->log2_max_pic_order_cnt_lsb_minus4;
      avc_img.CurrentPictureFrameNumber =
         h264_pic_info->pStdPictureInfo->frame_num;
   }

   if (pps->flags.pic_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] =
                  pps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] =
                  pps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] =
                  pps->pScalingLists->ScalingList8x8[0][q];
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] =
                  pps->pScalingLists->ScalingList8x8[3][q];
         }
      }
   }
   else if (sps->flags.seq_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] =
                  sps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] =
                  sps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] =
                  sps->pScalingLists->ScalingList8x8[0][q];
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] =
                  sps->pScalingLists->ScalingList8x8[3][q];
         }
      }
   }
   else {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned q = 0; q < 3 * 16; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned q = 0; q < 3 * 16; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = 0x10;
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = 0x10;
         }
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_DPB_STATE), avc_dpb) {
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
            vk_find_struct_const(frame_info->pReferenceSlots[i].pNext,
                                 VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
         const StdVideoDecodeH264ReferenceInfo *ref_info =
            dpb_slot->pStdReferenceInfo;
         int slot_idx = frame_info->pReferenceSlots[i].slotIndex;

         if (slot_idx < 0)
            continue;

         int idx = dpb_slots[slot_idx];

         avc_dpb.NonExistingFrame[idx] = ref_info->flags.is_non_existing;
         avc_dpb.LongTermFrame[idx] =
            ref_info->flags.used_for_long_term_reference;
         if (!ref_info->flags.top_field_flag
             && !ref_info->flags.bottom_field_flag)
            avc_dpb.UsedforReference[idx] = 3;
         else
            avc_dpb.UsedforReference[idx] =
               ref_info->flags.top_field_flag | (ref_info->
                                                 flags.bottom_field_flag <<
                                                 1);
         avc_dpb.LTSTFrameNumberList[idx] = ref_info->FrameNum;
      }
   }

#if GFX_VERx10 >= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_PICID_STATE), picid) {
      unsigned i = 0;
      picid.PictureIDRemappingDisable = false;

      for (i = 0; i < frame_info->referenceSlotCount; i++)
         picid.PictureID[i] = frame_info->pReferenceSlots[i].slotIndex;

      for (; i < ANV_VIDEO_H264_MAX_NUM_REF_FRAME; i++)
         picid.PictureID[i] = 0xffff;
   }
#endif

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_DIRECTMODE_STATE),
                  avc_directmode) {
#if GFX_VERx10 == 70
      for (int i = 0; i < 34; i++) {
         avc_directmode.POCList[i] = 0;
      }
      avc_directmode.DirectMVBufferArbitrationPriorityControl[0] = 0;
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
      struct anv_bo *dmv_bo = NULL;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         int slot_idx = frame_info->pReferenceSlots[i].slotIndex;

         if (slot_idx < 0)
            continue;

         int idx = dpb_slots[slot_idx];

         const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
            vk_find_struct_const(frame_info->pReferenceSlots[i].pNext,
                                 VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
         const struct anv_image_view *ref_iv =
            anv_image_view_from_handle(frame_info->
                                       pReferenceSlots[i].pPictureResource->
                                       imageViewBinding);
         const StdVideoDecodeH264ReferenceInfo *ref_info =
            dpb_slot->pStdReferenceInfo;

         if (i == 0) {
            dmv_bo = anv_image_address(ref_iv->image,
                                       &ref_iv->image->
                                       vid_dmv_top_surface).bo;
         }
#if GFX_VERx10 == 70
         /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
         uint32_t dmv_read_mocs = anv_mocs(cmd_buffer->device, ref_iv->image->bindings[0].address.bo, 0);
         
         avc_directmode.DirectMVBufferAddress[idx * 2] =
            anv_image_address(ref_iv->image,
                              &ref_iv->image->vid_dmv_top_surface);
         avc_directmode.DirectMVBufferCacheabilityControl[idx * 2] = dmv_read_mocs & 0x3;
         avc_directmode.DirectMVBufferGraphicsDataType[idx * 2] = (dmv_read_mocs >> 2) & 0x1;
         
         avc_directmode.DirectMVBufferAddress[idx * 2 + 1] =
            anv_image_address(ref_iv->image,
                              &ref_iv->image->vid_dmv_top_surface);
         avc_directmode.DirectMVBufferCacheabilityControl[idx * 2 + 1] = dmv_read_mocs & 0x3;
         avc_directmode.DirectMVBufferGraphicsDataType[idx * 2 + 1] = (dmv_read_mocs >> 2) & 0x1;
#endif
#if GFX_VERx10 == 75
         if (idx == 0)
            avc_directmode.DirectMVBuffer0Address =
               anv_image_address(ref_iv->image,
                                 &ref_iv->image->vid_dmv_top_surface);
         else
            avc_directmode.DirectMVBufferAddress1[idx - 1] =
               anv_image_address(ref_iv->image,
                                 &ref_iv->image->vid_dmv_top_surface);
#endif
         avc_directmode.POCList[2 * idx] = ref_info->PicOrderCnt[0];
         avc_directmode.POCList[2 * idx + 1] = ref_info->PicOrderCnt[1];
      }

#if GFX_VERx10 == 70
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t dmv_write_mocs = anv_mocs(cmd_buffer->device, img->bindings[0].address.bo, 0);
      
      avc_directmode.DirectMVBufferWriteAddress[0] =
         anv_image_address(img, &img->vid_dmv_top_surface);
      avc_directmode.DirectMVBufferWriteCacheabilityControl[0] = dmv_write_mocs & 0x3;
      avc_directmode.DirectMVBufferWriteGraphicsDataType[0] = (dmv_write_mocs >> 2) & 0x1;
      
      avc_directmode.DirectMVBufferWriteAddress[1] =
         anv_image_address(img, &img->vid_dmv_top_surface);
      avc_directmode.DirectMVBufferWriteCacheabilityControl[1] = dmv_write_mocs & 0x3;
      avc_directmode.DirectMVBufferWriteGraphicsDataType[1] = (dmv_write_mocs >> 2) & 0x1;
#else
      avc_directmode.DirectMVBufferWriteAddress =
         anv_image_address(img, &img->vid_dmv_top_surface);
#if GFX_VERx10 == 75
      avc_directmode.DirectMVBufferMOCS =
         anv_mocs(cmd_buffer->device, dmv_bo, 0);
      avc_directmode.DirectMVBufferWriteMOCS =
         anv_mocs(cmd_buffer->device,
                  avc_directmode.DirectMVBufferWriteAddress.bo, 0);
#endif
#endif

      avc_directmode.POCList[32] =
         h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
      avc_directmode.POCList[33] =
         h264_pic_info->pStdPictureInfo->PicOrderCnt[1];
   }

   uint32_t buffer_offset = frame_info->srcBufferOffset & 4095;

   if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
      fprintf(stderr, "H.264 Decode: %u slices, src_buffer=%p, bo=%p, mapped=%s\n",
              h264_pic_info->sliceCount, 
              (void*)src_buffer,
              src_buffer->address.bo,
              (src_buffer->address.bo && src_buffer->address.bo->map) ? "yes" : "no");
   }

   /* H.264 NAL units have a 3-byte start code prefix (0x000001) or 4-byte (0x00000001)
    * followed by a 1-byte NAL header. The hardware expects to start decoding after
    * the NAL header, so we skip the first 3 bytes of each slice.
    */
#define HEADER_OFFSET 3

   /* Map the bitstream buffer to access slice header data.
    * In long format mode, we need to parse slice headers to extract parameters.
    */
   void *buffer_map = NULL;
   bool needs_unmap = false;
   
   if (src_buffer->address.bo) {
      if (src_buffer->address.bo->map) {
         /* Buffer is already mapped */
         buffer_map = src_buffer->address.bo->map;
      } else {
         /* Buffer is not mapped, map it temporarily */
         VkResult result = anv_device_map_bo(cmd_buffer->device,
                                            src_buffer->address.bo,
                                            0,
                                            src_buffer->address.bo->size,
                                            0, &buffer_map);
         if (result == VK_SUCCESS) {
            needs_unmap = true;
            if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
               fprintf(stderr, "Mapped bitstream buffer: bo=%p, size=%lu, map=%p\n",
                      (void*)src_buffer->address.bo,
                      (unsigned long)src_buffer->address.bo->size,
                      buffer_map);
            }
         } else {
            if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
               fprintf(stderr, "Failed to map bitstream buffer for slice parsing\n");
            }
         }
      }
   }

   for (unsigned s = 0; s < h264_pic_info->sliceCount; s++) {
      bool last_slice = s == (h264_pic_info->sliceCount - 1);
      uint32_t current_offset = h264_pic_info->pSliceOffsets[s];

      uint32_t slice_data_size;
      if (last_slice)
          slice_data_size = frame_info->srcBufferRange - current_offset;
      else
          slice_data_size = h264_pic_info->pSliceOffsets[s+1] - current_offset;

      /* Default values in case buffer is not accessible */
      uint32_t slice_type = h264_pic_info->pStdPictureInfo->flags.is_intra ? 2 : 0;
      int32_t slice_qp_delta = 0;
      uint8_t nal_unit_type = 1; /* Non-IDR slice by default */
      uint8_t nal_ref_idc = 0;

      /* Parse slice header from the buffer if it's accessible */
      if (buffer_map && slice_data_size > 4) {
         const uint8_t *slice_data = (const uint8_t *)buffer_map +
                                      src_buffer->address.offset +
                                      frame_info->srcBufferOffset + current_offset;

         /* Skip start code prefix to find NAL header
          * H.264 NAL units start with 0x000001 (3-byte) or 0x00000001 (4-byte)
          * The NAL header is the byte immediately after the start code */
         const uint8_t *nal_start = slice_data;
         size_t nal_size = slice_data_size;
         
         /* Check for 3-byte start code (0x000001) */
         if (slice_data_size >= 4 && 
             slice_data[0] == 0x00 && slice_data[1] == 0x00 && slice_data[2] == 0x01) {
            nal_start = slice_data + 3;
            nal_size = slice_data_size - 3;
         }
         /* Check for 4-byte start code (0x00000001) */
         else if (slice_data_size >= 5 &&
                  slice_data[0] == 0x00 && slice_data[1] == 0x00 && 
                  slice_data[2] == 0x00 && slice_data[3] == 0x01) {
            nal_start = slice_data + 4;
            nal_size = slice_data_size - 4;
         }

         if (nal_size > 0) {
            /* Extract NAL unit type and ref_idc from NAL header
             * NAL header format: forbidden_zero_bit(1) | nal_ref_idc(2) | nal_unit_type(5) */
            uint8_t nal_header = nal_start[0];
            nal_ref_idc = (nal_header >> 5) & 0x3;
            nal_unit_type = nal_header & 0x1F;

            /* Parse the slice header */
            anv_h264_parse_slice_header(nal_start, nal_size,
                                       sps, pps,
                                       nal_unit_type, nal_ref_idc,
                                       &slice_type, &slice_qp_delta);
         }
      }

      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Slice[%d]: offset=%u, size=%u, type=%u, qp_delta=%d, nal_type=%u\n",
                 s, buffer_offset + current_offset, slice_data_size,
                 slice_type, slice_qp_delta, nal_unit_type);
      }

      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_SLICE_STATE), slice) {
         slice.SliceType = anv_h264_get_slice_type(slice_type);
         slice.Log2WeightDenominatorLuma = 0;
         slice.Log2WeightDenominatorChroma = 0;
         slice.CABACInitIDC = 0;
         slice.NumberofReferencePicturesinInterpredictionList0 =
            pps->num_ref_idx_l0_default_active_minus1 + 1;
         if (slice.SliceType == 1 /* B Slice */)
            slice.NumberofReferencePicturesinInterpredictionList1 =
               pps->num_ref_idx_l1_default_active_minus1 + 1;
         slice.SliceQuantizationParameter = pps->pic_init_qp_minus26 + 26
                                            + slice_qp_delta;
         slice.SliceHorizontalPosition = 0;
#if GFX_VERx10 >= 75
         slice.SliceVerticalPosition = 0;
         slice.LastSliceGroup = last_slice;
#endif
      }

      anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_BSD_OBJECT), avc_bsd) {
         avc_bsd.IndirectBSDDataLength = slice_data_size - HEADER_OFFSET;
         /* Start decoding after the 3-byte NAL header */
         avc_bsd.IndirectBSDDataStartAddress = buffer_offset + current_offset + HEADER_OFFSET;
      };
   }

   /* Unmap the buffer if we mapped it */
   if (needs_unmap && buffer_map) {
      anv_device_unmap_bo(cmd_buffer->device, src_buffer->address.bo,
                         buffer_map, src_buffer->address.bo->size);
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "Unmapped bitstream buffer\n");
      }
   }
#undef HEADER_OFFSET
}

void
genX(CmdDecodeVideoKHR) (VkCommandBuffer commandBuffer,
                         const VkVideoDecodeInfoKHR * frame_info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   switch (cmd_buffer->video.vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      anv_h264_decode_video(cmd_buffer, frame_info);
      break;
   default:
      assert(0);
   }
}
