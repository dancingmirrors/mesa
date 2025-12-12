/*
 * Copyright © 2025 dancingmirrors@icloud.com
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

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

#ifdef HAVE_VDPAU
#include "anv_video_vdpau_bridge.h"
#endif

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
#elif GFX_VER <= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.CommandStreamerStallEnable = 1;
      pc.StallAtPixelScoreboard = 1;
   }
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), wait) {
      wait.MFXSyncControlFlag = 1;
   }
#endif

   cmd_buffer->video.vid = NULL;
   cmd_buffer->video.params = NULL;
}

void
genX(CmdDecodeVideoKHR) (VkCommandBuffer commandBuffer,
                         const VkVideoDecodeInfoKHR * frame_info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

#ifdef HAVE_VDPAU
   /* VDPAU is always used for hasvk video decode */
   anv_vdpau_decode_frame(cmd_buffer, frame_info);
#endif
}
