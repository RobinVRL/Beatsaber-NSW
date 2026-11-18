/* Exercise real pinned Quest DEX and native Activity event ownership.
 * The fixtures back native event registration, not Android Java services. */
#include "unity_dex_horizon_java_runtime.h"
#include "unity_apk_dex_loader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    int on_main_thread;
    int native_ready;
    int valid_window;
    int subscribe_fails;
    int unsubscribe_fails;
    int window;
    int token;
    int subscribed;
    int subscriptions;
    int unsubscriptions;
} Fixture;

static int is_main(void *user) {
    return ((Fixture *)user)->on_main_thread;
}
static int is_ready(void *user) {
    return ((Fixture *)user)->native_ready;
}
static void *window(void *user) {
    return &((Fixture *)user)->window;
}
static int valid_window(void *user, void *candidate) {
    Fixture *f = user;
    return f->valid_window && candidate == &f->window;
}
static int subscribe(void *user, BsDexHorizonJavaRuntime *owner, void **token) {
    Fixture *f = user;
    assert(owner && token && !*token);
    if (f->subscribed) return 0;
    f->subscribed = 1;
    ++f->subscriptions;
    *token = &f->token;
    /* Even when a backend reports partial failure it must be rolled back. */
    return !f->subscribe_fails;
}
static int unsubscribe(void *user, void *token) {
    Fixture *f = user;
    assert(token == &f->token);
    if (!f->subscribed || f->unsubscribe_fails) return 0;
    f->subscribed = 0;
    ++f->unsubscriptions;
    return 1;
}
static int load_game(void *user) {
    (void)user; /* Fixture attests already completed original libgame load. */
    return 1;
}
static BsDexHorizonJavaNativeHooks hooks(Fixture *fixture) {
    return (BsDexHorizonJavaNativeHooks){
        .user = fixture, .on_main_thread = is_main, .native_ready = is_ready,
        .current_window = window, .valid_window = valid_window,
        .subscribe = subscribe, .unsubscribe = unsubscribe
    };
}

