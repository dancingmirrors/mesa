/*
 * Copyright 2013-2014  Rinat Ibragimov
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#define GL_GLEXT_PROTOTYPES
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <vdpau/vdpau.h>
#include <GL/gl.h>
#include "ctx-stack.h"
#include "globals.h"
#include "handle-storage.h"
#include "api.h"
#include "trace.h"


struct task_s {
    struct timespec         when;
    uint32_t                clip_width;
    uint32_t                clip_height;
    VdpOutputSurface        surface;
    unsigned int            wipe_tasks;
    VdpPresentationQueue    queue_id;
};

static GAsyncQueue *async_q = NULL;
static pthread_t    presentation_thread_id;
static int          compositor_detected = -1;  // -1 = not checked, 0 = no, 1 = yes


static
VdpTime
timespec2vdptime(struct timespec t)
{
    return (uint64_t)t.tv_sec * 1000 * 1000 * 1000 + t.tv_nsec;
}

static
struct timespec
vdptime2timespec(VdpTime t)
{
    struct timespec res;
    res.tv_sec = t / (1000*1000*1000);
    res.tv_nsec = t % (1000*1000*1000);
    return res;
}

/**
 * Check if a compositing window manager is running
 *
 * Based on mpv's vo_x11_screen_is_composited() implementation.
 * Compositing window managers cause VDPAU presentation queue timing
 * to be inaccurate because they buffer frames for composition.
 *
 * Detection methods:
 * 1. X11: Check if selection owner exists for _NET_WM_CM_Sn atom
 * 2. Wayland: Always composited (Wayland is always compositing)
 * 3. Environment: Check WAYLAND_DISPLAY to detect Wayland session
 *
 * NOTE: For hasvk video decode, the presentation queue is NOT used.
 * Decoding happens via vdp_decoder_render and data is copied back via
 * vdp_video_surface_get_bits_ycbcr. Presentation is handled by the
 * application (e.g., mpv via Vulkan swapchain). This function is only
 * relevant for applications that use VDPAU's presentation queue directly.
 *
 * When compositor is detected, we disable timing-based frame pacing
 * to avoid stuttering and improve performance for such applications.
 */
static
int
check_compositor(Display *display, int screen)
{
    /* Allow disabling compositor check via environment variable */
    if (global.quirks.disable_compositor_check)
        return 0;

    /* Check if we're running under Wayland (always composited)
     * WAYLAND_DISPLAY is set when running native Wayland or XWayland
     */
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display && wayland_display[0] != '\0') {
        /* Limit display name length for safe logging (prevent log injection) */
        char safe_display[64];
        snprintf(safe_display, sizeof(safe_display), "%.63s", wayland_display);
        traceInfo("Wayland session detected (via WAYLAND_DISPLAY=%s). "
                  "Presentation queue timing will be disabled.\n", safe_display);
        return 1;
    }

    /* Check for X11 compositor using _NET_WM_CM_Sn atom */
    if (display) {
        #define COMPOSITOR_ATOM_NAME_SIZE 50
        char atom_name[COMPOSITOR_ATOM_NAME_SIZE];
        snprintf(atom_name, sizeof(atom_name), "_NET_WM_CM_S%d", screen);
        Atom net_wm_cm = XInternAtom(display, atom_name, False);
        #undef COMPOSITOR_ATOM_NAME_SIZE
        int is_composited = (XGetSelectionOwner(display, net_wm_cm) != None);

        if (is_composited) {
            traceInfo("Compositing window manager detected (X11). "
                      "Presentation queue timing will be disabled for better performance.\n");
            return 1;
        }
    }

    return 0;
}

VdpStatus
vdpPresentationQueueBlockUntilSurfaceIdle(VdpPresentationQueue presentation_queue,
                                          VdpOutputSurface surface,
                                          VdpTime *first_presentation_time)

