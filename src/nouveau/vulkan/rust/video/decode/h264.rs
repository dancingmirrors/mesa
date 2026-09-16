// Copyright © 2024 Collabora, Ltd and Red Hat, Inc.
// SPDX-License-Identifier: MIT

//! H264 decode implementation. Takes inspiration from the early C version
//! written by Dave Airlie.

use rustc_hash::{FxHashMap, FxHashSet};

use nv_push_rs::*;
use nvidia_headers::classes::clc5b0::mthd as clc5b0;

use crate::ffi;
use crate::ffi::{
    _nvdec_h264_pic_s, nvk_cmd_buffer, nvk_image, nvk_image_view,
    StdVideoDecodeH264PictureInfo, StdVideoH264SequenceParameterSet,
    VkVideoBeginCodingInfoKHR, VkVideoReferenceSlotInfoKHR, VK_SUCCESS,
};
use crate::nvk_cmd_buffer_push;
use crate::util::vk_find_struct_const;
use crate::video::align_u32;
use crate::video::decode::VideoDecoder;

type Result<T = ()> = std::result::Result<T, ffi::VkResult>;

fn bad_input<T>() -> Result<T> {
    Err(ffi::VK_ERROR_UNKNOWN)
}

fn to_u32<T>(v: T) -> Result<u32>
where
    T: TryInto<u32>,
{
    v.try_into().map_err(|_| ffi::VK_ERROR_UNKNOWN)
}

fn to_i32<T>(v: T) -> Result<i32>
where
    T: TryInto<i32>,
{
    v.try_into().map_err(|_| ffi::VK_ERROR_UNKNOWN)
}

/// The type of picture being decoded.
#[derive(Debug, Default, Clone, Copy)]
enum PictureType {
    /// Top field has been decoded.
    Top = 1,
    /// Bottom field has been decoded.
    Bottom = 2,
    /// A frame, i.e.: either progressive content or both fields have
    /// been decoded.
    #[default]
    Frame = 3,
}

#[derive(Debug, Default)]
struct FrameData {
    /// The `pic_idx` value associated with this frame.
    pic_idx: Option<u32>,
    /// The `dpb_idx` value associated with this frame.
    dpb_idx: Option<u32>,
    /// Is this the first field or a complementary field for a given picture?
    first_field_or_complementary: bool,
    /// The type of picture being decoded. This keeps track of what we have seen
    /// so far.
    picture_ty: PictureType,
}

fn compute_opaque_buffer_sizes(
    sps: &StdVideoH264SequenceParameterSet,
) -> Option<(u32, u32, u32)> {
    let pic_height_in_map_units =
        sps.pic_height_in_map_units_minus1.checked_add(1)?;
    let pic_width_in_mbs = sps.pic_width_in_mbs_minus1.checked_add(1)?;

    let coloc_size = align_u32(
        align_u32(pic_height_in_map_units, 2)?
            .checked_mul(pic_width_in_mbs)?
            .checked_mul(64)?
            .checked_sub(63)?,
        0x100,
    )?;

    let mbhist_size = align_u32(pic_width_in_mbs.checked_mul(104)?, 0x100)?;
    let history_size = align_u32(pic_width_in_mbs.checked_mul(0x300)?, 0x200)?;

    Some((coloc_size, mbhist_size, history_size))
}

static EOS_ARRAY: [u8; 16] = [
    0x0, 0x0, 0x1, 0xb, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0xb, 0x0, 0x0, 0x0,
    0x0,
];

#[derive(Debug, Clone, Copy)]
struct GpuBufferAddresses {
    pic: u64,
    slice_offsets: u64,
    mbstatus: u64,
}

/// Suballocate `size` upload-heap bytes (256-byte aligned) and, if `data` is
/// given, copy it in. Returns the GPU address of the allocation.
unsafe fn upload_bytes(
    cmd: &mut nvk_cmd_buffer,
    data: Option<(*const u8, usize)>,
    size: u32,
) -> Result<u64> {
    let mut ptr = std::ptr::null_mut();
    let mut addr = 0;
    unsafe {
        let res = ffi::nvk_cmd_buffer_upload_alloc(
            cmd,
            size,
            256,
            &mut addr,
            &mut ptr as *mut *mut _ as *mut *mut std::ffi::c_void,
        );
        if res != VK_SUCCESS {
            return Err(res);
        }
        if let Some((src, len)) = data {
            std::ptr::copy_nonoverlapping(src, ptr as *mut u8, len);
        }
    }
    Ok(addr)
}

