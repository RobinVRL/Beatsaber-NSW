/* Android/Linux pthread scheduling ABI. Horizon does not expose POSIX
 * realtime scheduling policies or per-pthread CPU clock IDs. MIT license. */
#ifndef BS_BIONIC_SCHED_BRIDGE_H
#define BS_BIONIC_SCHED_BRIDGE_H
#include <pthread.h>
#include <time.h>
#include <sched.h>

int bs_bionic_sched_priority_max(int policy);
int bs_bionic_sched_priority_min(int policy);
int bs_bionic_pthread_getschedparam(pthread_t thread, int *policy,
                                  struct sched_param *parameters);
int bs_bionic_pthread_setschedparam(pthread_t thread, int policy,
                                  const struct sched_param *parameters);
int bs_bionic_pthread_getcpuclockid(pthread_t thread, clockid_t *clockid);

#endif
