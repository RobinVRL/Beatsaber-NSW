/* Reuse the Quest DEX-backed native setup and actual asset I/O fixture.
 * Its Java class registry is explicitly a TEST double, never production. */
#define main existing_resource_test_main
#include "test_unity_dex_horizon_java_resources.c"
#undef main
#include "unity_dex_android_framework_context.h"

static int witnessed_launch(void *user,
                            const BsDexHorizonLaunchReceipt *receipt) {
    Fixture *f = user;
    return f->owner_thread && receipt &&
        receipt->source_token == &f->native_cookie;
}

typedef struct JavaFixture {
    Fixture *native;
    const char *files_path, *obb_path;
    int activity, context, window, handler, looper, loader;
    int files, obb, intent, component, manager, appinfo, activityinfo;
    int appbundle, activitybundle, library_string;
    int attachment_live, wrong_getter, wrong_path, wrong_intent;
} JavaFixture;

static int check_class(void *user, void *object, const char *descriptor) {
    JavaFixture *j = user;
    if (!object || !descriptor || !j->attachment_live) return 0;
    struct {void *object; const char *name;} types[] = {
        {&j->activity, "Lcom/unity3d/player/UnityPlayerGameActivity;"},
        {&j->context, "Landroid/app/ContextImpl;"},
        {&j->window, "Landroid/view/Window;"},
        {&j->handler, "Landroid/os/Handler;"},
        {&j->looper, "Landroid/os/Looper;"},
        {&j->loader, "Ljava/lang/ClassLoader;"},
        {&j->files, "Ljava/io/File;"},
        {&j->obb, "Ljava/io/File;"},
        {&j->intent, "Landroid/content/Intent;"},
        {&j->component, "Landroid/content/ComponentName;"},
        {&j->manager, "Landroid/content/pm/PackageManager;"},
        {&j->appinfo, "Landroid/content/pm/ApplicationInfo;"},
        {&j->activityinfo, "Landroid/content/pm/ActivityInfo;"},
        {&j->appbundle, "Landroid/os/Bundle;"},
        {&j->activitybundle, "Landroid/os/Bundle;"},
        {&j->library_string, "Ljava/lang/String;"},
        {&j->native->java_resources, "Landroid/content/res/Resources;"},
        {&j->native->java_assetmanager,
            "Landroid/content/res/AssetManager;"},
        {&j->native->java_configuration,
            "Landroid/content/res/Configuration;"}
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; ++i)
        if (object == types[i].object &&
            !strcmp(descriptor, types[i].name)) return 1;
    return 0;
}

static int check_app(void *user, void *object,
                     const BsDexQuestApplicationInfo *model) {
    JavaFixture *j = user;
    return object == &j->appinfo && model &&
        !strcmp(model->package_name, "com.beatgames.beatsaber") &&
        model->target_sdk_version == 32 && model->native_library_dir;
}

static int check_activity_info(void *user, void *object,
                               const BsDexQuestActivityInfo *model) {
    JavaFixture *j = user;
    return object == &j->activityinfo && model &&
        !strcmp(model->name,
                "com.unity3d.player.UnityPlayerGameActivity") &&
        model->enabled && model->exported;
}

static int check_intent(void *user, void *intent, void *component,
                        const BsDexQuestLaunchFilter *filter) {
    JavaFixture *j = user;
    return !j->wrong_intent && intent == &j->intent &&
        component == &j->component && filter &&
        !strcmp(filter->action, "android.intent.action.MAIN") &&
        !strcmp(filter->package_name, "com.beatgames.beatsaber");
}

static int check_manager(void *user, void *manager, void *activity_info,
                         void *component) {
    JavaFixture *j = user;
    return manager == &j->manager && activity_info == &j->activityinfo &&
        component == &j->component;
}

static int check_bundle(void *user, void *bundle,
                        const BsApkManifest *manifest, int activity_scope) {
    JavaFixture *j = user;
    return manifest && manifest->metadata_count == 16 &&
        (activity_scope ? bundle == &j->activitybundle :
                          bundle == &j->appbundle);
}