/// Upload the parameters to the GPU.
fn upload_to_the_gpu(
    cmd: &mut nvk_cmd_buffer,
    nvh264: &_nvdec_h264_pic_s,
    slice_offsets: &[u32],
) -> Result<GpuBufferAddresses> {
    let pic_size = std::mem::size_of::<ffi::nvdec_h264_pic_s>() as u32;
    let pic_gpu_addr = unsafe {
        upload_bytes(
            cmd,
            Some((
                (nvh264 as *const _nvdec_h264_pic_s).cast(),
                pic_size as usize,
            )),
            pic_size,
        )
    }?;

    let slice_offsets_size = to_u32(std::mem::size_of_val(slice_offsets))?;
    let slice_offsets_address = unsafe {
        upload_bytes(
            cmd,
            Some((slice_offsets.as_ptr().cast(), slice_offsets_size as usize)),
            slice_offsets_size,
        )
    }?;

    // Just upload this to the GPU for now, we will hook it up later.
    let mbstatus_address = unsafe { upload_bytes(cmd, None, 4096) }?;

    Ok(GpuBufferAddresses {
        pic: pic_gpu_addr,
        slice_offsets: slice_offsets_address,
        mbstatus: mbstatus_address,
    })
}

/// Identifies a DPB picture.
///
/// With "separated" DPB each picture has its own image view (always array layer
/// 0); with "layered" DPB a single array image view backs every picture and the
/// array layer selects the picture. Keying on the image view alone therefore
/// collides in the layered case, so the array layer is part of the key. Two
/// video picture resources only match if they refer to the same image
/// subresource *and* specify the same coded offset and extent, so those are
/// part of the key as well.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
struct SlotKey {
    image_view: *const nvk_image_view,
    layer: u32,
    coded_offset: (i32, i32),
    coded_extent: (u32, u32),
}

/// Build the [`SlotKey`] for a Vulkan picture resource.
fn slot_key(res: &ffi::VkVideoPictureResourceInfoKHR) -> SlotKey {
    let image_view =
        unsafe { ffi::nvk_image_view_from_handle(res.imageViewBinding) };
    SlotKey {
        image_view,
        layer: unsafe { (*image_view).vk.base_array_layer }
            + res.baseArrayLayer,
        coded_offset: (res.codedOffset.x, res.codedOffset.y),
        coded_extent: (res.codedExtent.width, res.codedExtent.height),
    }
}

/// A GPU address in the form the engine's offset methods take: shifted right
/// by 8, so the low 8 bits have to be zero for the address to survive the
/// round trip.
fn addr_hi(addr: u64) -> Result<u32> {
    if addr & 0xff != 0 {
        return bad_input();
    }
    to_u32(addr >> 8)
}

/// Address (already shifted right by 8, as the hardware expects) of the given
/// plane and DPB array layer of an image. For "separated" DPB the layer is 0
/// and this is just the plane base address; for "layered" DPB the layer selects
/// the picture within the array image.
fn plane_layer_base(img: *mut nvk_image, plane: u8, layer: u32) -> Result<u32> {
    let base = unsafe { ffi::nvk_image_base_address(img, plane) };
    let array_stride =
        unsafe { (*img).planes[plane as usize].nil.array_stride_B };
    addr_hi(base + u64::from(layer) * array_stride)
}

/// Session data stored in opaque `nvk_video_session::rust` pointer.
#[derive(Default, Debug)]
pub(crate) struct Decoder {
    /// Data associated with each DPB picture (image view + array layer).
    slots: FxHashMap<SlotKey, FrameData>,
    /// A counter for the frame number. Note that the hardware wants a u32.
    frame_num: u32,
    /// The free picture slots, as a bitmask (bit `i` set means slot `i` is
    /// free).
    free_pic_slots: u32,
    /// The free DPB slots, as a bitmask (bit `i` set means slot `i` is free).
    free_dpb_slots: u32,
    num_pic_slots: u32,
}

