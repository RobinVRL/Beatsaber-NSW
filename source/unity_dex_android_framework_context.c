#include "unity_dex_android_framework_context.h"
#include <string.h>

static int ref_for(const BsDexAndroidFrameworkContext *owner,
                   BsDexHorizonAttachmentKind kind,
                   BsDexHorizonAttachmentRef *ref) {
    return owner && owner->initialized && owner->attachment &&
        owner->attachment->context && owner->resources &&
        owner->attachment_generation &&
        owner->attachment_generation == owner->attachment->generation &&
        owner->resources_generation == owner->resources->generation &&
        owner->context_generation == owner->attachment->context->generation &&
        owner->resources->initialized &&
        owner->resources->attachment == owner->attachment &&
        owner->resources->context == owner->attachment->context &&
        owner->resources->package == owner->attachment->package &&
        bs_dex_horizon_java_resources_native_ready(owner->resources) == 1 &&
        bs_dex_horizon_android_attachment_ref(owner->attachment,
                                               kind, ref) == 1;
}

static int native_observe(const BsDexAndroidFrameworkContext *owner,
                          BsDexFrameworkNativeContext *out) {
    BsDexHorizonAttachmentRef ref = {0};
    BsDexFrameworkNativeContext model = {0};
    BsDexHorizonContextConfig context_config = {0};
    if (!ref_for(owner, BS_HA_APPLICATION_INFO, &ref) ||
        !bs_dex_horizon_android_attachment_application(ref,
                                                       &model.application) ||
        !ref_for(owner, BS_HA_ACTIVITY_INFO, &ref) ||
        !bs_dex_horizon_android_attachment_activity(ref, &model.activity) ||
        !ref_for(owner, BS_HA_FILES_DIR, &ref) ||
        !bs_dex_horizon_android_attachment_path(ref, &model.files_dir) ||
        !ref_for(owner, BS_HA_OBB_DIR, &ref) ||
        !bs_dex_horizon_android_attachment_path(ref, &model.obb_dir) ||
        !ref_for(owner, BS_HA_EXTERNAL_FILES_DIR, &ref) ||
        !bs_dex_horizon_android_attachment_path(ref,
                                                &model.external_files_dir) ||
        !ref_for(owner, BS_HA_NATIVE_LIBRARY_DIR, &ref) ||
        !bs_dex_horizon_android_attachment_path(ref,
                                                &model.native_library_dir) ||
        !ref_for(owner, BS_HA_NATIVE_WINDOW, &ref) ||
        !bs_dex_horizon_android_attachment_native_handle(ref,
                                                          &model.nwindow) ||
        !ref_for(owner, BS_HA_NATIVE_ASSET_MANAGER, &ref) ||
        !bs_dex_horizon_android_attachment_native_handle(ref,
                                                &model.asset_manager) ||
        !ref_for(owner, BS_HA_NATIVE_LOOPER, &ref) ||
        !bs_dex_horizon_android_attachment_native_handle(ref,
                                                          &model.alooper) ||
        !ref_for(owner, BS_HA_CONFIGURATION, &ref) ||
        !bs_dex_horizon_android_attachment_configuration(
            ref, &context_config) ||
        !bs_dex_horizon_java_resources_configuration(
            owner->resources, &model.configuration) ||
        !bs_dex_horizon_main_looper_owner_thread_id(
            owner->attachment->looper, &model.owner_thread_id)) return 0;
    void *observed_manager = NULL;
    if (!bs_dex_horizon_java_resources_asset_manager(
             owner->resources, &observed_manager) ||
        !model.application || !model.activity ||
        !model.files_dir || !model.obb_dir ||
        !model.external_files_dir || !model.native_library_dir ||
        !model.asset_manager || !model.nwindow || !model.alooper ||
        !model.owner_thread_id ||
        model.asset_manager != observed_manager ||
        model.configuration.width_px != context_config.window_width_px ||
        model.configuration.height_px != context_config.window_height_px ||
        strcmp(model.configuration.language_tag,
               context_config.horizon_language_tag) ||
        strcmp(model.application->package_name,
               owner->resources->package->manifest.package_name) ||
        strcmp(model.activity->name,
               owner->resources->package->manifest.activity_class) ||
        strcmp(model.native_library_dir,
               owner->resources->package->native_library_dir)) return 0;
    *out = model;
    return 1;
}

int bs_dex_android_framework_context_init(
    BsDexAndroidFrameworkContext *owner,
    BsDexHorizonAndroidAttachment *attachment,
    BsDexHorizonJavaResources *resources) {
    if (!owner || owner->initialized || !attachment || !resources ||
        !attachment->context || !resources->initialized ||
        resources->attachment != attachment ||
        resources->context != attachment->context ||
        resources->package != attachment->package ||
        !attachment->generation || !resources->generation ||
        !attachment->context->generation) return 0;
    BsDexAndroidFrameworkContext candidate = {
        .attachment = attachment,
        .resources = resources,
        .attachment_generation = attachment->generation,
        .resources_generation = resources->generation,
        .context_generation = attachment->context->generation,
        .initialized = 1
    };
    BsDexFrameworkNativeContext native = {0};
    if (!native_observe(&candidate, &native)) return 0;
    *owner = candidate;
    return 1;
}

