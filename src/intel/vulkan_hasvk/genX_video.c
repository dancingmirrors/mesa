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

#if GFX_VER > 8
   UNREACHABLE("Unsupported hardware.");
#endif
#if GFX_VER == 8
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.PostSyncOperation = NoWrite;
   }
#elif GFX_VER <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DCFlushEnable = 1;
      pc.RenderTargetCacheFlushEnable = 1;
      pc.VFCacheInvalidationEnable = 1;
      pc.StateCacheInvalidationEnable = 1;
      pc.CommandStreamerStallEnable = 1;
      pc.StallAtPixelScoreboard = 1;

   };
#else
   UNREACHABLE("Unsupported hardware.");
#endif

#if GFX_VER <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), wait) {
      wait.MFXSyncControlFlag = 1;
   }
#endif

   cmd_buffer->video.vid = NULL;
   cmd_buffer->video.params = NULL;
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
                                     h264_pic_info->pStdPictureInfo->
                                     seq_parameter_set_id);
   const StdVideoH264PictureParameterSet *pps =
      vk_video_find_h264_dec_std_pps(&params->vk,
                                     h264_pic_info->pStdPictureInfo->
                                     pic_parameter_set_id);

   uint8_t dpb_slots[ANV_VIDEO_H264_MAX_DPB_SLOTS] = { 0, };

   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      int idx = frame_info->pReferenceSlots[i].slotIndex;
      if (idx < 0)
         continue;

      assert(idx < ANV_VIDEO_H264_MAX_DPB_SLOTS);
      dpb_slots[idx] = i;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_MODE_SELECT), sel) {
      sel.StandardSelect = SS_AVC;
      sel.CodecSelect = Decode;
      sel.DecoderShortFormatMode = ShortFormatDriverInterface;
      sel.DecoderModeSelect = VLDMode;  // Hardcoded

      sel.PreDeblockingOutputEnable = 0;
      sel.PostDeblockingOutputEnable = 1;
   }

   const struct anv_image_view *iv =
      anv_image_view_from_handle(frame_info->dstPictureResource.
                                 imageViewBinding);
   const struct anv_image *img = iv->image;

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_SURFACE_STATE), ss) {
      ss.Width = img->vk.extent.width - 1;

      ss.Height = img->vk.extent.height - 1;
      ss.SurfaceFormat = PLANAR_420_8;  // assert on this?
      ss.InterleaveChroma = 1;
      ss.SurfacePitch = img->planes[0].primary_surface.isl.row_pitch_B - 1;
      ss.TiledSurface =
         img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      ss.TileWalk = TW_YMAJOR;

#if GFX_VERx10 == 70
      ss.XOffsetforVCr = 0;
      ss.YOffsetforUCb = align(img->vk.extent.height, 16);
      ss.YOffsetforVCr = align(img->vk.extent.height, 16);
#else
      ss.YOffsetforUCb = align(img->vk.extent.height, 32);
      ss.YOffsetforVCr = align(img->vk.extent.height, 32);
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_BUF_ADDR_STATE), buf) {
      struct anv_address dest_addr = anv_image_address(img,
                                                       &img->planes
                                                       [0].primary_surface.
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
      uint32_t intra_mocs = anv_mocs(cmd_buffer->device,
                                     vid->vid_mem
                                     [ANV_VID_MEM_H264_INTRA_ROW_STORE].
                                     mem->bo, 0);
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
                  vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].
                  mem->bo, 0);
      buf.MBStatusBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.MBILDBStreamOutBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif

#if GFX_VERx10 == 80
      struct anv_bo *ref_bo = NULL;
#endif
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct anv_image_view *ref_iv =
            anv_image_view_from_handle(frame_info->pReferenceSlots[i].
                                       pPictureResource->imageViewBinding);

         buf.ReferencePictureAddress[i] = anv_image_address(ref_iv->image,
                                                            &ref_iv->image->
                                                            planes[0].
                                                            primary_surface.
                                                            memory_range);

#if GFX_VERx10 == 80
         if (i == 0)
            ref_bo = ref_iv->image->bindings[0].address.bo;
#endif
      }
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