impl Decoder {
    pub(crate) fn new(max_dpb_slots: u32) -> Self {
        Self {
            num_pic_slots: max_dpb_slots.min(16) + 1,
            ..Default::default()
        }
    }

    /// Gets the ith slot from the `begin_info`. These slots are the ones the
    /// application plans to use during the `vkCmdBeginVideoCodingKHR` and
    /// `vkCmdEndVideoCodingKHR` calls.
    fn get_ith_planned_slot(
        begin_info: &VkVideoBeginCodingInfoKHR,
        i: usize,
    ) -> Option<(VkVideoReferenceSlotInfoKHR, Option<SlotKey>)> {
        if i >= begin_info.referenceSlotCount as usize
            || begin_info.pReferenceSlots.is_null()
        {
            return None;
        }

        let ref_slot = unsafe { *begin_info.pReferenceSlots.add(i) };
        let key = unsafe { ref_slot.pPictureResource.as_ref() }.map(slot_key);

        Some((ref_slot, key))
    }

    /// Gets the ith slot for the frame currently being decoded. This is a slot
    /// that is referenced by the current frame.
    fn get_ith_slot_for_frame(
        frame_info: &ffi::VkVideoDecodeInfoKHR,
        i: usize,
    ) -> Result<(VkVideoReferenceSlotInfoKHR, SlotKey)> {
        if i >= frame_info.referenceSlotCount as usize
            || frame_info.pReferenceSlots.is_null()
        {
            return bad_input();
        }

        let ref_slot = unsafe { *frame_info.pReferenceSlots.add(i) };
        let res = unsafe { ref_slot.pPictureResource.as_ref() }
            .ok_or(ffi::VK_ERROR_UNKNOWN)?;

        Ok((ref_slot, slot_key(res)))
    }

    fn remove_invalid_slots(&mut self, begin_info: &VkVideoBeginCodingInfoKHR) {
        // Mark the pictures the application still plans to use, then sweep
        // away everything else, rather than scanning the planned slots once
        // per tracked picture.
        let slot_count = if begin_info.pReferenceSlots.is_null() {
            0
        } else {
            begin_info.referenceSlotCount
        };

        let mut keep = FxHashSet::with_capacity_and_hasher(
            slot_count as usize,
            Default::default(),
        );

        for i in 0..slot_count {
            let Some((ref_slot, key)) =
                Self::get_ith_planned_slot(begin_info, i as usize)
            else {
                break;
            };

            if ref_slot.slotIndex >= 0 {
                if let Some(key) = key {
                    keep.insert(key);
                }
            }
        }

        self.slots.retain(|key, _| keep.contains(key));

        self.free_pic_slots = (1u32 << self.num_pic_slots) - 1;
        self.free_dpb_slots = 0xffff;
        for frame_slot in self.slots.values() {
            if let Some(pic_idx) = frame_slot.pic_idx {
                self.free_pic_slots &= !(1 << pic_idx);
            }
            if let Some(dpb_idx) = frame_slot.dpb_idx {
                self.free_dpb_slots &= !(1 << dpb_idx);
            }
        }
    }