{
    if (!first_presentation_time)
        return VDP_STATUS_INVALID_POINTER;
    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData)
        return VDP_STATUS_INVALID_HANDLE;
    handle_release(presentation_queue);

    VdpOutputSurfaceData *surfData = handle_acquire(surface, HANDLETYPE_OUTPUT_SURFACE);
    if (NULL == surfData)
        return VDP_STATUS_INVALID_HANDLE;

    /* Use condition variable instead of busy loop.
     * The handle_acquire above ensures surfData remains valid even if the surface
     * is being destroyed concurrently. The while loop properly handles spurious
     * wakeups by rechecking the status. If the surface is destroyed while we wait,
     * vdpOutputSurfaceDestroy will set status to IDLE and broadcast, waking us up. */
    pthread_mutex_lock(&surfData->status_mutex);
    while (surfData->status != VDP_PRESENTATION_QUEUE_STATUS_IDLE) {
        pthread_cond_wait(&surfData->status_cond, &surfData->status_mutex);
    }

    *first_presentation_time = surfData->first_presentation_time;
    pthread_mutex_unlock(&surfData->status_mutex);
    handle_release(surface);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueQuerySurfaceStatus(VdpPresentationQueue presentation_queue,
                                       VdpOutputSurface surface, VdpPresentationQueueStatus *status,
                                       VdpTime *first_presentation_time)
{
    if (!status || !first_presentation_time)
        return VDP_STATUS_INVALID_POINTER;
    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData)
        return VDP_STATUS_INVALID_HANDLE;
    VdpOutputSurfaceData *surfData = handle_acquire(surface, HANDLETYPE_OUTPUT_SURFACE);
    if (NULL == surfData) {
        handle_release(presentation_queue);
        return VDP_STATUS_INVALID_HANDLE;
    }

    pthread_mutex_lock(&surfData->status_mutex);
    *status = surfData->status;
    *first_presentation_time = surfData->first_presentation_time;
    pthread_mutex_unlock(&surfData->status_mutex);

    handle_release(presentation_queue);
    handle_release(surface);

    return VDP_STATUS_OK;
}

static
void
free_glx_pixmaps(VdpPresentationQueueTargetData *pqTargetData)
{
    Display *dpy = pqTargetData->deviceData->display;

    // if pixmap is None, nothing was allocated
    if (None == pqTargetData->pixmap)
        return;

    glXDestroyGLXPixmap(dpy, pqTargetData->glx_pixmap);
    XFreeGC(dpy, pqTargetData->plain_copy_gc);
    XFreePixmap(dpy, pqTargetData->pixmap);
    pqTargetData->pixmap = None;
}

// create new pixmap, glx pixmap, GC if size has changed.
// This function relies on external serializing Xlib access
static
void
recreate_pixmaps_if_geometry_changed(VdpPresentationQueueTargetData *pqTargetData)
{
    Window          root_wnd;
    int             xpos, ypos;
    unsigned int    width, height, border_width, depth;
    Display        *dpy = pqTargetData->deviceData->display;

    XGetGeometry(dpy, pqTargetData->drawable, &root_wnd, &xpos, &ypos, &width, &height,
                 &border_width, &depth);
    if (width != pqTargetData->drawable_width || height != pqTargetData->drawable_height) {
        free_glx_pixmaps(pqTargetData);
        pqTargetData->drawable_width = width;
        pqTargetData->drawable_height = height;

        pqTargetData->pixmap = XCreatePixmap(dpy, pqTargetData->deviceData->root,
                                             pqTargetData->drawable_width,
                                             pqTargetData->drawable_height, depth);
        XGCValues gc_values = {.function = GXcopy, .graphics_exposures = True };
        pqTargetData->plain_copy_gc = XCreateGC(dpy, pqTargetData->pixmap,
                                                GCFunction | GCGraphicsExposures, &gc_values);
        pqTargetData->glx_pixmap = glXCreateGLXPixmap(dpy, pqTargetData->xvi, pqTargetData->pixmap);
        XSync(dpy, False);
    }
}