static int check_string(void *user, void *object, const char *value) {
    JavaFixture *j = user;
    return object == &j->library_string && value &&
        strstr(value, "native") != NULL;
}

static int invoke_noarg(void *user, void *receiver, const char *declaring,
                        const char *method, const char *prototype,
                        void **result) {
    JavaFixture *j = user;
    if (result) *result = NULL;
    if (!result || !j->attachment_live || !receiver ||
        !declaring || !method || !prototype ||
        prototype[0] != '(' || j->wrong_getter) return 0;
    struct {void *receiver; const char *method; void *result;} links[] = {
        {&j->activity, "getBaseContext", &j->context},
        {&j->activity, "getIntent", &j->intent},
        {&j->activity, "getWindow", &j->window},
        {&j->activity, "getClassLoader", &j->loader},
        {&j->activity, "getMainLooper", &j->looper},
        {&j->handler, "getLooper", &j->looper},
        {&j->context, "getPackageManager", &j->manager},
        {&j->context, "getApplicationInfo", &j->appinfo},
        {&j->context, "getFilesDir", &j->files},
        {&j->context, "getObbDir", &j->obb},
        {&j->context, "getAssets", &j->native->java_assetmanager},
        {&j->context, "getResources", &j->native->java_resources},
        {&j->native->java_resources, "getAssets",
            &j->native->java_assetmanager},
        {&j->native->java_resources, "getConfiguration",
            &j->native->java_configuration},
        {&j->intent, "getComponent", &j->component}
    };
    for (size_t i = 0; i < sizeof links / sizeof links[0]; ++i) {
        if (links[i].receiver == receiver && !strcmp(links[i].method, method)) {
            *result = links[i].result;
            return 1;
        }
    }
    return 0;
}

static int check_path(void *user, void *file, const char *path) {
    JavaFixture *j = user;
    return !j->wrong_path && path &&
        ((file == &j->files && !strcmp(path, j->files_path)) ||
         (file == &j->obb && !strcmp(path, j->obb_path)));
}

static int check_assets(void *user, void *java_assets, void *native_assets) {
    JavaFixture *j = user;
    return java_assets == &j->native->java_assetmanager &&
        native_assets == &j->native->native_manager;
}

static int check_config(void *user, void *java_config,
                        const BsDexHorizonObservedConfiguration *observed) {
    JavaFixture *j = user;
    return java_config == &j->native->java_configuration && observed &&
        observed->width_px == 1280 && observed->height_px == 720 &&
        !strcmp(observed->language_tag, "en-US");
}

static int check_receipt(void *user, void *intent,
                         const BsDexHorizonLaunchReceipt *receipt) {
    JavaFixture *j = user;
    return !j->wrong_intent && intent == &j->intent && receipt &&
        receipt->source_token == &j->native->native_cookie &&
        !strcmp(receipt->action, "android.intent.action.MAIN");
}

static int check_attach(void *user,
                        const BsDexFrameworkJavaAttachment *java,
                        const BsDexFrameworkNativeContext *native) {
    JavaFixture *j = user;
    return j->attachment_live && java && native &&
        java->activity == &j->activity && java->context_impl == &j->context &&
        java->window == &j->window && java->handler == &j->handler &&
        java->main_looper == &j->looper && native->owner_thread_id == 789 &&
        native->nwindow == &j->native->nwindow;
}

