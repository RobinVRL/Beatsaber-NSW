/* The Android guest's random APIs, backed by Horizon's secure random service.
 * POSIX rand48 is deliberately separate from the system C library RNG. */
#include "bionic_random_bridge.h"
#include <errno.h>
#include <stdint.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

/* Linux/Android GRND_NONBLOCK=1 and GRND_RANDOM=2. Horizon's secure random
 * source is nonblocking and supplies cryptographic randomness for both. */
ssize_t bs_bionic_getrandom(void *buffer, size_t length, unsigned flags) {
    if (flags & ~3u) { errno = EINVAL; return -1; }
    if (length && !buffer) { errno = EFAULT; return -1; }
    if (length > (size_t)SSIZE_MAX) length = (size_t)SSIZE_MAX;
#ifdef __SWITCH__
    if (length) randomGet(buffer, length);
    return (ssize_t)length;
#else
    (void)buffer;
    errno = ENOSYS;
    return -1;
#endif
}

int bs_bionic_getentropy(void *buffer, size_t length) {
    if (length > 256) { errno = EIO; return -1; }
    if (length && !buffer) { errno = EFAULT; return -1; }
#ifdef __SWITCH__
    if (length) randomGet(buffer, length);
    return 0;
#else
    (void)buffer;
    errno = ENOSYS;
    return -1;
#endif
}

static uint64_t s_rand48_state = UINT64_C(0x330e);

void bs_bionic_srand48(long seed) {
    s_rand48_state = (((uint64_t)(uint32_t)seed) << 16) | 0x330e;
}

long bs_bionic_lrand48(void) {
    s_rand48_state = (s_rand48_state * UINT64_C(0x5deece66d) + 0xb) &
                     UINT64_C(0xffffffffffff);
    return (long)(s_rand48_state >> 17);
}