static
void
do_presentation_queue_display(struct task_s *task)
{
    VdpPresentationQueueData *pqData =
        handle_acquire(task->queue_id, HANDLETYPE_PRESENTATION_QUEUE);
    if (!pqData)
        return;
    VdpDeviceData *deviceData = pqData->deviceData;
    const VdpOutputSurface surface = task->surface;
    const uint32_t clip_width = task->clip_width;
    const uint32_t clip_height = task->clip_height;

    VdpOutputSurfaceData *surfData = handle_acquire(surface, HANDLETYPE_OUTPUT_SURFACE);
    if (surfData == NULL) {
        handle_release(task->queue_id);
        return;
    }

    glx_ctx_lock();
    recreate_pixmaps_if_geometry_changed(pqData->targetData);
    glXMakeCurrent(deviceData->display, pqData->targetData->glx_pixmap, pqData->targetData->glc);

    const uint32_t target_width  = (clip_width > 0)  ? clip_width  : surfData->width;
    const uint32_t target_height = (clip_height > 0) ? clip_height : surfData->height;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, target_width, target_height, 0, -1.0, 1.0);
    glViewport(0, 0, target_width, target_height);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(1.0f/surfData->width, 1.0f/surfData->height, 1.0f);

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, surfData->tex_id);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
        glTexCoord2i(0, 0);                        glVertex2i(0, 0);
        glTexCoord2i(target_width, 0);             glVertex2i(target_width, 0);
        glTexCoord2i(target_width, target_height); glVertex2i(target_width, target_height);
        glTexCoord2i(0, target_height);            glVertex2i(0, target_height);
    glEnd();

    // Submit rendering commands without waiting for completion.
    // glFlush() ensures GPU commands are submitted but doesn't block.
    // This allows the presentation thread to move on to the next frame immediately,
    // enabling effective frame dropping on slow hardware. The GPU will process
    // frames asynchronously, and surfaces are safe due to reference counting.
    glFlush();
    GLenum gl_error = glGetError();

    // XCopyArea to present the frame to the window. Use XFlush instead of XSync
    // to avoid blocking. The rendering happens async on the GPU, and the window
    // update happens async via X11. Surface can be marked IDLE immediately.
    XCopyArea(deviceData->display, pqData->targetData->pixmap, pqData->targetData->drawable,
              pqData->targetData->plain_copy_gc, 0, 0, target_width, target_height, 0, 0);
    XFlush(deviceData->display);

    glx_ctx_unlock();

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&surfData->status_mutex);
    surfData->first_presentation_time = timespec2vdptime(now);
    surfData->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;
    pthread_cond_signal(&surfData->status_cond);
    pthread_mutex_unlock(&surfData->status_mutex);

    if (global.quirks.log_pq_delay) {
            const int64_t delta = timespec2vdptime(now) - surfData->queued_at;
            const struct timespec delta_ts = vdptime2timespec(delta);
            traceInfo("pqdelay %d.%09d %d.%09d\n", (int)now.tv_sec, (int)now.tv_nsec,
                      delta_ts.tv_sec, delta_ts.tv_nsec);
    }

    handle_release(surface);
    handle_release(task->queue_id);

    if (GL_NO_ERROR != gl_error) {
        traceError("error (%s): gl error %d\n", __func__, gl_error);
    }
}

static gint
compare_func(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const struct task_s *task_a = a;
    const struct task_s *task_b = b;

    if (task_a->when.tv_sec < task_b->when.tv_sec)
        return -1;
    else if (task_a->when.tv_sec > task_b->when.tv_sec)
        return 1;
    else if (task_a->when.tv_nsec < task_b->when.tv_nsec)
        return -1;
    else if (task_a->when.tv_nsec > task_b->when.tv_nsec)
        return 1;
    else
        return 0;
}