int bs_dex_android_framework_context_observe(
    const BsDexAndroidFrameworkContext *owner,
    BsDexFrameworkNativeContext *out) {
    if (out) *out = (BsDexFrameworkNativeContext){0};
    return out && native_observe(owner, out);
}

int bs_dex_android_framework_context_metadata(
    const BsDexAndroidFrameworkContext *owner,
    const char *name, int activity_scope, BsApkManifestValue *value) {
    if (value) *value = (BsApkManifestValue){0};
    BsDexHorizonAttachmentRef ref = {0};
    if (!value || !name || (activity_scope != 0 && activity_scope != 1) ||
        !ref_for(owner, activity_scope ? BS_HA_ACTIVITY_METADATA :
                        BS_HA_APPLICATION_METADATA, &ref)) return 0;
    return bs_dex_horizon_android_attachment_metadata(ref, name, value);
}

int bs_dex_android_framework_context_open_asset(
    BsDexAndroidFrameworkContext *owner, const char *relative_name,
    BsDexHorizonAssetStream *stream, uint64_t *length) {
    if (stream) *stream = (BsDexHorizonAssetStream){0};
    if (length) *length = 0;
    BsDexFrameworkNativeContext native = {0};
    return stream && length && native_observe(owner, &native) &&
        bs_dex_horizon_java_resources_open(owner->resources, relative_name,
                                           stream, length);
}

int bs_dex_android_framework_context_launch(
    const BsDexAndroidFrameworkContext *owner,
    BsDexHorizonLaunchReceipt *delivered) {
    if (delivered) *delivered = (BsDexHorizonLaunchReceipt){0};
    BsDexHorizonAttachmentRef ref = {0};
    return delivered && ref_for(owner, BS_HA_LAUNCH_RECEIPT, &ref) &&
        bs_dex_horizon_android_attachment_launch(ref, delivered);
}

static int executor_complete(const BsDexFrameworkJavaExecutor *java) {
    const BsDexQuestJavaWitness *w = java ? &java->package_witness : NULL;
    return java && java->verify_class && java->invoke_noarg &&
        java->verify_file_path && java->verify_native_asset_manager &&
        java->verify_configuration && java->verify_receipt &&
        java->verify_attachment && w && w->verify_class &&
        w->verify_app_info && w->verify_activity_info &&
        w->verify_launch_intent && w->verify_package_manager &&
        w->verify_bundle && w->verify_string_value;
}

static int class_is(const BsDexFrameworkJavaExecutor *java,
                    void *object, const char *descriptor) {
    return object && java->verify_class(java->user, object, descriptor) == 1;
}

/* Every link below invokes a real Java method on its original receiver and
 * checks that the returned reference is the independently verified owner. */
static int getter_is(const BsDexFrameworkJavaExecutor *java,
                     void *receiver, const char *declaring,
                     const char *name, const char *prototype,
                     void *expected) {
    void *observed = NULL;
    return receiver && expected &&
        java->invoke_noarg(java->user, receiver, declaring, name,
                           prototype, &observed) == 1 &&
        observed == expected;
}

