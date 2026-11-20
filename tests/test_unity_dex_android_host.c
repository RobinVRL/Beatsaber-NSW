/* Aggregate Android host delegates exactly to independently backed platform
 * services. Test pointers are fixtures, NOT native GameActivity readiness. */
#include "unity_apk_dex_loader.h"
#include "unity_dex_android_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    int activity, intent;
    int ui_thread;
    int verify_calls;
    int assignable_calls;
    int constructed, discarded, attached, reject_class;
    struct {
        BsDexValue referent; /* Interpreter ref, NEVER a native strong owner. */
        int valid;
        int released;
    } weak;
    int weak_creates, weak_gets, weak_clears, weak_releases;
} Fixture;

static int verify(void *user, const char *descriptor, void *object) {
    Fixture *fixture = user;
    ++fixture->verify_calls;
    if (!strcmp(descriptor,
                "Lcom/unity3d/player/UnityPlayerGameActivity;"))
        return object == &fixture->activity;
    if (!strcmp(descriptor, "Landroid/content/Intent;"))
        return object == &fixture->intent;
    return 0;
}
static int match_context(void *user, void *context) {
    Fixture *fixture = user;
    return fixture->attached && context == &fixture->activity;
}
static int on_ui_thread(void *user) {
    return ((Fixture *)user)->ui_thread;
}
static int observed_instance_of(void *user, void *object,
                                const char *descriptor, int *result,
                                const char **exception) {
    Fixture *fixture = user;
    ++fixture->assignable_calls;
    if (exception) *exception = NULL;
    if (!result || object != &fixture->activity) return 0;
    *result = !strcmp(descriptor, "Landroid/app/Activity;") ||
        (!fixture->reject_class &&
         !strcmp(descriptor, "Lcom/unity3d/player/UnityPlayerGameActivity;"));
    return 1;
}

static int load_game_library(void *user) {
    (void)user;
    return 1;
}

static int construct_activity(void *user, const char *subclass,
                              void **activity, const char **exception) {
    Fixture *fixture = user;
    assert(!strcmp(subclass, "Lcom/unity3d/player/UnityPlayerGameActivity;"));
    assert(!*activity && !*exception);
    ++fixture->constructed;
    *activity = &fixture->activity;
    return 1;
}

static void discard_activity(void *user, void *activity) {
    Fixture *fixture = user;
    assert(activity == &fixture->activity);
    ++fixture->discarded;
}

/* Simulated independent native weak-liveness source. Production must use
 * actual Java weak handles; this test can explicitly revoke the referent
 * without changing any VM-owned reference or retaining the Activity. */
static int weak_create(void *user, BsDexVM *vm, BsDexValue referent,
                       void **token, const char **exception) {
    Fixture *f = user;
    assert(!*token && !*exception);
    if (bs_dex_host_pointer(vm, referent) != &f->activity ||
        (f->weak.valid && !f->weak.released)) return 0;
    f->weak.referent = referent;
    f->weak.valid = 1;
    f->weak.released = 0;
    ++f->weak_creates;
    *token = &f->weak;
    return 1;
}

static int weak_get(void *user, BsDexVM *vm, void *token,
                    BsDexValue *result, const char **exception) {
    Fixture *f = user;
    (void)vm;
    assert(!*exception);
    if (token != &f->weak || f->weak.released) return 0;
    ++f->weak_gets;
    *result = f->weak.valid ? f->weak.referent : bs_dex_null();
    return 1;
}

static int weak_clear(void *user, BsDexVM *vm, void *token,
                      const char **exception) {
    Fixture *f = user;
    (void)vm;
    assert(!*exception);
    if (token != &f->weak || f->weak.released) return 0;
    f->weak.valid = 0;
    ++f->weak_clears;
    return 1;
}

