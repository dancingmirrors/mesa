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

/* Prevent blorp_genX_exec_brw.h from including genxml/genX_pack.h
 * by defining GENX_PACK_H before including it
 */
#ifndef GENX_PACK_H
#define GENX_PACK_H
#define GENX_PACK_H_DEFINED_BY_HASVK
#endif

/* Include hasvk-specific genxml */
#include "genxml/hasvk_genX_pack.h"

/* Now include the main blorp implementation */
#include "blorp_genX_exec_brw.h"

/* Undefine the guard if we set it */
#ifdef GENX_PACK_H_DEFINED_BY_HASVK
#undef GENX_PACK_H
#undef GENX_PACK_H_DEFINED_BY_HASVK
#endif

#endif /* BLORP_GENX_EXEC_HASVK_H */
