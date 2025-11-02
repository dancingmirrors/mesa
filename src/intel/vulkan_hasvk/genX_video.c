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

#include "genxml/gen_macros.h"
#include "genxml/genX_video_pack.h"

/* For gen7, we need PIPE_CONTROL for cache flush workarounds.
 * Since it's not in the video pack, we forward declare it here. */
#if GFX_VERx10 == 70
#include "util/bitpack_helpers.h"

#define __gen_uint(v, s, e) util_bitpack_uint(v, s, e)

struct GFX7_PIPE_CONTROL {
   uint32_t                             DWordLength;
   uint32_t                             CommandStreamerStallEnable;
   uint32_t                             DCFlushEnable;
};

static inline void
GFX7_PIPE_CONTROL_pack(__attribute__((unused)) __gen_user_data *data,
                       __attribute__((unused)) void * restrict dst,
                       __attribute__((unused)) const struct GFX7_PIPE_CONTROL * restrict values)
{
   uint32_t * restrict dw = (uint32_t * restrict) dst;

   dw[0] =
      __gen_uint(values->DWordLength, 0, 7) |
      __gen_uint(0x3, 16, 23) |  /* 3D Command Sub Opcode */
      __gen_uint(0x0, 24, 26) |   /* 3D Command Opcode */
      __gen_uint(0x3, 27, 28) |   /* Command SubType */
      __gen_uint(0x3, 29, 31);    /* Command Type */

   dw[1] =
      __gen_uint(values->CommandStreamerStallEnable, 20, 20) |
      __gen_uint(values->DCFlushEnable, 5, 5);

   dw[2] = 0;
   dw[3] = 0;
   dw[4] = 0;
}

#define GFX7_PIPE_CONTROL_length 5
#define GFX7_PIPE_CONTROL_length_bias 2
#define GFX7_PIPE_CONTROL_header \
   .DWordLength = 3

#endif

void
genX(CmdBeginVideoCodingKHR)(VkCommandBuffer commandBuffer,
                             const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_video_session, vid, pBeginInfo->videoSession);
   ANV_FROM_HANDLE(anv_video_session_params, params, pBeginInfo->videoSessionParameters);

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;
}

void
genX(CmdControlVideoCodingKHR)(VkCommandBuffer commandBuffer,
                               const VkVideoCodingControlInfoKHR *pCodingControlInfo)
{

}

