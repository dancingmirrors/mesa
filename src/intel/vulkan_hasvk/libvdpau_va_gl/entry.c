/*
 * Copyright 2013-2014  Rinat Ibragimov
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#include <ctype.h>
#include <stdbool.h>
#include <vdpau/vdpau.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "handle-storage.h"
#include "api.h"
#include "trace.h"
#include "globals.h"
#include "compat.h"


static void
trc_hk(void *longterm_param, void *shortterm_param, int origin, int after)
{
    (void)longterm_param;
    (void)origin;
    int before = !after;

    if (global.quirks.log_call_duration) {
        static __thread struct timespec start_ts = {0, 0};
        if (before) {
            clock_gettime(CLOCK_MONOTONIC, &start_ts);
        }

        if (after) {
            struct timespec end_ts;
            clock_gettime(CLOCK_MONOTONIC, &end_ts);
            double diff = (end_ts.tv_sec - start_ts.tv_sec) +
                          (end_ts.tv_nsec - start_ts.tv_nsec) / 1.0e9;
            printf("Duration %7.5f secs, %s, %s\n",
                diff, reverse_func_id(origin), reverse_status((VdpStatus)shortterm_param));
        }
    }

    if (before && global.quirks.log_timestamp) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        printf("%d.%03d ", (int)now.tv_sec, (int)now.tv_nsec/1000000);
    }

    if (before && global.quirks.log_thread_id) {
        printf("[%5ld] ", (long)get_current_thread_id());
    }
}

static
void
initialize_quirks(void)
{
    global.quirks.buggy_XCloseDisplay = 0;
    global.quirks.show_watermark = 0;
    global.quirks.log_thread_id = 0;
    global.quirks.log_call_duration = 0;
    global.quirks.log_pq_delay = 0;
    global.quirks.log_timestamp = 0;
    global.quirks.avoid_va = 0;
    global.quirks.log_stride = 0;

    /* Check INTEL_DEBUG environment variable for hasvk flag */
    const char *intel_debug = getenv("INTEL_DEBUG");
    if (intel_debug) {
        char *intel_debug_lc = strdup(intel_debug);
        if (intel_debug_lc) {
            /* Convert to lowercase for case-insensitive matching */
            for (int k = 0; intel_debug_lc[k] != 0; k++)
                intel_debug_lc[k] = tolower(intel_debug_lc[k]);

            /* Check for 'hasvk' as a standalone flag (word boundary check) */
            const char *pos = strstr(intel_debug_lc, "hasvk");
            if (pos) {
                /* Verify it's a complete word by checking boundaries */
                bool is_word_start = (pos == intel_debug_lc || pos[-1] == ',' || pos[-1] == ' ');
                bool is_word_end = (pos[5] == '\0' || pos[5] == ',' || pos[5] == ' ');
                if (is_word_start && is_word_end) {
                    /* Enable stride logging when INTEL_DEBUG=hasvk is set */
                    global.quirks.log_stride = 1;
                }
            }
            free(intel_debug_lc);
        }
        /* If strdup fails, we simply skip INTEL_DEBUG-based logging enable.
         * User can still use VDPAU_QUIRKS=logstride as fallback. */
    }

    const char *value = getenv("VDPAU_QUIRKS");
    if (!value)
        return;

    char *value_lc = strdup(value);
    if (NULL == value_lc)
        return;

    for (int k = 0; value_lc[k] != 0; k ++)
        value_lc[k] = tolower(value_lc[k]);

    // tokenize string
    const char delimiter = ',';
    char *item_start = value_lc;
    char *ptr = item_start;
    while (1) {
        int last = (0 == *ptr);
        if (delimiter == *ptr || 0 == *ptr) {
            *ptr = 0;

            if (!strcmp("xclosedisplay", item_start)) {
                global.quirks.buggy_XCloseDisplay = 1;
            } else
            if (!strcmp("showwatermark", item_start)) {
                global.quirks.show_watermark = 1;
            } else
            if (!strcmp("logthreadid", item_start)) {
                global.quirks.log_thread_id = 1;
            } else
            if (!strcmp("logcallduration", item_start)) {
                global.quirks.log_call_duration = 1;
            } else
            if (!strcmp("logpqdelay", item_start)) {
                global.quirks.log_pq_delay = 1;
            } else
            if (!strcmp("logtimestamp", item_start)) {
                global.quirks.log_timestamp = 1;
            } else
            if (!strcmp("logstride", item_start)) {
                global.quirks.log_stride = 1;
            } else
            if (!strcmp("avoidva", item_start)) {
                global.quirks.avoid_va = 1;
            }

            item_start = ptr + 1;
        }
        ptr ++;
        if (last)
            break;
    }

    free(value_lc);
}

__attribute__((constructor))
static void
va_gl_library_constructor(void)
{
    handle_initialize_storage();

    // Initialize global data
    initialize_quirks();

    // initialize tracer
    traceSetTarget(stdout);
    traceSetHook(trc_hk, NULL);
    // Tracing is disabled by default. Use VDPAU_LOG=1 to enable.
    traceEnableTracing(0);
    const char *value = getenv("VDPAU_LOG");
    if (value) {
        char *value_lc = strdup(value); // convert to lowercase
        if (value_lc) {
            for (int k = 0; value_lc[k] != 0; k ++) value_lc[k] = tolower(value_lc[k]);
            // enable tracing for truthy values
            if (!strcmp(value_lc, "1") ||
                !strcmp(value_lc, "true") ||
                !strcmp(value_lc, "on") ||
                !strcmp(value_lc, "enable") ||
                !strcmp(value_lc, "enabled"))
            {
                traceEnableTracing(1);
            }
            free(value_lc);
        }
    }
    traceInfo("Software VDPAU backend library initialized\n");
}

__attribute__((destructor))
static void
va_gl_library_destructor(void)
{
    handle_destory_storage();
}


/* Prototype for the VDPAU backend entry point (called by libvdpau wrapper) */
VdpStatus
vdp_imp_device_create_x11(Display *display, int screen, VdpDevice *device,
                          VdpGetProcAddress **get_proc_address);

__attribute__ ((visibility("default")))
VdpStatus
vdp_imp_device_create_x11(Display *display, int screen, VdpDevice *device,
                          VdpGetProcAddress **get_proc_address)
{
    return traceVdpDeviceCreateX11(display, screen, device, get_proc_address);
}
