/*
 * Copyright © 2024 Mesa Contributors
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

/**
 * H.264-specific VDPAU parameter translation for HasVK
 *
 * This module translates Vulkan Video H.264 structures to VDPAU H.264 structures.
 * VDPAU uses a simpler interface than VA-API for H.264 decode, which makes
 * the translation more straightforward.
 */

#include "anv_video_vdpau_bridge.h"
#include "anv_private.h"

#include <vdpau/vdpau.h>
#include <string.h>

#include "vk_video/vulkan_video_codec_h264std_decode.h"
#include "vk_video.h"

/**
 * Translate Vulkan H.264 picture info to VDPAU VdpPictureInfoH264
 *
 * VDPAU H.264 decode uses VdpPictureInfoH264 which contains:
 * - SPS/PPS information inline
 * - Reference frame array (up to 16 entries)
 * - Current picture info
 *
 * This is simpler than VA-API because:
 * - No separate slice parameter buffers
 * - Reference list management is done by the VDPAU implementation
 * - Slice header parsing is handled internally
 */
void
anv_vdpau_translate_h264_picture_params(
   struct anv_device *device,
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   const struct vk_video_session_parameters *params,
   struct anv_vdpau_session *session,
   VdpVideoSurface dst_surface,
   VdpPictureInfoH264 *vdp_pic)
{
   memset(vdp_pic, 0, sizeof(*vdp_pic));

   /* Get SPS and PPS from video session parameters */
   const struct vk_video_h264_sps *sps = NULL;
   const struct vk_video_h264_pps *pps = NULL;

   /* Find SPS with matching seq_parameter_set_id */
   for (unsigned i = 0; i < params->h264_dec.h264_sps_count; i++) {
      if (params->h264_dec.h264_sps[i].base.seq_parameter_set_id ==
          h264_pic_info->pStdPictureInfo->seq_parameter_set_id) {
         sps = &params->h264_dec.h264_sps[i];
         break;
      }
   }

   /* Find PPS with matching pic_parameter_set_id */
   for (unsigned i = 0; i < params->h264_dec.h264_pps_count; i++) {
      if (params->h264_dec.h264_pps[i].base.pic_parameter_set_id ==
          h264_pic_info->pStdPictureInfo->pic_parameter_set_id) {
         pps = &params->h264_dec.h264_pps[i];
         break;
      }
   }

   if (!sps || !pps) {
      return;
   }

   /* SPS fields */
   vdp_pic->num_ref_frames = sps->base.max_num_ref_frames;
   vdp_pic->frame_mbs_only_flag = sps->base.flags.frame_mbs_only_flag;
   vdp_pic->mb_adaptive_frame_field_flag = sps->base.flags.mb_adaptive_frame_field_flag;
   vdp_pic->log2_max_frame_num_minus4 = sps->base.log2_max_frame_num_minus4;
   vdp_pic->pic_order_cnt_type = sps->base.pic_order_cnt_type;
   vdp_pic->log2_max_pic_order_cnt_lsb_minus4 = sps->base.log2_max_pic_order_cnt_lsb_minus4;
   vdp_pic->delta_pic_order_always_zero_flag = sps->base.flags.delta_pic_order_always_zero_flag;
   vdp_pic->direct_8x8_inference_flag = sps->base.flags.direct_8x8_inference_flag;

   /* PPS fields */
   vdp_pic->entropy_coding_mode_flag = pps->base.flags.entropy_coding_mode_flag;
   vdp_pic->pic_order_present_flag = pps->base.flags.bottom_field_pic_order_in_frame_present_flag;
   vdp_pic->weighted_pred_flag = pps->base.flags.weighted_pred_flag;
   vdp_pic->weighted_bipred_idc = pps->base.weighted_bipred_idc;
   vdp_pic->deblocking_filter_control_present_flag = pps->base.flags.deblocking_filter_control_present_flag;
   vdp_pic->redundant_pic_cnt_present_flag = pps->base.flags.redundant_pic_cnt_present_flag;
   vdp_pic->transform_8x8_mode_flag = pps->base.flags.transform_8x8_mode_flag;
   vdp_pic->constrained_intra_pred_flag = pps->base.flags.constrained_intra_pred_flag;
   vdp_pic->chroma_qp_index_offset = pps->base.chroma_qp_index_offset;
   vdp_pic->second_chroma_qp_index_offset = pps->base.second_chroma_qp_index_offset;
   vdp_pic->pic_init_qp_minus26 = pps->base.pic_init_qp_minus26;
   vdp_pic->num_ref_idx_l0_active_minus1 = pps->base.num_ref_idx_l0_default_active_minus1;
   vdp_pic->num_ref_idx_l1_active_minus1 = pps->base.num_ref_idx_l1_default_active_minus1;

   /* Current picture info */
   vdp_pic->slice_count = h264_pic_info->sliceCount;
   vdp_pic->frame_num = h264_pic_info->pStdPictureInfo->frame_num;
   vdp_pic->field_pic_flag = h264_pic_info->pStdPictureInfo->flags.field_pic_flag;
   vdp_pic->bottom_field_flag = h264_pic_info->pStdPictureInfo->flags.bottom_field_flag;
   vdp_pic->is_reference = h264_pic_info->pStdPictureInfo->flags.is_reference;

   /* Picture Order Count for current picture */
   vdp_pic->field_order_cnt[0] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
   vdp_pic->field_order_cnt[1] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];

   /* Copy scaling lists from PPS if present, otherwise use flat scaling */
   if (pps->base.pScalingLists) {
      const StdVideoH264ScalingLists *scaling = pps->base.pScalingLists;

      /* 4x4 scaling lists (6 lists, 16 coefficients each) */
      for (int i = 0; i < 6; i++) {
         for (int j = 0; j < 16; j++) {
            vdp_pic->scaling_lists_4x4[i][j] = scaling->ScalingList4x4[i][j];
         }
      }

      /* 8x8 scaling lists (2 lists, 64 coefficients each) */
      for (int i = 0; i < 2; i++) {
         for (int j = 0; j < 64; j++) {
            vdp_pic->scaling_lists_8x8[i][j] = scaling->ScalingList8x8[i][j];
         }
      }
   } else {
      /* Use flat scaling (all 16s) */
      memset(vdp_pic->scaling_lists_4x4, 16, sizeof(vdp_pic->scaling_lists_4x4));
      memset(vdp_pic->scaling_lists_8x8, 16, sizeof(vdp_pic->scaling_lists_8x8));
   }

   /* Initialize all reference frames to invalid */
   for (unsigned i = 0; i < 16; i++) {
      vdp_pic->referenceFrames[i].surface = VDP_INVALID_HANDLE;
      vdp_pic->referenceFrames[i].is_long_term = VDP_FALSE;
      vdp_pic->referenceFrames[i].top_is_reference = VDP_FALSE;
      vdp_pic->referenceFrames[i].bottom_is_reference = VDP_FALSE;
      vdp_pic->referenceFrames[i].field_order_cnt[0] = 0;
      vdp_pic->referenceFrames[i].field_order_cnt[1] = 0;
      vdp_pic->referenceFrames[i].frame_idx = 0;
   }

   /* Populate reference frames from DPB */
   unsigned ref_idx = 0;

   for (unsigned i = 0; i < decode_info->referenceSlotCount && ref_idx < 16; i++) {
      const VkVideoReferenceSlotInfoKHR *ref_slot = &decode_info->pReferenceSlots[i];
      if (ref_slot->slotIndex < 0 || !ref_slot->pPictureResource)
         continue;

      const VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot_info =
         vk_find_struct_const(ref_slot->pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
      if (!dpb_slot_info || !dpb_slot_info->pStdReferenceInfo)
         continue;

      const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot_info->pStdReferenceInfo;

      /* Get the image from the reference slot */
      ANV_FROM_HANDLE(anv_image_view, ref_image_view, ref_slot->pPictureResource->imageViewBinding);
      if (!ref_image_view || !ref_image_view->image)
         continue;

      /* Lookup VDPAU surface for this image */
      VdpVideoSurface ref_surface = anv_vdpau_lookup_surface(session, ref_image_view->image);
      if (ref_surface == VDP_INVALID_HANDLE) {
         continue;
      }

      /* Fill in reference frame info */
      VdpReferenceFrameH264 *ref = &vdp_pic->referenceFrames[ref_idx];
      ref->surface = ref_surface;
      ref->frame_idx = ref_info->FrameNum;
      ref->is_long_term = ref_info->flags.used_for_long_term_reference ? VDP_TRUE : VDP_FALSE;

      /* Set which fields are used as references
       * When both flags are 0, it's a frame reference (both fields)
       * When one or both are set, specific fields are used
       */
      if (!ref_info->flags.top_field_flag && !ref_info->flags.bottom_field_flag) {
         /* Frame reference - both fields are used */
         ref->top_is_reference = VDP_TRUE;
         ref->bottom_is_reference = VDP_TRUE;
      } else {
         ref->top_is_reference = ref_info->flags.top_field_flag ? VDP_TRUE : VDP_FALSE;
         ref->bottom_is_reference = ref_info->flags.bottom_field_flag ? VDP_TRUE : VDP_FALSE;
      }

      ref->field_order_cnt[0] = ref_info->PicOrderCnt[0];
      ref->field_order_cnt[1] = ref_info->PicOrderCnt[1];

      ref_idx++;
   }
}