static void weak_release(void *user, BsDexVM *vm, void *token) {
    Fixture *f = user;
    (void)vm;
    assert(token == &f->weak && !f->weak.released);
    f->weak.released = 1;
    f->weak.valid = 0;
    ++f->weak_releases;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    BsUnityOriginalDexImage dex = {0};
    assert(bs_unity_apk_load_original_dex(argv[1], &dex));

    Fixture fixture = {.ui_thread = 1, .attached = 1};
    BsDexAndroidContextHost context = {
        .user = &fixture, .activity = &fixture.activity,
        .intent = &fixture.intent, .verify = verify
    };
    BsDexAndroidSurfaceServices surface_services = {
        .user = &fixture, .context_matches_activity = match_context,
        .on_ui_thread = on_ui_thread
    };
    BsDexAndroidSurfaceHost surface = {0};
    bs_dex_android_surface_host_init(&surface, &surface_services);
    BsDexAndroidHost combined = {
        .context = &context, .surface = &surface,
        .user = &fixture, .observed_instance_of = observed_instance_of
    };
    BsDexHost dispatch = {0};
    assert(bs_dex_android_host_callbacks(&combined, &dispatch));
    assert(!dispatch.validate_activity(dispatch.user, NULL));
    assert(!dispatch.validate_activity(dispatch.user, &fixture.intent));
    fixture.ui_thread = 0;
    assert(!dispatch.validate_activity(dispatch.user, &fixture.activity));
    fixture.ui_thread = 1;
    assert(dispatch.validate_activity(dispatch.user, &fixture.activity));

    BsDexVM *vm = bs_dex_vm_create(dex.data, dex.size, &dispatch);
    assert(vm);
    assert(bs_dex_vm_start_activity_oncreate(
        vm, &fixture.activity, bs_dex_null()) == 1);
    BsDexStop stopped = bs_dex_vm_run(vm, 10000);
    assert(stopped.kind == BS_DEX_STOP_HOST_MISSING);
    assert(stopped.code_off == 0x22e9d8 && stopped.dex_pc == 2);
    assert(stopped.steps >= 9);
    assert(bs_dex_vm_current_activity(vm).bits == 0);
    assert(bs_dex_vm_current_context(vm).bits == 0);
    assert(surface.observed_surface_constructors == 0);
    assert(surface.attached_vm == vm);
    /* Reusing a bound view adapter for a second VM would alias Java refs. */
    assert(!bs_dex_android_host_callbacks(&combined, &dispatch));

    BsDexValue activity = bs_dex_wrap_platform(
        vm, &fixture.activity,
        "Lcom/unity3d/player/UnityPlayerGameActivity;");
    assert(activity.bits);
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    assert(dispatch.invoke_platform(
        dispatch.user, vm, "Landroid/app/Activity;", "getIntent",
        "()Landroid/content/Intent;", 0x6e, &activity, 1,
        &result, &exception) == 1);
    assert(bs_dex_host_pointer(vm, result) == &fixture.intent);
    result = bs_dex_null();
    assert(dispatch.invoke_platform(
        dispatch.user, vm, "Landroid/app/Activity;", "setContentView",
        "(I)V", 0x6e, &activity, 1, &result, &exception) == 0);
    assert(!result.bits && !exception);
    int assignable = 0;
    assert(dispatch.instance_of(
        dispatch.user, vm, activity, "Landroid/app/Activity;",
        &assignable, &exception) == 1 && assignable);
    assert(fixture.assignable_calls == 1 && !exception);
    assert(dispatch.instance_of(
        dispatch.user, vm, result, "Landroid/app/Activity;",
        &assignable, &exception) == 0 && !assignable);
    assert(fixture.verify_calls > 0);

    /* Partial backend configuration is forbidden. A complete backend can
     * observe reclamation without any native strong Activity ownership. */
    assert(!dispatch.weak_reference_create && !dispatch.weak_reference_get);
    assert(!dispatch.weak_reference_clear && !dispatch.weak_reference_release);

    bs_dex_vm_destroy(vm);
    assert(bs_dex_android_surface_host_reset(&surface) == 1);
    combined.weak_reference_create = weak_create;
    assert(!bs_dex_android_host_callbacks(&combined, &dispatch));
    combined.weak_reference_get = weak_get;
    combined.weak_reference_clear = weak_clear;
    combined.weak_reference_release = weak_release;
    assert(bs_dex_android_host_callbacks(&combined, &dispatch));
    vm = bs_dex_vm_create(dex.data, dex.size, &dispatch);
    assert(vm);
    activity = bs_dex_wrap_platform(
        vm, &fixture.activity,
        "Lcom/unity3d/player/UnityPlayerGameActivity;");
    assert(activity.bits);
    void *token = NULL;
    assert(dispatch.weak_reference_create(dispatch.user, vm, activity,
                                          &token, &exception) == 1);
    assert(token == &fixture.weak && fixture.weak_creates == 1);
    BsDexValue live = bs_dex_null();
    assert(dispatch.weak_reference_get(dispatch.user, vm, token,
                                       &live, &exception) == 1);
    assert(live.bits == activity.bits && fixture.weak_gets == 1);
    /* A native weak handle must return null immediately on invalidation,
     * regardless of the interpreter still knowing the reference identity. */
    fixture.weak.valid = 0;
    live = activity;
    assert(dispatch.weak_reference_get(dispatch.user, vm, token,
                                       &live, &exception) == 1);
    assert(!live.bits && live.kind == BS_DEX_REF);
    assert(fixture.weak_gets == 2);
    assert(dispatch.weak_reference_clear(dispatch.user, vm, token,
                                         &exception) == 1);
    assert(fixture.weak_clears == 1);
    dispatch.weak_reference_release(dispatch.user, vm, token);
    assert(fixture.weak_releases == 1 && fixture.weak.released);
    assert(dispatch.weak_reference_get(dispatch.user, vm, token,
                                       &live, &exception) == 0);
    bs_dex_vm_destroy(vm);
    assert(bs_dex_android_surface_host_reset(&surface) == 1);

    context.activity = NULL;
    fixture.attached = 0;
    fixture.reject_class = 1;
    combined.load_game_library = load_game_library;
    combined.construct_activity_base = construct_activity;
    combined.discard_activity = discard_activity;
    /* This branch retains the precise missing native-weak-service stop so
     * later Quest VM changes cannot silently substitute a strong reference. */
    combined.weak_reference_create = NULL;
    combined.weak_reference_get = NULL;
    combined.weak_reference_clear = NULL;
    combined.weak_reference_release = NULL;
    assert(bs_dex_android_host_callbacks(&combined, &dispatch));
    vm = bs_dex_vm_create(dex.data, dex.size, &dispatch);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    stopped = bs_dex_vm_run(vm, 10000);
    assert(stopped.kind == BS_DEX_STOP_HOST_MISSING && stopped.steps == 9);
    assert(fixture.constructed == 1 && fixture.discarded == 1);
    assert(!context.activity);
    fixture.reject_class = 0;
    stopped = bs_dex_vm_run(vm, 10000);
    assert(stopped.kind == BS_DEX_STOP_HOST_MISSING);
    assert(stopped.steps > 9);
    assert(fixture.constructed == 2 && fixture.discarded == 1);
    assert(context.activity == &fixture.activity);
    assert(!dispatch.validate_activity(dispatch.user, context.activity));
    assert(!bs_dex_vm_current_activity(vm).bits);
    bs_dex_vm_destroy(vm);
    assert(bs_dex_android_surface_host_reset(&surface) == 1);

    /* Drive the ORIGINAL Quest constructor through the VM's new external
     * WeakReference path, then verify VM teardown releases that weak handle.
     * It remains a backend-observed weak ref; no Java GC success is assumed. */
    context.activity = NULL;
    combined.weak_reference_create = weak_create;
    combined.weak_reference_get = weak_get;
    combined.weak_reference_clear = weak_clear;
    combined.weak_reference_release = weak_release;
    assert(bs_dex_android_host_callbacks(&combined, &dispatch));
    vm = bs_dex_vm_create(dex.data, dex.size, &dispatch);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    const int creates_before = fixture.weak_creates;
    const int releases_before = fixture.weak_releases;
    stopped = bs_dex_vm_run(vm, 10000);
    assert(fixture.weak_creates == creates_before + 1);
    assert(stopped.steps > 62);
    assert(!fixture.weak.released);
    /* Invalidation is observable even while the original DEX still holds
     * the identity tag of its Activity referent. */
    fixture.weak.valid = 0;
    live = bs_dex_null();
    assert(dispatch.weak_reference_get(dispatch.user, vm, &fixture.weak,
                                       &live, &exception) == 1);
    assert(!live.bits && live.kind == BS_DEX_REF);
    bs_dex_vm_destroy(vm);
    assert(fixture.weak_releases == releases_before + 1);
    assert(fixture.weak.released);
    assert(bs_dex_android_surface_host_reset(&surface) == 1);
    bs_unity_apk_free_original_dex(&dex);
    puts("PASS: original DEX delegated host, concrete Android Activity gate, no invented UI");
    return 0;
}
