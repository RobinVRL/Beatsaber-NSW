#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "unity_dex_horizon_android_attachment.h"
#include <stdlib.h>
#include <string.h>

#if defined(__SWITCH__)
#include "asset_fs_bridge.h"
#endif

struct BsDexHorizonAttachmentSnapshot {
    BsApkManifest manifest;
    BsDexQuestApplicationInfo application;
    BsDexQuestActivityInfo activity;
    BsDexQuestLaunchFilter filter;
    BsDexHorizonContextConfig config;
    char apk_path[768], native_library_dir[768], data_dir[768];
    char files_dir[768], obb_dir[768], external_dir[768];
    uint32_t resources_crc;
    size_t resources_size;
    void *nwindow, *native_assets, *native_looper;
    uint64_t activity_generation, context_generation, owner_thread_id;
};

static int copy_text(char *dst, size_t capacity, const char *text) {
    if (!dst || !capacity || !text) return 0;
    size_t n = strlen(text);
    if (!n || n >= capacity) return 0;
    memcpy(dst, text, n + 1);
    return 1;
}

static int native_hooks_ready(const BsDexHorizonAttachmentNative *native) {
    return native && native->window && native->asset_manager;
}

static int manifest_matches(const BsApkManifest *a,
                            const BsApkManifest *b) {
    if (!a || !b ||
        strcmp(a->package_name, b->package_name) ||
        strcmp(a->version_name, b->version_name) ||
        strcmp(a->activity_class, b->activity_class) ||
        a->version_code != b->version_code ||
        a->min_sdk_version != b->min_sdk_version ||
        a->target_sdk_version != b->target_sdk_version ||
        a->compile_sdk_version != b->compile_sdk_version ||
        a->application_label_resource != b->application_label_resource ||
        a->activity_theme_resource != b->activity_theme_resource ||
        a->activity_config_changes != b->activity_config_changes ||
        a->activity_enabled != b->activity_enabled ||
        a->activity_exported != b->activity_exported ||
        a->launches_main_action != b->launches_main_action ||
        a->launches_launcher_category != b->launches_launcher_category ||
        a->launches_oculus_vr_category != b->launches_oculus_vr_category ||
        a->metadata_count != b->metadata_count)
        return 0;
    for (size_t i = 0; i < a->metadata_count; ++i) {
        const BsApkManifestMetadata *x = &a->metadata[i];
        const BsApkManifestMetadata *y = &b->metadata[i];
        if (strcmp(x->name, y->name) ||
            x->belongs_to_activity != y->belongs_to_activity ||
            x->value.kind != y->value.kind ||
            x->value.raw != y->value.raw ||
            strcmp(x->value.text, y->value.text))
            return 0;
    }
    return 1;
}

static int native_live(const BsDexHorizonAndroidAttachment *a) {
    if (!a || !a->snapshot || !a->runtime || !a->context ||
        !a->package || !a->looper ||
        !native_hooks_ready(&a->native) ||
        !bs_dex_horizon_java_runtime_native_base_ready(a->runtime) ||
        !bs_dex_horizon_main_looper_native_ready(a->looper) ||
        !a->runtime->verified_host || !a->package->initialized ||
        !a->package->native_paths_observed ||
        a->package != a->context->package ||
        a->snapshot->activity_generation !=
            a->runtime->verified_host->activity.generation ||
        a->snapshot->context_generation != a->context->generation ||
        a->snapshot->resources_crc != a->package->resources_crc ||
        !manifest_matches(&a->snapshot->manifest, &a->package->manifest))
        return 0;
    uint64_t owner = 0;
    if (!bs_dex_horizon_main_looper_owner_thread_id(a->looper, &owner) ||
        owner != a->snapshot->owner_thread_id ||
        a->snapshot->native_looper != a->looper->native_looper_handle)
        return 0;
    void *window = NULL, *assets = NULL;
    if (a->native.window(a->native.user, &window) != 1 ||
        a->native.asset_manager(a->native.user, &assets) != 1 ||
        !window || !assets ||
        window != a->snapshot->nwindow ||
        assets != a->snapshot->native_assets) return 0;
    const char *path = NULL;
    const BsDexHorizonContextFile *files =
        bs_dex_horizon_context_file(a->context, BS_HCTX_FILES_DIR);
    if (!files || !bs_dex_horizon_context_get_absolute_path(files, &path) ||
        strcmp(path, a->snapshot->files_dir)) return 0;
    files = bs_dex_horizon_context_file(a->context, BS_HCTX_OBB_DIR);
    if (!files || !bs_dex_horizon_context_get_absolute_path(files, &path) ||
        strcmp(path, a->snapshot->obb_dir)) return 0;
    files = bs_dex_horizon_context_file(a->context,
                                         BS_HCTX_EXTERNAL_FILES_DIR);
    if (!files || !bs_dex_horizon_context_get_absolute_path(files, &path) ||
        strcmp(path, a->snapshot->external_dir) ||
        !bs_dex_horizon_context_native_library_dir(a->context, &path) ||
        strcmp(path, a->snapshot->native_library_dir))
        return 0;
    return 1;
}