static void test_runtime(BsUnityOriginalDexImage *dex,
                         Fixture *fixture, int subscribe_failure) {
    BsDexHorizonJavaRuntime rt = {0};
    BsDexHorizonJavaNativeHooks backend = hooks(fixture);
    assert(bs_dex_horizon_java_runtime_init_with_hooks(&rt, &backend));
    BsDexHorizonActivityServices native = bs_dex_horizon_java_runtime_callbacks(&rt);
    assert(native.user == &rt && native.on_ui_thread && native.construct_base &&
           native.attach && native.valid_nwindow && native.verify_external &&
           native.verify_attachment && native.detach);
    BsDexHorizonActivityHost activity_host = {0};
    assert(!bs_dex_horizon_java_runtime_bind_verified_host(&rt, &activity_host));
    assert(bs_dex_horizon_activity_init(&activity_host, dex->data,
                                        dex->size, &native));
    assert(bs_dex_horizon_java_runtime_bind_verified_host(&rt, &activity_host));
    assert(!bs_dex_horizon_java_runtime_bind_verified_host(&rt, &activity_host));
    BsDexAndroidContextHost context = {0};
    BsDexAndroidSurfaceHost surface = {0};
    BsDexAndroidSurfaceServices native_surface = {
        .user = &activity_host,
        .on_ui_thread = bs_dex_horizon_activity_on_ui_thread,
        .context_matches_activity = bs_dex_horizon_activity_context_matches
    };
    bs_dex_android_surface_host_init(&surface, &native_surface);
    BsDexAndroidHost combined = {
        .context = &context, .surface = &surface,
        .load_game_library = load_game
    };
    assert(bs_dex_horizon_activity_bind(&activity_host, &combined, &context));
    BsDexHost vm_callbacks = {0};
    assert(bs_dex_android_host_callbacks(&combined, &vm_callbacks));
    BsDexVM *vm = bs_dex_vm_create(dex->data, dex->size, &vm_callbacks);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop stopped = bs_dex_vm_run(vm, 20000);
    if (subscribe_failure) {
        /* The callback created a real event token before reporting failure.
         * The failed constructor must unregister it and retain no Activity. */
        assert(stopped.kind == BS_DEX_STOP_HOST_MISSING &&
               stopped.host_class &&
               !strcmp(stopped.host_class, "Landroid/app/Activity;") &&
               stopped.host_method &&
               !strcmp(stopped.host_method, "<init>"));
        assert(!fixture->subscribed && fixture->subscriptions == 1 &&
               fixture->unsubscriptions == 1);
        assert(!rt.event_token && !rt.native_base_constructed &&
               !context.activity);
        fixture->subscribe_fails = 0;
        stopped = bs_dex_vm_run(vm, 20000);
    }
    /* At least one real native registration has completed, so the original
     * Quest VM can proceed beyond android.app.Activity.<init> into its own
     * DEX instructions (including the external WeakReference constructor). */
    assert(stopped.steps > 62);
    printf("Original constructor after native Activity base: steps=%u stop=%d "
           "code=0x%x pc=0x%x next=%s.%s\n",
           stopped.steps, stopped.kind, stopped.code_off, stopped.dex_pc,
           stopped.host_class ? stopped.host_class : "(none)",
           stopped.host_method ? stopped.host_method : "(none)");
    assert(context.activity == &activity_host.activity);
    assert(activity_host.activity.constructed &&
           activity_host.activity.alive);
    assert(rt.native_activity == &activity_host.activity &&
           rt.activity_generation == activity_host.activity.generation);
    assert(bs_dex_horizon_java_runtime_native_base_ready(&rt));
    assert(fixture->subscribed);
    assert(!vm_callbacks.validate_activity(vm_callbacks.user,
                                           &activity_host.activity));
    assert(!bs_dex_vm_current_activity(vm).bits);
    assert(!bs_dex_vm_current_context(vm).bits);
    assert(rt.event_count == 0);
    assert(bs_dex_horizon_java_runtime_record_applet_event(
               &rt, BS_HJ_APPLET_FOCUS));
    assert(bs_dex_horizon_java_runtime_record_applet_event(
               &rt, BS_HJ_APPLET_OPERATION_MODE));
    assert(rt.event_count == 2 &&
           rt.last_native_event == BS_HJ_APPLET_OPERATION_MODE);
    assert(!bs_dex_horizon_java_runtime_record_applet_event(&rt, 99));

    /* A live libnx NWindow must be revalidated; never use a stale backing
     * or infer readiness from a previously allocated Activity owner. */
    fixture->valid_window = 0;
    assert(!bs_dex_horizon_java_runtime_native_base_ready(&rt));
    fixture->valid_window = 1;
    fixture->on_main_thread = 0;
    assert(!bs_dex_horizon_java_runtime_native_base_ready(&rt));
    assert(!bs_dex_horizon_activity_release(&activity_host));
    fixture->on_main_thread = 1;
    assert(bs_dex_horizon_java_runtime_native_base_ready(&rt));

    /* No actual ContextImpl / Intent / Resources / Java Looper exist.
     * Even a real native event subscription must not authorize onCreate. */
    void *attached_window = &fixture->window;
    BsDexAndroidContextAttachment observed = {.activity = &fixture->window};
    const char *exception = "stale exception marker";
    assert(native.attach(native.user, &activity_host.activity,
                         &attached_window, &observed, &exception) == 0);
    assert(!attached_window && !observed.activity && !exception);
    assert(!native.verify_attachment(native.user, &activity_host.activity,
                                     &fixture->window, &observed));
    assert(!native.verify_external(native.user, "Landroid/content/Intent;",
                                   &fixture->window));
    assert(!bs_dex_horizon_java_runtime_shutdown(&rt));

    assert(bs_dex_horizon_java_runtime_record_applet_event(
               &rt, BS_HJ_APPLET_EXIT_REQUEST));
    assert(rt.exit_requested &&
           !bs_dex_horizon_java_runtime_native_base_ready(&rt));
    bs_dex_vm_destroy(vm);
    assert(activity_host.weak_outstanding == 0);
    assert(bs_dex_horizon_activity_release(&activity_host));
    assert(!fixture->subscribed && !rt.native_base_constructed &&
           !rt.event_token && !rt.native_activity);
    assert(fixture->unsubscriptions == fixture->subscriptions);
    assert(bs_dex_horizon_java_runtime_shutdown(&rt));
    assert(!bs_dex_horizon_java_runtime_record_applet_event(
               &rt, BS_HJ_APPLET_FOCUS));
    assert(bs_dex_android_surface_host_reset(&surface));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    BsUnityOriginalDexImage dex = {0};
    assert(bs_unity_apk_load_original_dex(argv[1], &dex));
    Fixture invalid = { .on_main_thread = 1, .native_ready = 0,
                        .valid_window = 1 };
    BsDexHorizonJavaRuntime rt = {0};
    BsDexHorizonJavaNativeHooks incomplete = hooks(&invalid);
    incomplete.subscribe = NULL;
    assert(!bs_dex_horizon_java_runtime_init_with_hooks(&rt, &incomplete));
    assert(!bs_dex_horizon_java_runtime_init(&rt, NULL));
    assert(!bs_dex_horizon_java_runtime_init(&rt,
                                            (BsDexHorizonServices *)&invalid));
    Fixture good = { .on_main_thread = 1, .native_ready = 1,
                     .valid_window = 1 };
    test_runtime(&dex, &good, 0);
    Fixture fail_subscribe = { .on_main_thread = 1, .native_ready = 1,
                               .valid_window = 1, .subscribe_fails = 1 };
    test_runtime(&dex, &fail_subscribe, 1);
    bs_unity_apk_free_original_dex(&dex);
    puts("PASS: original Quest Activity constructor owns real native event subscription, rollback and Java attach fail-closed");
    return 0;
}
