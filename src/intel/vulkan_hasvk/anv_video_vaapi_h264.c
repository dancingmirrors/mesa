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
 * H.264-specific VA-API parameter translation for HasVK
 * 
 * This module translates Vulkan Video H.264 structures to VA-API H.264 structures.
 */

#include "anv_video_vaapi_bridge.h"
#include "anv_private.h"

#include <va/va.h>
#include <string.h>

#include "vk_video/vulkan_video_codec_h264std_decode.h"
#include "vk_video.h"

/**
 * Translate Vulkan H.264 SPS to VA-API SPS fields in picture parameter buffer
 */
static void
translate_h264_sps(const StdVideoH264SequenceParameterSet *vk_sps,
                   VAPictureParameterBufferH264 *va_pic)
{
   /* SPS fields that go into picture parameter buffer */
   va_pic->seq_fields.bits.chroma_format_idc = vk_sps->chroma_format_idc;
   va_pic->seq_fields.bits.residual_colour_transform_flag = 0;
   va_pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = 
      vk_sps->flags.gaps_in_frame_num_value_allowed_flag;
   va_pic->seq_fields.bits.frame_mbs_only_flag = vk_sps->flags.frame_mbs_only_flag;
   va_pic->seq_fields.bits.mb_adaptive_frame_field_flag = 
      vk_sps->flags.mb_adaptive_frame_field_flag;
   va_pic->seq_fields.bits.direct_8x8_inference_flag = 
      vk_sps->flags.direct_8x8_inference_flag;
   va_pic->seq_fields.bits.MinLumaBiPredSize8x8 = 0;
   va_pic->seq_fields.bits.log2_max_frame_num_minus4 = vk_sps->log2_max_frame_num_minus4;
   va_pic->seq_fields.bits.pic_order_cnt_type = vk_sps->pic_order_cnt_type;
   va_pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 
      vk_sps->log2_max_pic_order_cnt_lsb_minus4;
   va_pic->seq_fields.bits.delta_pic_order_always_zero_flag = 
      vk_sps->flags.delta_pic_order_always_zero_flag;

   va_pic->num_ref_frames = vk_sps->max_num_ref_frames;
   va_pic->picture_width_in_mbs_minus1 = vk_sps->pic_width_in_mbs_minus1;
   va_pic->picture_height_in_mbs_minus1 = vk_sps->pic_height_in_map_units_minus1;
   va_pic->bit_depth_luma_minus8 = vk_sps->bit_depth_luma_minus8;
   va_pic->bit_depth_chroma_minus8 = vk_sps->bit_depth_chroma_minus8;
}

/**
 * Translate Vulkan H.264 PPS to VA-API PPS fields in picture parameter buffer
 */
static void
translate_h264_pps(const StdVideoH264PictureParameterSet *vk_pps,
                   VAPictureParameterBufferH264 *va_pic)
{
   /* PPS fields */
   va_pic->pic_fields.bits.entropy_coding_mode_flag = vk_pps->flags.entropy_coding_mode_flag;
   va_pic->pic_fields.bits.weighted_pred_flag = vk_pps->flags.weighted_pred_flag;
   va_pic->pic_fields.bits.weighted_bipred_idc = vk_pps->weighted_bipred_idc;
   va_pic->pic_fields.bits.transform_8x8_mode_flag = vk_pps->flags.transform_8x8_mode_flag;
   va_pic->pic_fields.bits.field_pic_flag = 0;
   va_pic->pic_fields.bits.constrained_intra_pred_flag = 
      vk_pps->flags.constrained_intra_pred_flag;
   va_pic->pic_fields.bits.pic_order_present_flag = 
      vk_pps->flags.bottom_field_pic_order_in_frame_present_flag;
   va_pic->pic_fields.bits.deblocking_filter_control_present_flag = 
      vk_pps->flags.deblocking_filter_control_present_flag;
   va_pic->pic_fields.bits.redundant_pic_cnt_present_flag = 
      vk_pps->flags.redundant_pic_cnt_present_flag;
   va_pic->pic_fields.bits.reference_pic_flag = 1;

   /* Note: num_slice_groups_minus1 is deprecated in VA-API and doesn't exist in Vulkan std headers */
   /* Note: num_ref_idx_l0/l1_active_minus1 are NOT in VAPictureParameterBufferH264 - they are 
    * per-slice parameters that go in VASliceParameterBufferH264 instead */
   
   va_pic->pic_init_qp_minus26 = vk_pps->pic_init_qp_minus26;
   va_pic->chroma_qp_index_offset = vk_pps->chroma_qp_index_offset;
   va_pic->second_chroma_qp_index_offset = vk_pps->second_chroma_qp_index_offset;
}

/**
 * Initialize a VA-API picture (current or reference)
 */