int bs_dex_horizon_android_attachment_prepare(
    BsDexHorizonAndroidAttachment *a,
    const BsDexQuestPackageHost *package,
    BsDexHorizonContextObjects *context,
    BsDexHorizonJavaRuntime *runtime,
    BsDexHorizonMainLooper *looper,
    const BsDexHorizonAttachmentNative *native) {
    if (!a || a->snapshot || a->adapter_installed || a->java_attached ||
        !package || !context || !runtime || !looper ||
        !native_hooks_ready(native) ||
        !package->initialized || !package->native_paths_observed ||
        context->package != package ||
        !bs_dex_horizon_java_runtime_native_base_ready(runtime) ||
        !bs_dex_horizon_main_looper_native_ready(looper) ||
        !runtime->verified_host ||
        !runtime->verified_host->original_classes_verified ||
        !runtime->verified_host->activity.alive ||
        !runtime->verified_host->activity.constructed ||
        runtime->verified_host->activity.attached ||
        a->generation == UINT64_MAX ||
        package->resources_crc != BS_QUEST_RESOURCES_ARSC_CRC)
        return 0;
    BsDexHorizonAttachmentSnapshot *snapshot =
        calloc(1, sizeof *snapshot);
    if (!snapshot) return 0;
    int success = 0;
    do {
        snapshot->manifest = package->manifest; /* owned full typed copy */
        snapshot->application = package->application;
        snapshot->activity = package->activity;
        snapshot->filter = package->launch_filter;
        snapshot->resources_size = BS_QUEST_RESOURCES_ARSC_SIZE;
        snapshot->resources_crc = package->resources_crc;
        if (!copy_text(snapshot->apk_path, sizeof snapshot->apk_path,
                       package->apk_path) ||
            !copy_text(snapshot->native_library_dir,
                       sizeof snapshot->native_library_dir,
                       package->native_library_dir) ||
            !copy_text(snapshot->data_dir, sizeof snapshot->data_dir,
                       package->data_dir)) break;
        snapshot->application.package_name =
            snapshot->manifest.package_name;
        snapshot->application.source_dir = snapshot->apk_path;
        snapshot->application.public_source_dir = snapshot->apk_path;
        snapshot->application.native_library_dir =
            snapshot->native_library_dir;
        snapshot->application.data_dir = snapshot->data_dir;
        snapshot->activity.name = snapshot->manifest.activity_class;
        snapshot->activity.package_name = snapshot->manifest.package_name;
        snapshot->filter.package_name = snapshot->manifest.package_name;
        snapshot->filter.activity_class = snapshot->manifest.activity_class;
        const BsDexHorizonContextFile *file =
            bs_dex_horizon_context_file(context, BS_HCTX_FILES_DIR);
        const char *path = NULL;
        if (!file || !bs_dex_horizon_context_get_absolute_path(file, &path) ||
            !copy_text(snapshot->files_dir, sizeof snapshot->files_dir, path))
            break;
        file = bs_dex_horizon_context_file(context, BS_HCTX_OBB_DIR);
        if (!file || !bs_dex_horizon_context_get_absolute_path(file, &path) ||
            !copy_text(snapshot->obb_dir, sizeof snapshot->obb_dir, path))
            break;
        file = bs_dex_horizon_context_file(context,
                                           BS_HCTX_EXTERNAL_FILES_DIR);
        if (!file || !bs_dex_horizon_context_get_absolute_path(file, &path) ||
            !copy_text(snapshot->external_dir,
                       sizeof snapshot->external_dir, path))
            break;
        if (!bs_dex_horizon_context_read_configuration(context,
                                                       &snapshot->config) ||
            !bs_dex_horizon_main_looper_owner_thread_id(
                looper, &snapshot->owner_thread_id))
            break;
        if (native->window(native->user, &snapshot->nwindow) != 1 ||
            native->asset_manager(native->user,
                                  &snapshot->native_assets) != 1 ||
            !snapshot->nwindow || !snapshot->native_assets ||
            snapshot->nwindow != runtime->native_window ||
            !looper->native_looper_handle) break;
        snapshot->native_looper = looper->native_looper_handle;
        snapshot->activity_generation =
            runtime->verified_host->activity.generation;
        snapshot->context_generation = context->generation;
        success = 1;
    } while (0);
    if (!success) {
        free(snapshot);
        return 0;
    }
    a->package = package;
    a->context = context;
    a->runtime = runtime;
    a->looper = looper;
    a->native = *native;
    a->snapshot = snapshot;
    ++a->generation;
    if (!native_live(a)) {
        free(snapshot);
        a->snapshot = NULL;
        a->package = NULL;
        a->context = NULL;
        a->runtime = NULL;
        a->looper = NULL;
        a->native = (BsDexHorizonAttachmentNative){0};
        return 0;
    }
    return 1;
}

