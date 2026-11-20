#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "unity_dex_horizon_java_resources.h"
#include "unity_apk_dex_loader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fixture {
    int owner_thread, valid_window, asset_available;
    int nwindow, native_manager, native_looper, native_cookie;
    int hook_live, opens, closes, registry_enabled, build_fails;
    int java_resources, java_assetmanager, java_configuration, java_owner;
    int registry_built, registry_destroyed;
    uint64_t main_tid, clock;
    char root[768];
    BsDexHorizonJavaRuntime *runtime;
} Fixture;

static int main_thread(void *user) {
    return ((Fixture *)user)->owner_thread;
}
static int ready_native(void *user) {
    return main_thread(user);
}
static void *observed_window(void *user) {
    return &((Fixture *)user)->nwindow;
}
static int valid_window(void *user, void *window) {
    Fixture *f = user;
    return f->owner_thread && f->valid_window && window == &f->nwindow;
}
static int subscribe(void *user, BsDexHorizonJavaRuntime *rt,
                     void **cookie) {
    Fixture *f = user;
    if (!rt || !cookie || f->hook_live || !f->owner_thread) return 0;
    f->hook_live = 1;
    *cookie = &f->native_cookie;
    return 1;
}
static int unsubscribe(void *user, void *cookie) {
    Fixture *f = user;
    if (cookie != &f->native_cookie || !f->hook_live ||
        !f->owner_thread) return 0;
    f->hook_live = 0;
    return 1;
}
static int actual_base(void *user) {
    Fixture *f = user;
    return f->runtime &&
        bs_dex_horizon_java_runtime_native_base_ready(f->runtime);
}
static int window_pixels(void *user, uint32_t *w, uint32_t *h) {
    Fixture *f = user;
    if (!f->owner_thread || !w || !h) return 0;
    *w = 1280;
    *h = 720;
    return 1;
}
static int language(void *user, char tag[9]) {
    if (!main_thread(user) || !tag) return 0;
    memcpy(tag, "en-US", 6);
    return 1;
}
static int loop_owner(void *user, uint64_t *tid) {
    Fixture *f = user;
    if (tid) *tid = 0;
    if (!tid || !f->owner_thread) return 0;
    *tid = f->main_tid;
    return 1;
}
static int loop_acquire(void *user, void **token) {
    Fixture *f = user;
    if (!token || !f->owner_thread) return 0;
    *token = &f->native_looper;
    return 1;
}
static void loop_release(void *user, void *token) {
    assert(token == &((Fixture *)user)->native_looper);
}
static int loop_wake(void *user, void *token) {
    Fixture *f = user;
    return token == &f->native_looper && f->owner_thread;
}
static int loop_clock(void *user, uint64_t *now) {
    Fixture *f = user;
    if (!now || !f->owner_thread) return 0;
    *now = f->clock;
    return 1;
}
static int probe_window(void *user, void **window) {
    Fixture *f = user;
    if (window) *window = NULL;
    if (!window || !valid_window(user, &f->nwindow)) return 0;
    *window = &f->nwindow;
    return 1;
}
static int probe_assets(void *user, void **manager) {
    Fixture *f = user;
    if (manager) *manager = NULL;
    if (!manager || !f->owner_thread || !f->asset_available) return 0;
    *manager = &f->native_manager;
    return 1;
}
static int observe_manager(void *user, void **manager) {
    Fixture *f = user;
    return probe_assets(f, manager);
}
static void *asset_open(void *user, void *manager, const char *name) {
    Fixture *f = user;
    if (!f->owner_thread || !f->asset_available ||
        manager != &f->native_manager || !name) return NULL;
    char path[1200];
    int n = snprintf(path, sizeof path, "%s/%s", f->root, name);
    if (n <= 0 || (size_t)n >= sizeof path) return NULL;
    FILE *stream = fopen(path, "rb");
    if (stream) ++f->opens;
    return stream;
}
static int asset_read(void *user, void *asset, void *buf, size_t size) {
    (void)user;
    if (!asset || (size && !buf)) return -1;
    size_t count = fread(buf, 1, size, asset);
    return ferror(asset) ? -1 : (int)count;
}
static int64_t asset_seek(void *user, void *asset,
                          int64_t offset, int whence) {
    (void)user;
    if (!asset || offset > INT32_MAX || offset < INT32_MIN ||
        fseek(asset, (long)offset, whence)) return -1;
    long location = ftell(asset);
    return location < 0 ? -1 : (int64_t)location;
}
static int64_t asset_length(void *user, void *asset) {
    (void)user;
    if (!asset) return -1;
    long origin = ftell(asset);
    if (origin < 0 || fseek(asset, 0, SEEK_END)) return -1;
    long length = ftell(asset);
    if (fseek(asset, origin, SEEK_SET)) return -1;
    return length < 0 ? -1 : (int64_t)length;
}
static void asset_close(void *user, void *asset) {
    Fixture *f = user;
    if (asset) {
        fclose(asset);
        ++f->closes;
    }
}

