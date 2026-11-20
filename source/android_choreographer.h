/* Android NDK frame callbacks backed by Horizon's real display vsync event.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_ANDROID_CHOREOGRAPHER_H
#define BS_ANDROID_CHOREOGRAPHER_H
#include <stdbool.h>
#include <stdint.h>

typedef struct BsChoreographer BsChoreographer;
typedef void (*BsChoreographerCallback)(long frame_time_ns, void *data);
typedef void (*BsChoreographerCallback64)(int64_t frame_time_ns, void *data);

/* AChoreographer is per-thread and requires an existing ALooper. */
BsChoreographer *bs_choreographer_get_instance(void);
void bs_choreographer_post_frame(BsChoreographer *choreographer,
                                BsChoreographerCallback callback, void *data);
void bs_choreographer_post_frame64(BsChoreographer *choreographer,
                                  BsChoreographerCallback64 callback, void *data);

/* Invoked by the owning ALooper thread. No callback runs off-thread. */
bool bs_choreographer_has_pending_current(void);
int bs_choreographer_dispatch_current(void);
/* Called when the final owning ALooper reference is released. */
void bs_choreographer_release_looper(uint64_t owner);

#endif
