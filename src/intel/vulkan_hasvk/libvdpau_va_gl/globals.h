/*
 * Copyright 2013-2014  Rinat Ibragimov
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#ifndef VA_GL_SRC_GLOBALS_H
#define VA_GL_SRC_GLOBALS_H

#include <pthread.h>

/** @brief place where all shared global variables live */
struct global_data {
    /** @brief tunables */
    struct {
        int buggy_XCloseDisplay;      ///< avoid calling XCloseDisplay
        int log_thread_id;            ///< include thread id into the log output
        int log_call_duration;        ///< measure call duration
        int log_pq_delay;             ///< measure delay between queueing and displaying presentation
                                      ///< queue introduces
        int log_timestamp;            ///< display timestamps
        int log_stride;               ///< log detailed stride/pitch information for debugging
        int log_slice_order;          ///< log H.264 slice ordering for debugging
        int disable_compositor_check; ///< disable automatic compositor detection (for testing)
    } quirks;
};

extern struct global_data global;

#endif /* VA_GL_SRC_GLOBALS_H */