static int ref_live(BsDexHorizonAttachmentRef ref,
                    BsDexHorizonAttachmentKind expected) {
    return ref.owner && ref.generation == ref.owner->generation &&
        ref.kind == expected && native_live(ref.owner);
}

int bs_dex_horizon_android_attachment_ref(
    const BsDexHorizonAndroidAttachment *a,
    BsDexHorizonAttachmentKind kind, BsDexHorizonAttachmentRef *out) {
    if (out) *out = (BsDexHorizonAttachmentRef){0};
    if (!out || kind <= 0 || kind >= BS_HA_OBJECT_COUNT ||
        !native_live(a)) return 0;
    if (kind == BS_HA_LAUNCH_RECEIPT) {
        BsDexHorizonLaunchReceipt delivered = {0};
        if (!bs_dex_horizon_context_read_launch(a->context, &delivered))
            return 0;
    }
    *out = (BsDexHorizonAttachmentRef){
        .owner = a, .generation = a->generation, .kind = kind
    };
    return 1;
}

int bs_dex_horizon_android_attachment_application(
    BsDexHorizonAttachmentRef ref, const BsDexQuestApplicationInfo **value) {
    if (value) *value = NULL;
    if (!value || !ref_live(ref, BS_HA_APPLICATION_INFO)) return 0;
    *value = &ref.owner->snapshot->application;
    return 1;
}

int bs_dex_horizon_android_attachment_activity(
    BsDexHorizonAttachmentRef ref, const BsDexQuestActivityInfo **value) {
    if (value) *value = NULL;
    if (!value || !ref_live(ref, BS_HA_ACTIVITY_INFO)) return 0;
    *value = &ref.owner->snapshot->activity;
    return 1;
}

int bs_dex_horizon_android_attachment_launch_filter(
    BsDexHorizonAttachmentRef ref, const BsDexQuestLaunchFilter **value) {
    if (value) *value = NULL;
    if (!value || !ref_live(ref, BS_HA_LAUNCH_FILTER)) return 0;
    *value = &ref.owner->snapshot->filter;
    return 1;
}