void
genX(CmdEndVideoCodingKHR)(VkCommandBuffer commandBuffer,
                           const VkVideoEndCodingInfoKHR *pEndCodingInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

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
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   const StdVideoH264SequenceParameterSet *sps = vk_video_find_h264_dec_std_sps(&params->vk, h264_pic_info->pStdPictureInfo->seq_parameter_set_id);
   const StdVideoH264PictureParameterSet *pps = vk_video_find_h264_dec_std_pps(&params->vk, h264_pic_info->pStdPictureInfo->pic_parameter_set_id);

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_MODE_SELECT), sel) {
      sel.StandardSelect = SS_AVC;
      sel.CodecSelect = Decode;
      sel.DecoderShortFormatMode = ShortFormatDriverInterface;
      sel.DecoderModeSelect = VLDMode; // Hardcoded

      sel.PreDeblockingOutputEnable = 0;
      sel.PostDeblockingOutputEnable = 1;
   }

   const struct anv_image_view *iv = anv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   const struct anv_image *img = iv->image;
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_SURFACE_STATE), ss) {
      ss.Width = img->vk.extent.width - 1;
      ss.Height = img->vk.extent.height - 1;
      ss.SurfaceFormat = PLANAR_420_8; // assert on this?
      ss.InterleaveChroma = 1;
      ss.SurfacePitch = img->planes[0].primary_surface.isl.row_pitch_B - 1;
      ss.TiledSurface = img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      ss.TileWalk = TW_YMAJOR;
      ss.CrVCbUPixelOffsetVDirection = 0;

      ss.YOffsetforUCb = align(img->vk.extent.height, 32);
      ss.XOffsetforUCb = 0;
      /* For interleaved chroma (NV12), V/Cr is interleaved with U/Cb,
       * so YOffsetforVCr must be 0 (relative to chroma plane start) */
      ss.YOffsetforVCr = 0;
      ss.XOffsetforVCr = 0;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_BUF_ADDR_STATE), buf) {
      bool use_pre_deblock = false;
      if (use_pre_deblock) {
         buf.PreDeblockingDestinationAddress = anv_image_address(img,
                                                                 &img->planes[0].primary_surface.memory_range);
      } else {
         buf.PostDeblockingDestinationAddress = anv_image_address(img,
                                                                  &img->planes[0].primary_surface.memory_range);
      }
#if GFX_VERx10 >= 75
      buf.PreDeblockingDestinationMOCS = anv_mocs(cmd_buffer->device, buf.PreDeblockingDestinationAddress.bo, 0);
      buf.PostDeblockingDestinationMOCS = anv_mocs(cmd_buffer->device, buf.PostDeblockingDestinationAddress.bo, 0);
      buf.OriginalUncompressedPictureSourceMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.StreamOutDataDestinationMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#elif GFX_VERx10 == 70
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t dest_mocs = anv_mocs(cmd_buffer->device, buf.PostDeblockingDestinationAddress.bo, 0);
      buf.PostDeblockingDestinationCacheabilityControl = dest_mocs & 0x3;
      buf.PostDeblockingDestinationGraphicsDataType = (dest_mocs >> 2) & 0x1;
      uint32_t null_mocs = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.OriginalUncompressedPictureSourceCacheabilityControl = null_mocs & 0x3;
      buf.OriginalUncompressedPictureSourceGraphicsDataType = (null_mocs >> 2) & 0x1;
      buf.StreamOutDataDestinationCacheabilityControl = null_mocs & 0x3;
      buf.StreamOutDataDestinationGraphicsDataType = (null_mocs >> 2) & 0x1;
#endif

#if GFX_VER == 8
      buf.IntraRowStoreScratchBufferAddressHigh = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].offset };
      buf.IntraRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, buf.IntraRowStoreScratchBufferAddressHigh.bo, 0);
      buf.DeblockingFilterRowStoreScratchAddressHigh = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo, vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset };
#else
      buf.IntraRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo,
                                                                     vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].offset };
#if GFX_VERx10 >= 75
      buf.IntraRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo, 0);
#elif GFX_VERx10 == 70
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t intra_mocs = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo, 0);
      buf.IntraRowStoreScratchBufferCacheabilityControl = intra_mocs & 0x3;
      buf.IntraRowStoreScratchBufferGraphicsDataType = (intra_mocs >> 2) & 0x1;
#endif
#if GFX_VERx10 == 70
      buf.DeblockingFilterRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo, vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset };
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t deblock_mocs = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo, 0);
      buf.DeblockingFilterRowStoreScratchBufferCacheabilityControl = deblock_mocs & 0x3;
      buf.DeblockingFilterRowStoreScratchBufferGraphicsDataType = (deblock_mocs >> 2) & 0x1;
#else
      buf.DeblockingFilterRowStoreScratchAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo, vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset };
#endif
#endif
#if GFX_VERx10 == 75
      buf.DeblockingFilterRowStoreScratchMOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo, 0);
      buf.MBStatusBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.MBILDBStreamOutBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif

#if GFX_VERx10 == 80
      struct anv_bo *ref_bo = NULL;
#endif
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct anv_image_view *ref_iv = anv_image_view_from_handle(frame_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         buf.ReferencePictureAddress[idx] = anv_image_address(ref_iv->image,
                                                            &ref_iv->image->planes[0].primary_surface.memory_range);
#if GFX_VERx10 == 70
         /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
         uint32_t ref_mocs = anv_mocs(cmd_buffer->device, ref_iv->image->bindings[0].address.bo, 0);
         buf.ReferencePictureCacheabilityControl[idx] = ref_mocs & 0x3;
         buf.ReferencePictureGraphicsDataType[idx] = (ref_mocs >> 2) & 0x1;
#elif GFX_VERx10 == 80
         if (i == 0)
            ref_bo = ref_iv->image->bindings[0].address.bo;
#endif
      }
