#ifndef BS_BIONIC_RANDOM_BRIDGE_H
#define BS_BIONIC_RANDOM_BRIDGE_H

#include <stddef.h>
#include <sys/types.h>

ssize_t bs_bionic_getrandom(void *buffer, size_t length, unsigned flags);
int bs_bionic_getentropy(void *buffer, size_t length);
void bs_bionic_srand48(long seed);
long bs_bionic_lrand48(void);

#endif