int bs_dex_horizon_android_attachment_metadata(
    BsDexHorizonAttachmentRef ref, const char *key,
    BsApkManifestValue *value) {
    if (value) *value = (BsApkManifestValue){0};
    if (!value || !key ||
        (!ref_live(ref, BS_HA_APPLICATION_METADATA) &&
         !ref_live(ref, BS_HA_ACTIVITY_METADATA))) return 0;
    const BsApkManifestMetadata *entry = bs_apk_manifest_metadata(
        &ref.owner->snapshot->manifest, key,
        ref.kind == BS_HA_ACTIVITY_METADATA);
    if (!entry) return 0;
    *value = entry->value;
    return 1;
}

int bs_dex_horizon_android_attachment_path(
    BsDexHorizonAttachmentRef ref, const char **path) {
    if (path) *path = NULL;
    if (!path || !ref.owner || !native_live(ref.owner) ||
        ref.generation != ref.owner->generation) return 0;
    const BsDexHorizonAttachmentSnapshot *s = ref.owner->snapshot;
    switch (ref.kind) {
    case BS_HA_FILES_DIR: *path = s->files_dir; break;
    case BS_HA_OBB_DIR: *path = s->obb_dir; break;
    case BS_HA_EXTERNAL_FILES_DIR: *path = s->external_dir; break;
    case BS_HA_NATIVE_LIBRARY_DIR: *path = s->native_library_dir; break;
    default: return 0;
    }
    return 1;
}

int bs_dex_horizon_android_attachment_configuration(
    BsDexHorizonAttachmentRef ref, BsDexHorizonContextConfig *value) {
    if (value) *value = (BsDexHorizonContextConfig){0};
    if (!value || !ref_live(ref, BS_HA_CONFIGURATION) ||
        !bs_dex_horizon_context_read_configuration(
            ref.owner->context, value)) return 0;
    return 1;
}

int bs_dex_horizon_android_attachment_resource_table(
    BsDexHorizonAttachmentRef ref, size_t *size, uint32_t *crc) {
    if (size) *size = 0;
    if (crc) *crc = 0;
    if (!size || !crc || !ref_live(ref, BS_HA_RESOURCE_TABLE)) return 0;
    *size = ref.owner->snapshot->resources_size;
    *crc = ref.owner->snapshot->resources_crc;
    return 1;
}

int bs_dex_horizon_android_attachment_native_handle(
    BsDexHorizonAttachmentRef ref, void **handle) {
    if (handle) *handle = NULL;
    if (!handle || !ref.owner || !native_live(ref.owner) ||
        ref.generation != ref.owner->generation) return 0;
    switch (ref.kind) {
    case BS_HA_NATIVE_WINDOW:
        *handle = ref.owner->snapshot->nwindow; break;
    case BS_HA_NATIVE_ASSET_MANAGER:
        *handle = ref.owner->snapshot->native_assets; break;
    case BS_HA_NATIVE_LOOPER:
        *handle = ref.owner->snapshot->native_looper; break;
    default: return 0;
    }
    return 1;
}

int bs_dex_horizon_android_attachment_launch(
    BsDexHorizonAttachmentRef ref, BsDexHorizonLaunchReceipt *delivered) {
    if (delivered) *delivered = (BsDexHorizonLaunchReceipt){0};
    if (!delivered || !ref_live(ref, BS_HA_LAUNCH_RECEIPT)) return 0;
    return bs_dex_horizon_context_read_launch(ref.owner->context, delivered);
}

static int java_callbacks_ready(const BsDexHorizonAttachmentJava *java) {
    const BsDexQuestJavaWitness *p = java ? &java->package_witness : NULL;
    return java && java->verify_main_java_loop && java->execute_attach &&
        java->verify_type && java->verify_attachment && java->detach &&
        p && p->verify_class && p->verify_app_info &&
        p->verify_activity_info && p->verify_launch_intent &&
        p->verify_package_manager && p->verify_bundle &&
        p->verify_string_value;
}

