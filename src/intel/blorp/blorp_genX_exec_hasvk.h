/*
 * Copyright © 2025 Intel Corporation
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

#ifndef BLORP_GENX_EXEC_HASVK_H
#define BLORP_GENX_EXEC_HASVK_H

/* This header is a wrapper around blorp_genX_exec_brw.h that uses
 * hasvk-specific genxml files (hasvk_genX_pack.h) instead of the
 * mainline genxml files (genX_pack.h).
 *
 * The hasvk genxml files were crafted when BRW was still the compiler
 * for Gen7/8 and have the correct structure definitions for these
 * generations.
 */

/* Include blorp private header for blorp_address and blorp_batch definitions */
#include "blorp_priv.h"

/* Define the required macros for genxml before including hasvk pack headers.
 * These are the same definitions that blorp_genX_exec_brw.h uses.
 */
#define __gen_address_type struct blorp_address
#define __gen_user_data struct blorp_batch
#define __gen_combine_address _blorp_combine_address

/* Forward declare _blorp_combine_address - full definition is in blorp_genX_exec_brw.h */
static uint64_t
_blorp_combine_address(struct blorp_batch *batch, void *location,
                       struct blorp_address address, uint32_t delta);

/* Now include hasvk genxml to get all the hasvk-specific definitions */
#include "genxml/hasvk_genX_pack.h"

/* Prevent blorp from re-including genX_pack.h since we already have hasvk definitions */
#define GENX_PACK_H

/* Now include the main blorp implementation */
#include "blorp_genX_exec_brw.h"

#endif /* BLORP_GENX_EXEC_HASVK_H */
