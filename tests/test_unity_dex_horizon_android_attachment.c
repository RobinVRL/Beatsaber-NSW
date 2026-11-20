/* Exact Quest DEX + manifest with host witnesses for isolated native
 * attachment graph. The mock Java callbacks below test the future callback
 * contract; they do NOT establish an Android JVM on Horizon. */
#include "unity_dex_horizon_android_attachment.h"
#include "unity_apk_dex_loader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    int on_main, valid_window, valid_asset, has_hook, verified_launch;
    int native_window, native_assets, native_looper, hook_token, launch_token;
    uint64_t main_tid, monotonic_time;
    BsDexHorizonJavaRuntime *runtime;
} Fixture;

typedef struct MockJava {
    int enabled, attached, detaches, token, deny_resources;
    int intent, component, manager, application_info, activity_info;
    int app_bundle, activity_bundle, native_lib_string;
    int resources, assets, files, obb;
} MockJava;

static int is_main(void *user) { return ((Fixture *)user)->on_main; }
static int native_ready(void *user) { return ((Fixture *)user)->on_main; }
static void *window(void *user) { return &((Fixture *)user)->native_window; }
static int valid_window(void *user, void *w) {
    Fixture *f = user;
    return f->on_main && f->valid_window && w == &f->native_window;
}
static int subscribe(void *user, BsDexHorizonJavaRuntime *rt, void **token) {
    Fixture *f = user;
    if (!rt || !token || !f->on_main || f->has_hook) return 0;
    f->has_hook = 1;
    *token = &f->hook_token;
    return 1;
}
static int unsubscribe(void *user, void *token) {
    Fixture *f = user;
    if (!f->on_main || !f->has_hook || token != &f->hook_token) return 0;
    f->has_hook = 0;
    return 1;
}
static int main_owner(void *user, uint64_t *tid) {
    Fixture *f = user;
    if (tid) *tid = 0;
    if (!tid || !f->on_main) return 0;
    *tid = f->main_tid;
    return 1;
}
static int ndk_acquire(void *user, void **token) {
    Fixture *f = user;
    if (!f->on_main || !token) return 0;
    *token = &f->native_looper;
    return 1;
}
static void ndk_release(void *user, void *token) {
    Fixture *f = user;
    assert(f->on_main && token == &f->native_looper);
}
static int ndk_wake(void *user, void *token) {
    Fixture *f = user;
    return f->on_main && token == &f->native_looper;
}
static int now_ns(void *user, uint64_t *now) {
    Fixture *f = user;
    if (!now || !f->on_main) return 0;
    *now = f->monotonic_time;
    return 1;
}
static int has_base(void *user) {
    Fixture *f = user;
    return f->runtime &&
        bs_dex_horizon_java_runtime_native_base_ready(f->runtime);
}
static int display(void *user, uint32_t *width, uint32_t *height) {
    Fixture *f = user;
    if (!f->on_main || !width || !height) return 0;
    *width = 1280;
    *height = 720;
    return 1;
}
static int language(void *user, char tag[9]) {
    Fixture *f = user;
    if (!f->on_main || !tag) return 0;
    memcpy(tag, "en-US", 6);
    return 1;
}
static int delivered_launch(void *user,
                            const BsDexHorizonLaunchReceipt *receipt) {
    Fixture *f = user;
    return f->verified_launch && receipt &&
        receipt->source_token == &f->launch_token;
}
static int probe_window(void *user, void **observed) {
    Fixture *f = user;
    if (observed) *observed = NULL;
    if (!observed || !valid_window(f, &f->native_window)) return 0;
    *observed = &f->native_window;
    return 1;
}
static int probe_assets(void *user, void **observed) {
    Fixture *f = user;
    if (observed) *observed = NULL;
    if (!observed || !f->on_main || !f->valid_asset) return 0;
    *observed = &f->native_assets;
    return 1;
}
static int load_game(void *user) { (void)user; return 1; }

