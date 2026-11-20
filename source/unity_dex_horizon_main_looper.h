/* Bounded native queue on the real Horizon applet main thread / ALooper.
 *
 * The original Quest DEX imports android.os.Handler.<init>, post,
 * postDelayed and removeCallbacks. This module supplies a real monotonic
 * ordered native work queue, owner-thread execution, wake, cancellation
 * and strong pending-Runnable lifetime through explicit Java host callbacks.
 *
 * Native ALooper existence does NOT construct Android java.os.Looper,
 * Handler, MessageQueue, Thread or Runnable. Until the original Java VM
 * independently constructs/verifies those objects AND exposes executable
 * Runnable.run, Java Handler attachment/posting remains unavailable.
 */
#ifndef BS_UNITY_DEX_HORIZON_MAIN_LOOPER_H
#define BS_UNITY_DEX_HORIZON_MAIN_LOOPER_H

#include "unity_dex_vm.h"
#include "unity_dex_horizon_java_runtime.h"
#include "unity_dex_horizon_services.h"
#include <stddef.h>
#include <stdint.h>

#define BS_HORIZON_MAIN_LOOPER_MAX_POSTS 64u

typedef struct BsDexHorizonMainLooperNative {
    void *user;
    int (*observe_owner_thread)(void *user, uint64_t *horizon_thread_id);
    int (*acquire_native_looper)(void *user, void **owned_handle);
    void (*release_native_looper)(void *user, void *owned_handle);
    int (*wake)(void *user, void *owned_handle);
    int (*monotonic_time_ns)(void *user, uint64_t *now);
} BsDexHorizonMainLooperNative;

/* The Java runtime must implement ALL callbacks. retain_runnable acquires
 * actual strong Java/VM ownership until delivery/cancellation. No raw
 * borrowed JNI jobject or weak tag may be posted. invoke_runnable executes
 * the original Runnable.run on the true owner thread. Returning 0 means
 * unavailable, -1 a witnessed Java exception, 1 complete execution. */
typedef struct BsDexHorizonMainLooperJava {
    void *user;
    int (*verify_handler)(void *user, BsDexVM *vm, BsDexValue handler);
    int (*retain_runnable)(void *user, BsDexVM *vm, BsDexValue runnable,
                           void **strong_hold);
    void (*release_runnable)(void *user, BsDexVM *vm, void *strong_hold);
    int (*invoke_runnable)(void *user, BsDexVM *vm, BsDexValue runnable,
                           const char **exception);
} BsDexHorizonMainLooperJava;

typedef struct BsDexHorizonQueuedRunnable {
    BsDexValue handler, runnable;
    void *strong_hold;
    uint64_t due_ns;
    uint64_t order;
} BsDexHorizonQueuedRunnable;

typedef struct BsDexHorizonMainLooper {
    BsDexHorizonMainLooperNative native;
    BsDexHorizonMainLooperJava java;
    BsDexVM *vm;                   /* borrowed; destroy after native shutdown */
    void *native_looper_handle;    /* independently acquired ALooper ref */
    uint64_t owner_thread_id;
    BsDexValue verified_handler;
    BsDexHorizonQueuedRunnable pending[BS_HORIZON_MAIN_LOOPER_MAX_POSTS];
    BsDexHorizonQueuedRunnable failed_task;
    size_t pending_count;
    uint64_t next_order;
    int observed_native_looper;
    int attached_java_handler;
    int failed_dispatch;
    /* Host retain/release callbacks may re-enter native APIs. Track their
     * enclosing operation so callbacks cannot destroy the queue mid-call.
     * Runnable.run itself may still post/cancel tasks on the owner thread. */
    int mutation_active;
    int dispatch_active;
    int shutdown_active;
    int failed_invoke_result;       /* callback return: 0 unavailable, -1 throw */
    char failed_exception[128];    /* bounded copy, never borrowed Java text */
} BsDexHorizonMainLooper;

/* Observe REAL NDK ALooper owned by the applet main thread; acquires an
 * independent native looper reference. Successful native observation grants
 * no Java Looper/Handler/Thread readiness. */
int bs_dex_horizon_main_looper_observe_main_loop(
    BsDexHorizonMainLooper *loop,
    const BsDexHorizonMainLooperNative *native);
int bs_dex_horizon_main_looper_owner_thread_id(
    const BsDexHorizonMainLooper *loop, uint64_t *thread_id);
int bs_dex_horizon_main_looper_native_ready(
    const BsDexHorizonMainLooper *loop);

/* Attach only after a separate VM verifies that an actual original Handler
 * constructor and owning Java main Looper exist. The callback owns the
 * proof; native ALooper presence cannot satisfy it. */
int bs_dex_horizon_main_looper_bind_java_handler(
    BsDexHorizonMainLooper *loop, BsDexVM *vm, BsDexValue handler,
    const BsDexHorizonMainLooperJava *java);
int bs_dex_horizon_main_looper_java_ready(
    const BsDexHorizonMainLooper *loop);

/* Functional owner-thread subset of Handler.post/postDelayed/removeCallbacks
 * after Java binding. No asynchronous VM mutation or off-thread calls are
 * accepted. Delay is Android milliseconds from observed monotonic time.
 * Posting consumes a strong VM reference, released exactly once. */
int bs_dex_horizon_main_looper_post(
    BsDexHorizonMainLooper *loop, BsDexValue handler,
    BsDexValue runnable, uint64_t delay_ms);
int bs_dex_horizon_main_looper_remove_callbacks(
    BsDexHorizonMainLooper *loop, BsDexValue handler,
    BsDexValue runnable, size_t *removed);
int bs_dex_horizon_main_looper_has_callbacks(
    const BsDexHorizonMainLooper *loop, BsDexValue handler,
    BsDexValue runnable);

/* Called from the existing owner-thread applet/ALooper pump, after the
 * native poll. Executes only due tasks in stable (due, insertion) order.
 * A missing/exceptional original Runnable.run fails closed, seals dispatch,
 * retains failed task until shutdown and never claims completion. */
int bs_dex_horizon_main_looper_drain(
    BsDexHorizonMainLooper *loop, size_t budget, size_t *executed);
/* Bounded recommended timeout for native bs_looper_poll_once; -1 if idle. */
int bs_dex_horizon_main_looper_next_timeout_ms(
    const BsDexHorizonMainLooper *loop, int *timeout_ms);
/* Shutdown on owner thread after stopping the applet pump and before the VM.
 * Cancels pending tasks, releases strong references and native ALooper ref. */
int bs_dex_horizon_main_looper_shutdown(BsDexHorizonMainLooper *loop);
/* Evidence of a failed actual guest callback; available only after the
 * dispatcher halts. The exception text is owned by the queue and remains
 * valid until shutdown. Neither function invokes guest Java code. */
int bs_dex_horizon_main_looper_failure_diagnostic(
    const BsDexHorizonMainLooper *loop, BsDexValue *runnable,
    int *invoke_result, const char **exception);

typedef struct BsDexHorizonMainLooperProduction {
    BsDexHorizonJavaRuntime *activity;
    BsDexHorizonServices *horizon;
} BsDexHorizonMainLooperProduction;

/* Production callbacks use libnx's actual applet main thread,
 * bs_looper_for_thread/acquire/release/wake and armGetSystemTick. */
BsDexHorizonMainLooperNative bs_dex_horizon_main_looper_production_callbacks(
    BsDexHorizonMainLooperProduction *production);

#endif
