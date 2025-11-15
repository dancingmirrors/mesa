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

/* This file acts as a dispatcher for video decoding. It now routes all decode
 * operations through the VA-API bridge instead of using native hardware decode.
 * 
 * The VA-API bridge provides stable video decode by leveraging the proven
 * VA-API implementation on Gen7/7.5/8 hardware through the crocus driver,
 * avoiding the GPU hangs that occur with direct hardware programming.
 * 
 * The legacy native implementations have been moved to the docs directory
 * for reference: docs/genX_video_short.c and docs/genX_video_long.c
 */

#include "anv_private.h"
#include "anv_video_vaapi_bridge.h"

#include <stdlib.h>
#include <inttypes.h>
#include "genxml/gen_macros.h"
#include "genxml/hasvk_genX_pack.h"
#include "genxml/hasvk_genX_video_pack.h"

/* Use VA-API bridge for all video decode operations */

void
genX(CmdBeginVideoCodingKHR) (VkCommandBuffer commandBuffer,
                              const VkVideoBeginCodingInfoKHR * pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_video_session, vid, pBeginInfo->videoSession);
   ANV_FROM_HANDLE(anv_video_session_params, params,
                   pBeginInfo->videoSessionParameters);

   /* Bind the video session and parameters to the command buffer.
    * The VA-API session is already initialized during vkCreateVideoSessionKHR.
    */
   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;
}

void
genX(CmdControlVideoCodingKHR) (VkCommandBuffer commandBuffer,
                                const VkVideoCodingControlInfoKHR *
                                pCodingControlInfo)
{
   /* Control operations (reset, etc.) are handled by VA-API internally.
    * No explicit action needed here.
    */
}

void genX(CmdEndVideoCodingKHR) (VkCommandBuffer commandBuffer,
                                 const VkVideoEndCodingInfoKHR *
                                 pEndCodingInfo)
{
   /* End coding - no cleanup needed as VA-API manages its own state.
    * Resources are cleaned up when the video session is destroyed.
    */
}

void genX(CmdDecodeVideoKHR) (VkCommandBuffer commandBuffer,
                              const VkVideoDecodeInfoKHR * frame_info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   /* Route all decode operations through VA-API bridge */
   VkResult result = anv_vaapi_decode_frame(cmd_buffer, frame_info);

   if (result != VK_SUCCESS) {
      if (unlikely(INTEL_DEBUG(DEBUG_PERF))) {
         fprintf(stderr, "VA-API decode failed: %d\n", result);
      }
   }
}
