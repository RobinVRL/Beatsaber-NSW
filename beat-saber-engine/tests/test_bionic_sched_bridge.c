#include "bionic_sched_bridge.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>

int main(void) {
    assert(bs_bionic_sched_priority_max(0) == 0);
    assert(bs_bionic_sched_priority_min(0) == 0);
    errno = 0;
    assert(bs_bionic_sched_priority_max(1) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_sched_priority_min(2) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_sched_priority_max(-1) == -1 && errno == EINVAL);
    pthread_t current = pthread_self();
    struct sched_param parameters = {.sched_priority = 17};
    int policy = 123;
    assert(bs_bionic_pthread_getschedparam(current, &policy, &parameters) == ENOTSUP);
    assert(policy == 123 && parameters.sched_priority == 17);
    assert(bs_bionic_pthread_getschedparam(current, NULL, &parameters) == EINVAL);
    assert(bs_bionic_pthread_setschedparam(current, 1, &parameters) == ENOTSUP);
    assert(bs_bionic_pthread_setschedparam(current, 0, NULL) == EINVAL);
    clockid_t clockid = CLOCK_REALTIME;
    assert(bs_bionic_pthread_getcpuclockid(current, &clockid) == ENOTSUP);
    assert(clockid == CLOCK_REALTIME);
    assert(bs_bionic_pthread_getcpuclockid(current, NULL) == EINVAL);
    puts("Bionic scheduler fail-closed ABI tests passed");
    return 0;
}