#if GFX_VERx10 == 80
      buf.ReferencePictureMOCS = anv_mocs(cmd_buffer->device, ref_bo, 0);
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_IND_OBJ_BASE_ADDR_STATE), index_obj) {
      index_obj.MFXIndirectBitstreamObjectAddress = anv_address_add(src_buffer->address,
                                                                    frame_info->srcBufferOffset & ~4095);
#if GFX_VERx10 == 75
      index_obj.MFXIndirectBitstreamObjectMOCS = anv_mocs(cmd_buffer->device, src_buffer->address.bo,
                                                          0);
      index_obj.MFXIndirectMVObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFDIndirectITCOEFFObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFDIndirectITDBLKObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFCIndirectPAKBSEObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#elif GFX_VERx10 == 70
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t bitstream_mocs = anv_mocs(cmd_buffer->device, src_buffer->address.bo, 0);
      index_obj.MFXIndirectBitstreamObjectCacheabilityControl = bitstream_mocs & 0x3;
      index_obj.MFXIndirectBitstreamObjectGraphicsDataType = (bitstream_mocs >> 2) & 0x1;
      uint32_t null_mocs = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFXIndirectMVObjectCacheabilityControl = null_mocs & 0x3;
      index_obj.MFXIndirectMVObjectGraphicsDataType = (null_mocs >> 2) & 0x1;
      index_obj.MFDIndirectITCOEFFObjectCacheabilityControl = null_mocs & 0x3;
      index_obj.MFDIndirectITCOEFFObjectGraphicsDataType = (null_mocs >> 2) & 0x1;
      index_obj.MFDIndirectITDBLKObjectCacheabilityControl = null_mocs & 0x3;
      index_obj.MFDIndirectITDBLKObjectGraphicsDataType = (null_mocs >> 2) & 0x1;
      index_obj.MFCIndirectPAKBSEObjectCacheabilityControl = null_mocs & 0x3;
      index_obj.MFCIndirectPAKBSEObjectGraphicsDataType = (null_mocs >> 2) & 0x1;
#endif
#if GFX_VER == 7
      index_obj.MFXIndirectBitstreamObjectAccessUpperBound = (struct anv_address) { NULL, 0x80000000 };
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_BSP_BUF_BASE_ADDR_STATE), bsp) {
      bsp.BSDMPCRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].offset };
#if GFX_VERx10 == 75
      bsp.BSDMPCRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo, 0);
#elif GFX_VERx10 == 70
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t bsd_mocs = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo, 0);
      bsp.BSDMPCRowStoreScratchBufferCacheabilityControl = bsd_mocs & 0x3;
      bsp.BSDMPCRowStoreScratchBufferGraphicsDataType = (bsd_mocs >> 2) & 0x1;
#endif

      bsp.MPRRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].offset };