    /// Forcibly find a frame. If the frame has not been submitted, it's an
    /// application error.
    fn find_submitted_frame<'a>(
        slots: &'a mut FxHashMap<SlotKey, FrameData>,
        key: SlotKey,
    ) -> Result<&'a mut FrameData> {
        slots.get_mut(&key).ok_or(ffi::VK_ERROR_UNKNOWN)
    }

    /// Get the `pic_idx` value associated with the given DPB picture. If this is
    /// the first time we are seeing this picture, then allocate a new `pic_idx`
    /// value.
    ///
    /// `pic_idx` selects the picture's colocated-MV buffer, which the firmware
    /// reads for temporal prediction, so it must not be reused while the
    /// picture is still referenced. The application's DPB slot index already
    /// has that lifetime, so prefer it when that index is free. Pictures
    /// without a DPB slot, and pictures whose slot index is already held by
    /// another live picture (interlaced field decoding hits this), fall back
    /// to the lowest free index.
    fn get_pic_idx(&mut self, key: SlotKey, slot_index: i32) -> Result<u32> {
        if let Some(frame_data) = self.slots.get(&key) {
            if let Some(pic_idx) = frame_data.pic_idx {
                return Ok(pic_idx);
            }
        }

        // Prefer the stable application DPB slot as the colMv slot. Fall back to
        // a free slot only for pictures with no DPB slot (slot_index < 0).
        let pic_idx = if slot_index >= 0
            && slot_index < self.num_pic_slots as i32
            && self.free_pic_slots & (1 << slot_index) != 0
        {
            slot_index as u32
        } else {
            if self.free_pic_slots == 0 {
                return bad_input();
            }
            self.free_pic_slots.trailing_zeros()
        };

        self.free_pic_slots &= !(1 << pic_idx);

        let frame_data = FrameData {
            pic_idx: Some(pic_idx),
            ..Default::default()
        };

        self.slots.insert(key, frame_data);

        Ok(pic_idx)
    }

    /// Get the `dpb_idx` value associated with the given DPB picture or assign
    /// one if needed. This picture *must* have been submitted already.
    fn get_dpb_idx(&mut self, key: SlotKey) -> Result<u32> {
        let frame_data = Self::find_submitted_frame(&mut self.slots, key)?;

        if let Some(dpb_idx) = frame_data.dpb_idx {
            return Ok(dpb_idx);
        }

        if self.free_dpb_slots == 0 {
            return bad_input();
        }

        let dpb_idx = self.free_dpb_slots.trailing_zeros();
        frame_data.dpb_idx = Some(dpb_idx);
        self.free_dpb_slots &= !(1 << dpb_idx);

        Ok(dpb_idx)
    }

    fn release_slot(&mut self, key: SlotKey) {
        match self.slots.get(&key) {
            Some(frame_data) if frame_data.dpb_idx.is_none() => (),
            _ => return,
        }

        if let Some(frame_data) = self.slots.remove(&key) {
            if let Some(pic_idx) = frame_data.pic_idx {
                self.free_pic_slots |= 1 << pic_idx;
            }
        }
    }

    fn is_field(&mut self, key: SlotKey) -> Result<bool> {
        Ok(Self::find_submitted_frame(&mut self.slots, key)?
            .first_field_or_complementary)
    }

    fn set_field(&mut self, key: SlotKey, is_field: bool) -> Result {
        Self::find_submitted_frame(&mut self.slots, key)?
            .first_field_or_complementary = is_field;
        Ok(())
    }

    fn get_picture_type(&mut self, key: SlotKey) -> Result<PictureType> {
        Ok(Self::find_submitted_frame(&mut self.slots, key)?.picture_ty)
    }

    fn set_picture_type(
        &mut self,
        key: SlotKey,
        picture_ty: PictureType,
    ) -> Result {
        Self::find_submitted_frame(&mut self.slots, key)?.picture_ty =
            picture_ty;
        Ok(())
    }

    fn set_reference_frames(
        &mut self,
        nvh264: &mut ffi::nvdec_h264_pic_s,
        frame_info: &ffi::VkVideoDecodeInfoKHR,
        (luma_base, chroma_base): (&mut [u32; 17], &mut [u32; 17]),
    ) -> Result {
        for i in 0..frame_info.referenceSlotCount as usize {
            let (vk_ref_slot, key) =
                Decoder::get_ith_slot_for_frame(frame_info, i)?;

            let img = unsafe { (*key.image_view).vk.image as *mut nvk_image };

            let dpb_slot = vk_find_struct_const!(
                vk_ref_slot.pNext,
                VIDEO_DECODE_H264_DPB_SLOT_INFO,
                KHR
            )
            .ok_or(ffi::VK_ERROR_UNKNOWN)?;

            let vk_ref_info = unsafe { dpb_slot.pStdReferenceInfo.as_ref() }
                .ok_or(ffi::VK_ERROR_UNKNOWN)?;

            let pic_idx = self.get_pic_idx(key, vk_ref_slot.slotIndex)?;
            let dpb_idx = self.get_dpb_idx(key)?;

            let is_field = self.is_field(key)?;
            let picture_ty = self.get_picture_type(key)?;

            let marking =
                if vk_ref_info.flags.used_for_long_term_reference() != 0 {
                    2
                } else {
                    1
                };

            let top_field_marking = match picture_ty {
                PictureType::Top | PictureType::Frame => marking,
                _ => 0,
            };

            let bottom_field_marking = match picture_ty {
                PictureType::Bottom | PictureType::Frame => marking,
                _ => 0,
            };

            let dpb_entry = &mut nvh264.dpb[dpb_idx as usize];

            dpb_entry.set_index(pic_idx);
            dpb_entry.set_col_idx(pic_idx);
            dpb_entry.set_is_field(is_field as u32);
            dpb_entry.set_state(picture_ty as u32);
            dpb_entry.set_top_field_marking(top_field_marking);
            dpb_entry.set_bottom_field_marking(bottom_field_marking);

            dpb_entry.FieldOrderCnt[0] =
                if vk_ref_info.PicOrderCnt[0] != i32::MAX {
                    vk_ref_info.PicOrderCnt[0] as u32
                } else {
                    vk_ref_info.PicOrderCnt[1] as u32
                };

            dpb_entry.FieldOrderCnt[1] =
                if vk_ref_info.PicOrderCnt[1] != i32::MAX {
                    vk_ref_info.PicOrderCnt[1] as u32
                } else {
                    vk_ref_info.PicOrderCnt[0] as u32
                };

            dpb_entry.FrameIdx = vk_ref_info.FrameNum.into();

            dpb_entry.set_is_long_term(
                vk_ref_info.flags.used_for_long_term_reference(),
            );

            dpb_entry.set_not_existing(vk_ref_info.flags.is_non_existing());

            luma_base[pic_idx as usize] = plane_layer_base(img, 0, key.layer)?;
            chroma_base[pic_idx as usize] =
                plane_layer_base(img, 1, key.layer)?;
        }

        Ok(())
    }

    fn set_current_picture_slot(
        &mut self,
        key: SlotKey,
        slot_index: i32,
        std_pic_info: &StdVideoDecodeH264PictureInfo,
        interlaced: bool,
    ) -> Result<u32> {
        if !interlaced && self.slots.contains_key(&key) {
            return bad_input();
        }

        let pic_idx = self.get_pic_idx(key, slot_index)?;

        let is_field_pic = std_pic_info.flags.field_pic_flag() != 0;
        let is_complementary_field_pair =
            std_pic_info.flags.complementary_field_pair() != 0;
        let is_bottom_field = std_pic_info.flags.bottom_field_flag() != 0;

        self.set_field(key, is_field_pic || is_complementary_field_pair)?;

        let picture_ty = if is_field_pic {
            if is_complementary_field_pair {
                PictureType::Frame
            } else if is_bottom_field {
                PictureType::Bottom
            } else {
                PictureType::Top
            }
        } else {
            PictureType::Frame
        };
        self.set_picture_type(key, picture_ty)?;

        Ok(pic_idx)
    }
}