int bs_dex_horizon_android_attachment_set_java(
    BsDexHorizonAndroidAttachment *a,
    const BsDexHorizonAttachmentJava *java) {
    if (!a || !a->snapshot || a->java_attached || a->java_verified ||
        !java_callbacks_ready(java) || !native_live(a)) return 0;
    a->java = *java;
    return 1;
}

static int actual_java_type(BsDexHorizonAndroidAttachment *a,
                            const char *descriptor, void *object) {
    return a && a->java_verified && java_callbacks_ready(&a->java) &&
        native_live(a) && object &&
        a->java.verify_type(a->java.user, descriptor, object) == 1;
}

static int verify_all_readback(BsDexHorizonAndroidAttachment *a,
                               void *activity, void *window,
                               void *token,
                               const BsDexQuestJavaOwners *owners,
                               const BsDexAndroidContextAttachment *actual) {
    if (!a || !activity || !window || !token || !actual || !owners ||
        !native_live(a) ||
        !bs_dex_horizon_main_looper_java_ready(a->looper) ||
        actual->activity != activity ||
        window != a->snapshot->nwindow ||
        !actual->intent || !actual->application_info ||
        !actual->resources || !actual->assets ||
        !actual->package_manager || !actual->metadata_bundle ||
        !actual->native_library_dir || !actual->files_dir ||
        !actual->obb_dir || !actual->metadata_field_observed)
        return 0;
    BsDexAndroidContextAttachment staged = {
        .activity = activity,
        .resources = actual->resources,
        .assets = actual->assets,
        .files_dir = actual->files_dir,
        .obb_dir = actual->obb_dir
    };
    if (!bs_dex_quest_package_stage_context(
            a->package, owners, &a->java.package_witness, &staged) ||
        staged.intent != actual->intent ||
        staged.application_info != actual->application_info ||
        staged.package_manager != actual->package_manager ||
        staged.metadata_bundle != actual->metadata_bundle ||
        staged.native_library_dir != actual->native_library_dir ||
        a->java.verify_type(a->java.user,
            "Lcom/unity3d/player/UnityPlayerGameActivity;", activity) != 1 ||
        a->java.verify_type(a->java.user,
            "Landroid/content/res/Resources;", actual->resources) != 1 ||
        a->java.verify_type(a->java.user,
            "Landroid/content/res/AssetManager;", actual->assets) != 1 ||
        a->java.verify_type(a->java.user,
            "Ljava/io/File;", actual->files_dir) != 1 ||
        a->java.verify_type(a->java.user,
            "Ljava/io/File;", actual->obb_dir) != 1 ||
        a->java.verify_attachment(a->java.user, activity, window, token,
                                   actual) != 1)
        return 0;
    return 1;
}

static int wrapper_ui(void *user) {
    BsDexHorizonAndroidAttachment *a = user;
    return a && a->previous.on_ui_thread &&
        a->previous.on_ui_thread(a->previous.user) == 1;
}

static int wrapper_window(void *user, void *window) {
    BsDexHorizonAndroidAttachment *a = user;
    return a && a->previous.valid_nwindow &&
        a->previous.valid_nwindow(a->previous.user, window) == 1;
}

static int wrapper_construct(void *user, void *activity) {
    BsDexHorizonAndroidAttachment *a = user;
    return a && a->previous.construct_base &&
        a->previous.construct_base(a->previous.user, activity) == 1;
}