static int java_class(void *user, void *object, const char *descriptor) {
    MockJava *j = user;
    if (!j->enabled || !object || !descriptor) return 0;
    if (!strcmp(descriptor, "Landroid/content/Intent;"))
        return object == &j->intent;
    if (!strcmp(descriptor, "Landroid/content/ComponentName;"))
        return object == &j->component;
    if (!strcmp(descriptor, "Landroid/content/pm/PackageManager;"))
        return object == &j->manager;
    if (!strcmp(descriptor, "Landroid/content/pm/ApplicationInfo;"))
        return object == &j->application_info;
    if (!strcmp(descriptor, "Landroid/content/pm/ActivityInfo;"))
        return object == &j->activity_info;
    if (!strcmp(descriptor, "Landroid/os/Bundle;"))
        return object == &j->app_bundle || object == &j->activity_bundle;
    if (!strcmp(descriptor, "Ljava/lang/String;"))
        return object == &j->native_lib_string;
    return 0;
}
static int app_info(void *user, void *info,
                    const BsDexQuestApplicationInfo *model) {
    MockJava *j = user;
    return j->enabled && info == &j->application_info &&
        model && model->target_sdk_version == 32 &&
        model->native_library_dir &&
        !strcmp(model->package_name, "com.beatgames.beatsaber");
}
static int activity_info(void *user, void *info,
                         const BsDexQuestActivityInfo *model) {
    MockJava *j = user;
    return j->enabled && info == &j->activity_info && model &&
        !strcmp(model->name,
                "com.unity3d.player.UnityPlayerGameActivity");
}
static int launch_info(void *user, void *intent, void *component,
                       const BsDexQuestLaunchFilter *model) {
    MockJava *j = user;
    return j->enabled && intent == &j->intent &&
        component == &j->component && model &&
        !strcmp(model->action, "android.intent.action.MAIN");
}
static int manager_info(void *user, void *manager,
                        void *info, void *component) {
    MockJava *j = user;
    return j->enabled && manager == &j->manager &&
        info == &j->activity_info && component == &j->component;
}
static int bundle_info(void *user, void *bundle,
                       const BsApkManifest *manifest, int activity_scope) {
    MockJava *j = user;
    return j->enabled && manifest &&
        (activity_scope ? bundle == &j->activity_bundle :
                          bundle == &j->app_bundle) &&
        bs_apk_manifest_metadata(manifest, "unity.run-without-focus", 0) &&
        bs_apk_manifest_metadata(manifest, "com.oculus.vr.focusaware", 1);
}
static int string_info(void *user, void *string,
                       const char *expected) {
    MockJava *j = user;
    return j->enabled && string == &j->native_lib_string &&
        expected && *expected;
}
static int java_type(void *user, const char *type, void *object) {
    MockJava *j = user;
    if (!j->enabled || !type || !object) return 0;
    if (!strcmp(type, "Lcom/unity3d/player/UnityPlayerGameActivity;"))
        return 1; /* Fixture receives the exact DEX-verified host owner. */
    if (!strcmp(type, "Landroid/content/res/Resources;"))
        return !j->deny_resources && object == &j->resources;
    if (!strcmp(type, "Landroid/content/res/AssetManager;"))
        return object == &j->assets;
    if (!strcmp(type, "Ljava/io/File;"))
        return object == &j->files || object == &j->obb;
    return java_class(j, object, type);
}
static int java_loop(void *user, void *activity,
                     const BsDexHorizonMainLooper *owner) {
    MockJava *j = user;
    return j->enabled && activity &&
        bs_dex_horizon_main_looper_java_ready(owner);
}
static int attach_java(void *user, void *activity, void *nwindow,
                       const BsDexHorizonAndroidAttachment *native_model,
                       const BsDexHorizonLaunchReceipt *launch,
                       BsDexQuestJavaOwners *package_java,
                       BsDexAndroidContextAttachment *observed,
                       void **token, const char **exception) {
    MockJava *j = user;
    if (!j->enabled || j->attached || !activity || !nwindow ||
        !native_model || !launch || !launch->source_token ||
        !package_java || !observed || !token || !exception) return 0;
    *exception = NULL;
    *package_java = (BsDexQuestJavaOwners){
        .launch_intent = &j->intent, .component = &j->component,
        .package_manager = &j->manager,
        .application_info = &j->application_info,
        .application_metadata_bundle = &j->app_bundle,
        .activity_info = &j->activity_info,
        .activity_metadata_bundle = &j->activity_bundle,
        .native_library_dir_string = &j->native_lib_string
    };
    *observed = (BsDexAndroidContextAttachment){
        .activity = activity, .intent = &j->intent,
        .application_info = &j->application_info,
        .resources = &j->resources, .assets = &j->assets,
        .package_manager = &j->manager,
        .metadata_bundle = &j->app_bundle,
        .metadata_field_observed = 1,
        .files_dir = &j->files, .obb_dir = &j->obb,
        .native_library_dir = &j->native_lib_string
    };
    j->attached = 1;
    *token = &j->token;
    return 1;
}
static int java_attached(void *user, void *activity, void *nwindow,
                         void *token,
                         const BsDexAndroidContextAttachment *observed) {
    MockJava *j = user;
    return j->enabled && j->attached && activity && nwindow &&
        token == &j->token && observed &&
        observed->application_info == &j->application_info &&
        observed->resources == &j->resources &&
        observed->assets == &j->assets;
}
static int java_detach(void *user, void *activity, void *nwindow,
                       void *token) {
    MockJava *j = user;
    if (!activity || !nwindow || !j->attached || token != &j->token)
        return 0;
    j->attached = 0;
    ++j->detaches;
    return 1;
}
static int handler_ready(void *user, BsDexVM *vm, BsDexValue handler) {
    MockJava *j = user;
    return j->enabled && vm && handler.kind == BS_DEX_REF &&
        handler.bits == UINT64_C(0x77);
}
static int retain(void *user, BsDexVM *vm, BsDexValue runnable,
                  void **held) {
    (void)user;
    (void)vm;
    (void)runnable;
    if (held) *held = NULL;
    return 0; /* No Runnable.run available in this test. */
}
static void release(void *user, BsDexVM *vm, void *held) {
    (void)user; (void)vm; (void)held;
}
static int invoke(void *user, BsDexVM *vm, BsDexValue runnable,
                  const char **exception) {
    (void)user; (void)vm; (void)runnable;
    if (exception) *exception = NULL;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 7);
    BsUnityOriginalDexImage dex = {0};
    assert(bs_unity_apk_load_original_dex(argv[1], &dex));
    Fixture f = {.on_main = 1, .valid_window = 1, .valid_asset = 1,
                 .main_tid = 7123, .monotonic_time = 1000000000ull};
    BsDexHorizonJavaRuntime runtime = {0};
    f.runtime = &runtime;
    BsDexHorizonJavaNativeHooks hooks = {
        .user = &f, .on_main_thread = is_main, .native_ready = native_ready,
        .current_window = window, .valid_window = valid_window,
        .subscribe = subscribe, .unsubscribe = unsubscribe
    };
    assert(bs_dex_horizon_java_runtime_init_with_hooks(&runtime, &hooks));
    BsDexHorizonActivityHost host = {0};
    BsDexHorizonActivityServices base =
        bs_dex_horizon_java_runtime_callbacks(&runtime);
    assert(bs_dex_horizon_activity_init(&host, dex.data, dex.size, &base));
    assert(bs_dex_horizon_java_runtime_bind_verified_host(&runtime, &host));
    BsDexAndroidContextHost context_host = {0};
    BsDexAndroidSurfaceHost surface = {0};
    BsDexAndroidSurfaceServices surface_svc = {
        .user = &host, .on_ui_thread = bs_dex_horizon_activity_on_ui_thread,
        .context_matches_activity = bs_dex_horizon_activity_context_matches
    };
    bs_dex_android_surface_host_init(&surface, &surface_svc);
    BsDexAndroidHost bridge = {
        .context = &context_host, .surface = &surface,
        .load_game_library = load_game
    };
    assert(bs_dex_horizon_activity_bind(&host, &bridge, &context_host));
    BsDexHost vmtable = {0};
    assert(bs_dex_android_host_callbacks(&bridge, &vmtable));
    BsDexVM *vm = bs_dex_vm_create(dex.data, dex.size, &vmtable);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop stopped = bs_dex_vm_run(vm, 20000);
    assert(stopped.steps > 62 && host.activity.alive &&
           bs_dex_horizon_java_runtime_native_base_ready(&runtime));

    BsDexQuestPackageHost package = {0};
    assert(bs_dex_quest_package_open(&package, argv[1]));
    assert(bs_dex_quest_package_observe_native_paths(
        &package, argv[2], argv[3]));
    BsDexHorizonContextNative context_native = {
        .user = &f, .on_owner_thread = is_main,
        .native_activity_base_ready = has_base,
        .read_window_pixels = display, .read_horizon_language = language
    };
    BsDexHorizonContextObjects files = {0};
    assert(bs_dex_horizon_context_objects_init(
        &files, &package, &context_native, argv[4], argv[5], argv[6]));
    BsDexHorizonMainLooperNative native_loop = {
        .user = &f, .observe_owner_thread = main_owner,
        .acquire_native_looper = ndk_acquire,
        .release_native_looper = ndk_release, .wake = ndk_wake,
        .monotonic_time_ns = now_ns
    };
    BsDexHorizonMainLooper looper = {0};
    assert(bs_dex_horizon_main_looper_observe_main_loop(&looper,
                                                         &native_loop));
    BsDexHorizonAttachmentNative native_attach = {
        .user = &f, .window = probe_window,
        .asset_manager = probe_assets
    };
    BsDexHorizonAndroidAttachment adapter = {0};
    assert(bs_dex_horizon_android_attachment_prepare(
        &adapter, &package, &files, &runtime, &looper, &native_attach));
    assert(!bs_dex_horizon_android_attachment_prepare(
        &adapter, &package, &files, &runtime, &looper, &native_attach));

    BsDexHorizonAttachmentRef value = {0};
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_APPLICATION_INFO, &value));
    BsDexHorizonAttachmentRef old_ref = value;
    const BsDexQuestApplicationInfo *app = NULL;
    assert(bs_dex_horizon_android_attachment_application(value, &app));
    assert(app && app->target_sdk_version == 32 &&
           !strcmp(app->package_name, "com.beatgames.beatsaber"));
    assert(!bs_dex_horizon_android_attachment_activity(value, NULL));
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_ACTIVITY_INFO, &value));
    const BsDexQuestActivityInfo *activity = NULL;
    assert(bs_dex_horizon_android_attachment_activity(value, &activity));
    assert(activity && activity->theme_resource == 0x7f0e00ffu);
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_APPLICATION_METADATA, &value));
    BsApkManifestValue metadata = {0};
    assert(bs_dex_horizon_android_attachment_metadata(
        value, "unity.run-without-focus", &metadata));
    assert(metadata.kind == BS_APK_VALUE_BOOLEAN && metadata.raw == 1);
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_ACTIVITY_METADATA, &value));
    assert(bs_dex_horizon_android_attachment_metadata(
        value, "com.oculus.vr.focusaware", &metadata));
    assert(metadata.kind == BS_APK_VALUE_BOOLEAN);
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_LAUNCH_FILTER, &value));
    const BsDexQuestLaunchFilter *filter = NULL;
    assert(bs_dex_horizon_android_attachment_launch_filter(value, &filter));
    assert(filter && !strcmp(filter->action, "android.intent.action.MAIN"));
    assert(!bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_LAUNCH_RECEIPT, &value));
    assert(!value.owner);
    const char *path = NULL;
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_FILES_DIR, &value));
    assert(bs_dex_horizon_android_attachment_path(value, &path) &&
           !strcmp(path, argv[4]));
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_NATIVE_LIBRARY_DIR, &value));
    assert(bs_dex_horizon_android_attachment_path(value, &path) &&
           !strcmp(path, argv[2]));
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_CONFIGURATION, &value));
    BsDexHorizonContextConfig config = {0};
    assert(bs_dex_horizon_android_attachment_configuration(value, &config));
    assert(config.window_width_px == 1280 && config.window_height_px == 720);
    size_t size = 0;
    uint32_t crc = 0;
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_RESOURCE_TABLE, &value));
    assert(bs_dex_horizon_android_attachment_resource_table(
        value, &size, &crc));
    assert(size == BS_QUEST_RESOURCES_ARSC_SIZE &&
           crc == BS_QUEST_RESOURCES_ARSC_CRC);
    void *handle = NULL;
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_NATIVE_ASSET_MANAGER, &value));
    assert(bs_dex_horizon_android_attachment_native_handle(value, &handle) &&
           handle == &f.native_assets);
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_NATIVE_WINDOW, &value));
    assert(bs_dex_horizon_android_attachment_native_handle(value, &handle) &&
           handle == &f.native_window);
    f.valid_asset = 0;
    assert(!bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_NATIVE_WINDOW, &value));
    f.valid_asset = 1;
    package.manifest.version_code ^= 1u;
    assert(!bs_dex_horizon_android_attachment_application(old_ref, &app));
    package.manifest.version_code ^= 1u;
    f.on_main = 0;
    assert(!bs_dex_horizon_android_attachment_application(old_ref, &app));
    f.on_main = 1;

    assert(bs_dex_horizon_android_attachment_install(&adapter, &host));
    assert(!bs_dex_horizon_android_attachment_install(&adapter, &host));
    assert(host.platform.user == &adapter);
    void *out_window = &f.native_window;
    BsDexAndroidContextAttachment readback = {.activity = &host.activity};
    const char *exception = "unobserved exception";
    assert(host.platform.attach(host.platform.user, &host.activity,
                                &out_window, &readback, &exception) == 0);
    assert(!out_window && !readback.activity && !exception);
    assert(!bs_dex_horizon_android_attachment_java_attached(&adapter));
    assert(!context_host.attachment_evidenced);

    MockJava j = {0};
    BsDexHorizonAttachmentJava simulated_java = {
        .user = &j,
        .verify_main_java_loop = java_loop,
        .execute_attach = attach_java,
        .verify_type = java_type,
        .verify_attachment = java_attached,
        .detach = java_detach,
        .package_witness = {
            .user = &j, .verify_class = java_class,
            .verify_app_info = app_info,
            .verify_activity_info = activity_info,
            .verify_launch_intent = launch_info,
            .verify_package_manager = manager_info,
            .verify_bundle = bundle_info,
            .verify_string_value = string_info
        }
    };
    BsDexHorizonAttachmentJava incomplete = simulated_java;
    incomplete.verify_type = NULL;
    assert(!bs_dex_horizon_android_attachment_set_java(&adapter,
                                                         &incomplete));
    assert(bs_dex_horizon_android_attachment_set_java(&adapter,
                                                       &simulated_java));
    assert(host.platform.attach(host.platform.user, &host.activity,
                                &out_window, &readback, &exception) == 0);
    assert(!j.attached); /* Still no delivered Intent. */

    files.native.verify_launch_receipt = delivered_launch;
    f.verified_launch = 1;
    BsDexHorizonLaunchReceipt receipt = {
        .action = "android.intent.action.MAIN",
        .package_name = "com.beatgames.beatsaber",
        .activity_class = "com.unity3d.player.UnityPlayerGameActivity",
        .launcher_category = 1, .vr_category = 1,
        .source_token = &f.launch_token
    };
    assert(bs_dex_horizon_context_bind_launch(&files, &receipt));
    assert(bs_dex_horizon_android_attachment_ref(
        &adapter, BS_HA_LAUNCH_RECEIPT, &value));
    BsDexHorizonLaunchReceipt verified = {0};
    assert(bs_dex_horizon_android_attachment_launch(value, &verified));
    assert(verified.source_token == &f.launch_token);
    assert(host.platform.attach(host.platform.user, &host.activity,
                                &out_window, &readback, &exception) == 0);
    assert(!j.attached); /* Native ALooper is not Java Handler/Looper. */
    BsDexHorizonMainLooperJava handler = {
        .user = &j, .verify_handler = handler_ready,
        .retain_runnable = retain, .release_runnable = release,
        .invoke_runnable = invoke
    };
    BsDexValue java_handler = {.kind = BS_DEX_REF, .bits = 0x77u};
    j.enabled = 1; /* Simulated Java backend witnesses for contract test. */
    assert(bs_dex_horizon_main_looper_bind_java_handler(
        &looper, vm, java_handler, &handler));
    assert(bs_dex_horizon_main_looper_java_ready(&looper));
    f.verified_launch = 0;
    assert(host.platform.attach(host.platform.user, &host.activity,
                                &out_window, &readback, &exception) == 0);
    assert(!j.attached); /* Revoked native launch receipt. */
    f.verified_launch = 1;

    /* Even with a supplied native launch and Handler, an unavailable Java
     * Resources class must abort and release the attempted Java attachment. */
    j.deny_resources = 1;
    assert(bridge.attach_and_observe_activity(
        bridge.user, &host.activity, &readback, &exception) == 0);
    assert(!host.activity.attached && !j.attached && j.detaches == 1);
    assert(!bs_dex_horizon_android_attachment_java_attached(&adapter));
    j.deny_resources = 0;

    /* Isolated positive fixture proves ALL witnesses are required. It does
     * not claim live Android ContextImpl, Looper or Java classes on Switch. */
    assert(bridge.attach_and_observe_activity(
        bridge.user, &host.activity, &readback, &exception) == 1);
    assert(j.attached && host.activity.attached);
    assert(bs_dex_horizon_android_attachment_java_attached(&adapter));
    assert(bs_dex_android_context_bind_attachment(&context_host, &readback));
    assert(context_host.attachment_evidenced);
    j.enabled = 0;
    assert(!bs_dex_horizon_android_attachment_java_attached(&adapter));
    j.enabled = 1;
    assert(bs_dex_horizon_android_attachment_java_attached(&adapter));
    f.valid_window = 0;
    assert(!bs_dex_horizon_android_attachment_java_attached(&adapter));
    f.valid_window = 1;
    assert(bs_dex_horizon_android_attachment_java_attached(&adapter));
    assert(!bs_dex_horizon_android_attachment_release(&adapter));
    bs_dex_vm_destroy(vm);
    assert(bs_dex_horizon_activity_release(&host));
    assert(!j.attached && j.detaches == 2);
    assert(!context_host.attachment_evidenced);
    assert(bs_dex_horizon_android_attachment_release(&adapter));
    assert(!bs_dex_horizon_android_attachment_application(old_ref, &app));
    assert(host.platform.user == &runtime);
    BsDexHorizonServices fake_production = {0};
    BsDexHorizonAttachmentNative not_on_horizon =
        bs_dex_horizon_android_attachment_production_native(
            &fake_production);
    assert(!not_on_horizon.window && !not_on_horizon.asset_manager);
    assert(bs_dex_horizon_main_looper_shutdown(&looper));
    assert(bs_dex_horizon_context_objects_release(&files));
    assert(bs_dex_horizon_java_runtime_shutdown(&runtime));
    bs_dex_quest_package_close(&package);
    assert(bs_dex_android_surface_host_reset(&surface));
    bs_unity_apk_free_original_dex(&dex);
    puts("PASS: original Quest typed native attachment graph, live launch and strict Java callback gates/teardown");
    return 0;
}