impl VideoDecoder for Decoder {
    fn begin(
        &mut self,
        _cmd: &mut nvk_cmd_buffer,
        begin_info: &ffi::VkVideoBeginCodingInfoKHR,
    ) {
        self.remove_invalid_slots(&begin_info);
    }

    fn decode(
        &mut self,
        cmd: &mut nvk_cmd_buffer,
        frame_info: &ffi::VkVideoDecodeInfoKHR,
    ) -> Result {
        let h264_pic_info = vk_find_struct_const!(
            frame_info.pNext,
            VIDEO_DECODE_H264_PICTURE_INFO,
            KHR
        )
        .ok_or(ffi::VK_ERROR_UNKNOWN)?;

        let std_pic_info = unsafe { h264_pic_info.pStdPictureInfo.as_ref() }
            .ok_or(ffi::VK_ERROR_UNKNOWN)?;

        // Use the common helper so that inline session parameters
        // (VK_KHR_video_maintenance2) are honored: it falls back to the
        // session-parameters object when no inline SPS/PPS is supplied, and
        // tolerates a session created without a parameters object.
        let mut sps_ptr: *const ffi::StdVideoH264SequenceParameterSet =
            std::ptr::null();
        let mut pps_ptr: *const ffi::StdVideoH264PictureParameterSet =
            std::ptr::null();
        unsafe {
            ffi::vk_video_get_h264_parameters(
                &(*cmd.state.video.vid).vk,
                cmd.state.video.params as *const _,
                frame_info as *const _,
                &h264_pic_info as *const _,
                &mut sps_ptr,
                &mut pps_ptr,
            );
        }
        if sps_ptr.is_null() || pps_ptr.is_null() {
            return Err(ffi::VK_ERROR_UNKNOWN);
        }

        let sps = unsafe { *sps_ptr };
        let pps = unsafe { *pps_ptr };

        // I do not know why the size of the coloc buffer is not passed to the hardware.
        let (coloc_size, mbhist_size, history_size) =
            compute_opaque_buffer_sizes(&sps).ok_or(ffi::VK_ERROR_UNKNOWN)?;

        let session = unsafe { cmd.state.video.vid.as_ref() }
            .ok_or(ffi::VK_ERROR_UNKNOWN)?;

        let coloc_total = u64::from(coloc_size)
            .checked_mul(u64::from(self.num_pic_slots))
            .ok_or(ffi::VK_ERROR_UNKNOWN)?;

        if session.mems.iter().any(|m| m.addr == 0)
            || coloc_total > session.mems[0].size_B
            || u64::from(mbhist_size) > session.mems[1].size_B
            || u64::from(history_size) > session.mems[2].size_B
        {
            return bad_input();
        }

        let dst_iv = unsafe {
            ffi::nvk_image_view_from_handle(
                frame_info.dstPictureResource.imageViewBinding,
            )
        };
        let dst_img_ptr = unsafe { *dst_iv }.vk.image as *mut ffi::nvk_image;
        let dst_img = unsafe { &mut *dst_img_ptr };

        let mut nvh264 = _nvdec_h264_pic_s::default();

        nvh264.explicitEOSPresentFlag = 1;
        nvh264.eos = EOS_ARRAY;
        // Decode watchdog: gptimer_on is set in the control params, but the
        // firmware needs a non-zero timeout or the timer never fires and a
        // wedged decode hangs the engine forever. Matches the blob.
        nvh264.gptimer_timeout_value = 81000000;

        nvh264.slice_count = h264_pic_info.sliceCount.into();
        let src_range = to_u32(frame_info.srcBufferRange)?;
        nvh264.stream_len = src_range
            .checked_add(std::mem::size_of_val(&EOS_ARRAY) as u32)
            .ok_or(ffi::VK_ERROR_UNKNOWN)?;

        nvh264.mbhist_buffer_size = mbhist_size;
        nvh264.log2_max_pic_order_cnt_lsb_minus4 =
            sps.log2_max_pic_order_cnt_lsb_minus4.into();
        nvh264.delta_pic_order_always_zero_flag =
            sps.flags.delta_pic_order_always_zero_flag() as i32;
        nvh264.frame_mbs_only_flag = to_i32(sps.flags.frame_mbs_only_flag())?;
        nvh264.PicWidthInMbs = to_i32(
            sps.pic_width_in_mbs_minus1
                .checked_add(1)
                .ok_or(ffi::VK_ERROR_UNKNOWN)?,
        )?;

        nvh264.FrameHeightInMbs = to_i32(
            sps.pic_height_in_map_units_minus1
                .checked_add(1)
                .ok_or(ffi::VK_ERROR_UNKNOWN)?,
        )?;
        if nvh264.frame_mbs_only_flag == 0 {
            nvh264.FrameHeightInMbs = nvh264
                .FrameHeightInMbs
                .checked_mul(2)
                .ok_or(ffi::VK_ERROR_UNKNOWN)?;
        }

        nvh264.set_tileFormat(1);

        let y_log2 = dst_img.planes[0].nil.levels[0].tiling.y_log2;
        if y_log2 == 0 {
            return bad_input();
        }
        nvh264.set_gob_height((y_log2 - 1).into());

        nvh264.entropy_coding_mode_flag =
            pps.flags.entropy_coding_mode_flag() as _;
        nvh264.pic_order_present_flag =
            pps.flags.bottom_field_pic_order_in_frame_present_flag() as _;
        nvh264.num_ref_idx_l0_active_minus1 =
            pps.num_ref_idx_l0_default_active_minus1.into();
        nvh264.num_ref_idx_l1_active_minus1 =
            pps.num_ref_idx_l1_default_active_minus1.into();
        nvh264.deblocking_filter_control_present_flag =
            pps.flags.deblocking_filter_control_present_flag() as _;
        nvh264.redundant_pic_cnt_present_flag =
            pps.flags.redundant_pic_cnt_present_flag() as _;
        nvh264.transform_8x8_mode_flag =
            pps.flags.transform_8x8_mode_flag() as _;
        nvh264.pitch_luma = dst_img.planes[0].nil.levels[0].row_stride_B;
        nvh264.pitch_chroma = dst_img.planes[1].nil.levels[0].row_stride_B;
        nvh264.luma_bot_offset = to_u32(nvh264.PicWidthInMbs)? * 16;
        if nvh264.pitch_chroma % 2 != 0 {
            return bad_input();
        }
        nvh264.chroma_bot_offset = nvh264.pitch_chroma / 2;

        nvh264.HistBufferSize = history_size >> 8;

        let is_field = std_pic_info.flags.field_pic_flag() != 0;
        let mbaff_frame_flag = sps.flags.mb_adaptive_frame_field_flag() != 0;
        let mbaff_frame_flag = mbaff_frame_flag && !is_field;
        nvh264.set_MbaffFrameFlag(mbaff_frame_flag.into());

        nvh264.set_direct_8x8_inference_flag(
            sps.flags.direct_8x8_inference_flag() as _,
        );
        nvh264.set_weighted_pred_flag(pps.flags.weighted_pred_flag() as _);
        nvh264.set_constrained_intra_pred_flag(
            pps.flags.constrained_intra_pred_flag() as _,
        );
        nvh264.set_ref_pic_flag(std_pic_info.flags.is_reference() as _);
        nvh264.set_field_pic_flag(std_pic_info.flags.field_pic_flag() as _);
        nvh264
            .set_bottom_field_flag(std_pic_info.flags.bottom_field_flag() as _);
        nvh264.set_second_field(
            std_pic_info.flags.complementary_field_pair() as _
        );
        nvh264.set_log2_max_frame_num_minus4(
            sps.log2_max_frame_num_minus4.into(),
        );
        nvh264.set_chroma_format_idc(sps.chroma_format_idc);
        nvh264.set_pic_order_cnt_type(sps.pic_order_cnt_type);
        nvh264.set_pic_init_qp_minus26(pps.pic_init_qp_minus26.into());
        nvh264.set_chroma_qp_index_offset(pps.chroma_qp_index_offset.into());
        nvh264.set_second_chroma_qp_index_offset(
            pps.second_chroma_qp_index_offset.into(),
        );

        nvh264.set_weighted_bipred_idc(pps.weighted_bipred_idc);
        nvh264.set_frame_num(std_pic_info.frame_num.into());

        nvh264.CurrFieldOrderCnt[0] = if std_pic_info.PicOrderCnt[0] != i32::MAX
        {
            std_pic_info.PicOrderCnt[0]
        } else {
            std_pic_info.PicOrderCnt[1]
        };

        nvh264.CurrFieldOrderCnt[1] = if std_pic_info.PicOrderCnt[1] != i32::MAX
        {
            std_pic_info.PicOrderCnt[1]
        } else {
            std_pic_info.PicOrderCnt[0]
        };

        nvh264.WeightScale = [[[0x10; 4]; 4]; 6];
        nvh264.WeightScale8x8 = [[[0x10; 8]; 8]; 2];

        // NVDEC wants sliceCount+1 entries: the byte offset of each slice plus a
        // final entry marking the end of the last slice. Vulkan's pSliceOffsets
        // only has sliceCount entries (the slice starts), so reading index
        // [sliceCount] is out of bounds.
        let slice_count = h264_pic_info.sliceCount as usize;
        let max_slices = i64::from(nvh264.PicWidthInMbs)
            * i64::from(nvh264.FrameHeightInMbs);
        if slice_count == 0
            || slice_count as i64 > max_slices
            || h264_pic_info.pSliceOffsets.is_null()
        {
            return bad_input();
        }
        let mut slice_offsets = Vec::with_capacity(slice_count + 1);
        for i in 0..slice_count {
            slice_offsets.push(unsafe { *h264_pic_info.pSliceOffsets.add(i) });
        }
        slice_offsets.push(src_range);

        let mut luma_base = [0; 17];
        let mut chroma_base = [0; 17];

        self.set_reference_frames(
            &mut nvh264,
            &frame_info,
            (&mut luma_base, &mut chroma_base),
        )?;

        let dst_key = slot_key(&frame_info.dstPictureResource);
        let dst_layer = dst_key.layer;
        let setup_slot_index = if frame_info.pSetupReferenceSlot.is_null() {
            -1
        } else {
            unsafe { (*frame_info.pSetupReferenceSlot).slotIndex }
        };
        let cur_pic_idx = self.set_current_picture_slot(
            dst_key,
            setup_slot_index,
            std_pic_info,
            nvh264.frame_mbs_only_flag == 0,
        )?;
        nvh264.set_CurrPicIdx(cur_pic_idx);
        nvh264.set_CurrColIdx(cur_pic_idx);

        nvh264.set_lossless_ipred8x8_filter_enable(0);
        nvh264.set_qpprime_y_zero_transform_bypass_flag(
            sps.flags.qpprime_y_zero_transform_bypass_flag(),
        );

        luma_base[cur_pic_idx as usize] =
            plane_layer_base(dst_img_ptr, 0, dst_layer)?;
        chroma_base[cur_pic_idx as usize] =
            plane_layer_base(dst_img_ptr, 1, dst_layer)?;

        let src_buffer =
            unsafe { ffi::nvk_buffer_from_handle(frame_info.srcBuffer) };
        let src_address = unsafe {
            ffi::vk_buffer_address(
                &(*src_buffer).vk,
                frame_info.srcBufferOffset,
            )
        };

        let GpuBufferAddresses {
            pic: pic_gpu_address,
            slice_offsets: slice_offsets_address,
            mbstatus: mbstatus_address,
        } = upload_to_the_gpu(cmd, &nvh264, &slice_offsets)?;

        let mut push = Push::new();

        push.push_mthd(clc5b0::SetApplicationId {
            id: clc5b0::SetApplicationIdId::H264,
        });

        push.push_mthd(clc5b0::SetControlParams {
            codec_type: clc5b0::SetControlParamsCodecType::H264,
            gptimer_on: 1,
            err_conceal_on: 1,
            mbtimer_on: 1,
            error_frm_idx: self
                .frame_num
                .checked_rem(sps.max_num_ref_frames.into())
                .unwrap_or(0),
            ret_error: 0,
            ec_intra_frame_using_pslc: 0,
            all_intra_frame: 0,
            reserved: Default::default(),
        });

        push.push_mthd(clc5b0::SetDrvPicSetupOffset {
            offset: addr_hi(pic_gpu_address)?,
        });

        push.push_mthd(clc5b0::SetInBufBaseOffset {
            offset: addr_hi(src_address)?,
        });

        push.push_mthd(clc5b0::SetPictureIndex {
            index: self.frame_num,
        });

        push.push_mthd(clc5b0::SetSliceOffsetsBufOffset {
            offset: addr_hi(slice_offsets_address)?,
        });

        push.push_mthd(clc5b0::SetColocDataOffset {
            offset: addr_hi(session.mems[0].addr)?,
        });

        push.push_mthd(clc5b0::SetHistoryOffset {
            offset: addr_hi(session.mems[2].addr)?,
        });

        push.push_mthd(clc5b0::SetNvdecStatusOffset {
            offset: addr_hi(mbstatus_address)?,
        });

        for (i, &offset) in luma_base.iter().enumerate() {
            push.push_array_mthd(i, clc5b0::SetPictureLumaOffset { offset });
        }

        for (i, &offset) in chroma_base.iter().enumerate() {
            push.push_array_mthd(i, clc5b0::SetPictureChromaOffset { offset });
        }

        push.push_mthd(clc5b0::H264SetMbhistBufOffset {
            offset: addr_hi(session.mems[1].addr)?,
        });

        push.push_mthd(clc5b0::Execute {
            notify: clc5b0::ExecuteNotify::Disable,
            notify_on: clc5b0::ExecuteNotifyOn::End,
            awaken: clc5b0::ExecuteAwaken::Disable,
        });

        nvk_cmd_buffer_push(cmd, push);
        self.frame_num = self.frame_num.wrapping_add(1);

        let second_field = std_pic_info.flags.complementary_field_pair() != 0;
        if setup_slot_index < 0 && (!is_field || second_field) {
            self.release_slot(dst_key);
        }

        Ok(())
    }
}
