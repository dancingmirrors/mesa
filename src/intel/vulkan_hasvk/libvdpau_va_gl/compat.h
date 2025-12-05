/*
 * Copyright 2013-2014  Rinat Ibragimov
 *
 * This file is part of libvdpau-va-gl
 *
 * libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING for details.
 */

#ifndef VA_GL_SRC_COMPAT_H
#define VA_GL_SRC_COMPAT_H

#include <unistd.h>
#include <sys/syscall.h>
#ifdef __FreeBSD__
#include <sys/thr.h>
#endif

/* Branch prediction hints */
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif


#if defined(__linux__)
typedef int thread_id_t;
#elif defined(__FreeBSD__)
typedef long thread_id_t;
#else
#error Unknown OS
#endif

static inline thread_id_t
get_current_thread_id(void)
{
#if defined(__linux__)
    return syscall(__NR_gettid);
#elif defined(__FreeBSD__)
    long thread_id;
    thr_self(&thread_id);
    return thread_id;
#endif
}

static inline size_t
thread_is_alive(thread_id_t tid)
{
#if defined(__linux__)
    return kill(tid, 0) == 0;
#elif defined(__FreeBSD__)
    return thr_kill(tid, 0) == 0;
#endif
}


#endif // VA_GL_SRC_COMPAT_H