static int wrapper_attach(void *user, void *activity, void **out_window,
                          BsDexAndroidContextAttachment *out,
                          const char **exception) {
    BsDexHorizonAndroidAttachment *a = user;
    if (out_window) *out_window = NULL;
    if (out) *out = (BsDexAndroidContextAttachment){0};
    if (exception) *exception = NULL;
    if (!a || !out_window || !out || !exception ||
        !a->adapter_installed || a->java_attached ||
        !a->activity_host || activity != &a->activity_host->activity ||
        !native_live(a) || !wrapper_ui(a) ||
        !bs_dex_horizon_main_looper_java_ready(a->looper) ||
        !java_callbacks_ready(&a->java) ||
        a->java.verify_main_java_loop(a->java.user, activity,
                                      a->looper) != 1)
        return 0;
    BsDexHorizonLaunchReceipt actual_launch = {0};
    if (!bs_dex_horizon_context_read_launch(a->context, &actual_launch))
        return 0;
    void *native_window = a->snapshot->nwindow;
    if (!wrapper_window(a, native_window)) return 0;
    BsDexQuestJavaOwners package_java = {0};
    BsDexAndroidContextAttachment observed = {0};
    void *token = NULL;
    const char *raised = NULL;
    int executed = a->java.execute_attach(
        a->java.user, activity, native_window, a, &actual_launch,
        &package_java, &observed, &token, &raised);
    int verified = executed == 1 && !raised &&
        verify_all_readback(a, activity, native_window,
                             token, &package_java, &observed);
    if (!verified) {
        if (token && a->java.detach(a->java.user, activity,
                                    native_window, token) != 1) {
            /* Keep the actual outstanding Java side effect for retry by
             * ActivityHost.release(); never publish it as verified. */
            a->attached_token = token;
            a->attached_window = native_window;
            a->java_attached = 1;
            a->java_verified = 0;
        }
        if (executed < 0 && raised) *exception = raised;
        return executed < 0 && raised ? -1 : 0;
    }
    a->attached_token = token;
    a->attached_window = native_window;
    a->actual_readback = observed;
    a->java_attached = 1;
    a->java_verified = 1;
    *out_window = native_window;
    *out = observed;
    return 1;
}

static int wrapper_verify_external(void *user, const char *descriptor,
                                   void *object) {
    return actual_java_type(user, descriptor, object);
}

static int wrapper_verify_attached(
    void *user, void *activity, void *window,
    const BsDexAndroidContextAttachment *observed) {
    BsDexHorizonAndroidAttachment *a = user;
    if (!a || !observed || !a->java_verified || !a->java_attached ||
        !a->attached_token || a->attached_window != window ||
        !native_live(a) || !wrapper_window(a, window) ||
        activity != &a->activity_host->activity ||
        observed->activity != a->actual_readback.activity ||
        observed->intent != a->actual_readback.intent ||
        observed->application_info != a->actual_readback.application_info ||
        observed->resources != a->actual_readback.resources ||
        observed->assets != a->actual_readback.assets ||
        observed->package_manager != a->actual_readback.package_manager ||
        observed->metadata_bundle != a->actual_readback.metadata_bundle ||
        observed->metadata_field_observed !=
            a->actual_readback.metadata_field_observed ||
        observed->files_dir != a->actual_readback.files_dir ||
        observed->obb_dir != a->actual_readback.obb_dir ||
        observed->native_library_dir !=
            a->actual_readback.native_library_dir)
        return 0;
    return a->java.verify_attachment(
        a->java.user, activity, window, a->attached_token, observed) == 1;
}

static int wrapper_detach(void *user, void *activity, void *window) {
    BsDexHorizonAndroidAttachment *a = user;
    if (!a || !a->adapter_installed || !a->activity_host ||
        activity != &a->activity_host->activity || !wrapper_ui(a))
        return 0;
    if (a->java_attached) {
        if (!java_callbacks_ready(&a->java) ||
            (window && a->attached_window != window) ||
            a->java.detach(a->java.user, activity, a->attached_window,
                            a->attached_token) != 1)
            return 0;
        a->java_attached = 0;
        a->java_verified = 0;
        a->attached_token = NULL;
        a->attached_window = NULL;
        a->actual_readback = (BsDexAndroidContextAttachment){0};
    } else if (window) {
        return 0; /* An unowned Java Window must never be silently ignored. */
    }
    /* Native Java base owns the genuine appletHook, not a Java Window. The
     * true attached Window was torn down by the Java backend above. */
    return a->previous.detach &&
        a->previous.detach(a->previous.user, activity, NULL) == 1;
}

