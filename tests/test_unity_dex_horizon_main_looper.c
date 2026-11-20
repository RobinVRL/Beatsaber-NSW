/* Native ALooper-backed queue semantics; Java Runnable hooks are fixture
 * witnesses, not proof of an actual executable guest Runnable.run VM API. */
#include "unity_dex_horizon_main_looper.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fixture {
    int owner_alive, native_acquire_ok, java_handler_alive;
    int wake_fails, retain_fails, invoke_fails;
    uint64_t owner_id, monotonic_now;
    int native_looper;
    int native_refs, native_acquires, native_releases, wake_calls;
    int held, retained, released;
    uintptr_t ran[100];
    size_t ran_count;
    BsDexHorizonMainLooper *active_loop;
    int reenter_release, reenter_run, nested_blocked;
} Fixture;

typedef struct Hold {
    uintptr_t runnable;
} Hold;

static int native_owner(void *user, uint64_t *thread_id) {
    Fixture *f = user;
    if (thread_id) *thread_id = 0;
    if (!thread_id || !f->owner_alive) return 0;
    *thread_id = f->owner_id;
    return 1;
}
static int acquire_looper(void *user, void **handle) {
    Fixture *f = user;
    if (handle) *handle = NULL;
    if (!handle || !f->owner_alive || !f->native_acquire_ok) return 0;
    ++f->native_acquires;
    ++f->native_refs;
    *handle = &f->native_looper;
    return 1;
}
static void release_looper(void *user, void *handle) {
    Fixture *f = user;
    assert(handle == &f->native_looper && f->native_refs > 0);
    --f->native_refs;
    ++f->native_releases;
}
static int native_wake(void *user, void *handle) {
    Fixture *f = user;
    if (handle != &f->native_looper || !f->owner_alive ||
        f->wake_fails) return 0;
    ++f->wake_calls;
    return 1;
}
static int now_ns(void *user, uint64_t *value) {
    Fixture *f = user;
    if (!value || !f->owner_alive) return 0;
    *value = f->monotonic_now;
    return 1;
}
static int verified_handler(void *user, BsDexVM *vm, BsDexValue handler) {
    Fixture *f = user;
    return vm == (BsDexVM *)f && f->java_handler_alive &&
        handler.kind == BS_DEX_REF && handler.bits == 0x100u;
}
static int retain_runnable(void *user, BsDexVM *vm, BsDexValue runnable,
                           void **hold) {
    Fixture *f = user;
    assert(vm == (BsDexVM *)f && hold && !*hold);
    if (f->retain_fails || runnable.kind != BS_DEX_REF || !runnable.bits)
        return 0;
    Hold *h = malloc(sizeof *h);
    if (!h) return 0;
    h->runnable = runnable.bits;
    *hold = h;
    ++f->retained;
    ++f->held;
    return 1;
}
static void release_runnable(void *user, BsDexVM *vm, void *hold) {
    Fixture *f = user;
    Hold *h = hold;
    assert(vm == (BsDexVM *)f && h && f->held > 0 && h->runnable);
    if (f->reenter_release) {
        /* A Java ref destructor is foreign host code; it must not be able
         * to destroy the queue in the middle of a cancel/release. */
        assert(!bs_dex_horizon_main_looper_shutdown(f->active_loop));
        ++f->nested_blocked;
    }
    ++f->released;
    --f->held;
    free(h);
}
static int invoke_runnable(void *user, BsDexVM *vm, BsDexValue runnable,
                           const char **exception) {
    Fixture *f = user;
    assert(vm == (BsDexVM *)f && runnable.bits && exception);
    *exception = NULL;
    if (f->invoke_fails) {
        if (f->invoke_fails < 0) {
            *exception = "Ljava/lang/IllegalStateException;";
            return -1;
        }
        return 0;
    }
    if (f->reenter_run) {
        size_t nested_executed = 123;
        assert(!bs_dex_horizon_main_looper_drain(
            f->active_loop, 1, &nested_executed));
        assert(!nested_executed);
        assert(!bs_dex_horizon_main_looper_shutdown(f->active_loop));
        /* Android Handler permits an original Runnable.run to post another
         * callback on the SAME thread while it is dispatching. */
        BsDexValue another = {.kind = BS_DEX_REF, .bits = 0x77u};
        BsDexValue handler = {.kind = BS_DEX_REF, .bits = 0x100u};
        assert(bs_dex_horizon_main_looper_post(
            f->active_loop, handler, another, 0));
        ++f->nested_blocked;
        f->reenter_run = 0;
    }
    assert(f->ran_count < sizeof f->ran / sizeof f->ran[0]);
    f->ran[f->ran_count++] = runnable.bits;
    return 1;
}

