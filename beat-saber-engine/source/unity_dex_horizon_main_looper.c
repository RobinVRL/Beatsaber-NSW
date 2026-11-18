#include "unity_dex_horizon_main_looper.h"
#include <limits.h>
#include <string.h>

#if defined(__SWITCH__)
#include <switch.h>
#include "android_window_bridge.h"
#endif

static int has_native_callbacks(const BsDexHorizonMainLooperNative *native) {
    return native && native->observe_owner_thread &&
        native->acquire_native_looper && native->release_native_looper &&
        native->wake && native->monotonic_time_ns;
}

static int has_java_callbacks(const BsDexHorizonMainLooperJava *java) {
    return java && java->verify_handler && java->retain_runnable &&
        java->release_runnable && java->invoke_runnable;
}

static int same_ref(BsDexValue a, BsDexValue b) {
    return a.kind == b.kind && a.bits == b.bits;
}

static void record_failure_exception(BsDexHorizonMainLooper *loop,
                                     const char *exception) {
    if (!exception) return;
    size_t size = 0;
    while (size + 1 < sizeof loop->failed_exception && exception[size])
        ++size;
    memcpy(loop->failed_exception, exception, size);
    loop->failed_exception[size] = '\0';
}

static int owner_alive(const BsDexHorizonMainLooper *loop) {
    if (!loop || !loop->observed_native_looper || !loop->native_looper_handle ||
        !has_native_callbacks(&loop->native) || !loop->owner_thread_id)
        return 0;
    uint64_t actual = 0;
    return loop->native.observe_owner_thread(loop->native.user, &actual) == 1 &&
        actual == loop->owner_thread_id;
}

int bs_dex_horizon_main_looper_observe_main_loop(
    BsDexHorizonMainLooper *loop,
    const BsDexHorizonMainLooperNative *native) {
    if (!loop || !has_native_callbacks(native)) return 0;
    if (loop->observed_native_looper)
        return owner_alive(loop) && loop->native.user == native->user &&
            loop->native.observe_owner_thread ==
                native->observe_owner_thread &&
            loop->native.acquire_native_looper ==
                native->acquire_native_looper &&
            loop->native.release_native_looper ==
                native->release_native_looper &&
            loop->native.wake == native->wake &&
            loop->native.monotonic_time_ns == native->monotonic_time_ns;
    if (loop->native_looper_handle ||
        loop->pending_count || loop->attached_java_handler) return 0;
    uint64_t owner_id = 0;
    if (native->observe_owner_thread(native->user, &owner_id) != 1 ||
        !owner_id) return 0;
    void *handle = NULL;
    int acquired = native->acquire_native_looper(native->user, &handle);
    if (acquired != 1 || !handle) {
        if (handle) native->release_native_looper(native->user, handle);
        return 0;
    }
    uint64_t after = 0;
    uint64_t now = 0;
    if (native->observe_owner_thread(native->user, &after) != 1 ||
        after != owner_id ||
        native->monotonic_time_ns(native->user, &now) != 1) {
        native->release_native_looper(native->user, handle);
        return 0;
    }
    *loop = (BsDexHorizonMainLooper){0};
    loop->native = *native;
    loop->native_looper_handle = handle;
    loop->owner_thread_id = owner_id;
    loop->observed_native_looper = 1;
    return 1;
}

int bs_dex_horizon_main_looper_native_ready(
    const BsDexHorizonMainLooper *loop) {
    return owner_alive(loop);
}

int bs_dex_horizon_main_looper_owner_thread_id(
    const BsDexHorizonMainLooper *loop, uint64_t *thread_id) {
    if (thread_id) *thread_id = 0;
    if (!thread_id || !owner_alive(loop)) return 0;
    *thread_id = loop->owner_thread_id;
    return 1;
}

