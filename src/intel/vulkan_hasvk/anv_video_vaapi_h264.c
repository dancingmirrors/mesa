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
   /* Adjust picture_height_in_mbs_minus1 for field vs frame coding
    * When frame_mbs_only_flag is 0, the height is in map units (field pairs),
    * and we need to multiply by 2 to get the actual height in MBs.
    * When frame_mbs_only_flag is 1, it's already in MBs.
    */
   if (vk_sps->flags.frame_mbs_only_flag) {
      va_pic->picture_height_in_mbs_minus1 = vk_sps->pic_height_in_map_units_minus1;
   } else {
      /* Field coding: height is in map units, convert to MBs */
      va_pic->picture_height_in_mbs_minus1 = (vk_sps->pic_height_in_map_units_minus1 + 1) * 2 - 1;
   }
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
      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "Invalid SPS/PPS IDs in H.264 decode: sps_id=%u pps_id=%u (sps=%p pps=%p)\n",
                 h264_pic_info->pStdPictureInfo->seq_parameter_set_id,
                 h264_pic_info->pStdPictureInfo->pic_parameter_set_id,
                 (void*)sps, (void*)pps);
      }
      /* Initialize with safe defaults to prevent crashes downstream */
      va_pic->CurrPic.picture_id = dst_surface_id;
      va_pic->CurrPic.flags = VA_PICTURE_H264_INVALID;
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

   /* Add reference pictures from reference slots
    *
    * Important: Only add references that are:
    * 1. Valid (have valid slot index and picture resource)
    * 2. Have valid DPB slot info
    * 3. Have successfully imported VA surface
    *
    * Note about field flags in StdVideoDecodeH264ReferenceInfo:
    * - When both top_field_flag and bottom_field_flag are 0, this is a FRAME reference
    *   (both fields together form the reference, used in frame coding)
    * - When one or both flags are 1, specific fields are used as references (field coding)
    * This matches the interpretation in the legacy native implementation
    * (see docs/genX_video_short.c for reference).
    *
    * Use a separate dpb_idx counter to pack the ReferenceFrames array densely,
    * as the VA-API driver expects a contiguous array of valid references without gaps.
    */
   unsigned dpb_idx = 0;

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API H.264: Building DPB from %u reference slots\n",
              decode_info->referenceSlotCount);
   }

   for (unsigned i = 0; i < decode_info->referenceSlotCount && dpb_idx < 16; i++) {
      const VkVideoReferenceSlotInfoKHR *ref_slot = &decode_info->pReferenceSlots[i];
      if (ref_slot->slotIndex < 0 || !ref_slot->pPictureResource) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "  Slot %u: invalid slot index or no picture resource\n", i);
         }
         continue;
      }

      const VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot_info =
         vk_find_struct_const(ref_slot->pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
      if (!dpb_slot_info || !dpb_slot_info->pStdReferenceInfo) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "  Slot %u (index %d): no DPB slot info or StdReferenceInfo\n",
                    i, ref_slot->slotIndex);
         }
         continue;
      }

      const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot_info->pStdReferenceInfo;

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "  Slot %u (index %d): FrameNum=%u top_field=%u bottom_field=%u long_term=%u non_existing=%u POC=[%d,%d]\n",
                 i, ref_slot->slotIndex,
                 ref_info->FrameNum,
                 ref_info->flags.top_field_flag,
                 ref_info->flags.bottom_field_flag,
                 ref_info->flags.used_for_long_term_reference,
                 ref_info->flags.is_non_existing,
                 ref_info->PicOrderCnt[0],
                 ref_info->PicOrderCnt[1]);
      }

      /* Get the image from the reference slot */
      if (!ref_slot->pPictureResource || !ref_slot->pPictureResource->imageViewBinding) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "  Slot %u: no picture resource or image view binding\n", i);
         }
         continue;
      }

      ANV_FROM_HANDLE(anv_image_view, ref_image_view, ref_slot->pPictureResource->imageViewBinding);
      if (!ref_image_view || !ref_image_view->image) {
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "  Slot %u: no image view or image\n", i);
         }
         continue;
      }

      /* Lookup VA surface ID from session mapping */
      VASurfaceID ref_surface = anv_vaapi_lookup_surface(session, ref_image_view->image);
      if (ref_surface == VA_INVALID_SURFACE) {
         /* Reference surface not yet imported - skip it
          * The VA-API driver can still decode if some references are missing,
          * though quality may be degraded */
         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "  Slot %u: surface not found in VA mapping\n", i);
            fprintf(stderr, "Reference frame at slot %d not found in VA surface mapping, skipping\n",
                    ref_slot->slotIndex);
         }
         continue;
      }

      /* Add to DPB at the next available index */
      init_va_picture(&va_pic->ReferenceFrames[dpb_idx], ref_surface,
                     ref_info->FrameNum,
                     ref_info->flags.used_for_long_term_reference ?
                     VA_PICTURE_H264_LONG_TERM_REFERENCE :
                     VA_PICTURE_H264_SHORT_TERM_REFERENCE);
      va_pic->ReferenceFrames[dpb_idx].TopFieldOrderCnt = ref_info->PicOrderCnt[0];
      va_pic->ReferenceFrames[dpb_idx].BottomFieldOrderCnt = ref_info->PicOrderCnt[1];

      /* Set field flags for VA-API
       * Note: When both top_field_flag and bottom_field_flag are 0, this is a frame reference
       * and we don't set VA_PICTURE_H264_TOP_FIELD or VA_PICTURE_H264_BOTTOM_FIELD.
       * The reference type (short-term vs long-term) was already set by init_va_picture above.
       */
      if (ref_info->flags.top_field_flag && !ref_info->flags.bottom_field_flag)
         va_pic->ReferenceFrames[dpb_idx].flags |= VA_PICTURE_H264_TOP_FIELD;
      else if (!ref_info->flags.top_field_flag && ref_info->flags.bottom_field_flag)
         va_pic->ReferenceFrames[dpb_idx].flags |= VA_PICTURE_H264_BOTTOM_FIELD;
      /* If both flags are set, both fields are used independently - don't set field flags */
      /* If neither flag is set, this is a frame reference - don't set field flags */

      if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
         fprintf(stderr, "  Slot %u -> DPB[%u]: surface_id=%u frame_num=%u flags=0x%x POC=[%d,%d]\n",
                 i, dpb_idx,
                 ref_surface,
                 ref_info->FrameNum,
                 va_pic->ReferenceFrames[dpb_idx].flags,
                 ref_info->PicOrderCnt[0],
                 ref_info->PicOrderCnt[1]);
      }

      dpb_idx++;
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API H.264: Final DPB contains %u reference frames\n", dpb_idx);
   }

   va_pic->frame_num = h264_pic_info->pStdPictureInfo->frame_num;
}