static int java_build(void *user, const BsDexQuestPackageHost *package,
                      const BsDexHorizonObservedConfiguration *observed,
                      void *manager, BsDexHorizonJavaResourceObjects *out) {
    Fixture *f = user;
    if (!out || !package || !observed ||
        manager != &f->native_manager ||
        observed->width_px != 1280 || observed->height_px != 720 ||
        strcmp(observed->language_tag, "en-US") ||
        strcmp(package->manifest.package_name, "com.beatgames.beatsaber"))
        return 0;
    ++f->registry_built;
    /* This is an explicitly synthetic host TEST registry. Production must
     * replace with a real guest Java constructor/retained class registry. */
    *out = (BsDexHorizonJavaResourceObjects){
        .resources = &f->java_resources,
        .asset_manager = &f->java_assetmanager,
        .configuration = &f->java_configuration,
        .strong_owner = &f->java_owner
    };
    return !f->build_fails;
}
static int java_class(void *user, void *object, const char *descriptor) {
    Fixture *f = user;
    if (!f->registry_enabled) return 0;
    return (!strcmp(descriptor, "Landroid/content/res/Resources;") &&
            object == &f->java_resources) ||
        (!strcmp(descriptor, "Landroid/content/res/AssetManager;") &&
            object == &f->java_assetmanager) ||
        (!strcmp(descriptor, "Landroid/content/res/Configuration;") &&
            object == &f->java_configuration);
}
static int java_link(void *user,
                     const BsDexHorizonJavaResourceObjects *objects,
                     void *manager,
                     const BsDexHorizonObservedConfiguration *config) {
    Fixture *f = user;
    return f->registry_enabled && objects &&
        objects->strong_owner == &f->java_owner &&
        objects->resources == &f->java_resources &&
        objects->asset_manager == &f->java_assetmanager &&
        objects->configuration == &f->java_configuration &&
        manager == &f->native_manager &&
        config && config->width_px == 1280 &&
        config->height_px == 720 &&
        !strcmp(config->language_tag, "en-US");
}
static void java_destroy(void *user,
                         BsDexHorizonJavaResourceObjects *objects) {
    Fixture *f = user;
    assert(objects && objects->strong_owner == &f->java_owner);
    ++f->registry_destroyed;
    *objects = (BsDexHorizonJavaResourceObjects){0};
}
static int load_game(void *user) { (void)user; return 1; }

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
    BsDexStop stopped = bs_dex_vm_run(vm, 20000);
    assert(stopped.steps > 62 &&
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
    BsDexHorizonAttachmentNative attachment_services = {
        .user = &f, .window = probe_window,
        .asset_manager = probe_assets
    };
    BsDexHorizonAndroidAttachment attachment = {0};
    assert(bs_dex_horizon_android_attachment_prepare(
        &attachment, &package, &files, &runtime, &looper,
        &attachment_services));

    BsDexHorizonJavaResourcesNative native = {
        .user = &f, .observed_asset_manager = observe_manager,
        .open_asset = asset_open, .read_asset = asset_read,
        .seek_asset = asset_seek, .length_asset = asset_length,
        .close_asset = asset_close
    };
    BsDexHorizonJavaResources resource = {0};
    assert(!bs_dex_horizon_java_resources_native_ready(&resource));
    assert(bs_dex_horizon_java_resources_init(
        &resource, &attachment, &files, &package, &native));
    assert(bs_dex_horizon_java_resources_native_ready(&resource));
    assert(!bs_dex_horizon_java_resources_init(
        &resource, &attachment, &files, &package, &native));
    void *manager = NULL;
    assert(bs_dex_horizon_java_resources_asset_manager(
        &resource, &manager) && manager == &f.native_manager);
    BsDexHorizonObservedConfiguration config = {0};
    assert(bs_dex_horizon_java_resources_configuration(&resource, &config));
    assert(config.width_px == 1280 && config.height_px == 720);
    assert(!strcmp(config.language_tag, "en-US"));
    assert(config.observation_number > 0);
    void *java_object = &f.java_resources;
    assert(!bs_dex_horizon_java_resources_java_get_resources(
        &resource, &java_object) && !java_object);
    assert(!bs_dex_horizon_java_resources_java_get_assets(
        &resource, &java_object) && !java_object);
    int fake_boolean = 123;
    const char *fake_string = "fabricated";
    assert(!bs_dex_horizon_java_resources_get_boolean(
        &resource, 0x7f0d0021, &fake_boolean) && fake_boolean == 0);
    assert(!bs_dex_horizon_java_resources_get_string(
        &resource, 0x7f0d0021, &fake_string) && !fake_string);

    BsDexHorizonAssetStream stream = {0};
    uint64_t length = 111;
    assert(!bs_dex_horizon_java_resources_open(
        &resource, "../AndroidManifest.xml", &stream, &length));
    assert(!bs_dex_horizon_java_resources_open(
        &resource, "/bin/Data/boot.config", &stream, &length));
    assert(!bs_dex_horizon_java_resources_open(
        &resource, "bin\\Data\\boot.config", &stream, &length));
    assert(!bs_dex_horizon_java_resources_open(
        &resource, "bin//Data/boot.config", &stream, &length));
    assert(!bs_dex_horizon_java_resources_open(
        &resource, "missing.bin", &stream, &length));
    assert(!stream.owner && length == 0);
    assert(bs_dex_horizon_java_resources_open(
        &resource, "bin/Data/boot.config", &stream, &length));
    assert(length == 768);
    char sample[50] = {0};
    size_t count = 0;
    assert(bs_dex_horizon_java_resources_read(
        stream, sample, 22, &count));
    assert(count == 22 && !memcmp(sample, "gfx-enable-gfx-jobs=1\n", 22));
    uint64_t position = 0;
    assert(bs_dex_horizon_java_resources_seek(
        stream, 0, SEEK_SET, &position) && position == 0);
    assert(bs_dex_horizon_java_resources_seek(
        stream, 0, SEEK_END, &position) && position == 768);
    assert(bs_dex_horizon_java_resources_read(
        stream, sample, sizeof sample, &count) && count == 0);
    assert(bs_dex_horizon_java_resources_close(stream));
    assert(!bs_dex_horizon_java_resources_close(stream));
    BsDexHorizonAssetStream old_stream = stream;
    assert(bs_dex_horizon_java_resources_open(
        &resource, "bin/Data/boot.config", &stream, &length));
    assert(!bs_dex_horizon_java_resources_read(
        old_stream, sample, 5, &count));
    BsDexHorizonAssetStream filled[BS_HJR_MAX_OPEN_ASSETS - 1] = {0};
    for (size_t i = 0; i < BS_HJR_MAX_OPEN_ASSETS - 1; ++i)
        assert(bs_dex_horizon_java_resources_open(
            &resource, "bin/Data/boot.config", &filled[i], &length));
    BsDexHorizonAssetStream overflow = {0};
    assert(!bs_dex_horizon_java_resources_open(
        &resource, "bin/Data/boot.config", &overflow, &length));
    assert(!overflow.owner && length == 0);
    for (size_t i = 0; i < BS_HJR_MAX_OPEN_ASSETS - 1; ++i)
        assert(bs_dex_horizon_java_resources_close(filled[i]));
    package.manifest.version_code ^= 1u;
    assert(!bs_dex_horizon_java_resources_native_ready(&resource));
    assert(!bs_dex_horizon_java_resources_read(stream, sample, 5, &count));
    package.manifest.version_code ^= 1u;
    assert(bs_dex_horizon_java_resources_native_ready(&resource));

    BsDexHorizonJavaResourceRegistry registry = {
        .user = &f, .build = java_build, .verify_class = java_class,
        .verify_link = java_link, .destroy = java_destroy
    };
    assert(!bs_dex_horizon_java_resources_bind_java(&resource, NULL));
    f.build_fails = 1;
    assert(!bs_dex_horizon_java_resources_bind_java(&resource, &registry));
    assert(f.registry_destroyed == 1); /* partial refs rolled back */
    f.build_fails = 0;
    assert(!bs_dex_horizon_java_resources_bind_java(&resource, &registry));
    assert(f.registry_destroyed == 2); /* missing class registry witness */
    f.registry_enabled = 1; /* injected TEST class registry */
    assert(bs_dex_horizon_java_resources_bind_java(&resource, &registry));
    assert(bs_dex_horizon_java_resources_java_get_resources(
        &resource, &java_object) && java_object == &f.java_resources);
    assert(bs_dex_horizon_java_resources_java_get_assets(
        &resource, &java_object) && java_object == &f.java_assetmanager);
    assert(bs_dex_horizon_java_resources_java_get_configuration(
        &resource, &java_object) && java_object == &f.java_configuration);
    f.registry_enabled = 0;
    assert(!bs_dex_horizon_java_resources_java_get_configuration(
        &resource, &java_object) && !java_object);
    f.registry_enabled = 1;

    /* Asset revocation blocks reads but close always frees the real FILE. */
    f.asset_available = 0;
    assert(!bs_dex_horizon_java_resources_native_ready(&resource));
    assert(!bs_dex_horizon_java_resources_read(
        stream, sample, 5, &count));
    assert(!count);
    assert(bs_dex_horizon_java_resources_close(stream));
    assert(f.opens == f.closes);
    f.asset_available = 1;

    /* Shutdown closes still-open streams and destroys strong Java refs. */
    assert(bs_dex_horizon_java_resources_open(
        &resource, "bin/Data/boot.config", &stream, &length));
    assert(bs_dex_horizon_java_resources_shutdown(&resource));
    assert(f.opens == f.closes);
    assert(f.registry_destroyed == 3);
    assert(!bs_dex_horizon_java_resources_read(
        stream, sample, sizeof sample, &count));
    assert(!bs_dex_horizon_java_resources_native_ready(&resource));
    assert(bs_dex_horizon_java_resources_init(
        &resource, &attachment, &files, &package, &native));
    assert(!bs_dex_horizon_java_resources_read(
        old_stream, sample, sizeof sample, &count));
    assert(bs_dex_horizon_java_resources_shutdown(&resource));

    bs_dex_vm_destroy(vm);
    assert(bs_dex_horizon_activity_release(&activity));
    assert(bs_dex_horizon_android_attachment_release(&attachment));
    assert(bs_dex_horizon_main_looper_shutdown(&looper));
    assert(bs_dex_horizon_context_objects_release(&files));
    assert(bs_dex_horizon_java_runtime_shutdown(&runtime));
    bs_dex_quest_package_close(&package);
    assert(bs_dex_android_surface_host_reset(&surface));
    bs_unity_apk_free_original_dex(&dex);
    puts("PASS: original Quest native AssetManager stream bytes/lifetimes and strict Java Resources/Configuration registry gates");
    return 0;
}
