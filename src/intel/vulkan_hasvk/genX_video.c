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

/* This file acts as a dispatcher between short and long mode implementations
 * of video decoding. The short mode is the default and stable implementation.
 * The long mode is WIP and can be activated by setting the environment
 * variable INTEL_HASVK_VIDEO_DECODE_LONG=1.
 */

#include "anv_private.h"

#include <stdlib.h>
#include <inttypes.h>
#include "genxml/gen_macros.h"
#include "genxml/hasvk_genX_pack.h"
#include "genxml/hasvk_genX_video_pack.h"

/* Forward declarations for _short functions */
void genX(CmdBeginVideoCodingKHR_short) (VkCommandBuffer commandBuffer,
                                         const VkVideoBeginCodingInfoKHR * pBeginInfo);
void genX(CmdControlVideoCodingKHR_short) (VkCommandBuffer commandBuffer,
                                           const VkVideoCodingControlInfoKHR * pCodingControlInfo);
void genX(CmdEndVideoCodingKHR_short) (VkCommandBuffer commandBuffer,
                                       const VkVideoEndCodingInfoKHR * pEndCodingInfo);
void genX(CmdDecodeVideoKHR_short) (VkCommandBuffer commandBuffer,
                                    const VkVideoDecodeInfoKHR * frame_info);

/* Forward declarations for _long functions */
void genX(CmdBeginVideoCodingKHR_long) (VkCommandBuffer commandBuffer,
                                        const VkVideoBeginCodingInfoKHR * pBeginInfo);
void genX(CmdControlVideoCodingKHR_long) (VkCommandBuffer commandBuffer,
                                          const VkVideoCodingControlInfoKHR * pCodingControlInfo);
void genX(CmdEndVideoCodingKHR_long) (VkCommandBuffer commandBuffer,
                                      const VkVideoEndCodingInfoKHR * pEndCodingInfo);
void genX(CmdDecodeVideoKHR_long) (VkCommandBuffer commandBuffer,
                                   const VkVideoDecodeInfoKHR * frame_info);

/* Include the short mode implementation (default) */
#include "genX_video_short.c"

/* Include the long mode implementation (WIP, broken) */
#include "genX_video_long.c"

/* Helper to check if long mode is enabled */
static inline bool
anv_video_use_long_mode(void)
{
   static int long_mode = -1;
   if (long_mode == -1) {
      const char *env = getenv("INTEL_HASVK_VIDEO_DECODE_LONG");
      long_mode = (env && env[0] == '1') ? 1 : 0;
   }
   return long_mode == 1;
}

/* Dispatcher functions that delegate to the appropriate implementation */

void
genX(CmdBeginVideoCodingKHR) (VkCommandBuffer commandBuffer,
                              const VkVideoBeginCodingInfoKHR * pBeginInfo)
{
   if (anv_video_use_long_mode())
      genX(CmdBeginVideoCodingKHR_long)(commandBuffer, pBeginInfo);
   else
      genX(CmdBeginVideoCodingKHR_short)(commandBuffer, pBeginInfo);
}

void
genX(CmdControlVideoCodingKHR) (VkCommandBuffer commandBuffer,
                                const VkVideoCodingControlInfoKHR *
                                pCodingControlInfo)
{
   if (anv_video_use_long_mode())
      genX(CmdControlVideoCodingKHR_long)(commandBuffer, pCodingControlInfo);
   else
      genX(CmdControlVideoCodingKHR_short)(commandBuffer, pCodingControlInfo);
}

void
genX(CmdEndVideoCodingKHR) (VkCommandBuffer commandBuffer,
                            const VkVideoEndCodingInfoKHR *
                            pEndCodingInfo)
{
   if (anv_video_use_long_mode())
      genX(CmdEndVideoCodingKHR_long)(commandBuffer, pEndCodingInfo);
   else
      genX(CmdEndVideoCodingKHR_short)(commandBuffer, pEndCodingInfo);
}

void
genX(CmdDecodeVideoKHR) (VkCommandBuffer commandBuffer,
                         const VkVideoDecodeInfoKHR * frame_info)
{
   if (anv_video_use_long_mode())
      genX(CmdDecodeVideoKHR_long)(commandBuffer, frame_info);
   else
      genX(CmdDecodeVideoKHR_short)(commandBuffer, frame_info);
}