#if GFX_VERx10 == 75
      bsp.MPRRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device,  vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo, 0);
      bsp.BitplaneReadBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#elif GFX_VERx10 == 70
      /* IVB: MOCS fields are split into CacheabilityControl and GraphicsDataType */
      uint32_t mpr_mocs = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo, 0);
      bsp.MPRRowStoreScratchBufferCacheabilityControl = mpr_mocs & 0x3;
      bsp.MPRRowStoreScratchBufferGraphicsDataType = (mpr_mocs >> 2) & 0x1;
      uint32_t bitplane_mocs = anv_mocs(cmd_buffer->device, NULL, 0);
      bsp.BitplaneReadBufferCacheabilityControl = bitplane_mocs & 0x3;
      bsp.BitplaneReadBufferGraphicsDataType = (bitplane_mocs >> 2) & 0x1;
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_DPB_STATE), avc_dpb) {
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
            vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
         const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot->pStdReferenceInfo;
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         avc_dpb.NonExistingFrame[idx] = ref_info->flags.is_non_existing;
         avc_dpb.LongTermFrame[idx] = ref_info->flags.used_for_long_term_reference;
         if (!ref_info->flags.top_field_flag && !ref_info->flags.bottom_field_flag)
            avc_dpb.UsedforReference[idx] = 3;
         else
            avc_dpb.UsedforReference[idx] = ref_info->flags.top_field_flag | (ref_info->flags.bottom_field_flag << 1);
         avc_dpb.LTSTFrameNumberList[idx] = ref_info->FrameNum;
      }
   }

#if GFX_VERx10 >= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_PICID_STATE), picid) {
      picid.PictureIDRemappingDisable = true;
   }
