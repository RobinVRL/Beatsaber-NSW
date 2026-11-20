/* Host service fixtures model native Horizon observations; tests establish
 * class lineage, externally completed attachment, and non-owning weak token
 * semantics. This is not evidence of a production libnx UI/Window backend. */
#include "unity_apk_dex_loader.h"
#include "unity_dex_horizon_activity_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fixture {
    int main_thread;
    int window_valid;
    int base_alive;
    int attached;
    int construct_count;
    int attach_count;
    int detach_count;
    int window, intent, appinfo, resources, assets, metadata;
    int files_dir, obb_dir, native_lib_dir;
    void *native_activity;
} Fixture;

static int on_ui(void *user) {
    return ((Fixture *)user)->main_thread;
}
static int construct_base(void *user, void *activity) {
    Fixture *f = user;
    if (!f->main_thread || f->base_alive || !activity) return 0;
    f->native_activity = activity;
    f->base_alive = 1;
    ++f->construct_count;
    return 1;
}
static int attach_native(void *user, void *activity, void **window,
                         BsDexAndroidContextAttachment *observed,
                         const char **exception) {
    Fixture *f = user;
    (void)exception;
    if (!f->main_thread || !f->base_alive || f->attached ||
        activity != f->native_activity) return 0;
    ++f->attach_count;
    f->attached = 1;
    *window = &f->window;
    *observed = (BsDexAndroidContextAttachment){
        .activity = activity, .intent = &f->intent,
        .application_info = &f->appinfo, .resources = &f->resources,
        .assets = &f->assets, .metadata_bundle = &f->metadata,
        .metadata_field_observed = 1, .files_dir = &f->files_dir,
        .obb_dir = &f->obb_dir,
        .native_library_dir = &f->native_lib_dir
    };
    return 1;
}
static int valid_window(void *user, void *window) {
    Fixture *f = user;
    return f->window_valid && window == &f->window;
}
static int verify_external(void *user, const char *type, void *object) {
    Fixture *f = user;
    if (!f->attached || !type || !object) return 0;
    if (!strcmp(type, "Landroid/content/Intent;"))
        return object == &f->intent;
    if (!strcmp(type, "Landroid/content/pm/ApplicationInfo;"))
        return object == &f->appinfo;
    if (!strcmp(type, "Landroid/content/res/Resources;"))
        return object == &f->resources;
    if (!strcmp(type, "Landroid/content/res/AssetManager;"))
        return object == &f->assets;
    if (!strcmp(type, "Landroid/os/Bundle;"))
        return object == &f->metadata;
    if (!strcmp(type, "Ljava/io/File;"))
        return object == &f->files_dir || object == &f->obb_dir;
    if (!strcmp(type, "Ljava/lang/String;"))
        return object == &f->native_lib_dir;
    return 0;
}
static int verify_attach(void *user, void *activity, void *window,
                         const BsDexAndroidContextAttachment *observed) {
    Fixture *f = user;
    return f->main_thread && f->attached && f->base_alive &&
           activity == f->native_activity && window == &f->window &&
           observed && observed->activity == activity &&
           observed->intent == &f->intent &&
           observed->application_info == &f->appinfo &&
           observed->resources == &f->resources &&
           observed->assets == &f->assets;
}
static int detach_native(void *user, void *activity, void *window) {
    Fixture *f = user;
    if (!f->main_thread || !f->base_alive ||
        activity != f->native_activity ||
        (window && window != &f->window)) return 0;
    f->base_alive = 0;
    f->attached = 0;
    f->native_activity = NULL;
    ++f->detach_count;
    return 1;
}
static int load_game_library(void *user) {
    (void)user; /* Already loaded verified original game JNI library. */
    return 1;
}

static BsDexHorizonActivityServices services(Fixture *fixture) {
    return (BsDexHorizonActivityServices){
        .user = fixture,
        .on_ui_thread = on_ui,
        .construct_base = construct_base,
        .attach = attach_native,
        .valid_nwindow = valid_window,
        .verify_attachment = verify_attach,
        .verify_external = verify_external,
        .detach = detach_native
    };
}