/**
 * Translate Vulkan H.264 slice header to VA-API slice parameter buffer
 *
 * IMPORTANT: On Intel Gen7/7.5/8, the VA-API driver does NOT parse the slice header
 * to build RefPicList0/RefPicList1. Instead, it expects the application to provide
 * these lists that map reference indices to DPB frame store indices.
 *
 * According to Intel PRM, each RefPicList entry is a byte with:
 * - Bit 7: Non-Existing flag (should be 0 for valid entries, 1 for non-existing)
 * - Bit 6: Long term reference flag
 * - Bit 5: Field picture flag (0=frame, 1=field)
 * - Bit 4:0: Frame store index/ID
 *
 * The list should contain all 32 entries, with non-existing entries set to 0xFF.
 */
void
anv_vaapi_translate_h264_slice_params(
   struct anv_device *device,
   const VkVideoDecodeInfoKHR *decode_info,
   const VkVideoDecodeH264PictureInfoKHR *h264_pic_info,
   struct anv_vaapi_session *session,
   const VAPictureParameterBufferH264 *va_pic,
   uint32_t slice_offset,
   uint32_t slice_size,
   VASliceParameterBufferH264 *va_slice)
{
   memset(va_slice, 0, sizeof(*va_slice));

   /* Provide slice data location in the bitstream buffer */
   va_slice->slice_data_size = slice_size;
   va_slice->slice_data_offset = slice_offset;
   va_slice->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

   /* Initialize all RefPicList entries to non-existing (0xFF per Intel PRM) */
   for (unsigned i = 0; i < 32; i++) {
      va_slice->RefPicList0[i].picture_id = VA_INVALID_SURFACE;
      va_slice->RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
      va_slice->RefPicList1[i].picture_id = VA_INVALID_SURFACE;
      va_slice->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
   }

   /* Build RefPicList0 and RefPicList1 from the DPB ReferenceFrames array
    *
    * The VA-API driver on Intel Gen7/7.5/8 needs us to populate these lists.
    * We map each valid DPB entry to the RefPicList. The driver will use these
    * to resolve reference indices from the parsed slice header.
    *
    * For simplicity, we create an initial reference list by copying all valid
    * DPB entries in order. The slice header's ref_pic_list_modification commands
    * (if any) are parsed by the driver and applied to reorder this list.
    */
   unsigned ref_count = 0;
   for (unsigned i = 0; i < 16; i++) {
      if (va_pic->ReferenceFrames[i].picture_id == VA_INVALID_SURFACE ||
          va_pic->ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID)
         break;

      if (ref_count < 32) {
         /* Copy reference to both L0 and L1 lists
          * The driver will determine which list(s) to actually use based on slice type */
         va_slice->RefPicList0[ref_count] = va_pic->ReferenceFrames[i];
         va_slice->RefPicList1[ref_count] = va_pic->ReferenceFrames[i];

         if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
            fprintf(stderr, "  RefPicList[%u]: surface_id=%u frame_num=%u flags=0x%x POC=[%d,%d]\n",
                    ref_count,
                    va_pic->ReferenceFrames[i].picture_id,
                    va_pic->ReferenceFrames[i].frame_idx,
                    va_pic->ReferenceFrames[i].flags,
                    va_pic->ReferenceFrames[i].TopFieldOrderCnt,
                    va_pic->ReferenceFrames[i].BottomFieldOrderCnt);
         }

         ref_count++;
      }
   }

   if (unlikely(INTEL_DEBUG(DEBUG_HASVK))) {
      fprintf(stderr, "VA-API H.264: Built RefPicList with %u references from DPB\n", ref_count);
   }

   /* The VA-API driver will now:
    * - Parse the slice header from the bitstream
    * - Extract slice_type, num_ref_idx_l0/l1_active_minus1, etc.
    * - Apply any ref_pic_list_modification commands to reorder the lists
    * - Use the final lists for motion compensation
    */
}