static
void *
presentation_thread(void *param)
{
    GQueue *int_q = g_queue_new();          // internal queue of task, always sorted

    while (1) {
        gint64 timeout;
        struct task_s *task = g_queue_peek_head(int_q);

        if (task) {
            // internal queue have a task
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            timeout = (task->when.tv_sec - now.tv_sec) * 1000 * 1000 +
                      (task->when.tv_nsec - now.tv_nsec) / 1000;
            if (timeout <= 0) {
                // task is ready to go
                g_queue_pop_head(int_q); // remove it from queue

                // Frame dropping: On slow hardware, rendering can take longer than
                // the frame time, causing a backlog. We aggressively drop frames
                // to maintain real-time playback while ensuring we never get stuck.
                //
                // Strategy: Find the newest ready frame and drop everything older.
                // This minimizes processing time when heavily behind schedule.

                // Scan queue to find the newest (least late) ready frame
                struct task_s *newest_ready_task = task;
                gint64 newest_ready_timeout = timeout;
                gint total_dropped = 0;

                GList *iter = g_queue_peek_head_link(int_q);
                while (iter != NULL) {
                    struct task_s *next_task = iter->data;
                    gint64 next_timeout = (next_task->when.tv_sec - now.tv_sec) * 1000 * 1000 +
                                         (next_task->when.tv_nsec - now.tv_nsec) / 1000;
                    if (next_timeout <= 0) {
                        // This frame is also ready - check if it's newer
                        if (next_timeout > newest_ready_timeout) {
                            newest_ready_timeout = next_timeout;
                            newest_ready_task = next_task;
                        }
                    } else {
                        // Stop when we hit a frame that's not ready yet
                        break;
                    }
                    iter = g_list_next(iter);
                }

                // If current frame is NOT the newest ready frame, drop it
                if (task != newest_ready_task) {
                    // This frame is old, drop it to catch up to the newest ready frame
                    VdpOutputSurfaceData *surfData = handle_acquire(task->surface, HANDLETYPE_OUTPUT_SURFACE);
                    if (surfData) {
                        // Mark surface as idle without displaying
                        pthread_mutex_lock(&surfData->status_mutex);
                        surfData->first_presentation_time = timespec2vdptime(now);
                        surfData->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;
                        pthread_cond_signal(&surfData->status_cond);
                        pthread_mutex_unlock(&surfData->status_mutex);
                        handle_release(task->surface);
                    }
                    total_dropped++;
                    g_slice_free(struct task_s, task);

                    // Now batch-drop ALL frames before the newest ready frame
                    // This is more efficient than processing them one by one
                    while (!g_queue_is_empty(int_q)) {
                        struct task_s *next = g_queue_peek_head(int_q);
                        if (next == newest_ready_task) {
                            // Found the newest ready frame, stop dropping
                            break;
                        }

                        // Drop this frame too
                        g_queue_pop_head(int_q);
                        VdpOutputSurfaceData *nextSurfData = handle_acquire(next->surface, HANDLETYPE_OUTPUT_SURFACE);
                        if (nextSurfData) {
                            pthread_mutex_lock(&nextSurfData->status_mutex);
                            nextSurfData->first_presentation_time = timespec2vdptime(now);
                            nextSurfData->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;
                            pthread_cond_signal(&nextSurfData->status_cond);
                            pthread_mutex_unlock(&nextSurfData->status_mutex);
                            handle_release(next->surface);
                        }
                        total_dropped++;
                        g_slice_free(struct task_s, next);
                    }

                    if (global.quirks.log_pq_delay && total_dropped > 0) {
                        traceInfo("Batch dropped %d frames, current was %lld us late, keeping newest at %lld us late\n",
                                  total_dropped, (long long)(-timeout), (long long)(-newest_ready_timeout));
                    }

                    // Continue to next iteration which will process the newest ready frame
                    continue;
                }

                // Display this frame (it's the newest ready frame)
                do_presentation_queue_display(task);
                g_slice_free(struct task_s, task);
                continue;
            }
        } else {
            // no tasks in queue, sleep for a while
            timeout = 1000 * 1000; // one second
        }

        task = g_async_queue_timeout_pop(async_q, timeout);
        if (task) {
            if (task->wipe_tasks) {
                // create new internal queue by filtering old
                GQueue *new_q = g_queue_new();
                while (!g_queue_is_empty(int_q)) {
                    struct task_s *t = g_queue_pop_head(int_q);
                    if (t->queue_id != task->queue_id)
                        g_queue_push_tail(new_q, t);
                }
                g_queue_free(int_q);
                int_q = new_q;

                g_slice_free(struct task_s, task);
                continue;
            }
            g_queue_insert_sorted(int_q, task, compare_func, NULL);
        }
    }

    g_queue_free(int_q);
    return NULL;
}