int bs_dex_horizon_main_looper_bind_java_handler(
    BsDexHorizonMainLooper *loop, BsDexVM *vm, BsDexValue handler,
    const BsDexHorizonMainLooperJava *java) {
    if (!loop || !vm || handler.kind != BS_DEX_REF || !handler.bits ||
        loop->attached_java_handler || !owner_alive(loop) ||
        !has_java_callbacks(java) ||
        java->verify_handler(java->user, vm, handler) != 1) return 0;
    loop->java = *java;
    loop->vm = vm;
    loop->verified_handler = handler;
    loop->attached_java_handler = 1;
    return 1;
}

int bs_dex_horizon_main_looper_java_ready(
    const BsDexHorizonMainLooper *loop) {
    return loop && loop->attached_java_handler && !loop->failed_dispatch &&
        !loop->shutdown_active &&
        loop->vm && loop->verified_handler.kind == BS_DEX_REF &&
        loop->verified_handler.bits && owner_alive(loop) &&
        has_java_callbacks(&loop->java) &&
        loop->java.verify_handler(loop->java.user, loop->vm,
                                   loop->verified_handler) == 1;
}

int bs_dex_horizon_main_looper_post(
    BsDexHorizonMainLooper *loop, BsDexValue handler,
    BsDexValue runnable, uint64_t delay_ms) {
    if (!bs_dex_horizon_main_looper_java_ready(loop) ||
        loop->mutation_active ||
        !same_ref(handler, loop->verified_handler) ||
        runnable.kind != BS_DEX_REF || !runnable.bits ||
        loop->pending_count >= BS_HORIZON_MAIN_LOOPER_MAX_POSTS ||
        loop->next_order == UINT64_MAX) return 0;
    uint64_t now = 0;
    if (loop->native.monotonic_time_ns(loop->native.user, &now) != 1 ||
        delay_ms > (UINT64_MAX - now) / UINT64_C(1000000)) return 0;
    loop->mutation_active = 1;
    void *hold = NULL;
    if (loop->java.retain_runnable(loop->java.user, loop->vm, runnable,
                                    &hold) != 1 || !hold) {
        if (hold)
            loop->java.release_runnable(loop->java.user, loop->vm, hold);
        loop->mutation_active = 0;
        return 0;
    }
    BsDexHorizonQueuedRunnable task = {
        .handler = handler,
        .runnable = runnable,
        .strong_hold = hold,
        .due_ns = now + delay_ms * UINT64_C(1000000),
        .order = ++loop->next_order
    };
    loop->pending[loop->pending_count++] = task;
    if (loop->native.wake(loop->native.user,
                          loop->native_looper_handle) != 1) {
        --loop->pending_count;
        loop->pending[loop->pending_count] =
            (BsDexHorizonQueuedRunnable){0};
        loop->java.release_runnable(loop->java.user, loop->vm, hold);
        loop->mutation_active = 0;
        return 0;
    }
    loop->mutation_active = 0;
    return 1;
}

int bs_dex_horizon_main_looper_remove_callbacks(
    BsDexHorizonMainLooper *loop, BsDexValue handler,
    BsDexValue runnable, size_t *removed) {
    if (removed) *removed = 0;
    if (!removed || !bs_dex_horizon_main_looper_java_ready(loop) ||
        loop->mutation_active ||
        !same_ref(handler, loop->verified_handler) ||
        runnable.kind != BS_DEX_REF || !runnable.bits) return 0;
    loop->mutation_active = 1;
    BsDexHorizonQueuedRunnable canceled[BS_HORIZON_MAIN_LOOPER_MAX_POSTS];
    size_t dst = 0, count = 0;
    for (size_t i = 0; i < loop->pending_count; ++i) {
        BsDexHorizonQueuedRunnable entry = loop->pending[i];
        if (same_ref(entry.handler, handler) &&
            same_ref(entry.runnable, runnable)) {
            canceled[count++] = entry;
        } else {
            loop->pending[dst++] = entry;
        }
    }
    for (size_t i = dst; i < loop->pending_count; ++i)
        loop->pending[i] = (BsDexHorizonQueuedRunnable){0};
    loop->pending_count = dst;
    /* Publish the compacted queue before invoking release callbacks, which
     * can re-enter host code. The canceled holds are never still queued. */
    for (size_t i = 0; i < count; ++i)
        loop->java.release_runnable(loop->java.user, loop->vm,
                                     canceled[i].strong_hold);
    *removed = count;
    if (count)
        (void)loop->native.wake(loop->native.user, loop->native_looper_handle);
    loop->mutation_active = 0;
    return 1;
}

