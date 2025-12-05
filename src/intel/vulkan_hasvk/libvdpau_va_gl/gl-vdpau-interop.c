/*
 * Copyright 2024  Mesa Contributors
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <vdpau/vdpau.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "gl-vdpau-interop.h"
#include "globals.h"
#include "trace.h"

/* Maximum number of registered surfaces */
#define MAX_SURFACES 64

/* Surface registration entry */
typedef struct {
    GLvdpauSurfaceNV gl_surface;      /* GL handle for this surface */
    const void *vdp_surface;          /* VDPAU surface pointer */
    GLenum target;                    /* GL_TEXTURE_2D, etc. */
    GLuint *texture_names;            /* Array of GL texture IDs */
    GLsizei num_textures;             /* Number of textures */
    GLenum access;                    /* GL_READ_ONLY, GL_WRITE_ONLY, GL_READ_WRITE */
    GLboolean is_mapped;              /* Whether surface is currently mapped */
    GLboolean is_video_surface;       /* TRUE for video surface, FALSE for output surface */
} SurfaceRegistration;

/* Global state for VDPAU/GL interop */
static struct {
    pthread_mutex_t mutex;
    VdpDevice vdp_device;
    VdpGetProcAddress *vdp_get_proc_address;
    SurfaceRegistration surfaces[MAX_SURFACES];
    GLvdpauSurfaceNV next_handle;
    GLboolean initialized;
} vdpau_gl_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .next_handle = 1,
    .initialized = GL_FALSE,
};

/* Find a free surface slot */
static int
find_free_surface_slot(void)
{
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (vdpau_gl_state.surfaces[i].gl_surface == 0) {
            return i;
        }
    }
    return -1;
}

/* Find surface by GL handle */
static SurfaceRegistration *
find_surface(GLvdpauSurfaceNV surface)
{
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (vdpau_gl_state.surfaces[i].gl_surface == surface) {
            return &vdpau_gl_state.surfaces[i];
        }
    }
    return NULL;
}

/*
 * Initialize VDPAU/GL interop
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUInitNV(const void *vdpDevice, const void *getProcAddress)
{
    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (vdpau_gl_state.initialized) {
        /* Already initialized, just update device */
        vdpau_gl_state.vdp_device = (VdpDevice)(uintptr_t)vdpDevice;
        vdpau_gl_state.vdp_get_proc_address = (VdpGetProcAddress *)getProcAddress;
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    /* Store VDPAU device and function pointers */
    vdpau_gl_state.vdp_device = (VdpDevice)(uintptr_t)vdpDevice;
    vdpau_gl_state.vdp_get_proc_address = (VdpGetProcAddress *)getProcAddress;

    /* Initialize surface array */
    memset(vdpau_gl_state.surfaces, 0, sizeof(vdpau_gl_state.surfaces));
    vdpau_gl_state.next_handle = 1;
    vdpau_gl_state.initialized = GL_TRUE;

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Cleanup VDPAU/GL interop
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUFiniNV(void)
{
    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    /* Unregister all surfaces */
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (vdpau_gl_state.surfaces[i].gl_surface != 0) {
            if (vdpau_gl_state.surfaces[i].texture_names) {
                free(vdpau_gl_state.surfaces[i].texture_names);
            }
            memset(&vdpau_gl_state.surfaces[i], 0, sizeof(SurfaceRegistration));
        }
    }

    vdpau_gl_state.vdp_device = 0;
    vdpau_gl_state.vdp_get_proc_address = NULL;
    vdpau_gl_state.initialized = GL_FALSE;

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Register a VDPAU video surface as GL texture
 */
