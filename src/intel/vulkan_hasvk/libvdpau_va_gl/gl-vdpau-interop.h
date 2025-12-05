/*
 * Copyright 2024 Mesa Contributors
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#ifndef VA_GL_SRC_GL_VDPAU_INTEROP_H
#define VA_GL_SRC_GL_VDPAU_INTEROP_H

#include <GL/gl.h>

/*
 * Get GL extension function by name
 * Used to hook into glXGetProcAddress
 */
void *vdpau_gl_get_proc_address(const char *name);

#endif /* VA_GL_SRC_GL_VDPAU_INTEROP_H */