#endif

   uint32_t pic_height = sps->pic_height_in_map_units_minus1 + 1;
   if (!sps->flags.frame_mbs_only_flag)
      pic_height *= 2;

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

      avc_img.WeightedBiPredictionIDC = pps->weighted_bipred_idc;
      avc_img.WeightedPredictionEnable = pps->flags.weighted_pred_flag;
      avc_img.FirstChromaQPOffset = pps->chroma_qp_index_offset;
      avc_img.SecondChromaQPOffset = pps->second_chroma_qp_index_offset;
      avc_img.FieldPicture = h264_pic_info->pStdPictureInfo->flags.field_pic_flag;
      avc_img.MBAFFMode = (sps->flags.mb_adaptive_frame_field_flag &&
                           !h264_pic_info->pStdPictureInfo->flags.field_pic_flag);
      avc_img.FrameMBOnly = sps->flags.frame_mbs_only_flag;
      avc_img._8x8IDCTTransformMode = pps->flags.transform_8x8_mode_flag;
      avc_img.Direct8x8Inference = sps->flags.direct_8x8_inference_flag;
      avc_img.ConstrainedIntraPrediction = pps->flags.constrained_intra_pred_flag;
      avc_img.NonReferencePicture = !h264_pic_info->pStdPictureInfo->flags.is_reference;
      avc_img.EntropyCodingSyncEnable = pps->flags.entropy_coding_mode_flag;
      avc_img.ChromaFormatIDC = sps->chroma_format_idc;
      avc_img.TrellisQuantizationChromaDisable = true;
      avc_img.NumberofReferenceFrames = frame_info->referenceSlotCount;
      avc_img.NumberofActiveReferencePicturesfromL0 = pps->num_ref_idx_l0_default_active_minus1 + 1;
      avc_img.NumberofActiveReferencePicturesfromL1 = pps->num_ref_idx_l1_default_active_minus1 + 1;
      avc_img.InitialQPValue = pps->pic_init_qp_minus26;
      avc_img.PicOrderPresent = pps->flags.bottom_field_pic_order_in_frame_present_flag;
      avc_img.DeltaPicOrderAlwaysZero = sps->flags.delta_pic_order_always_zero_flag;
      avc_img.PicOrderCountType = sps->pic_order_cnt_type;
      avc_img.DeblockingFilterControlPresent = pps->flags.deblocking_filter_control_present_flag;
      avc_img.RedundantPicCountPresent = pps->flags.redundant_pic_cnt_present_flag;
      avc_img.Log2MaxFrameNumber = sps->log2_max_frame_num_minus4;
      avc_img.Log2MaxPicOrderCountLSB = sps->log2_max_pic_order_cnt_lsb_minus4;
      avc_img.CurrentPictureFrameNumber = h264_pic_info->pStdPictureInfo->frame_num;
   }

   if (pps->flags.pic_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = pps->pScalingLists->ScalingList8x8[0][q];
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = pps->pScalingLists->ScalingList8x8[3][q];
         }
      }
   } else if (sps->flags.seq_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = sps->pScalingLists->ScalingList8x8[0][q];
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = sps->pScalingLists->ScalingList8x8[3][q];
         }
      }
   } else {
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

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_DIRECTMODE_STATE), avc_directmode) {
      /* bind reference frame DMV */
      #if defined(__GNUC__)
      #pragma GCC diagnostic push
      #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
      #endif
      struct anv_bo *dmv_bo = NULL;
      #if defined(__GNUC__)
      #pragma GCC diagnostic pop
      #endif
#if GFX_VERx10 == 75
      /* HSW: Initialize all MOCS values to non-zero default since the field
       * is marked nonzero="true". Only slots with actual references will be
       * updated below. */
      uint32_t default_mocs = anv_mocs(cmd_buffer->device, NULL, 0);
      for (unsigned i = 0; i < 16; i++) {
         avc_directmode.DirectMVBufferMOCS[i] = default_mocs;
      }
#endif
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
            vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
         const struct anv_image_view *ref_iv = anv_image_view_from_handle(frame_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
         const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot->pStdReferenceInfo;

         if (i == 0) {
            dmv_bo = anv_image_address(ref_iv->image,
                                       &ref_iv->image->vid_dmv_top_surface).bo;
         }

#if GFX_VERx10 == 70
         /* IVB: Each reference slot needs TWO entries (top/bottom field) in grouped arrays */
         uint32_t top_idx = idx * 2;
         uint32_t bottom_idx = idx * 2 + 1;

         struct anv_address dmv_addr = anv_image_address(ref_iv->image,
                                                         &ref_iv->image->vid_dmv_top_surface);
         uint32_t dmv_read_mocs = anv_mocs(cmd_buffer->device, ref_iv->image->bindings[0].address.bo, 0);

         /* Top field */
         avc_directmode.DirectMVBufferAddress[top_idx] = dmv_addr;
         avc_directmode.DirectMVBufferCacheabilityControl[top_idx] = dmv_read_mocs & 0x3;
         avc_directmode.DirectMVBufferGraphicsDataType[top_idx] = (dmv_read_mocs >> 2) & 0x1;

         /* Bottom field (typically same as top for decode) */
         avc_directmode.DirectMVBufferAddress[bottom_idx] = dmv_addr;
         avc_directmode.DirectMVBufferCacheabilityControl[bottom_idx] = dmv_read_mocs & 0x3;
         avc_directmode.DirectMVBufferGraphicsDataType[bottom_idx] = (dmv_read_mocs >> 2) & 0x1;
#elif GFX_VERx10 == 75
         /* HSW: Simple array of 16 addresses with per-buffer MOCS */
         avc_directmode.DirectMVBufferAddress[idx] = anv_image_address(ref_iv->image,
                                                                       &ref_iv->image->vid_dmv_top_surface);
         avc_directmode.DirectMVBufferMOCS[idx] = anv_mocs(cmd_buffer->device, ref_iv->image->bindings[0].address.bo, 0);
#elif GFX_VER == 8
         /* BDW: Simple array of 16 addresses */
         avc_directmode.DirectMVBufferAddress[idx] = anv_image_address(ref_iv->image,
                                                                       &ref_iv->image->vid_dmv_top_surface);
#endif
         avc_directmode.POCList[2 * idx] = ref_info->PicOrderCnt[0];
         avc_directmode.POCList[2 * idx + 1] = ref_info->PicOrderCnt[1];
      }

#if GFX_VERx10 == 70
      /* IVB: Write buffer also uses grouped arrays with two entries */
      uint32_t dmv_write_mocs = anv_mocs(cmd_buffer->device, img->bindings[0].address.bo, 0);
      struct anv_address write_addr = anv_image_address(img, &img->vid_dmv_top_surface);

      avc_directmode.DirectMVBufferWriteAddress[0] = write_addr;
      avc_directmode.DirectMVBufferWriteCacheabilityControl[0] = dmv_write_mocs & 0x3;
      avc_directmode.DirectMVBufferWriteGraphicsDataType[0] = (dmv_write_mocs >> 2) & 0x1;

      avc_directmode.DirectMVBufferWriteAddress[1] = write_addr;
      avc_directmode.DirectMVBufferWriteCacheabilityControl[1] = dmv_write_mocs & 0x3;
      avc_directmode.DirectMVBufferWriteGraphicsDataType[1] = (dmv_write_mocs >> 2) & 0x1;
#else
      /* HSW and later */
      avc_directmode.DirectMVBufferWriteAddress = anv_image_address(img,
                                                                    &img->vid_dmv_top_surface);
#if GFX_VERx10 == 75
      avc_directmode.DirectMVBufferWriteMOCS = anv_mocs(cmd_buffer->device, avc_directmode.DirectMVBufferWriteAddress.bo, 0);
#elif GFX_VER == 8
      /* TODO(gen8-video): Properly configure DirectMVBufferAttributes and
       * DirectMVBufferWriteAttributes for optimal caching behavior.
       * For now, leaving them as zero-initialized which uses default caching.
       * This may impact performance but should be functionally correct.
       */
#endif
#endif

      avc_directmode.POCList[32] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
      avc_directmode.POCList[33] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];
   }

   uint32_t buffer_offset = frame_info->srcBufferOffset & 4095;