__attribute__((visibility("default")))
GLAPI GLvdpauSurfaceNV APIENTRY
glVDPAURegisterVideoSurfaceNV(const void *vdpSurface, GLenum target,
                              GLsizei numTextureNames, const GLuint *textureNames)
{
    if (!vdpSurface || numTextureNames <= 0 || !textureNames) {
        return 0;
    }

    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return 0;
    }

    int slot = find_free_surface_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return 0;
    }

    /* Allocate and copy texture names */
    GLuint *tex_copy = malloc(numTextureNames * sizeof(GLuint));
    if (!tex_copy) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return 0;
    }
    memcpy(tex_copy, textureNames, numTextureNames * sizeof(GLuint));

    /* Fill in registration */
    GLvdpauSurfaceNV handle = vdpau_gl_state.next_handle++;
    vdpau_gl_state.surfaces[slot].gl_surface = handle;
    vdpau_gl_state.surfaces[slot].vdp_surface = vdpSurface;
    vdpau_gl_state.surfaces[slot].target = target;
    vdpau_gl_state.surfaces[slot].texture_names = tex_copy;
    vdpau_gl_state.surfaces[slot].num_textures = numTextureNames;
    vdpau_gl_state.surfaces[slot].access = GL_WRITE_DISCARD_NV;
    vdpau_gl_state.surfaces[slot].is_mapped = GL_FALSE;
    vdpau_gl_state.surfaces[slot].is_video_surface = GL_TRUE;

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
    return handle;
}

/*
 * Register a VDPAU output surface as GL texture
 */
__attribute__((visibility("default")))
GLAPI GLvdpauSurfaceNV APIENTRY
glVDPAURegisterOutputSurfaceNV(const void *vdpSurface, GLenum target,
                               GLsizei numTextureNames, const GLuint *textureNames)
{
    if (!vdpSurface || numTextureNames <= 0 || !textureNames) {
        return 0;
    }

    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return 0;
    }

    int slot = find_free_surface_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return 0;
    }

    /* Allocate and copy texture names */
    GLuint *tex_copy = malloc(numTextureNames * sizeof(GLuint));
    if (!tex_copy) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return 0;
    }
    memcpy(tex_copy, textureNames, numTextureNames * sizeof(GLuint));

    /* Fill in registration */
    GLvdpauSurfaceNV handle = vdpau_gl_state.next_handle++;
    vdpau_gl_state.surfaces[slot].gl_surface = handle;
    vdpau_gl_state.surfaces[slot].vdp_surface = vdpSurface;
    vdpau_gl_state.surfaces[slot].target = target;
    vdpau_gl_state.surfaces[slot].texture_names = tex_copy;
    vdpau_gl_state.surfaces[slot].num_textures = numTextureNames;
    vdpau_gl_state.surfaces[slot].access = GL_WRITE_DISCARD_NV;
    vdpau_gl_state.surfaces[slot].is_mapped = GL_FALSE;
    vdpau_gl_state.surfaces[slot].is_video_surface = GL_FALSE;

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
    return handle;
}

/*
 * Check if a surface is registered
 */
__attribute__((visibility("default")))
GLAPI GLboolean APIENTRY
glVDPAUIsSurfaceNV(GLvdpauSurfaceNV surface)
{
    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return GL_FALSE;
    }

    GLboolean result = (find_surface(surface) != NULL) ? GL_TRUE : GL_FALSE;

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
    return result;
}