int main(int argc, char **argv) {
    assert(argc == 8);
    Fixture f = {.owner_thread = 1, .valid_window = 1,
                 .asset_available = 1, .main_tid = 789,
                 .clock = UINT64_C(1000000000)};
    assert(strlen(argv[7]) < sizeof f.root);
    strcpy(f.root, argv[7]);
    BsUnityOriginalDexImage dex = {0};
    assert(bs_unity_apk_load_original_dex(argv[1], &dex));
    BsDexHorizonJavaRuntime runtime = {0};
    f.runtime = &runtime;
    BsDexHorizonJavaNativeHooks lifecycle = {
        .user = &f, .on_main_thread = main_thread,
        .native_ready = ready_native,
        .current_window = observed_window,
        .valid_window = valid_window, .subscribe = subscribe,
        .unsubscribe = unsubscribe
    };
    assert(bs_dex_horizon_java_runtime_init_with_hooks(&runtime, &lifecycle));
    BsDexHorizonActivityHost activity = {0};
    BsDexHorizonActivityServices services =
        bs_dex_horizon_java_runtime_callbacks(&runtime);
    assert(bs_dex_horizon_activity_init(&activity, dex.data,
                                        dex.size, &services));
    assert(bs_dex_horizon_java_runtime_bind_verified_host(
        &runtime, &activity));
    BsDexAndroidContextHost context_host = {0};
    BsDexAndroidSurfaceHost surface = {0};
    BsDexAndroidSurfaceServices surface_service = {
        .user = &activity,
        .on_ui_thread = bs_dex_horizon_activity_on_ui_thread,
        .context_matches_activity = bs_dex_horizon_activity_context_matches
    };
    bs_dex_android_surface_host_init(&surface, &surface_service);
    BsDexAndroidHost combined = {
        .context = &context_host, .surface = &surface,
        .load_game_library = load_game
    };
    assert(bs_dex_horizon_activity_bind(&activity, &combined, &context_host));
    BsDexHost callbacks = {0};
    assert(bs_dex_android_host_callbacks(&combined, &callbacks));
    BsDexVM *vm = bs_dex_vm_create(dex.data, dex.size, &callbacks);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop stop = bs_dex_vm_run(vm, 20000);
    assert(stop.steps > 62 &&
           bs_dex_horizon_java_runtime_native_base_ready(&runtime));

    BsDexQuestPackageHost package = {0};
    assert(bs_dex_quest_package_open(&package, argv[1]));
    assert(bs_dex_quest_package_observe_native_paths(
        &package, argv[2], argv[3]));
    BsDexHorizonContextNative native_context = {
        .user = &f, .on_owner_thread = main_thread,
        .native_activity_base_ready = actual_base,
        .read_window_pixels = window_pixels,
        .read_horizon_language = language
        /* The real production source has NO delivered Android launch. */
    };
    BsDexHorizonContextObjects files = {0};
    assert(bs_dex_horizon_context_objects_init(
        &files, &package, &native_context, argv[4], argv[5], argv[6]));
    BsDexHorizonMainLooperNative native_loop = {
        .user = &f, .observe_owner_thread = loop_owner,
        .acquire_native_looper = loop_acquire,
        .release_native_looper = loop_release, .wake = loop_wake,
        .monotonic_time_ns = loop_clock
    };
    BsDexHorizonMainLooper looper = {0};
    assert(bs_dex_horizon_main_looper_observe_main_loop(
        &looper, &native_loop));
    BsDexHorizonAttachmentNative native_attachment = {
        .user = &f, .window = probe_window, .asset_manager = probe_assets
    };
    BsDexHorizonAndroidAttachment attachment = {0};
    assert(bs_dex_horizon_android_attachment_prepare(
        &attachment, &package, &files, &runtime, &looper,
        &native_attachment));
    BsDexHorizonJavaResourcesNative native_assets = {
        .user = &f, .observed_asset_manager = observe_manager,
        .open_asset = asset_open, .read_asset = asset_read,
        .seek_asset = asset_seek, .length_asset = asset_length,
        .close_asset = asset_close
    };
    BsDexHorizonJavaResources resources = {0};
    assert(bs_dex_horizon_java_resources_init(
        &resources, &attachment, &files, &package, &native_assets));

    BsDexAndroidFrameworkContext bridge = {0};
    assert(!bs_dex_android_framework_context_init(&bridge, NULL, &resources));
    assert(bs_dex_android_framework_context_init(
        &bridge, &attachment, &resources));
    assert(!bs_dex_android_framework_context_init(
        &bridge, &attachment, &resources));
    BsDexFrameworkNativeContext readback = {0};
    assert(bs_dex_android_framework_context_observe(&bridge, &readback));
    assert(readback.application && readback.activity &&
           !strcmp(readback.application->package_name,
                   "com.beatgames.beatsaber") &&
           !strcmp(readback.activity->name,
                   "com.unity3d.player.UnityPlayerGameActivity"));
    assert(!strcmp(readback.files_dir, argv[4]));
    assert(!strcmp(readback.obb_dir, argv[5]));
    assert(!strcmp(readback.external_files_dir, argv[6]));
    assert(!strcmp(readback.native_library_dir, argv[2]));
    assert(readback.configuration.width_px == 1280 &&
           readback.configuration.height_px == 720 &&
           !strcmp(readback.configuration.language_tag, "en-US"));
    assert(readback.asset_manager == &f.native_manager &&
           readback.nwindow == &f.nwindow &&
           readback.alooper == &f.native_looper &&
           readback.owner_thread_id == 789);

    BsApkManifestValue metadata = {0};
    assert(bs_dex_android_framework_context_metadata(
        &bridge, "unity.run-without-focus", 0, &metadata));
    assert(metadata.kind == BS_APK_VALUE_BOOLEAN && metadata.raw);
    assert(!bs_dex_android_framework_context_metadata(
        &bridge, "unity.run-without-focus", 1, &metadata));
    assert(!bs_dex_android_framework_context_metadata(
        &bridge, "unity.run-without-focus", 2, &metadata));

    BsDexHorizonAssetStream stream = {0};
    uint64_t length = 100;
    assert(!bs_dex_android_framework_context_open_asset(
        &bridge, "../AndroidManifest.xml", &stream, &length));
    assert(!stream.owner && !length);
    assert(bs_dex_android_framework_context_open_asset(
        &bridge, "bin/Data/boot.config", &stream, &length));
    assert(length == 768);
    char first_line[22];
    size_t count = 0;
    assert(bs_dex_horizon_java_resources_read(
        stream, first_line, sizeof first_line, &count));
    assert(count == sizeof first_line &&
           !memcmp(first_line, "gfx-enable-gfx-jobs=1\n", count));
    assert(bs_dex_horizon_java_resources_close(stream));

    BsDexHorizonLaunchReceipt delivered = {0};
    assert(!bs_dex_android_framework_context_launch(&bridge, &delivered));
    assert(!delivered.action && !delivered.source_token);
    BsDexAndroidContextAttachment java_readback = {.activity = &f};
    BsDexFrameworkJavaAttachment fake_java = {.activity = &f};
    BsDexFrameworkJavaExecutor incomplete = {0};
    assert(!bs_dex_android_framework_context_verify_java(
        &bridge, &fake_java, &incomplete, &java_readback));
    assert(!java_readback.activity && !java_readback.intent);

    BsDexHorizonLaunchReceipt guessed = {
        .action = "android.intent.action.MAIN",
        .package_name = package.manifest.package_name,
        .activity_class = package.manifest.activity_class,
        .launcher_category = 1, .vr_category = 1,
        .source_token = &f.native_cookie
    };
    assert(!bs_dex_horizon_context_bind_launch(&files, &guessed));
    files.native.verify_launch_receipt = witnessed_launch;
    guessed.action = "android.intent.action.VIEW";
    assert(!bs_dex_horizon_context_bind_launch(&files, &guessed));
    guessed.action = "android.intent.action.MAIN";
    guessed.source_token = NULL;
    assert(!bs_dex_horizon_context_bind_launch(&files, &guessed));
    guessed.source_token = &f.native_cookie;
    assert(bs_dex_horizon_context_bind_launch(&files, &guessed));
    assert(bs_dex_android_framework_context_launch(&bridge, &delivered));
    assert(!strcmp(delivered.action, "android.intent.action.MAIN"));
    assert(delivered.source_token == &f.native_cookie);
    assert(!bs_dex_android_framework_context_verify_java(
        &bridge, &fake_java, &incomplete, &java_readback));
    assert(!java_readback.activity);

    /* TEST simulated executable JVM only: unlike the production adapter it
     * has explicit witnesses for class identity and invoked getter returns. */
    f.registry_enabled = 1;
    BsDexHorizonJavaResourceRegistry java_registry = {
        .user = &f, .build = java_build, .verify_class = java_class,
        .verify_link = java_link, .destroy = java_destroy
    };
    assert(bs_dex_horizon_java_resources_bind_java(&resources,
                                                    &java_registry));
    JavaFixture j = {.native = &f, .attachment_live = 1,
                     .files_path = argv[4], .obb_path = argv[5]};
    BsDexFrameworkJavaAttachment candidate = {
        .activity = &j.activity, .context_impl = &j.context,
        .window = &j.window, .handler = &j.handler,
        .main_looper = &j.looper, .class_loader = &j.loader,
        .files_dir = &j.files, .obb_dir = &j.obb,
        .package_java = {
            .launch_intent = &j.intent, .component = &j.component,
            .package_manager = &j.manager,
            .application_info = &j.appinfo,
            .activity_info = &j.activityinfo,
            .application_metadata_bundle = &j.appbundle,
            .activity_metadata_bundle = &j.activitybundle,
            .native_library_dir_string = &j.library_string
        }
    };
    BsDexFrameworkJavaExecutor executable = {
        .user = &j, .verify_class = check_class,
        .invoke_noarg = invoke_noarg,
        .verify_file_path = check_path,
        .verify_native_asset_manager = check_assets,
        .verify_configuration = check_config,
        .verify_receipt = check_receipt,
        .verify_attachment = check_attach,
        .package_witness = {
            .user = &j, .verify_class = check_class,
            .verify_app_info = check_app,
            .verify_activity_info = check_activity_info,
            .verify_launch_intent = check_intent,
            .verify_package_manager = check_manager,
            .verify_bundle = check_bundle,
            .verify_string_value = check_string
        }
    };
    assert(bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));
    assert(java_readback.activity == &j.activity &&
           java_readback.intent == &j.intent &&
           java_readback.resources == &f.java_resources &&
           java_readback.assets == &f.java_assetmanager &&
           java_readback.metadata_bundle == &j.appbundle &&
           java_readback.metadata_field_observed);
    j.wrong_getter = 1;
    assert(!bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));
    assert(!java_readback.activity);
    j.wrong_getter = 0;
    j.wrong_path = 1;
    assert(!bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));
    j.wrong_path = 0;
    j.wrong_intent = 1;
    assert(!bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));
    j.wrong_intent = 0;
    j.attachment_live = 0;
    assert(!bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));
    j.attachment_live = 1;
    assert(bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));

    f.owner_thread = 0;
    assert(!bs_dex_android_framework_context_launch(&bridge, &delivered));
    assert(!delivered.action);
    assert(!bs_dex_android_framework_context_observe(&bridge, &readback));
    assert(!readback.application);
    f.owner_thread = 1;
    assert(bs_dex_android_framework_context_observe(&bridge, &readback));
    f.asset_available = 0;
    assert(!bs_dex_android_framework_context_open_asset(
        &bridge, "bin/Data/boot.config", &stream, &length));
    assert(!stream.owner && !length);
    f.asset_available = 1;
    assert(bs_dex_android_framework_context_verify_java(
        &bridge, &candidate, &executable, &java_readback));
    package.manifest.version_code ^= 1u;
    assert(!bs_dex_android_framework_context_observe(&bridge, &readback));
    package.manifest.version_code ^= 1u;
    assert(bs_dex_android_framework_context_observe(&bridge, &readback));

    bs_dex_android_framework_context_clear(&bridge);
    assert(!bs_dex_android_framework_context_observe(&bridge, &readback));
    assert(bs_dex_horizon_java_resources_shutdown(&resources));
    bs_dex_vm_destroy(vm);
    assert(bs_dex_horizon_activity_release(&activity));
    assert(bs_dex_horizon_android_attachment_release(&attachment));
    assert(bs_dex_horizon_main_looper_shutdown(&looper));
    assert(bs_dex_horizon_context_objects_release(&files));
    assert(bs_dex_horizon_java_runtime_shutdown(&runtime));
    bs_dex_quest_package_close(&package);
    assert(bs_dex_android_surface_host_reset(&surface));
    bs_unity_apk_free_original_dex(&dex);
    assert(f.opens == f.closes);
    puts("PASS: original DEX Context native data/assets, strict launch receipt and Java attachment gates");
    return 0;
}
