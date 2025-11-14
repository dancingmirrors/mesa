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
 *
 * We need to include the hasvk genxml BEFORE including blorp_genX_exec_brw.h
 * and then redefine the GENX macro to use the hasvk versions.
 */

/* First, include hasvk genxml to get all the hasvk-specific definitions */
#include "genxml/hasvk_genX_pack.h"

/* Now redefine GENX to use hasvk prefix for the blorp code.
 * Save the original definition first.
 */
#pragma push_macro("GENX")
#undef GENX

#if GFX_VERx10 == 70
#  define GENX(X) GFX7_##X
#elif GFX_VERx10 == 75
#  define GENX(X) GFX75_##X
#elif GFX_VERx10 == 80
#  define GENX(X) GFX8_##X
#else
#  error "hasvk only supports Gen 7, 7.5, and 8"
#endif

/* Prevent blorp from including genX_pack.h since we already have hasvk definitions */
#define GENX_PACK_H

/* Now include the main blorp implementation */
#include "blorp_genX_exec_brw.h"

/* Restore original GENX macro */
#pragma pop_macro("GENX")

#endif /* BLORP_GENX_EXEC_HASVK_H */
