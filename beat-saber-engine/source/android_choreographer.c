/* AChoreographer_getInstance/postFrameCallback/postFrameCallback64.
 * The caller's own ALooper dispatches callbacks on a Horizon VI vsync.
 * This only provides actual display frame scheduling, not VR frame timing.
 * SPDX-License-Identifier: MIT
 */
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "android_choreographer.h"
#include "android_window_bridge.h"
#include "guest_tls.h"
#include "util.h"

#define BS_CHOREOGRAPHER_MAX_THREADS 16
#define BS_CHOREOGRAPHER_MAX_CALLBACKS 32

typedef struct BsChoreographerEntry {
    union {
        BsChoreographerCallback callback;
        BsChoreographerCallback64 callback64;
    } function;
    void *data;
    bool is64;
} BsChoreographerEntry;

struct BsChoreographer {
    bool initialized;
    uint64_t owner;
    void *looper;
    ViDisplay display;
    Event vsync;
    /* false: VI vsync was refused (seen on hardware, 0x1272) and callbacks
     * are paced by a 60 Hz system-tick timer instead. */
    bool has_vsync;
    u64 next_tick;
    unsigned pending_count;
    BsChoreographerEntry pending[BS_CHOREOGRAPHER_MAX_CALLBACKS];
};

static struct BsChoreographer g_choreographers[BS_CHOREOGRAPHER_MAX_THREADS];
static Mutex g_choreographer_lock;

static uint64_t bs_choreographer_tid(void) {
    uint64_t tid = 0;
    if (R_FAILED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) return 0;
    return tid;
}

