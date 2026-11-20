#include "unity_dex_horizon_services.h"
#include <string.h>

#if defined(__SWITCH__)
#include <switch.h>
#include <sys/stat.h>
#include "config.h"
#include "asset_fs_bridge.h"
#include "jni_bridge.h"

#define BS_STAGED_FILES GAME_HOME "/files"
#define BS_STAGED_OBB GAME_HOME "/obb"
#define BS_STAGED_EXTERNAL GAME_HOME "/external-files"

static int actual_thread_id(uint64_t *out) {
    u64 current = 0;
    if (!out || R_FAILED(svcGetThreadId(&current, CUR_THREAD_HANDLE))) return 0;
    *out = (uint64_t)current;
    return 1;
}

static int actual_main_thread_id(uint64_t *out) {
    if (!out) return 0;
    Handle main_thread = envGetMainThreadHandle();
    u64 id = 0;
    if (!main_thread || R_FAILED(svcGetThreadId(&id, main_thread))) return 0;
    *out = (uint64_t)id;
    return 1;
}

static int observed_directory(const char *path) {
    struct stat statbuf;
    return path && stat(path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode);
}

static int observe_original_boot_config(void *native_manager) {
    if (!native_manager) return 0;
    /* Exactly the original Quest boot.config through the NDK asset map;
     * checking bytes proves actual staged storage was readable. */
    void *asset = bs_android_assetmanager_open(
        native_manager, "bin/Data/boot.config", 0);
    if (!asset) return 0;
    int64_t len = bs_android_asset_length64(asset);
    unsigned char first = 0;
    int nread = bs_android_asset_read(asset, &first, sizeof first);
    bs_android_asset_close(asset);
    return len > 0 && nread == 1;
}
#endif

int bs_dex_horizon_services_observe(BsDexHorizonServices *services) {
    if (!services) return 0;
    memset(services, 0, sizeof *services);
#if defined(__SWITCH__)
    BsDexHorizonNativeObservations *r = &services->observed;
    uint64_t thread_id = 0, main_thread_id = 0;
    /* A runnable applet is a necessary native UI host, but cannot establish
     * any Java Thread/Looper or Android Activity ownership by itself. */
    if (appletGetAppletType() != AppletType_Application ||
        !actual_thread_id(&thread_id) ||
        !actual_main_thread_id(&main_thread_id) ||
        thread_id != main_thread_id) return 0;
    r->applet_thread_id = thread_id;
    r->applet_main_thread_observed = 1;
    NWindow *window = nwindowGetDefault();
    if (window && nwindowIsValid(window)) {
        u32 width = 0, height = 0;
        if (R_SUCCEEDED(nwindowGetDimensions(window, &width, &height)) &&
            width > 0 && height > 0) {
            r->nwindow = window;
            r->width_px = width;
            r->height_px = height;
            r->native_window_valid = 1;
        }
    }
    void *env = bs_jni_env();
    if (env) {
        r->jni_thread_attached = 1;
        /* The JNI bridge's AssetManager jobject is synthetic Java metadata,
         * while the resulting AAssetManager* and staged file reads are real
         * native services. Record this distinction explicitly. */
        void *java_token = bs_jni_asset_manager();
        if (java_token) {
            void *manager = bs_android_assetmanager_from_java(env, java_token);
            if (manager && observe_original_boot_config(manager)) {
                r->jni_asset_manager = java_token;
                r->native_asset_manager = manager;
                r->native_asset_opened = 1;
            }
        }
    }
    r->files_dir_exists = observed_directory(BS_STAGED_FILES);
    r->obb_dir_exists = observed_directory(BS_STAGED_OBB);
    r->external_files_dir_exists = observed_directory(BS_STAGED_EXTERNAL);
    return bs_dex_horizon_services_native_ready(services);
#else
    return 0;
#endif
}

int bs_dex_horizon_services_on_applet_thread(const BsDexHorizonServices *services) {
    if (!services || !services->observed.applet_main_thread_observed) return 0;
#if defined(__SWITCH__)
    uint64_t current_id = 0, main_thread_id = 0;
    return appletGetAppletType() == AppletType_Application &&
           actual_thread_id(&current_id) &&
           actual_main_thread_id(&main_thread_id) &&
           current_id == main_thread_id &&
           current_id == services->observed.applet_thread_id;
#else
    return 0;
#endif
}