VdpStatus
vdpPresentationQueueCreate(VdpDevice device, VdpPresentationQueueTarget presentation_queue_target,
                           VdpPresentationQueue *presentation_queue)
{
    if (!presentation_queue)
        return VDP_STATUS_INVALID_POINTER;
    VdpDeviceData *deviceData = handle_acquire(device, HANDLETYPE_DEVICE);
    if (NULL == deviceData)
        return VDP_STATUS_INVALID_HANDLE;

    VdpPresentationQueueTargetData *targetData =
        handle_acquire(presentation_queue_target, HANDLETYPE_PRESENTATION_QUEUE_TARGET);
    if (NULL == targetData) {
        handle_release(device);
        return VDP_STATUS_INVALID_HANDLE;
    }

    VdpPresentationQueueData *data = calloc(1, sizeof(VdpPresentationQueueData));
    if (NULL == data) {
        handle_release(device);
        handle_release(presentation_queue_target);
        return VDP_STATUS_RESOURCES;
    }

    data->type = HANDLETYPE_PRESENTATION_QUEUE;
    data->device = device;
    data->deviceData = deviceData;
    data->target = presentation_queue_target;
    data->targetData = targetData;
    data->bg_color.red = 0.0;
    data->bg_color.green = 0.0;
    data->bg_color.blue = 0.0;
    data->bg_color.alpha = 0.0;

    ref_device(deviceData);
    ref_pq_target(targetData);
    *presentation_queue = handle_insert(data);

    // initialize queue and launch worker thread
    if (!async_q) {
        async_q = g_async_queue_new();
        pthread_create(&presentation_thread_id, NULL, presentation_thread, data);
    }

    handle_release(device);
    handle_release(presentation_queue_target);

    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueDestroy(VdpPresentationQueue presentation_queue)
{
    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData)
        return VDP_STATUS_INVALID_HANDLE;

    struct task_s *task = g_slice_new0(struct task_s);
    task->when = vdptime2timespec(0);   // as early as possible
    task->queue_id = presentation_queue;
    task->wipe_tasks = 1;
    g_async_queue_push(async_q, task);

    handle_expunge(presentation_queue);
    unref_device(pqData->deviceData);
    unref_pq_target(pqData->targetData);

    free(pqData);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueSetBackgroundColor(VdpPresentationQueue presentation_queue,
                                       VdpColor *const background_color)
{
    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData)
        return VDP_STATUS_INVALID_HANDLE;

    if (background_color) {
        pqData->bg_color = *background_color;
    } else {
        pqData->bg_color.red = 0.0;
        pqData->bg_color.green = 0.0;
        pqData->bg_color.blue = 0.0;
        pqData->bg_color.alpha = 0.0;
    }

    handle_release(presentation_queue);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueGetBackgroundColor(VdpPresentationQueue presentation_queue,
                                       VdpColor *background_color)
{
    if (!background_color)
        return VDP_STATUS_INVALID_POINTER;
    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData)
        return VDP_STATUS_INVALID_HANDLE;

    *background_color = pqData->bg_color;

    handle_release(presentation_queue);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueGetTime(VdpPresentationQueue presentation_queue, VdpTime *current_time)
{
    if (!current_time)
        return VDP_STATUS_INVALID_POINTER;

    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData) {
        /* No valid queue, just return current time */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        *current_time = timespec2vdptime(now);
        return VDP_STATUS_OK;
    }

    VdpDeviceData *deviceData = pqData->deviceData;

    /* Check for compositor on first call (lazy initialization) */
    if (compositor_detected == -1) {
        compositor_detected = check_compositor(deviceData->display, deviceData->screen);
    }

    handle_release(presentation_queue);

    /* Always return current monotonic time.
     *
     * We use CLOCK_MONOTONIC instead of CLOCK_REALTIME to avoid timing issues
     * caused by system time adjustments (NTP, manual changes, etc.). This ensures
     * that presentation times are always monotonically increasing, which is critical
     * for proper A/V sync especially during initial video load under high system load.
     *
     * Note: Even without a compositor, VDPAU timing is not accurate enough
     * for proper frame pacing because:
     * 1. We don't have access to actual vsync events
     * 2. The presentation thread uses glFlush() + XFlush() (non-blocking)
     * 3. Actual presentation time depends on compositor buffering
     *
     * Applications like mpv will detect compositor and disable timing-based
     * frame dropping, relying instead on the presentation queue's own
     * frame dropping logic in the presentation thread.
     */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    *current_time = timespec2vdptime(now);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueDisplay(VdpPresentationQueue presentation_queue, VdpOutputSurface surface,
                            uint32_t clip_width, uint32_t clip_height,
                            VdpTime earliest_presentation_time)
{
    VdpPresentationQueueData *pqData =
        handle_acquire(presentation_queue, HANDLETYPE_PRESENTATION_QUEUE);
    if (NULL == pqData)
        return VDP_STATUS_INVALID_HANDLE;

    VdpOutputSurfaceData *surfData = handle_acquire(surface, HANDLETYPE_OUTPUT_SURFACE);
    if (NULL == surfData) {
        handle_release(presentation_queue);
        return VDP_STATUS_INVALID_HANDLE;
    }
    if (pqData->deviceData != surfData->deviceData) {
        handle_release(surface);
        handle_release(presentation_queue);
        return VDP_STATUS_HANDLE_DEVICE_MISMATCH;
    }

    /* Check for compositor on first call (lazy initialization) */
    if (compositor_detected == -1) {
        compositor_detected = check_compositor(pqData->deviceData->display,
                                               pqData->deviceData->screen);
    }

    struct task_s *task = g_slice_new0(struct task_s);

    /* When compositor is detected, ignore earliest_presentation_time and
     * display frames immediately (use current time). This prevents the
     * presentation queue from artificially delaying frames based on
     * inaccurate timing information from the compositing window manager.
     *
     * With a compositor, the window manager buffers frames for composition,
     * making VDPAU's timing unreliable. By displaying immediately, we:
     * 1. Reduce input lag
     * 2. Let the compositor handle frame pacing
     * 3. Avoid stuttering from incorrect timing predictions
     * 4. Improve performance by not sleeping when we shouldn't
     */
    if (compositor_detected) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        task->when = now;
    } else {
        /* earliest_presentation_time is based on CLOCK_MONOTONIC time since
         * vdpPresentationQueueGetTime returns CLOCK_MONOTONIC time.
         * Applications (like mpv) query current time, add a delta, and pass
         * it back here, ensuring timestamp consistency.
         */
        task->when = vdptime2timespec(earliest_presentation_time);
    }

    task->clip_width = clip_width;
    task->clip_height = clip_height;
    task->surface = surface;
    task->queue_id = presentation_queue;

    pthread_mutex_lock(&surfData->status_mutex);
    surfData->first_presentation_time = 0;
    surfData->status = VDP_PRESENTATION_QUEUE_STATUS_QUEUED;
    pthread_mutex_unlock(&surfData->status_mutex);

    if (global.quirks.log_pq_delay) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        surfData->queued_at = timespec2vdptime(now);
    }

    g_async_queue_push(async_q, task);

    handle_release(presentation_queue);
    handle_release(surface);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueTargetCreateX11(VdpDevice device, Drawable drawable,
                                    VdpPresentationQueueTarget *target)
{
    if (!target)
        return VDP_STATUS_INVALID_POINTER;
    VdpDeviceData *deviceData = handle_acquire(device, HANDLETYPE_DEVICE);
    if (NULL == deviceData)
        return VDP_STATUS_INVALID_HANDLE;

    VdpPresentationQueueTargetData *data = calloc(1, sizeof(VdpPresentationQueueTargetData));
    if (NULL == data) {
        handle_release(device);
        return VDP_STATUS_RESOURCES;
    }

    glx_ctx_lock();
    data->type = HANDLETYPE_PRESENTATION_QUEUE_TARGET;
    data->device = device;
    data->deviceData = deviceData;
    data->drawable = drawable;
    data->refcount = 0;
    pthread_mutex_init(&data->refcount_mutex, NULL);

    // emulate geometry change. Hope there will be no drawables of such size
    data->drawable_width = (unsigned int)(-1);
    data->drawable_height = (unsigned int)(-1);
    data->pixmap = None;

    // No double buffering since we are going to render to glx pixmap
    GLint att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, None };
    data->xvi = glXChooseVisual(deviceData->display, deviceData->screen, att);
    if (NULL == data->xvi) {
        traceError("error (%s): glXChooseVisual failed\n", __func__);
        free(data);
        glx_ctx_unlock();
        handle_release(device);
        return VDP_STATUS_ERROR;
    }
    recreate_pixmaps_if_geometry_changed(data);

    // create context for dislaying result (can share display lists with deviceData->glc
    data->glc = glXCreateContext(deviceData->display, data->xvi, deviceData->root_glc, GL_TRUE);
    ref_device(deviceData);
    *target = handle_insert(data);
    glx_ctx_unlock();

    handle_release(device);
    return VDP_STATUS_OK;
}