int main(int argc, char **argv) {
    assert(argc == 2);
    BsUnityOriginalDexImage dex = {0};
    assert(bs_unity_apk_load_original_dex(argv[1], &dex));

    Fixture f = {.main_thread = 1, .window_valid = 1};
    BsDexHorizonActivityHost host = {0};
    BsDexHorizonActivityServices backend = services(&f);
    backend.verify_attachment = NULL;
    assert(!bs_dex_horizon_activity_init(&host, dex.data, dex.size,
                                         &backend));
    backend = services(&f);
    assert(!bs_dex_horizon_activity_init(&host, dex.data, dex.size - 1,
                                         &backend));
    uint8_t *modified = malloc(dex.size);
    assert(modified);
    memcpy(modified, dex.data, dex.size);
    modified[0x10] ^= 1; /* Pinned checksum / DEX identity check. */
    assert(!bs_dex_horizon_activity_init(&host, modified, dex.size,
                                         &backend));
    free(modified);
    assert(bs_dex_horizon_activity_init(&host, dex.data, dex.size,
                                        &backend));

    BsDexAndroidContextHost context = {0};
    BsDexAndroidSurfaceHost surface = {0};
    BsDexAndroidSurfaceServices surface_api = {
        .user = &host,
        .on_ui_thread = bs_dex_horizon_activity_on_ui_thread,
        .context_matches_activity = bs_dex_horizon_activity_context_matches
    };
    bs_dex_android_surface_host_init(&surface, &surface_api);
    BsDexAndroidHost combined = {
        .surface = &surface,
        .load_game_library = load_game_library
    };
    assert(bs_dex_horizon_activity_bind(&host, &combined, &context));
    BsDexHost dispatch = {0};
    assert(bs_dex_android_host_callbacks(&combined, &dispatch));

    /* A valid native platform constructor and external weak registry are
     * sufficient for original DEX execution beyond WeakReference.<init>. */
    BsDexVM *vm = bs_dex_vm_create(dex.data, dex.size, &dispatch);
    assert(vm);
    assert(bs_dex_vm_start_activity_constructor(vm));
    BsDexStop stopped = bs_dex_vm_run(vm, 10000);
    assert(f.construct_count == 1 && host.activity.constructed);
    assert(stopped.steps > 62 && host.weak_outstanding >= 1);
    assert(!host.activity.attached);
    assert(!dispatch.validate_activity(dispatch.user, &host.activity));
    BsDexValue original_activity_ref = bs_dex_wrap_platform(vm,
        &host.activity, "Lcom/unity3d/player/UnityPlayerGameActivity;");
    assert(original_activity_ref.bits);
    void *weak_token = NULL;
    const char *weak_exception = NULL;
    assert(dispatch.weak_reference_create(dispatch.user, vm,
        original_activity_ref, &weak_token, &weak_exception) == 1);
    BsDexValue resolved = bs_dex_null();
    assert(dispatch.weak_reference_get(dispatch.user, vm, weak_token,
        &resolved, &weak_exception) == 1);
    assert(resolved.bits == original_activity_ref.bits);
    assert(!bs_dex_horizon_activity_release(&host)); /* live VM owns weak */
    assert(bs_dex_horizon_activity_invalidate(&host));
    assert(!host.activity.alive && f.detach_count == 1);
    resolved = original_activity_ref;
    assert(dispatch.weak_reference_get(dispatch.user, vm, weak_token,
        &resolved, &weak_exception) == 1);
    assert(!resolved.bits && resolved.kind == BS_DEX_REF);
    dispatch.weak_reference_release(dispatch.user, vm, weak_token);
    bs_dex_vm_destroy(vm);
    assert(host.weak_outstanding == 0);
    assert(bs_dex_horizon_activity_release(&host));
    assert(bs_dex_android_surface_host_reset(&surface));

    /* Independently check the native attach side effect and atomic observed
     * Android Context/Window readback before onCreate is admissible. */
    void *activity = NULL;
    const char *exception = NULL;
    assert(combined.construct_activity_base(combined.user,
        "Lcom/unity3d/player/UnityPlayerGameActivity;",
        &activity, &exception) == 1);
    assert(activity == &host.activity);
    int subtype = 0;
    assert(combined.observed_instance_of(combined.user, activity,
        "Landroid/app/Activity;", &subtype, &exception) == 1 && subtype);
    /* Java class ancestry exists before attachment; this MUST NOT make
     * Context's getters or Activity.attach available. */
    assert(combined.observed_instance_of(combined.user, activity,
        "Landroid/content/Context;", &subtype, &exception) == 1 && subtype);
    assert(combined.observed_instance_of(combined.user, activity,
        "Landroid/content/ContextWrapper;", &subtype, &exception) == 1 && subtype);
    assert(combined.observed_instance_of(combined.user, activity,
        "Landroid/view/ContextThemeWrapper;", &subtype, &exception) == 1 && subtype);
    assert(combined.observed_instance_of(combined.user, activity,
        "Ljava/lang/Object;", &subtype, &exception) == 1 && subtype);
    assert(combined.observed_instance_of(combined.user, activity,
        "Landroid/content/Intent;", &subtype, &exception) == 1 && !subtype);
    assert(!bs_dex_horizon_activity_context_matches(&host, activity));
    BsDexAndroidContextAttachment observed = {0};
    /* A NWindow that fails independent validity may not attach. */
    f.window_valid = 0;
    assert(combined.attach_and_observe_activity(combined.user, activity,
               &observed, &exception) == 0);
    assert(!host.activity.attached && !host.activity.constructed &&
           !host.activity.alive && f.detach_count == 2);
    /* The preceding rejected attach rolled back the native base; create an
     * actual new instance through the original constructor service. */
    assert(!bs_dex_horizon_activity_invalidate(&host));
    assert(bs_dex_horizon_activity_release(&host));
    f.window_valid = 1;
    assert(combined.construct_activity_base(combined.user,
        "Lcom/unity3d/player/UnityPlayerGameActivity;",
        &activity, &exception) == 1);
    assert(combined.attach_and_observe_activity(combined.user, activity,
        &observed, &exception) == 1);
    assert(context.activity == NULL);
    assert(bs_dex_android_context_bind_attachment(&context, &observed));
    assert(host.activity.attached && context.attachment_evidenced);
    assert(bs_dex_horizon_activity_context_matches(&host, activity));
    assert(dispatch.validate_activity(dispatch.user, activity));
    f.window_valid = 0;
    assert(!dispatch.validate_activity(dispatch.user, activity));
    f.window_valid = 1;
    assert(bs_dex_horizon_activity_release(&host));
    assert(!context.attachment_evidenced && !context.activity);
    assert(f.detach_count >= 3);
    bs_unity_apk_free_original_dex(&dex);
    puts("PASS: pinned original Activity inheritance, native attach, weak lifecycle and NWindow invalidation");
    return 0;
}
