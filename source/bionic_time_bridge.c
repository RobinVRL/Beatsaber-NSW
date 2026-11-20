/* Bionic (Linux AArch64) clock ABI for original Quest guest code.
 * SPDX-License-Identifier: MIT
 */
#include "bionic_time_bridge.h"

#include <errno.h>
#include <stdint.h>

bool bs_linux_clock_to_host(int linux_clock, clockid_t *host_clock) {
    switch (linux_clock) {
    case BS_LINUX_CLOCK_REALTIME:
    case BS_LINUX_CLOCK_REALTIME_COARSE:
        *host_clock = CLOCK_REALTIME;
        return true;
    case BS_LINUX_CLOCK_MONOTONIC:
    case BS_LINUX_CLOCK_MONOTONIC_RAW:
    case BS_LINUX_CLOCK_MONOTONIC_COARSE:
    case BS_LINUX_CLOCK_BOOTTIME:
    case BS_LINUX_CLOCK_PROCESS_CPUTIME_ID:
    case BS_LINUX_CLOCK_THREAD_CPUTIME_ID:
        *host_clock = CLOCK_MONOTONIC;
        return true;
    default:
        return false;
    }
}

int bs_bionic_clock_gettime(int linux_clock, struct timespec *out) {
    clockid_t host;
    if (!bs_linux_clock_to_host(linux_clock, &host)) {
        errno = EINVAL;
        return -1;
    }
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    return clock_gettime(host, out);
}

int bs_bionic_clock_getres(int linux_clock, struct timespec *out) {
    clockid_t host;
    if (!bs_linux_clock_to_host(linux_clock, &host)) {
        errno = EINVAL;
        return -1;
    }
    if (!out) return 0;
    return clock_getres(host, out);
}

static int64_t timespec_ns(const struct timespec *t) {
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

void bs_host_deadline_convert(clockid_t from, const struct timespec *deadline,
                              clockid_t to, struct timespec *out) {
    if (from == to) {
        *out = *deadline;
        return;
    }
    struct timespec now_from = {0}, now_to = {0};
    clock_gettime(from, &now_from);
    clock_gettime(to, &now_to);
    int64_t remaining = timespec_ns(deadline) - timespec_ns(&now_from);
    if (remaining < 0) remaining = 0;
    int64_t target = timespec_ns(&now_to) + remaining;
    out->tv_sec = (time_t)(target / 1000000000LL);
    out->tv_nsec = (long)(target % 1000000000LL);
}