int bs_dex_horizon_main_looper_has_callbacks(
    const BsDexHorizonMainLooper *loop, BsDexValue handler,
    BsDexValue runnable) {
    if (!bs_dex_horizon_main_looper_java_ready(loop) ||
        !same_ref(handler, loop->verified_handler) ||
        runnable.kind != BS_DEX_REF || !runnable.bits) return 0;
    for (size_t i = 0; i < loop->pending_count; ++i)
        if (same_ref(loop->pending[i].handler, handler) &&
            same_ref(loop->pending[i].runnable, runnable))
            return 1;
    return 0;
}

static size_t earliest_pending(const BsDexHorizonMainLooper *loop) {
    size_t best = 0;
    for (size_t i = 1; i < loop->pending_count; ++i) {
        const BsDexHorizonQueuedRunnable *a = &loop->pending[i];
        const BsDexHorizonQueuedRunnable *b = &loop->pending[best];
        if (a->due_ns < b->due_ns ||
            (a->due_ns == b->due_ns && a->order < b->order))
            best = i;
    }
    return best;
}

int bs_dex_horizon_main_looper_drain(
    BsDexHorizonMainLooper *loop, size_t budget, size_t *executed) {
    if (executed) *executed = 0;
    if (!executed || !bs_dex_horizon_main_looper_java_ready(loop) ||
        !budget || loop->mutation_active || loop->dispatch_active) return 0;
    loop->dispatch_active = 1;
    while (loop->pending_count && *executed < budget) {
        uint64_t now = 0;
        if (loop->native.monotonic_time_ns(loop->native.user, &now) != 1) {
            loop->dispatch_active = 0;
            return 0;
        }
        size_t best = earliest_pending(loop);
        if (loop->pending[best].due_ns > now) break;
        BsDexHorizonQueuedRunnable task = loop->pending[best];
        for (size_t i = best + 1; i < loop->pending_count; ++i)
            loop->pending[i - 1] = loop->pending[i];
        loop->pending[--loop->pending_count] =
            (BsDexHorizonQueuedRunnable){0};
        const char *exception = NULL;
        int executed_original = loop->java.invoke_runnable(
            loop->java.user, loop->vm, task.runnable, &exception);
        if (executed_original != 1 || exception) {
            /* Do not retry an original Runnable.run which may have partly
             * executed. Retain its Java strong ref until explicit teardown. */
            loop->failed_task = task;
            loop->failed_dispatch = 1;
            loop->failed_invoke_result = executed_original;
            record_failure_exception(loop, exception);
            loop->dispatch_active = 0;
            return 0;
        }
        loop->java.release_runnable(loop->java.user, loop->vm,
                                     task.strong_hold);
        ++*executed;
    }
    loop->dispatch_active = 0;
    return 1;
}

int bs_dex_horizon_main_looper_next_timeout_ms(
    const BsDexHorizonMainLooper *loop, int *timeout_ms) {
    if (timeout_ms) *timeout_ms = -1;
    if (!timeout_ms || !bs_dex_horizon_main_looper_java_ready(loop)) return 0;
    if (!loop->pending_count) return 1;
    uint64_t now = 0;
    if (loop->native.monotonic_time_ns(loop->native.user, &now) != 1)
        return 0;
    uint64_t due = loop->pending[earliest_pending(loop)].due_ns;
    if (due <= now) {
        *timeout_ms = 0;
        return 1;
    }
    uint64_t remaining = due - now;
    uint64_t rounded = remaining / UINT64_C(1000000) +
        (remaining % UINT64_C(1000000) != 0);
    *timeout_ms = rounded > INT_MAX ? INT_MAX : (int)rounded;
    return 1;
}