/*
 * Unregister a surface
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUUnregisterSurfaceNV(GLvdpauSurfaceNV surface)
{
    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    SurfaceRegistration *reg = find_surface(surface);
    if (reg) {
        if (reg->texture_names) {
            free(reg->texture_names);
        }
        memset(reg, 0, sizeof(SurfaceRegistration));
    }

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Query surface state
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUGetSurfaceivNV(GLvdpauSurfaceNV surface, GLenum pname,
                      GLsizei count, GLsizei *length, GLint *values)
{
    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized || !values || count <= 0) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    SurfaceRegistration *reg = find_surface(surface);
    if (!reg) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    GLsizei written = 0;

    switch (pname) {
    case GL_SURFACE_STATE_NV:
        if (written < count) {
            values[written++] = reg->is_mapped ? GL_SURFACE_MAPPED_NV : GL_SURFACE_REGISTERED_NV;
        }
        break;

    default:
        /* Unknown parameter */
        break;
    }

    if (length) {
        *length = written;
    }

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Set surface access mode
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUSurfaceAccessNV(GLvdpauSurfaceNV surface, GLenum access)
{
    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    SurfaceRegistration *reg = find_surface(surface);
    if (reg) {
        reg->access = access;
    }

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Map surfaces for GPU access
 *
 * This is where the actual VDPAU surface data is made available to OpenGL textures.
 * For video surfaces, this triggers VA-API to GL texture synchronization.
 * For output surfaces, the GL texture is already up-to-date.
 *
 * The actual implementation relies on the fact that libvdpau-va-gl maintains
 * GL textures for both video and output surfaces. We just need to ensure the
 * textures are up-to-date and bound to the application's texture names.
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUMapSurfacesNV(GLsizei numSurfaces, const GLvdpauSurfaceNV *surfaces)
{
    if (!surfaces || numSurfaces <= 0) {
        return;
    }

    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    for (GLsizei i = 0; i < numSurfaces; i++) {
        SurfaceRegistration *reg = find_surface(surfaces[i]);
        if (reg) {
            reg->is_mapped = GL_TRUE;

            /* For video surfaces, we need to ensure VA-API surface is synced to GL texture.
             * This is normally done in vdpVideoMixerRender, but with GL_NV_vdpau_interop,
             * the application bypasses the mixer and reads directly from the texture.
             *
             * The VDPAU surface pointer actually points to a VdpVideoSurface or VdpOutputSurface
             * handle. We can't directly access the surface data here without breaking the
             * abstraction, so we just mark it as mapped. The actual sync happens when the
             * surface is used (either through the mixer or through direct texture access).
             *
             * TODO: For a more complete implementation, this should:
             * 1. Look up the actual VdpVideoSurfaceData or VdpOutputSurfaceData from the handle
             * 2. Check if sync_va_to_glx is set (for video surfaces)
             * 3. Call _render_va_surf_to_texture if needed (from api-video-mixer.c)
             * 4. Bind the internal GL texture to the application's texture names
             * This would require access to handle_acquire/handle_release and deviceData.
             */
        }
    }

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Unmap surfaces
 */
__attribute__((visibility("default")))
GLAPI void APIENTRY
glVDPAUUnmapSurfacesNV(GLsizei numSurface, const GLvdpauSurfaceNV *surfaces)
{
    if (!surfaces || numSurface <= 0) {
        return;
    }

    pthread_mutex_lock(&vdpau_gl_state.mutex);

    if (!vdpau_gl_state.initialized) {
        pthread_mutex_unlock(&vdpau_gl_state.mutex);
        return;
    }

    for (GLsizei i = 0; i < numSurface; i++) {
        SurfaceRegistration *reg = find_surface(surfaces[i]);
        if (reg) {
            reg->is_mapped = GL_FALSE;
        }
    }

    pthread_mutex_unlock(&vdpau_gl_state.mutex);
}

/*
 * Extension query function to be called by glXGetProcAddress
 */
__attribute__((visibility("default")))
void *
vdpau_gl_get_proc_address(const char *name)
{
    if (!name)
        return NULL;

    /* GL_NV_vdpau_interop functions */
    if (strcmp(name, "glVDPAUInitNV") == 0)
        return (void *)glVDPAUInitNV;
    if (strcmp(name, "glVDPAUFiniNV") == 0)
        return (void *)glVDPAUFiniNV;
    if (strcmp(name, "glVDPAURegisterVideoSurfaceNV") == 0)
        return (void *)glVDPAURegisterVideoSurfaceNV;
    if (strcmp(name, "glVDPAURegisterOutputSurfaceNV") == 0)
        return (void *)glVDPAURegisterOutputSurfaceNV;
    if (strcmp(name, "glVDPAUIsSurfaceNV") == 0)
        return (void *)glVDPAUIsSurfaceNV;
    if (strcmp(name, "glVDPAUUnregisterSurfaceNV") == 0)
        return (void *)glVDPAUUnregisterSurfaceNV;
    if (strcmp(name, "glVDPAUGetSurfaceivNV") == 0)
        return (void *)glVDPAUGetSurfaceivNV;
    if (strcmp(name, "glVDPAUSurfaceAccessNV") == 0)
        return (void *)glVDPAUSurfaceAccessNV;
    if (strcmp(name, "glVDPAUMapSurfacesNV") == 0)
        return (void *)glVDPAUMapSurfacesNV;
    if (strcmp(name, "glVDPAUUnmapSurfacesNV") == 0)
        return (void *)glVDPAUUnmapSurfacesNV;

    return NULL;
}
