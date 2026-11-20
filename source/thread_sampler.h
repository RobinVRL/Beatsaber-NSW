#ifndef BS_THREAD_SAMPLER_H
#define BS_THREAD_SAMPLER_H

#include <stddef.h>
#include <stdint.h>

/* Register/unregister the calling thread for watchdog PC sampling. */
void bs_sampler_register_current(uint64_t tid);
void bs_sampler_unregister_current(void);
/* Horizon thread handle of a registered native pthread_t, or 0. */
uint32_t bs_sampler_handle_for_pthread(uintptr_t thread);

typedef void (*bs_sampler_sink)(void *user, const char *line, size_t length);
/* Pause each registered thread briefly and report PC/LR/frame chain.
 * Lock-free; safe to call from the watchdog thread. */
size_t bs_sampler_dump(bs_sampler_sink sink, void *user);

#endif