int bs_dex_horizon_main_looper_shutdown(BsDexHorizonMainLooper *loop) {
    if (!owner_alive(loop) || loop->dispatch_active ||
        loop->mutation_active || loop->shutdown_active) return 0;
    loop->shutdown_active = 1;
    if (loop->attached_java_handler && has_java_callbacks(&loop->java)) {
        for (size_t i = 0; i < loop->pending_count; ++i)
            loop->java.release_runnable(loop->java.user, loop->vm,
                                         loop->pending[i].strong_hold);
        if (loop->failed_task.strong_hold)
            loop->java.release_runnable(loop->java.user, loop->vm,
                                         loop->failed_task.strong_hold);
    }
    loop->native.release_native_looper(loop->native.user,
                                       loop->native_looper_handle);
    *loop = (BsDexHorizonMainLooper){0};
    return 1;
}

int bs_dex_horizon_main_looper_failure_diagnostic(
    const BsDexHorizonMainLooper *loop, BsDexValue *runnable,
    int *invoke_result, const char **exception) {
    if (runnable) *runnable = (BsDexValue){0};
    if (invoke_result) *invoke_result = 0;
    if (exception) *exception = NULL;
    if (!loop || !loop->failed_dispatch ||
        !loop->failed_task.strong_hold || !runnable ||
        !invoke_result || !exception) return 0;
    *runnable = loop->failed_task.runnable;
    *invoke_result = loop->failed_invoke_result;
    *exception = loop->failed_exception[0] ? loop->failed_exception : NULL;
    return 1;
}

#if defined(__SWITCH__)
static int prod_observe_owner(void *user, uint64_t *thread_id) {
    BsDexHorizonMainLooperProduction *p = user;
    if (thread_id) *thread_id = 0;
    if (!p || !thread_id || !p->activity || !p->horizon ||
        !bs_dex_horizon_java_runtime_native_base_ready(p->activity) ||
        !bs_dex_horizon_services_on_applet_thread(p->horizon)) return 0;
    u64 current = 0, main = 0;
    Handle handle = envGetMainThreadHandle();
    if (!handle ||
        R_FAILED(svcGetThreadId(&current, CUR_THREAD_HANDLE)) ||
        R_FAILED(svcGetThreadId(&main, handle)) ||
        current != main || current != p->horizon->observed.applet_thread_id)
        return 0;
    *thread_id = current;
    return 1;
}

static int prod_acquire(void *user, void **handle) {
    if (handle) *handle = NULL;
    uint64_t thread_id = 0;
    if (!handle || !prod_observe_owner(user, &thread_id)) return 0;
    void *looper = bs_looper_for_thread();
    if (!looper) return 0;
    bs_looper_acquire(looper);
    *handle = looper;
    return 1;
}

static void prod_release(void *user, void *handle) {
    (void)user;
    if (handle) bs_looper_release(handle);
}

static int prod_wake(void *user, void *handle) {
    uint64_t thread_id = 0;
    if (!handle || !prod_observe_owner(user, &thread_id) ||
        bs_looper_for_thread() != handle) return 0;
    bs_looper_wake(handle);
    return 1;
}

static int prod_now(void *user, uint64_t *ns) {
    uint64_t thread_id = 0;
    if (ns) *ns = 0;
    if (!ns || !prod_observe_owner(user, &thread_id)) return 0;
    *ns = armTicksToNs(armGetSystemTick());
    return 1;
}
#endif

BsDexHorizonMainLooperNative bs_dex_horizon_main_looper_production_callbacks(
    BsDexHorizonMainLooperProduction *production) {
    BsDexHorizonMainLooperNative callbacks = {0};
#if defined(__SWITCH__)
    if (!production || !production->activity || !production->horizon)
        return callbacks;
    callbacks.user = production;
    callbacks.observe_owner_thread = prod_observe_owner;
    callbacks.acquire_native_looper = prod_acquire;
    callbacks.release_native_looper = prod_release;
    callbacks.wake = prod_wake;
    callbacks.monotonic_time_ns = prod_now;
#else
    (void)production;
#endif
    return callbacks;
}