#if GFX_VERx10 == 70
   /* Ivy Bridge PRM: FrameHeightInMbs = (2 - frame_mbs_only_flag) * PicHeightInMapUnits
    * PicHeightInMbs = FrameHeightInMbs / (1 + field_pic_flag)
    * For Ivy Bridge, we need to account for field_pic_flag in the height calculation.
    */
   uint32_t frame_height_in_mbs = pic_height;
   uint32_t pic_width_in_mbs = sps->pic_width_in_mbs_minus1 + 1;
   if (h264_pic_info->pStdPictureInfo->flags.field_pic_flag) {
      frame_height_in_mbs = pic_height / 2;
   }
#endif

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_IMG_STATE), avc_img) {
      avc_img.FrameWidth = sps->pic_width_in_mbs_minus1;
#if GFX_VERx10 == 70
      avc_img.FrameHeight = frame_height_in_mbs - 1;
      avc_img.FrameSize = pic_width_in_mbs * frame_height_in_mbs;
#else
      avc_img.FrameHeight = pic_height - 1;
      avc_img.FrameSize = (sps->pic_width_in_mbs_minus1 + 1) * pic_height;
#endif

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
                           && !h264_pic_info->pStdPictureInfo->flags.
                           field_pic_flag);
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
               ref_info->flags.top_field_flag | (ref_info->flags.
                                                 bottom_field_flag << 1);
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
            anv_image_view_from_handle(frame_info->pReferenceSlots[i].
                                       pPictureResource->imageViewBinding);
         const StdVideoDecodeH264ReferenceInfo *ref_info =
            dpb_slot->pStdReferenceInfo;

         if (i == 0) {
            dmv_bo = anv_image_address(ref_iv->image,
                                       &ref_iv->image->vid_dmv_top_surface).
               bo;
         }

#if GFX_VERx10 == 70
         avc_directmode.DirectMVBufferAddress[idx * 2] =
            anv_image_address(ref_iv->image,
                              &ref_iv->image->vid_dmv_top_surface);
         avc_directmode.DirectMVBufferAddress[idx * 2 + 1] =
            anv_image_address(ref_iv->image,
                              &ref_iv->image->vid_dmv_top_surface);
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
      avc_directmode.DirectMVBufferWriteAddress[0] = anv_image_address(img,
                                                                       &img->
                                                                       vid_dmv_top_surface);
      avc_directmode.DirectMVBufferWriteAddress[1] =
         anv_image_address(img, &img->vid_dmv_top_surface);
#else
      avc_directmode.DirectMVBufferWriteAddress = anv_image_address(img,
                                                                    &img->
                                                                    vid_dmv_top_surface);
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

#define HEADER_OFFSET 3

   uint32_t buffer_offset = frame_info->srcBufferOffset & 4095;
#define HEADER_OFFSET 3

#if GFX_VER <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DCFlushEnable = 1;
      pc.RenderTargetCacheFlushEnable = 1;
      pc.VFCacheInvalidationEnable = 1;
      pc.StateCacheInvalidationEnable = 1;
      pc.CommandStreamerStallEnable = 1;
      pc.StallAtPixelScoreboard = 1;
   };