#define HEADER_OFFSET 3
#if GFX_VERx10 == 70
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DWordLength = 2;
      pc.CommandStreamerStallEnable = 1;
   }
#endif
   for (unsigned s = 0; s < h264_pic_info->sliceCount; s++) {
      bool last_slice = s == (h264_pic_info->sliceCount - 1);
      uint32_t current_offset = h264_pic_info->pSliceOffsets[s];
      uint32_t this_end;
      if (!last_slice) {
         uint32_t next_offset = h264_pic_info->pSliceOffsets[s + 1];
         uint32_t next_end = h264_pic_info->pSliceOffsets[s + 2];
         if (s == h264_pic_info->sliceCount - 2)
            next_end = frame_info->srcBufferRange;
         anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_SLICEADDR), sliceaddr) {
            sliceaddr.IndirectBSDDataLength = next_end - next_offset - HEADER_OFFSET;
            /* start decoding after the 3-byte header. */
            sliceaddr.IndirectBSDDataStartAddress = buffer_offset + next_offset + HEADER_OFFSET;
         };
         this_end = next_offset;
      } else
         this_end = frame_info->srcBufferRange;
      anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_BSD_OBJECT), avc_bsd) {
         avc_bsd.IndirectBSDDataLength = this_end - current_offset - HEADER_OFFSET;
         /* start decoding after the 3-byte header. */
         avc_bsd.IndirectBSDDataStartAddress = buffer_offset + current_offset + HEADER_OFFSET;
         avc_bsd.InlineData.LastSlice = last_slice;
         avc_bsd.InlineData.FixPrevMBSkipped = 1;
#if GFX_VERx10 >= 75
         avc_bsd.InlineData.IntraPredictionErrorControl = 1;
         avc_bsd.InlineData.Intra8x84x4PredictionErrorConcealmentControl = 1;
         avc_bsd.InlineData.ISliceConcealmentMode = 1;
#endif
      };
   }

#if GFX_VERx10 == 70
   /* On Ivy Bridge, we need to flush the data cache after video decode
    * operations to ensure decoded frame data is visible when used as
    * reference frames for subsequent P/B frames. Without this flush,
    * we get cache coherency issues causing visual corruption.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.CommandStreamerStallEnable = 1;
      pc.DCFlushEnable = 1;
   }
#endif
}

void
genX(CmdDecodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoDecodeInfoKHR *frame_info)
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

#ifdef VK_ENABLE_BETA_EXTENSIONS
void
genX(CmdEncodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoEncodeInfoKHR *pEncodeInfo)
{
}
#endif