static void
init_va_picture(VAPictureH264 *va_pic,
                VASurfaceID surface_id,
                int32_t frame_idx,
                uint32_t flags)
{
   va_pic->picture_id = surface_id;
   va_pic->frame_idx = frame_idx;
   va_pic->flags = flags;
   va_pic->TopFieldOrderCnt = 0;
   va_pic->BottomFieldOrderCnt = 0;
}

/**
 * Translate Vulkan H.264 picture info to VA-API picture parameter buffer
 */
void
anv_vaapi_translate_h264_picture_params(
   struct anv_device *device,
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   const struct vk_video_session_parameters *params,
   struct anv_vaapi_session *session,
   VASurfaceID dst_surface_id,
   VAPictureParameterBufferH264 *va_pic)
{
   memset(va_pic, 0, sizeof(*va_pic));

   /* Get SPS and PPS from video session parameters by searching for matching IDs */
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
      vk_loge(VK_LOG_OBJS(&device->vk.base),
              "Invalid SPS/PPS IDs in H.264 decode");
      return;
   }

   /* Translate SPS fields */
   translate_h264_sps(&sps->base, va_pic);

   /* Translate PPS fields */
   translate_h264_pps(&pps->base, va_pic);

   /* Set current picture */
   init_va_picture(&va_pic->CurrPic, dst_surface_id, 
                   h264_pic_info->pStdPictureInfo->frame_num, 0);
   va_pic->CurrPic.TopFieldOrderCnt = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
   va_pic->CurrPic.BottomFieldOrderCnt = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];

   /* Initialize all reference pictures to invalid */
   for (unsigned i = 0; i < 16; i++) {
      va_pic->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
      va_pic->ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
   }

   /* Set up reference frames from DPB */
   if (decode_info->pSetupReferenceSlot) {
      const VkVideoReferenceSlotInfoKHR *setup_slot = decode_info->pSetupReferenceSlot;
      if (setup_slot->slotIndex >= 0 && setup_slot->pPictureResource) {
         /* This frame will be used as a reference */
         va_pic->CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
      }
   }

   /* Add reference pictures from reference slots */
   for (unsigned i = 0; i < decode_info->referenceSlotCount && i < 16; i++) {
      const VkVideoReferenceSlotInfoKHR *ref_slot = &decode_info->pReferenceSlots[i];
      if (ref_slot->slotIndex < 0 || !ref_slot->pPictureResource)
         continue;

      const VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot_info =
         vk_find_struct_const(ref_slot->pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
      if (!dpb_slot_info)
         continue;

      const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot_info->pStdReferenceInfo;
      
      /* Get the image from the reference slot */
      if (!ref_slot->pPictureResource || !ref_slot->pPictureResource->imageViewBinding)
         continue;
      
      ANV_FROM_HANDLE(anv_image_view, ref_image_view, ref_slot->pPictureResource->imageViewBinding);
      if (!ref_image_view || !ref_image_view->image)
         continue;
      
      /* Lookup VA surface ID from session mapping */
      VASurfaceID ref_surface = anv_vaapi_lookup_surface(session, ref_image_view->image);
      if (ref_surface == VA_INVALID_SURFACE) {
         /* Reference surface not yet imported - this shouldn't happen if decode_frame
          * properly imported all reference surfaces before calling this function */
         continue;
      }
      
      init_va_picture(&va_pic->ReferenceFrames[i], ref_surface,
                     ref_info->FrameNum,
                     ref_info->flags.used_for_long_term_reference ?
                     VA_PICTURE_H264_LONG_TERM_REFERENCE :
                     VA_PICTURE_H264_SHORT_TERM_REFERENCE);
      va_pic->ReferenceFrames[i].TopFieldOrderCnt = ref_info->PicOrderCnt[0];
      va_pic->ReferenceFrames[i].BottomFieldOrderCnt = ref_info->PicOrderCnt[1];
   }

   va_pic->frame_num = h264_pic_info->pStdPictureInfo->frame_num;
}

/**
 * Translate Vulkan H.264 slice header to VA-API slice parameter buffer
 * 
 * Note: VA-API drivers parse slice headers directly from the bitstream,
 * so we only need to provide minimal information here. The driver will
 * extract slice_type, reference lists, and other parameters automatically.
 * This is much more reliable than trying to parse the slice header ourselves.
 */
void
anv_vaapi_translate_h264_slice_params(
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   uint32_t slice_offset,
   uint32_t slice_size,
   VASliceParameterBufferH264 *va_slice)
{
   memset(va_slice, 0, sizeof(*va_slice));

   /* Provide slice data location in the bitstream buffer.
    * VA-API will parse the slice header from the bitstream automatically.
    */
   
   va_slice->slice_data_size = slice_size;
   va_slice->slice_data_offset = slice_offset;
   va_slice->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

   /* The VA-API driver will parse and extract:
    * - slice_type
    * - num_ref_idx_l0_active_minus1 / num_ref_idx_l1_active_minus1
    * - reference picture lists
    * - quantization parameters
    * - deblocking filter parameters
    * All from the bitstream automatically, avoiding complex manual parsing.
    */
}