#endif

   for (unsigned s = 0; s < h264_pic_info->sliceCount; s++) {
      bool last_slice = s == (h264_pic_info->sliceCount - 1);
      uint32_t current_offset = h264_pic_info->pSliceOffsets[s];
      uint32_t this_end;
      if (!last_slice) {
#if GFX_VER <= 75
         anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
            pc.DCFlushEnable = 1;
            pc.RenderTargetCacheFlushEnable = 1;
            pc.VFCacheInvalidationEnable = 1;
            pc.StateCacheInvalidationEnable = 1;
            pc.CommandStreamerStallEnable = 1;
            pc.StallAtPixelScoreboard = 1;
         }
#endif
         uint32_t next_offset = h264_pic_info->pSliceOffsets[s + 1];
         uint32_t next_end = h264_pic_info->pSliceOffsets[s + 2];
         if (s == h264_pic_info->sliceCount - 2)
            next_end = frame_info->srcBufferRange;
         anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_SLICEADDR),
                        sliceaddr) {
            sliceaddr.IndirectBSDDataLength =
               next_end - next_offset - HEADER_OFFSET;
            sliceaddr.IndirectBSDDataStartAddress =
               buffer_offset + next_offset + HEADER_OFFSET;
         };
         this_end = next_offset;
      }
      else
         this_end = frame_info->srcBufferRange;
      anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_BSD_OBJECT), avc_bsd) {
         avc_bsd.IndirectBSDDataLength =
            this_end - current_offset - HEADER_OFFSET;
         avc_bsd.IndirectBSDDataStartAddress =
            buffer_offset + current_offset + HEADER_OFFSET;
         avc_bsd.InlineData.LastSlice = last_slice;
         avc_bsd.InlineData.FixPrevMBSkipped = 1;
#if GFX_VERx10 == 70
         avc_bsd.InlineData.MBHeaderErrorHandling = 1;
         avc_bsd.InlineData.EntropyErrorHandling = 1;
         avc_bsd.InlineData.MPRErrorHandling = 1;
         avc_bsd.InlineData.BSDPrematureCompleteErrorHandling = 1;
         avc_bsd.InlineData.MBErrorConcealmentPSliceWeightPredictionDisable =
            0;
         avc_bsd.InlineData.
            MBErrorConcealmentPSliceMotionVectorsOverrideDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentPSliceReferenceIndexOverrideDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBSpatialWeightPredictionDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBSpatialMotionVectorsOverrideDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBSpatialReferenceIndexOverrideDisable = 0;
         avc_bsd.InlineData.MBErrorConcealmentBSpatialPredictionMode = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBTemporalWeightPredictionDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBTemporalMotionVectorsOverrideEnable = 1;
         avc_bsd.InlineData.
            MBErrorConcealmentBTemporalReferenceIndexOverrideEnable = 1;
         avc_bsd.InlineData.MBErrorConcealmentBTemporalPredictionMode = 0;
         avc_bsd.InlineData.ConcealmentPictureID = 0;
         avc_bsd.InlineData.InitCurrentMBNumber = 0;
         avc_bsd.InlineData.ConcealmentMethod = 0;
#endif
#if GFX_VERx10 == 75
         avc_bsd.InlineData.MBHeaderErrorHandling = 1;
         avc_bsd.InlineData.EntropyErrorHandling = 1;
         avc_bsd.InlineData.MPRErrorHandling = 1;
         avc_bsd.InlineData.BSDPrematureCompleteErrorHandling = 1;
         avc_bsd.InlineData.MBErrorConcealmentPSliceWeightPredictionDisable =
            0;
         avc_bsd.InlineData.
            MBErrorConcealmentPSliceMotionVectorsOverrideDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBSpatialWeightPredictionDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBSpatialMotionVectorsOverrideDisable = 0;
         avc_bsd.InlineData.MBErrorConcealmentBSpatialPredictionMode = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBTemporalWeightPredictionDisable = 0;
         avc_bsd.InlineData.
            MBErrorConcealmentBTemporalMotionVectorsOverrideEnable = 1;
         avc_bsd.InlineData.MBErrorConcealmentBTemporalPredictionMode = 0;
         avc_bsd.InlineData.IntraPredMode4x48x8LumaErrorControl = 1;
         avc_bsd.InlineData.InitCurrentMBNumber = 0;
         avc_bsd.InlineData.ConcealmentMethod = 0;
         avc_bsd.InlineData.IntraPredictionErrorControl = 1;
         avc_bsd.InlineData.Intra8x84x4PredictionErrorConcealmentControl = 1;
         avc_bsd.InlineData.BSliceTemporalInterConcealmentMode = 0;
         avc_bsd.InlineData.BSliceSpatialInterConcealmentMode = 0;
         avc_bsd.InlineData.BSliceInterDirectTypeConcealmentMode = 0;
         avc_bsd.InlineData.BSliceConcealmentMode = 0;
         avc_bsd.InlineData.PSliceInterConcealmentMode = 0;
         avc_bsd.InlineData.PSliceConcealmentMode = 0;
         avc_bsd.InlineData.ConcealmentReferencePictureFieldBit = 0;
         avc_bsd.InlineData.ISliceConcealmentMode = 1;
         avc_bsd.InlineData.ConcealmentPictureID = 0;
#endif
      };

#if GFX_VER <= 75
      if (!last_slice) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), wait) {
            wait.MFXSyncControlFlag = 1;
         };
      }
#endif
   }

#if GFX_VER <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), wait) {
      wait.MFXSyncControlFlag = 1;
   };
#endif
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