int bs_dex_horizon_services_valid_nwindow(const BsDexHorizonServices *services,
                                          void *window) {
    if (!services || !window || !services->observed.native_window_valid ||
        window != services->observed.nwindow ||
        !bs_dex_horizon_services_on_applet_thread(services)) return 0;
#if defined(__SWITCH__)
    if (nwindowGetDefault() != (NWindow *)window ||
        !nwindowIsValid((NWindow *)window)) return 0;
    u32 width = 0, height = 0;
    return R_SUCCEEDED(nwindowGetDimensions((NWindow *)window, &width,
                                            &height)) &&
        width > 0 && height > 0 &&
        width == services->observed.width_px &&
        height == services->observed.height_px;
#else
    return 0;
#endif
}

int bs_dex_horizon_services_observe_surface(BsDexHorizonServices *services,
                                            void *jni_surface) {
    if (!services) return 0;
    services->observed.observed_jni_surface = NULL;
    services->observed.native_jni_surface_verified = 0;
#if defined(__SWITCH__)
    if (!jni_surface || !bs_dex_horizon_services_valid_nwindow(
                            services, services->observed.nwindow)) return 0;
    void *env = bs_jni_env();
    if (!env || !bs_jni_is_backed_surface(env, jni_surface) ||
        bs_jni_backed_surface_nwindow(env, jni_surface) !=
            services->observed.nwindow) return 0;
    services->observed.observed_jni_surface = jni_surface;
    services->observed.native_jni_surface_verified = 1;
    return 1;
#else
    (void)jni_surface;
    return 0;
#endif
}

int bs_dex_horizon_services_native_ready(const BsDexHorizonServices *services) {
    if (!services) return 0;
    const BsDexHorizonNativeObservations *r = &services->observed;
    return r->applet_main_thread_observed && r->jni_thread_attached &&
        r->native_window_valid && r->native_asset_opened &&
        r->native_asset_manager && r->jni_asset_manager &&
        r->files_dir_exists && r->obb_dir_exists &&
        r->external_files_dir_exists &&
        bs_dex_horizon_services_valid_nwindow(services, r->nwindow);
}

static int on_ui_thread(void *user) {
    return bs_dex_horizon_services_on_applet_thread(user);
}

static int valid_window(void *user, void *window) {
    return bs_dex_horizon_services_valid_nwindow(user, window);
}

static int unavailable_constructor(void *user, void *activity) {
    (void)user;
    (void)activity;
    /* A live NWindow and staged APK do not execute Activity.<init>(). */
    return 0;
}

static int unavailable_attach(void *user, void *activity, void **window,
                              BsDexAndroidContextAttachment *observed,
                              const char **exception) {
    (void)user;
    (void)activity;
    if (window) *window = NULL;
    if (observed) *observed = (BsDexAndroidContextAttachment){0};
    if (exception) *exception = NULL;
    /* No executable Activity.attach, Java Intent/Resources or Java Looper. */
    return 0;
}

static int unavailable_attachment_verification(
    void *user, void *activity, void *window,
    const BsDexAndroidContextAttachment *observed) {
    (void)user;
    (void)activity;
    (void)window;
    (void)observed;
    return 0;
}

static int unavailable_java_type(void *user, const char *descriptor,
                                 void *object) {
    (void)user;
    (void)descriptor;
    (void)object;
    /* JNI's NDK AssetManager bridge does not validate actual Java class
     * identity or provide AppInfo/Resources/Intent object getters. */
    return 0;
}

static int detach_unconstructed(void *user, void *activity, void *window) {
    (void)user;
    (void)activity;
    /* Called when a rejected constructor is rolled back. No native Java
     * Activity or Window attachment was installed by this adapter. */
    return window == NULL;
}

BsDexHorizonActivityServices bs_dex_horizon_services_callbacks(
    BsDexHorizonServices *services) {
    BsDexHorizonActivityServices callbacks = {0};
    if (!services) return callbacks;
    callbacks.user = services;
    callbacks.on_ui_thread = on_ui_thread;
    callbacks.construct_base = unavailable_constructor;
    callbacks.attach = unavailable_attach;
    callbacks.valid_nwindow = valid_window;
    callbacks.verify_attachment = unavailable_attachment_verification;
    callbacks.verify_external = unavailable_java_type;
    callbacks.detach = detach_unconstructed;
    return callbacks;
}