int bs_dex_horizon_android_attachment_install(
    BsDexHorizonAndroidAttachment *a,
    BsDexHorizonActivityHost *verified_host) {
    if (!a || !verified_host || a->adapter_installed || !native_live(a) ||
        verified_host != a->runtime->verified_host ||
        !verified_host->original_classes_verified ||
        verified_host->activity.attached ||
        !verified_host->activity.constructed ||
        !verified_host->activity.alive) return 0;
    const BsDexHorizonActivityServices expected =
        bs_dex_horizon_java_runtime_callbacks(a->runtime);
    const BsDexHorizonActivityServices old = verified_host->platform;
    if (old.user != expected.user ||
        old.on_ui_thread != expected.on_ui_thread ||
        old.construct_base != expected.construct_base ||
        old.attach != expected.attach ||
        old.valid_nwindow != expected.valid_nwindow ||
        old.verify_external != expected.verify_external ||
        old.verify_attachment != expected.verify_attachment ||
        old.detach != expected.detach) return 0;
    a->previous = old;
    a->activity_host = verified_host;
    BsDexHorizonActivityServices replacement = {
        .user = a,
        .on_ui_thread = wrapper_ui,
        .construct_base = wrapper_construct,
        .attach = wrapper_attach,
        .valid_nwindow = wrapper_window,
        .verify_attachment = wrapper_verify_attached,
        .verify_external = wrapper_verify_external,
        .detach = wrapper_detach
    };
    verified_host->platform = replacement;
    a->adapter_installed = 1;
    return 1;
}

int bs_dex_horizon_android_attachment_java_attached(
    const BsDexHorizonAndroidAttachment *a) {
    return a && a->java_attached && a->java_verified &&
        a->activity_host && a->activity_host->activity.attached &&
        wrapper_verify_attached((void *)a,
            &a->activity_host->activity, a->attached_window,
            &a->actual_readback);
}

int bs_dex_horizon_android_attachment_release(
    BsDexHorizonAndroidAttachment *a) {
    if (!a || !a->snapshot || a->java_attached ||
        (a->adapter_installed && a->activity_host &&
         (a->activity_host->activity.constructed ||
          a->activity_host->activity.alive ||
          a->activity_host->activity.attached))) return 0;
    if (a->adapter_installed) a->activity_host->platform = a->previous;
    uint64_t generation = a->generation;
    free(a->snapshot);
    *a = (BsDexHorizonAndroidAttachment){0};
    a->generation = generation;
    return 1;
}

#if defined(__SWITCH__)
static int production_window(void *user, void **window) {
    BsDexHorizonServices *h = user;
    if (window) *window = NULL;
    if (!h || !window || !h->observed.nwindow ||
        !bs_dex_horizon_services_valid_nwindow(
            h, h->observed.nwindow)) return 0;
    *window = h->observed.nwindow;
    return 1;
}

static int production_assets(void *user, void **manager) {
    BsDexHorizonServices *h = user;
    if (manager) *manager = NULL;
    if (!h || !manager || !h->observed.native_asset_manager ||
        !h->observed.native_asset_opened ||
        !bs_dex_horizon_services_native_ready(h)) return 0;
    void *asset = bs_android_assetmanager_open(
        h->observed.native_asset_manager, "bin/Data/boot.config", 0);
    if (!asset) return 0;
    uint8_t first = 0;
    int read = bs_android_asset_read(asset, &first, sizeof first);
    int64_t length = bs_android_asset_length64(asset);
    bs_android_asset_close(asset);
    if (length <= 0 || read != 1) return 0;
    *manager = h->observed.native_asset_manager;
    return 1;
}
#endif

BsDexHorizonAttachmentNative
bs_dex_horizon_android_attachment_production_native(
    BsDexHorizonServices *horizon) {
    BsDexHorizonAttachmentNative native = {0};
#if defined(__SWITCH__)
    if (horizon) {
        native.user = horizon;
        native.window = production_window;
        native.asset_manager = production_assets;
    }
#else
    (void)horizon;
#endif
    return native;
}
