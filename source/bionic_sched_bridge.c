/* The guest asks for Linux SCHED_OTHER/FIFO/RR semantics; blindly passing
 * Linux priorities to Horizon would invert their meaning or change the
 * realtime behavior. Unsupported operations return the POSIX error codes
 * expected by pthread, rather than a fake success. MIT license. */
#include "bionic_sched_bridge.h"
#include "bionic_errno.h"
#include <errno.h>
#include <stdint.h>

enum { BS_LINUX_SCHED_OTHER = 0, BS_LINUX_SCHED_FIFO = 1,
       BS_LINUX_SCHED_RR = 2 };

static int priority_bound(int policy) {
    if (policy == BS_LINUX_SCHED_OTHER) return 0;
    /* Linux's FIFO/RR priority bounds (1..99) cannot be promised on
     * Horizon: kernel thread priorities and permissions differ. */
    errno = (policy == BS_LINUX_SCHED_FIFO || policy == BS_LINUX_SCHED_RR)
            ? ENOTSUP : EINVAL;
    return -1;
}
int bs_bionic_sched_priority_max(int policy) {
    return priority_bound(policy);
}
int bs_bionic_sched_priority_min(int policy) {
    return priority_bound(policy);
}

int bs_bionic_pthread_getschedparam(pthread_t thread, int *policy,
                                  struct sched_param *parameters) {
    (void)thread;
    if (!policy || !parameters) return EINVAL;
    /* No faithful mapping from pthread_t to native Horizon scheduler state
     * is exposed by libnx. Leave caller outputs unchanged on failure. */
    return ENOTSUP;
}
int bs_bionic_pthread_setschedparam(pthread_t thread, int policy,
                                  const struct sched_param *parameters) {
    (void)thread;
    if (!parameters || policy < 0) return EINVAL;
    return ENOTSUP;
}
int bs_bionic_pthread_getcpuclockid(pthread_t thread, clockid_t *clockid) {
    (void)thread;
    if (!clockid) return EINVAL;
    /* CLOCK_THREAD_CPUTIME_ID in newlib is a constant, not a portable
     * per-thread clock handle for another pthread. Do not fabricate one.
     * Returned directly to the guest, so use the Linux number. */
    return bs_errno_to_bionic(ENOTSUP);
}