int bs_dex_android_framework_context_verify_java(
    const BsDexAndroidFrameworkContext *owner,
    const BsDexFrameworkJavaAttachment *candidate,
    const BsDexFrameworkJavaExecutor *java,
    BsDexAndroidContextAttachment *out) {
    if (out) *out = (BsDexAndroidContextAttachment){0};
    BsDexFrameworkNativeContext native = {0};
    BsDexHorizonLaunchReceipt delivered = {0};
    if (!out || !candidate || !executor_complete(java) ||
        !native_observe(owner, &native) ||
        !bs_dex_android_framework_context_launch(owner, &delivered) ||
        !candidate->activity || !candidate->context_impl ||
        !candidate->window || !candidate->handler ||
        !candidate->main_looper || !candidate->class_loader ||
        !candidate->files_dir || !candidate->obb_dir) return 0;

    void *resources = NULL, *assets = NULL, *configuration = NULL;
    BsDexQuestJavaOwners verified_package = {0};
    if (!bs_dex_horizon_java_resources_java_get_resources(
            owner->resources, &resources) ||
        !bs_dex_horizon_java_resources_java_get_assets(
            owner->resources, &assets) ||
        !bs_dex_horizon_java_resources_java_get_configuration(
            owner->resources, &configuration) ||
        !bs_dex_quest_package_verify_java_owners(
            owner->resources->package, &candidate->package_java,
            &java->package_witness, &verified_package) ||
        !class_is(java, candidate->activity,
                 "Lcom/unity3d/player/UnityPlayerGameActivity;") ||
        !class_is(java, candidate->context_impl,
                 "Landroid/app/ContextImpl;") ||
        !class_is(java, candidate->window, "Landroid/view/Window;") ||
        !class_is(java, candidate->main_looper, "Landroid/os/Looper;") ||
        !class_is(java, candidate->handler, "Landroid/os/Handler;") ||
        !class_is(java, candidate->class_loader,
                 "Ljava/lang/ClassLoader;") ||
        !class_is(java, candidate->files_dir, "Ljava/io/File;") ||
        !class_is(java, candidate->obb_dir, "Ljava/io/File;") ||
        !class_is(java, resources, "Landroid/content/res/Resources;") ||
        !class_is(java, assets, "Landroid/content/res/AssetManager;") ||
        !class_is(java, configuration,
                 "Landroid/content/res/Configuration;") ||
        java->verify_file_path(java->user, candidate->files_dir,
                               native.files_dir) != 1 ||
        java->verify_file_path(java->user, candidate->obb_dir,
                               native.obb_dir) != 1 ||
        java->verify_native_asset_manager(
            java->user, assets, native.asset_manager) != 1 ||
        java->verify_configuration(java->user, configuration,
                                   &native.configuration) != 1 ||
        java->verify_receipt(java->user,
            verified_package.launch_intent, &delivered) != 1 ||
        java->verify_attachment(java->user, candidate, &native) != 1)
        return 0;

    const char *context = "Landroid/content/Context;";
    const char *activity = "Landroid/app/Activity;";
    if (!getter_is(java, candidate->activity,
                   "Landroid/content/ContextWrapper;", "getBaseContext",
                   "()Landroid/content/Context;", candidate->context_impl) ||
        !getter_is(java, candidate->activity, activity, "getIntent",
                   "()Landroid/content/Intent;",
                   verified_package.launch_intent) ||
        !getter_is(java, candidate->activity, activity, "getWindow",
                   "()Landroid/view/Window;", candidate->window) ||
        !getter_is(java, candidate->activity, context, "getClassLoader",
                   "()Ljava/lang/ClassLoader;", candidate->class_loader) ||
        !getter_is(java, candidate->activity, context, "getMainLooper",
                   "()Landroid/os/Looper;", candidate->main_looper) ||
        !getter_is(java, candidate->handler,
                   "Landroid/os/Handler;", "getLooper",
                   "()Landroid/os/Looper;", candidate->main_looper) ||
        !getter_is(java, candidate->context_impl, context,
                   "getPackageManager", "()Landroid/content/pm/PackageManager;",
                   verified_package.package_manager) ||
        !getter_is(java, candidate->context_impl, context,
                   "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;",
                   verified_package.application_info) ||
        !getter_is(java, candidate->context_impl, context, "getFilesDir",
                   "()Ljava/io/File;", candidate->files_dir) ||
        !getter_is(java, candidate->context_impl, context, "getObbDir",
                   "()Ljava/io/File;", candidate->obb_dir) ||
        !getter_is(java, candidate->context_impl, context, "getAssets",
                   "()Landroid/content/res/AssetManager;", assets) ||
        !getter_is(java, candidate->context_impl, context, "getResources",
                   "()Landroid/content/res/Resources;", resources) ||
        !getter_is(java, resources, "Landroid/content/res/Resources;",
                   "getAssets", "()Landroid/content/res/AssetManager;", assets) ||
        !getter_is(java, resources, "Landroid/content/res/Resources;",
                   "getConfiguration",
                   "()Landroid/content/res/Configuration;", configuration) ||
        !getter_is(java, verified_package.launch_intent,
                   "Landroid/content/Intent;", "getComponent",
                   "()Landroid/content/ComponentName;",
                   verified_package.component)) return 0;

    /* An external caller must still bind this to its actual Activity.attach
     * lifecycle; neither this snapshot nor this function attach an Activity. */
    *out = (BsDexAndroidContextAttachment){
        .activity = candidate->activity,
        .intent = verified_package.launch_intent,
        .application_info = verified_package.application_info,
        .resources = resources,
        .assets = assets,
        .package_manager = verified_package.package_manager,
        .metadata_bundle = verified_package.application_metadata_bundle,
        .metadata_field_observed = 1,
        .files_dir = candidate->files_dir,
        .obb_dir = candidate->obb_dir,
        .native_library_dir = verified_package.native_library_dir_string
    };
    return 1;
}

void bs_dex_android_framework_context_clear(
    BsDexAndroidFrameworkContext *owner) {
    if (owner) *owner = (BsDexAndroidFrameworkContext){0};
}
