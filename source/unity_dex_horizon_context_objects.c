#include "unity_dex_horizon_context_objects.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#if defined(_WIN32)
#define BS_HCTX_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#else
#define BS_HCTX_ISDIR(mode) S_ISDIR(mode)
#endif

static int exists_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && BS_HCTX_ISDIR(st.st_mode);
}

static int acceptable_absolute_path(const char *path) {
    if (!path || !*path || strstr(path, "/../") || strstr(path, "\\..\\") ||
        strstr(path, "/./") || strstr(path, "\\.\\") ||
        strstr(path, "//")) return 0;
    size_t n = strlen(path);
    if (n < 2 || (n >= 3 && !strcmp(path + n - 3, "/..")) ||
        (n >= 3 && !strcmp(path + n - 3, "\\.."))) return 0;
    /* sdmc: is a real mounted libnx filesystem. Windows/Unix absolute paths
     * are allowed in host tests; no relative directory is accepted. */
    return !strncmp(path, "sdmc:/", 6) || path[0] == '/' ||
        (n >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                    (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
}

static int copy_existing_dir(char target[768], const char *source) {
    if (!acceptable_absolute_path(source) ||
        !exists_directory(source)) return 0;
    size_t n = strlen(source);
    if (n >= 768) return 0;
    memcpy(target, source, n + 1);
    return 1;
}

static int valid_tag(const char tag[9]) {
    if (!tag || !tag[0]) return 0;
    size_t length = 0;
    for (; length < 9 && tag[length]; ++length) {
        char c = tag[length];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return length > 0 && length < 9;
}

static int native_callbacks_backed(const BsDexHorizonContextNative *native) {
    return native && native->on_owner_thread &&
        native->native_activity_base_ready &&
        native->read_window_pixels && native->read_horizon_language;
}

static int native_ready(const BsDexHorizonContextObjects *owner) {
    return owner && owner->initialized && owner->package &&
        owner->package->initialized && owner->package->native_paths_observed &&
        native_callbacks_backed(&owner->native) &&
        owner->native.on_owner_thread(owner->native.user) == 1 &&
        owner->native.native_activity_base_ready(owner->native.user) == 1;
}

static int observe_configuration(const BsDexHorizonContextNative *native,
                                  BsDexHorizonContextConfig *out) {
    if (!native_callbacks_backed(native) || !out ||
        native->on_owner_thread(native->user) != 1 ||
        native->native_activity_base_ready(native->user) != 1) return 0;
    uint32_t width = 0, height = 0;
    char tag[9] = {0};
    if (native->read_window_pixels(native->user, &width, &height) != 1 ||
        !width || !height ||
        native->read_horizon_language(native->user, tag) != 1 ||
        !valid_tag(tag)) return 0;
    *out = (BsDexHorizonContextConfig){
        .window_width_px = width, .window_height_px = height
    };
    memcpy(out->horizon_language_tag, tag, sizeof tag);
    return 1;
}

static void init_file(BsDexHorizonContextFile *file,
                      BsDexHorizonContextObjects *owner,
                      BsDexHorizonContextFileKind kind,
                      const char *path) {
    *file = (BsDexHorizonContextFile){0};
    file->owner = owner;
    file->kind = kind;
    file->generation = owner->generation;
    size_t length = strlen(path);
    memcpy(file->absolute_path, path, length + 1);
}

int bs_dex_horizon_context_objects_init(
    BsDexHorizonContextObjects *out, const BsDexQuestPackageHost *package,
    const BsDexHorizonContextNative *native,
    const char *files_dir, const char *obb_dir,
    const char *external_files_dir) {
    if (!out || out->initialized || !package || !package->initialized ||
        !package->native_paths_observed || !native_callbacks_backed(native) ||
        !files_dir || !obb_dir || !external_files_dir) return 0;
    BsDexHorizonContextConfig config = {0};
    if (!observe_configuration(native, &config)) return 0;
    BsDexHorizonContextObjects candidate = {0};
    if (!copy_existing_dir(candidate.files.absolute_path, files_dir) ||
        !copy_existing_dir(candidate.obb.absolute_path, obb_dir) ||
        !copy_existing_dir(candidate.external_files.absolute_path,
                           external_files_dir)) return 0;
    char paths[3][768];
    memcpy(paths[0], candidate.files.absolute_path, 768);
    memcpy(paths[1], candidate.obb.absolute_path, 768);
    memcpy(paths[2], candidate.external_files.absolute_path, 768);
    if (!strcmp(paths[0], paths[1]) || !strcmp(paths[0], paths[2]) ||
        !strcmp(paths[1], paths[2])) return 0;

    /* No Java objects are created; construction is an atomic native
     * snapshot. Generation invalidates borrowed C handles on release. */
    uint64_t generation = out->generation + 1;
    if (!generation) return 0;
    *out = (BsDexHorizonContextObjects){0};
    out->native = *native;
    out->package = package;
    out->generation = generation;
    out->configuration = config;
    out->configuration.observation_number = 1;
    init_file(&out->files, out, BS_HCTX_FILES_DIR, paths[0]);
    init_file(&out->obb, out, BS_HCTX_OBB_DIR, paths[1]);
    init_file(&out->external_files, out, BS_HCTX_EXTERNAL_FILES_DIR,
              paths[2]);
    out->initialized = 1;
    return 1;
}

const BsDexHorizonContextFile *bs_dex_horizon_context_file(
    const BsDexHorizonContextObjects *owner, BsDexHorizonContextFileKind kind) {
    if (!native_ready(owner)) return NULL;
    const BsDexHorizonContextFile *file =
        kind == BS_HCTX_FILES_DIR ? &owner->files :
        kind == BS_HCTX_OBB_DIR ? &owner->obb :
        kind == BS_HCTX_EXTERNAL_FILES_DIR ? &owner->external_files : NULL;
    if (!file || file->owner != owner ||
        file->generation != owner->generation ||
        !exists_directory(file->absolute_path)) return NULL;
    return file;
}

int bs_dex_horizon_context_get_absolute_path(
    const BsDexHorizonContextFile *file, const char **path) {
    if (path) *path = NULL;
    if (!file || !path || !file->owner || !native_ready(file->owner) ||
        file->generation != file->owner->generation ||
        bs_dex_horizon_context_file(file->owner, file->kind) != file)
        return 0;
    *path = file->absolute_path;
    return 1;
}

int bs_dex_horizon_context_native_library_dir(
    const BsDexHorizonContextObjects *owner, const char **path) {
    if (path) *path = NULL;
    if (!path || !native_ready(owner) ||
        !owner->package->application.native_library_dir) return 0;
    *path = owner->package->application.native_library_dir;
    return 1;
}

int bs_dex_horizon_context_read_configuration(
    BsDexHorizonContextObjects *owner, BsDexHorizonContextConfig *snapshot) {
    if (snapshot) *snapshot = (BsDexHorizonContextConfig){0};
    if (!snapshot || !native_ready(owner)) return 0;
    BsDexHorizonContextConfig current = {0};
    if (!observe_configuration(&owner->native, &current) ||
        owner->configuration.observation_number == UINT64_MAX) return 0;
    current.observation_number =
        owner->configuration.observation_number + 1;
    owner->configuration = current;
    *snapshot = current;
    return 1;
}

static int receipt_matches(const BsDexHorizonContextObjects *owner,
                           const BsDexHorizonLaunchReceipt *receipt) {
    if (!native_ready(owner) || !receipt || !receipt->source_token ||
        !owner->native.verify_launch_receipt) return 0;
    const BsDexQuestLaunchFilter *expected =
        bs_dex_quest_package_launch_filter(owner->package);
    return expected && receipt->action && receipt->package_name &&
        receipt->activity_class &&
        !strcmp(receipt->action, expected->action) &&
        !strcmp(receipt->package_name, expected->package_name) &&
        !strcmp(receipt->activity_class, expected->activity_class) &&
        receipt->launcher_category == !!expected->launcher_category &&
        receipt->vr_category == !!expected->vr_category &&
        owner->native.verify_launch_receipt(owner->native.user, receipt) == 1;
}

int bs_dex_horizon_context_bind_launch(
    BsDexHorizonContextObjects *owner,
    const BsDexHorizonLaunchReceipt *receipt) {
    if (!owner || owner->launch_observed ||
        !receipt_matches(owner, receipt)) return 0;
    const size_t action_n = strlen(receipt->action);
    const size_t package_n = strlen(receipt->package_name);
    const size_t class_n = strlen(receipt->activity_class);
    if (action_n >= sizeof owner->launch_action ||
        package_n >= sizeof owner->launch_package_name ||
        class_n >= sizeof owner->launch_activity_class) return 0;
    memcpy(owner->launch_action, receipt->action, action_n + 1);
    memcpy(owner->launch_package_name, receipt->package_name, package_n + 1);
    memcpy(owner->launch_activity_class, receipt->activity_class, class_n + 1);
    owner->delivered_launch = *receipt;
    owner->delivered_launch.action = owner->launch_action;
    owner->delivered_launch.package_name = owner->launch_package_name;
    owner->delivered_launch.activity_class = owner->launch_activity_class;
    owner->launch_observed = 1;
    return 1;
}

int bs_dex_horizon_context_read_launch(
    const BsDexHorizonContextObjects *owner,
    BsDexHorizonLaunchReceipt *receipt) {
    if (receipt) *receipt = (BsDexHorizonLaunchReceipt){0};
    if (!receipt || !owner || !owner->launch_observed ||
        !receipt_matches(owner, &owner->delivered_launch)) return 0;
    *receipt = owner->delivered_launch;
    return 1;
}

int bs_dex_horizon_context_observe_startup(
    const BsDexHorizonContextObjects *owner,
    BsDexHorizonStartupObservation *out) {
    if (out) *out = (BsDexHorizonStartupObservation){0};
    if (!out || !native_ready(owner) || !owner->native.observe_startup)
        return 0;
    BsDexHorizonStartupObservation current = {0};
    if (owner->native.observe_startup(owner->native.user, &current) != 1 ||
        (current.loader_has_argv != 0 && current.loader_has_argv != 1) ||
        (current.loader_has_info != 0 && current.loader_has_info != 1) ||
        (current.loader_is_nso != 0 && current.loader_is_nso != 1) ||
        !native_ready(owner)) return 0;
    *out = current;
    return 1;
}

int bs_dex_horizon_context_java_attachment_available(
    const BsDexHorizonContextObjects *owner) {
    (void)owner;
    /* No Android ContextImpl/Intent/Resources/AssetManager/Looper exists. */
    return 0;
}

int bs_dex_horizon_context_objects_release(BsDexHorizonContextObjects *owner) {
    if (!owner || !owner->initialized) return 0;
    /* Borrowed native C handles must not be used after this call. There is
     * no Java JNI reference to release and no libnx hook owned here. */
    uint64_t generation = owner->generation;
    *owner = (BsDexHorizonContextObjects){0};
    owner->generation = generation;
    return 1;
}

#if defined(__SWITCH__)
static int production_main(void *user) {
    BsDexHorizonContextProduction *p = user;
    return p && p->horizon &&
        bs_dex_horizon_services_on_applet_thread(p->horizon);
}

static int production_native_activity(void *user) {
    BsDexHorizonContextProduction *p = user;
    return p && p->native_activity &&
        bs_dex_horizon_java_runtime_native_base_ready(p->native_activity);
}

static int production_window(void *user, uint32_t *width, uint32_t *height) {
    BsDexHorizonContextProduction *p = user;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!p || !p->horizon || !width || !height ||
        !production_main(user) || !production_native_activity(user))
        return 0;
    void *window = p->horizon->observed.nwindow;
    if (!bs_dex_horizon_services_valid_nwindow(p->horizon, window))
        return 0;
    u32 measured_width = 0, measured_height = 0;
    if (R_FAILED(nwindowGetDimensions((NWindow *)window,
                                      &measured_width, &measured_height)) ||
        !measured_width || !measured_height) return 0;
    *width = measured_width;
    *height = measured_height;
    return 1;
}

static int production_language(void *user, char tag[9]) {
    if (!tag) return 0;
    memset(tag, 0, 9);
    if (!production_main(user) || !production_native_activity(user) ||
        R_FAILED(setInitialize())) return 0;
    u64 language = 0;
    Result rc = setGetSystemLanguage(&language);
    setExit();
    if (R_FAILED(rc)) return 0;
    memcpy(tag, &language, sizeof language);
    return valid_tag(tag);
}

static int production_startup(
    void *user, BsDexHorizonStartupObservation *out) {
    if (out) *out = (BsDexHorizonStartupObservation){0};
    if (!out || !production_main(user) || !production_native_activity(user))
        return 0;
    BsDexHorizonStartupObservation observed = {
        .applet_type = (int)appletGetAppletType(),
        .applet_resource_user_id = appletGetAppletResourceUserId(),
        .loader_has_argv = envHasArgv() && envGetArgv() != NULL,
        .loader_has_info =
            envGetLoaderInfo() != NULL && envGetLoaderInfoSize() != 0,
        .loader_is_nso = envIsNso()
    };
    if (!production_main(user) || !production_native_activity(user))
        return 0;
    *out = observed;
    return 1;
}
#endif

BsDexHorizonContextNative bs_dex_horizon_context_production_callbacks(
    BsDexHorizonContextProduction *production) {
    BsDexHorizonContextNative callbacks = {0};
#if defined(__SWITCH__)
    if (!production || !production->horizon ||
        !production->native_activity) return callbacks;
    callbacks.user = production;
    callbacks.on_owner_thread = production_main;
    callbacks.native_activity_base_ready = production_native_activity;
    callbacks.read_window_pixels = production_window;
    callbacks.read_horizon_language = production_language;
    callbacks.observe_startup = production_startup;
    /* Startup argv and applet identity are Horizon facts. No libnx startup
     * source supplies Android Intent action/component/category or a Java
     * Activity owner; verify_launch_receipt therefore remains NULL. */
#else
    (void)production;
#endif
    return callbacks;
}