VdpStatus
vdpPresentationQueueTargetDestroy(VdpPresentationQueueTarget presentation_queue_target)
{
    VdpPresentationQueueTargetData *pqTargetData =
        handle_acquire(presentation_queue_target, HANDLETYPE_PRESENTATION_QUEUE_TARGET);
    if (NULL == pqTargetData)
        return VDP_STATUS_INVALID_HANDLE;
    VdpDeviceData *deviceData = pqTargetData->deviceData;

    if (0 != pqTargetData->refcount) {
        traceError("warning (%s): non-zero reference count (%d)\n", __func__,
                   pqTargetData->refcount);
        handle_release(presentation_queue_target);
        return VDP_STATUS_ERROR;
    }

    // drawable may be destroyed already, so one should activate global context
    glx_ctx_push_thread_local(deviceData);
    glXDestroyContext(deviceData->display, pqTargetData->glc);
    free_glx_pixmaps(pqTargetData);

    GLenum gl_error = glGetError();
    glx_ctx_pop();
    if (GL_NO_ERROR != gl_error) {
        traceError("error (%s): gl error %d\n", __func__, gl_error);
        handle_release(presentation_queue_target);
        return VDP_STATUS_ERROR;
    }

    unref_device(deviceData);
    XFree(pqTargetData->xvi);
    pthread_mutex_destroy(&pqTargetData->refcount_mutex);
    handle_expunge(presentation_queue_target);
    free(pqTargetData);
    return VDP_STATUS_OK;
}