static BsDexHorizonMainLooperNative native(Fixture *f) {
    return (BsDexHorizonMainLooperNative){
        .user = f, .observe_owner_thread = native_owner,
        .acquire_native_looper = acquire_looper,
        .release_native_looper = release_looper, .wake = native_wake,
        .monotonic_time_ns = now_ns
    };
}
static BsDexHorizonMainLooperJava java(Fixture *f) {
    return (BsDexHorizonMainLooperJava){
        .user = f, .verify_handler = verified_handler,
        .retain_runnable = retain_runnable,
        .release_runnable = release_runnable,
        .invoke_runnable = invoke_runnable
    };
}
static BsDexValue ref(uintptr_t ptr) {
    return (BsDexValue){.bits = ptr, .kind = BS_DEX_REF};
}

int main(void) {
    Fixture f = {
        .owner_alive = 1, .native_acquire_ok = 1,
        .java_handler_alive = 1,
        .owner_id = UINT64_C(0xabc), .monotonic_now = 1000000000ull
    };
    BsDexHorizonMainLooper loop = {0};
    f.active_loop = &loop;
    BsDexHorizonMainLooperNative ndk = native(&f);
    BsDexHorizonMainLooperNative partial = ndk;
    partial.monotonic_time_ns = NULL;
    assert(!bs_dex_horizon_main_looper_observe_main_loop(&loop, &partial));
    assert(!loop.observed_native_looper && !f.native_refs);
    assert(bs_dex_horizon_main_looper_observe_main_loop(&loop, &ndk));
    assert(f.native_refs == 1 && f.native_acquires == 1);
    /* Lazy repeated observation must not acquire a second ALooper ref. */
    assert(bs_dex_horizon_main_looper_observe_main_loop(&loop, &ndk));
    assert(f.native_acquires == 1);
    uint64_t main_tid = 0;
    assert(bs_dex_horizon_main_looper_owner_thread_id(&loop, &main_tid));
    assert(main_tid == f.owner_id);
    assert(bs_dex_horizon_main_looper_native_ready(&loop));
    assert(!bs_dex_horizon_main_looper_java_ready(&loop));
    assert(!bs_dex_horizon_main_looper_post(&loop, ref(0x100),
                                             ref(1), 0));
    BsDexHorizonMainLooperJava jc = java(&f);
    BsDexHorizonMainLooperJava incomplete = jc;
    incomplete.invoke_runnable = NULL;
    assert(!bs_dex_horizon_main_looper_bind_java_handler(
        &loop, (BsDexVM *)&f, ref(0x100), &incomplete));
    assert(bs_dex_horizon_main_looper_bind_java_handler(
        &loop, (BsDexVM *)&f, ref(0x100), &jc));
    assert(!bs_dex_horizon_main_looper_bind_java_handler(
        &loop, (BsDexVM *)&f, ref(0x100), &jc));
    assert(bs_dex_horizon_main_looper_java_ready(&loop));

    /* Real monotonic due times, FIFO order at equal deadline. */
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(1), 25));
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(2), 10));
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(3), 10));
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(4), 0));
    assert(loop.pending_count == 4 && f.held == 4);
    int timeout = -999;
    assert(bs_dex_horizon_main_looper_next_timeout_ms(&loop, &timeout));
    assert(timeout == 0);
    size_t executed = 100;
    assert(bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 1 && f.ran_count == 1 && f.ran[0] == 4);
    assert(f.held == 3 && f.released == 1);
    /* A reentrant Runnable can enqueue additional original work but cannot
     * recursively drain or destroy the current dispatch stack. */
    f.reenter_run = 1;
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(0x76), 0));
    assert(bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 2 && f.ran[1] == 0x76 && f.ran[2] == 0x77);
    assert(f.nested_blocked == 1 && f.held == 3);
    /* Reset the callback trace for the deadline ordering checks below. */
    f.ran_count = 1;
    assert(bs_dex_horizon_main_looper_next_timeout_ms(&loop, &timeout));
    assert(timeout == 10);
    f.monotonic_now += 9000000ull;
    assert(bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 0);
    assert(bs_dex_horizon_main_looper_next_timeout_ms(&loop, &timeout));
    assert(timeout == 1);
    f.monotonic_now += 1000000ull;
    assert(bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 2 && f.ran_count == 3);
    assert(f.ran[1] == 2 && f.ran[2] == 3);
    f.monotonic_now += 15000000ull;
    assert(bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 1 && f.ran[3] == 1);
    assert(!f.held && f.retained == f.released);
    assert(bs_dex_horizon_main_looper_next_timeout_ms(&loop, &timeout));
    assert(timeout == -1);

    /* removeCallbacks removes ALL still-pending matching Java identity
     * references, and releases each retained strong owner exactly once. */
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(5), 30));
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(5), 30));
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(6), 30));
    assert(bs_dex_horizon_main_looper_has_callbacks(
        &loop, ref(0x100), ref(5)));
    size_t removed = 999;
    f.reenter_release = 1;
    assert(bs_dex_horizon_main_looper_remove_callbacks(
        &loop, ref(0x100), ref(5), &removed));
    f.reenter_release = 0;
    assert(f.nested_blocked == 3);
    assert(removed == 2 && f.held == 1 && loop.pending_count == 1);
    assert(!bs_dex_horizon_main_looper_has_callbacks(
        &loop, ref(0x100), ref(5)));
    assert(bs_dex_horizon_main_looper_has_callbacks(
        &loop, ref(0x100), ref(6)));
    assert(bs_dex_horizon_main_looper_remove_callbacks(
        &loop, ref(0x100), ref(5), &removed));
    assert(removed == 0);
    assert(!bs_dex_horizon_main_looper_remove_callbacks(
        &loop, ref(0x101), ref(6), &removed));
    assert(removed == 0);
    f.monotonic_now += 30000000ull;
    assert(bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 1 && f.ran[4] == 6 && f.held == 0);

    /* Absent original Java witness/foreign thread refuses all mutation. */
    f.owner_alive = 0;
    assert(!bs_dex_horizon_main_looper_native_ready(&loop));
    assert(!bs_dex_horizon_main_looper_owner_thread_id(&loop, &main_tid));
    assert(!main_tid);
    assert(!bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(7), 0));
    assert(!bs_dex_horizon_main_looper_shutdown(&loop));
    f.owner_alive = 1;
    f.java_handler_alive = 0;
    assert(!bs_dex_horizon_main_looper_java_ready(&loop));
    assert(!bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(7), 0));
    f.java_handler_alive = 1;

    /* Failed wake and failed strong retain MUST roll back. */
    f.wake_fails = 1;
    int retained = f.retained, released = f.released;
    assert(!bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(7), 0));
    assert(loop.pending_count == 0);
    assert(f.retained == retained + 1 && f.released == released + 1);
    f.wake_fails = 0;
    f.retain_fails = 1;
    assert(!bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(7), 0));
    assert(loop.pending_count == 0);
    f.retain_fails = 0;

    /* Bounded queue refuses overflow without retaining extra tasks. */
    for (size_t i = 0; i < BS_HORIZON_MAIN_LOOPER_MAX_POSTS; ++i)
        assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100),
                                               ref(8), 5000));
    assert(!bs_dex_horizon_main_looper_post(
        &loop, ref(0x100), ref(8), 5000));
    assert(f.held == BS_HORIZON_MAIN_LOOPER_MAX_POSTS);
    assert(bs_dex_horizon_main_looper_remove_callbacks(
        &loop, ref(0x100), ref(8), &removed));
    assert(removed == BS_HORIZON_MAIN_LOOPER_MAX_POSTS && !f.held);

    /* An unavailable original Runnable.run MUST seal dispatch and retain
     * the in-flight strong owner for teardown, never claim execution. */
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(9), 0));
    assert(bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(10), 0));
    f.invoke_fails = -1;
    assert(!bs_dex_horizon_main_looper_drain(&loop, 8, &executed));
    assert(executed == 0 && f.held == 2 && loop.failed_dispatch);
    assert(loop.failed_task.runnable.bits == 9);
    BsDexValue failed = {0};
    int failure_code = 0;
    const char *failure_exception = NULL;
    assert(bs_dex_horizon_main_looper_failure_diagnostic(
        &loop, &failed, &failure_code, &failure_exception));
    assert(failed.kind == BS_DEX_REF && failed.bits == 9);
    assert(failure_code == -1 && failure_exception &&
           !strcmp(failure_exception, "Ljava/lang/IllegalStateException;"));
    assert(!bs_dex_horizon_main_looper_java_ready(&loop));
    assert(!bs_dex_horizon_main_looper_post(&loop, ref(0x100), ref(11), 0));
    assert(bs_dex_horizon_main_looper_shutdown(&loop));
    assert(!f.held && f.retained == f.released);
    assert(f.native_refs == 0 && f.native_releases == 1);
    assert(!bs_dex_horizon_main_looper_native_ready(&loop));
    BsDexHorizonMainLooperProduction production = {0};
    BsDexHorizonMainLooperNative no_horizon =
        bs_dex_horizon_main_looper_production_callbacks(&production);
    assert(!no_horizon.observe_owner_thread);
    puts("PASS: native main ALooper observation, timed Handler subset ordering, removal, identity, failure and reference lifetimes");
    return 0;
}
