/*
 * Copyright © 2015 Intel Corporation
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

/* This is a hasvk-specific version of genX_pack.h that uses the old-style
 * genxml files (hasvk_gen7_pack.h, etc.) for compatibility with the
 * hasvk-vulkan-video-decode branch.
 */

#ifndef HASVK_GENX_PACK_H
#define HASVK_GENX_PACK_H

#ifndef GFX_VERx10
#  error "The GFX_VERx10 macro must be defined"
#endif

#if (GFX_VERx10 == 70)
#  include "genxml/hasvk_gen7_pack.h"
#elif (GFX_VERx10 == 75)
#  include "genxml/hasvk_gen75_pack.h"
#elif (GFX_VERx10 == 80)
#  include "genxml/hasvk_gen8_pack.h"
#else
#  error "hasvk only supports Gen 7, 7.5, and 8"
#endif

#endif /* HASVK_GENX_PACK_H */
