/* Bionic (Linux AArch64) clock ABI for original Quest guest code.
 * SPDX-License-Identifier: MIT
 *
 * Linux clock IDs (REALTIME=0, MONOTONIC=1, ...) differ from newlib's
 * (REALTIME=1, MONOTONIC=4). Binding newlib clock_gettime directly made the
 * guest's CLOCK_REALTIME fail with EINVAL (leaving its timespec unwritten)
 * and its CLOCK_MONOTONIC silently read wall time.
 */
#ifndef BS_BIONIC_TIME_BRIDGE_H
#define BS_BIONIC_TIME_BRIDGE_H

#include <stdbool.h>
#include <time.h>

enum {
    BS_LINUX_CLOCK_REALTIME = 0,
    BS_LINUX_CLOCK_MONOTONIC = 1,
    BS_LINUX_CLOCK_PROCESS_CPUTIME_ID = 2,
    BS_LINUX_CLOCK_THREAD_CPUTIME_ID = 3,
    BS_LINUX_CLOCK_MONOTONIC_RAW = 4,
    BS_LINUX_CLOCK_REALTIME_COARSE = 5,
    BS_LINUX_CLOCK_MONOTONIC_COARSE = 6,
    BS_LINUX_CLOCK_BOOTTIME = 7,
};

/* Map a Linux clock ID onto the host clock that provides its semantics.
 * Horizon has no CPU-time clocks; those map to the monotonic clock. */
bool bs_linux_clock_to_host(int linux_clock, clockid_t *host_clock);

int bs_bionic_clock_gettime(int linux_clock, struct timespec *out);
int bs_bionic_clock_getres(int linux_clock, struct timespec *out);

/* Convert an absolute deadline on host clock `from` into the equivalent
 * absolute deadline on host clock `to`. */
void bs_host_deadline_convert(clockid_t from, const struct timespec *deadline,
                              clockid_t to, struct timespec *out);

#endif