BsChoreographer *bs_choreographer_get_instance(void) {
    /* All callbacks ultimately execute original guest code. An unrelated
     * native thread without Quest Bionic TLS must not own that entrypoint. */
    if (!bs_guest_tls_is_thread_attached()) {
        debugPrintf("[choreographer] refusing frame callbacks without guest TLS\n");
        return NULL;
    }
    /* Android's thread Choreographer is attached to its thread's Looper. */
    void *looper = bs_looper_for_thread();
    if (!looper) {
        debugPrintf("[choreographer] thread lacks an ALooper\n");
        return NULL;
    }
    uint64_t owner = bs_choreographer_tid();
    if (!owner) return NULL;
    mutexLock(&g_choreographer_lock);
    BsChoreographer *entry = NULL;
    for (unsigned i = 0; i < BS_CHOREOGRAPHER_MAX_THREADS; ++i) {
        BsChoreographer *current = &g_choreographers[i];
        if (current->initialized && current->owner == owner) {
            entry = current;
            break;
        }
        if (!entry && !current->initialized) entry = current;
    }
    if (entry && !entry->initialized) {
#if defined(__SWITCH2__)
        /* Under kaiHEN the display belongs to OunceVK; a second VI session
         * from here never returned (frames never started, kaiHEN went down
         * after). Pace with the timer instead and never touch VI. */
        Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        debugPrintf("[choreographer] Switch 2: VI vsync skipped\n");
        if (0) {
#else
        debugPrintf("[choreographer] opening VI vsync\n");
        Result rc = viInitialize(ViServiceType_Default);
        if (R_SUCCEEDED(rc)) {
#endif
            rc = viOpenDefaultDisplay(&entry->display);
            if (R_SUCCEEDED(rc)) {
                rc = viGetDisplayVsyncEvent(&entry->display, &entry->vsync);
                if (R_FAILED(rc)) viCloseDisplay(&entry->display);
            }
            if (R_FAILED(rc)) viExit();
        }
        /* Without frame callbacks Unity never renders, so a missing VI vsync
         * falls back to timer pacing instead of refusing the Choreographer. */
        entry->has_vsync = R_SUCCEEDED(rc);
        entry->next_tick = 0;
        entry->owner = owner;
        entry->looper = looper;
        entry->initialized = true;
        if (entry->has_vsync)
            debugPrintf("[choreographer] instance for tid=%lu (VI vsync)\n",
                        (unsigned long)owner);
        else
            debugPrintf("[choreographer] Horizon VI vsync unavailable (0x%x); "
                        "instance for tid=%lu uses 60 Hz timer\n", rc, (unsigned long)owner);
    }
    mutexUnlock(&g_choreographer_lock);
    return entry;
}

void bs_choreographer_release_looper(uint64_t owner) {
    if (!owner) return;
    mutexLock(&g_choreographer_lock);
    for (unsigned i = 0; i < BS_CHOREOGRAPHER_MAX_THREADS; ++i) {
        BsChoreographer *current = &g_choreographers[i];
        if (!current->initialized || current->owner != owner) continue;
        /* Every callback is dropped on release. None may outlive its Looper,
         * and VI event handles are returned to Horizon without leaking them. */
        current->initialized = false;
        current->pending_count = 0;
        if (current->has_vsync) {
            eventClose(&current->vsync);
            viCloseDisplay(&current->display);
            viExit();
        }
        memset(current, 0, sizeof(*current));
        break;
    }
    mutexUnlock(&g_choreographer_lock);
}

static void bs_choreographer_post(BsChoreographer *choreographer,
                                 BsChoreographerCallback legacy,
                                 BsChoreographerCallback64 modern,
                                 void *data, bool is64) {
    if (!choreographer || (is64 ? !modern : !legacy)) {
        debugPrintf("[choreographer] callback rejected: invalid handle\n");
        return;
    }
    uint64_t caller = bs_choreographer_tid();
    void *wake_looper = NULL;
    mutexLock(&g_choreographer_lock);
    bool known_handle = false;
    for (unsigned i = 0; i < BS_CHOREOGRAPHER_MAX_THREADS; ++i)
        if (choreographer == &g_choreographers[i]) {
            known_handle = true;
            break;
        }
    if (!known_handle || !choreographer->initialized ||
        choreographer->pending_count == BS_CHOREOGRAPHER_MAX_CALLBACKS) {
        debugPrintf("[choreographer] callback rejected: invalid/filled queue\n");
    } else {
        /* A pending event from BEFORE the post must not deliver the callback
         * for an already elapsed display frame. Wait for the next real vsync. */
        /* The VI event is a remote readable event: libnx clears it with
         * svcResetSignal, which reports InvalidState when it is simply not
         * signalled. That is the expected state, not an error. */
        if (choreographer->pending_count == 0 && choreographer->has_vsync)
            (void)eventClear(&choreographer->vsync);
        BsChoreographerEntry *entry =
            &choreographer->pending[choreographer->pending_count++];
        if (is64) entry->function.callback64 = modern;
        else entry->function.callback = legacy;
        entry->data = data;
        entry->is64 = is64;
        static int posts;
        if (posts++ < 3)
            debugPrintf("[choreographer] post%s callback from tid=%lu\n",
                        is64 ? "64" : "", (unsigned long)caller);
        /* Android allows another thread to post into an existing Choreographer,
         * but callback execution MUST remain on the owner's Looper thread. */
        if (caller != choreographer->owner)
            wake_looper = choreographer->looper;
    }
    mutexUnlock(&g_choreographer_lock);
    if (wake_looper) bs_looper_wake(wake_looper);
}

void bs_choreographer_post_frame(BsChoreographer *choreographer,
                                BsChoreographerCallback callback, void *data) {
    bs_choreographer_post(choreographer, callback, NULL, data, false);
}

void bs_choreographer_post_frame64(BsChoreographer *choreographer,
                                  BsChoreographerCallback64 callback, void *data) {
    bs_choreographer_post(choreographer, NULL, callback, data, true);
}

static BsChoreographer *bs_choreographer_current_locked(uint64_t owner) {
    for (unsigned i = 0; i < BS_CHOREOGRAPHER_MAX_THREADS; ++i)
        if (g_choreographers[i].initialized &&
            g_choreographers[i].owner == owner)
            return &g_choreographers[i];
    return NULL;
}

bool bs_choreographer_has_pending_current(void) {
    uint64_t owner = bs_choreographer_tid();
    mutexLock(&g_choreographer_lock);
    BsChoreographer *entry = bs_choreographer_current_locked(owner);
    bool pending = entry && entry->pending_count != 0;
    mutexUnlock(&g_choreographer_lock);
    return pending;
}

int bs_choreographer_dispatch_current(void) {
    if (!bs_guest_tls_is_thread_attached()) return 0;
    uint64_t owner = bs_choreographer_tid();
    if (!owner) return 0;
    /* Keep VI event and callbacks per thread. Never invoke guest code while
     * holding either the VI lock or the ALooper's pipe/condition lock. */
    mutexLock(&g_choreographer_lock);
    BsChoreographer *choreographer = bs_choreographer_current_locked(owner);
    if (!choreographer || !choreographer->pending_count) {
        mutexUnlock(&g_choreographer_lock);
        return 0;
    }
    if (choreographer->has_vsync) {
        if (R_FAILED(eventWait(&choreographer->vsync, 0))) {
            mutexUnlock(&g_choreographer_lock);
            return 0;
        }
        /* Not autoclear: consume this vsync so the next one is waited for. */
        (void)eventClear(&choreographer->vsync);
    } else {
        const u64 period = armNsToTicks(16666667ull);
        u64 now_tick = armGetSystemTick();
        if (now_tick < choreographer->next_tick) {
            mutexUnlock(&g_choreographer_lock);
            return 0;
        }
        /* Keep a steady cadence, but never try to catch up missed frames. */
        choreographer->next_tick = choreographer->next_tick + period > now_tick
            ? choreographer->next_tick + period : now_tick + period;
    }
    BsChoreographerEntry callbacks[BS_CHOREOGRAPHER_MAX_CALLBACKS];
    unsigned count = choreographer->pending_count;
    memcpy(callbacks, choreographer->pending, count * sizeof(callbacks[0]));
    choreographer->pending_count = 0;
    mutexUnlock(&g_choreographer_lock);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        debugPrintf("[choreographer] clock unavailable, dropping frame callbacks\n");
        return 0;
    }
    int64_t timestamp = (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
    static uint64_t dispatches;
    if (++dispatches <= 3 || dispatches % 600 == 0)
        debugPrintf("[choreographer] vsync dispatch #%llu (%u callbacks)\n",
                    (unsigned long long)dispatches, count);
    for (unsigned i = 0; i < count; ++i) {
        if (callbacks[i].is64)
            callbacks[i].function.callback64(timestamp, callbacks[i].data);
        else
            callbacks[i].function.callback((long)timestamp, callbacks[i].data);
    }
    return (int)count;
}
