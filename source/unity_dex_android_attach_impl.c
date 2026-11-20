#include "unity_dex_android_attach_impl.h"
#include <string.h>

static int complete_java(const BsDexFrameworkJavaExecutor *java) {
    const BsDexQuestJavaWitness *w = java ? &java->package_witness : NULL;
    return java && java->verify_class && java->invoke_noarg &&
        java->verify_file_path && java->verify_native_asset_manager &&
        java->verify_configuration && java->verify_receipt &&
        java->verify_attachment && w && w->verify_class &&
        w->verify_app_info && w->verify_activity_info &&
        w->verify_launch_intent && w->verify_package_manager &&
        w->verify_bundle && w->verify_string_value;
}

static int complete_guest(const BsDexAndroidAttachGuest *guest) {
    return guest && guest->verify_main_java_loop && guest->attach &&
        guest->verify_attached && guest->detach;
}

static int owner_live(const BsDexAndroidAttachImpl *a) {
    const BsDexAndroidFrameworkContext *ctx = a ? a->native_owner : NULL;
    return ctx && ctx->initialized && ctx->attachment && ctx->resources &&
        ctx->attachment_generation && ctx->resources_generation &&
        a->attachment_generation == ctx->attachment_generation &&
        a->resources_generation == ctx->resources_generation &&
        ctx->attachment_generation == ctx->attachment->generation &&
        ctx->resources_generation == ctx->resources->generation &&
        ctx->resources->attachment == ctx->attachment;
}

static int same_receipt(const BsDexHorizonLaunchReceipt *a,
                        const BsDexHorizonLaunchReceipt *b) {
    return a && b && a->action && b->action &&
        a->package_name && b->package_name &&
        a->activity_class && b->activity_class &&
        a->source_token && a->source_token == b->source_token &&
        !strcmp(a->action, b->action) &&
        !strcmp(a->package_name, b->package_name) &&
        !strcmp(a->activity_class, b->activity_class) &&
        a->launcher_category == b->launcher_category &&
        a->vr_category == b->vr_category;
}

static int preserve_receipt(BsDexAndroidAttachImpl *a,
                            const BsDexHorizonLaunchReceipt *launch) {
    if (!a || !launch || !launch->action || !launch->package_name ||
        !launch->activity_class || !launch->source_token) return 0;
    size_t action = strlen(launch->action);
    size_t package_name = strlen(launch->package_name);
    size_t activity = strlen(launch->activity_class);
    if (!action || action >= sizeof a->launch_action ||
        !package_name || package_name >= sizeof a->launch_package_name ||
        !activity || activity >= sizeof a->launch_activity_class) return 0;
    memcpy(a->launch_action, launch->action, action + 1);
    memcpy(a->launch_package_name, launch->package_name, package_name + 1);
    memcpy(a->launch_activity_class, launch->activity_class, activity + 1);
    a->delivered = *launch;
    a->delivered.action = a->launch_action;
    a->delivered.package_name = a->launch_package_name;
    a->delivered.activity_class = a->launch_activity_class;
    return 1;
}

static int same_readback(const BsDexAndroidContextAttachment *a,
                         const BsDexAndroidContextAttachment *b) {
    return a && b && a->activity == b->activity &&
        a->intent == b->intent &&
        a->application_info == b->application_info &&
        a->resources == b->resources && a->assets == b->assets &&
        a->package_manager == b->package_manager &&
        a->metadata_bundle == b->metadata_bundle &&
        a->metadata_field_observed == b->metadata_field_observed &&
        a->files_dir == b->files_dir && a->obb_dir == b->obb_dir &&
        a->native_library_dir == b->native_library_dir;
}

static int observe(BsDexAndroidAttachImpl *a, void *activity, void *window,
                   const BsDexHorizonLaunchReceipt *delivered,
                   BsDexFrameworkNativeContext *native,
                   BsDexHorizonLaunchReceipt *actual) {
    if (native) *native = (BsDexFrameworkNativeContext){0};
    if (actual) *actual = (BsDexHorizonLaunchReceipt){0};
    if (!native || !actual || !owner_live(a) || !activity || !window ||
        !a->native_owner->attachment->activity_host ||
        activity != &a->native_owner->attachment->activity_host->activity ||
        !bs_dex_android_framework_context_observe(a->native_owner, native) ||
        native->nwindow != window ||
        !bs_dex_android_framework_context_launch(a->native_owner, actual) ||
        (delivered && !same_receipt(actual, delivered))) return 0;
    return 1;
}

static int verify_loop(void *user, void *activity,
                       const BsDexHorizonMainLooper *native_owner) {
    BsDexAndroidAttachImpl *a = user;
    if (!owner_live(a) || !native_owner || !activity ||
        native_owner != a->native_owner->attachment->looper ||
        !a->native_owner->attachment->activity_host ||
        activity != &a->native_owner->attachment->activity_host->activity ||
        !bs_dex_horizon_main_looper_java_ready(native_owner)) return 0;
    return a->guest.verify_main_java_loop(
        a->guest.user, activity, native_owner) == 1;
}

static int execute(void *user, void *activity, void *window,
                   const BsDexHorizonAndroidAttachment *native_model,
                   const BsDexHorizonLaunchReceipt *delivered,
                   BsDexQuestJavaOwners *package_java,
                   BsDexAndroidContextAttachment *observed,
                   void **attached_token, const char **exception) {
    BsDexAndroidAttachImpl *a = user;
    if (package_java) *package_java = (BsDexQuestJavaOwners){0};
    if (observed) *observed = (BsDexAndroidContextAttachment){0};
    if (attached_token) *attached_token = NULL;
    if (exception) *exception = NULL;
    if (!a || !package_java || !observed || !attached_token || !exception ||
        !owner_live(a) || a->active || a->token ||
        native_model != a->native_owner->attachment ||
        !verify_loop(a, activity, native_model->looper)) return 0;

    BsDexFrameworkNativeContext native = {0};
    BsDexHorizonLaunchReceipt actual = {0};
    if (!observe(a, activity, window, delivered, &native, &actual)) return 0;

    BsDexFrameworkJavaAttachment graph = {0};
    void *token = NULL;
    const char *raised = NULL;
    int result = a->guest.attach(a->guest.user, activity, &native,
                                 &actual, &graph, &token, &raised);
    /* An outstanding token is never dropped: wrapper_attach() calls detach
     * even when this callback reports a failed/partial guest attach. */
    if (token) {
        a->activity = activity;
        a->window = window;
        a->token = token;
        a->graph = graph;
        a->active = 1;
        *attached_token = token;
    }
    if (result != 1 || raised || !token || graph.activity != activity) {
        if (result < 0 && raised) *exception = raised;
        return result < 0 && raised ? -1 : 0;
    }

    BsDexAndroidContextAttachment readback = {0};
    BsDexFrameworkNativeContext current = {0};
    BsDexHorizonLaunchReceipt current_launch = {0};
    if (!bs_dex_android_framework_context_verify_java(
            a->native_owner, &graph, &a->java, &readback) ||
        !observe(a, activity, window, &actual, &current, &current_launch) ||
        a->guest.verify_attached(a->guest.user, activity, token,
                                 &graph, &current, &current_launch) != 1 ||
        !preserve_receipt(a, &actual))
        return 0;

    a->readback = readback;
    *package_java = graph.package_java;
    *observed = readback;
    return 1;
}

static int verify_type(void *user, const char *descriptor, void *object) {
    BsDexAndroidAttachImpl *a = user;
    return a && owner_live(a) && a->active && a->token &&
        descriptor && object && a->java.verify_class(
            a->java.user, object, descriptor) == 1;
}

static int verify_attached(void *user, void *activity, void *window,
                           void *token,
                           const BsDexAndroidContextAttachment *expected) {
    BsDexAndroidAttachImpl *a = user;
    if (!a || !expected || !owner_live(a) || !a->active ||
        !token || token != a->token || activity != a->activity ||
        !verify_loop(a, activity, a->native_owner->attachment->looper) ||
        !same_readback(expected, &a->readback)) return 0;
    BsDexFrameworkNativeContext native = {0};
    BsDexHorizonLaunchReceipt launch = {0};
    BsDexAndroidContextAttachment current = {0};
    return observe(a, activity, window, &a->delivered, &native, &launch) &&
        bs_dex_android_framework_context_verify_java(
            a->native_owner, &a->graph, &a->java, &current) == 1 &&
        same_readback(&current, expected) &&
        a->guest.verify_attached(a->guest.user, activity, token,
                                 &a->graph, &native, &launch) == 1;
}

static int detach(void *user, void *activity, void *window, void *token) {
    BsDexAndroidAttachImpl *a = user;
    if (!a || !a->active || !a->token || !token ||
        activity != a->activity || token != a->token ||
        window != a->window ||
        a->guest.detach(a->guest.user, activity, token, &a->graph) != 1)
        return 0;
    a->activity = NULL;
    a->window = NULL;
    a->token = NULL;
    a->graph = (BsDexFrameworkJavaAttachment){0};
    a->readback = (BsDexAndroidContextAttachment){0};
    a->delivered = (BsDexHorizonLaunchReceipt){0};
    a->launch_action[0] = 0;
    a->launch_package_name[0] = 0;
    a->launch_activity_class[0] = 0;
    a->active = 0;
    return 1;
}

int bs_dex_android_attach_impl_observe_java_window(
    BsDexAndroidAttachImpl *a, void *activity, void **java_window) {
    if (java_window) *java_window = NULL;
    if (!a || !java_window || !a->graph.window || !activity ||
        !a->java.verify_class || !a->java.invoke_noarg ||
        !verify_attached(a, activity, a->window, a->token, &a->readback) ||
        a->java.verify_class(a->java.user, a->graph.window,
                             "Landroid/view/Window;") != 1) return 0;
    void *actual = NULL;
    if (a->java.invoke_noarg(a->java.user, activity,
                             "Landroid/app/Activity;", "getWindow",
                             "()Landroid/view/Window;", &actual) != 1 ||
        actual != a->graph.window) return 0;
    *java_window = actual;
    return 1;
}

int bs_dex_android_attach_impl_init(
    BsDexAndroidAttachImpl *a,
    BsDexAndroidFrameworkContext *native_owner,
    const BsDexFrameworkJavaExecutor *java,
    const BsDexAndroidAttachGuest *guest) {
    if (!a || a->native_owner || a->active || a->token ||
        !native_owner || !native_owner->initialized ||
        !native_owner->attachment || !native_owner->resources ||
        !native_owner->attachment_generation ||
        !native_owner->resources_generation ||
        !complete_java(java) || !complete_guest(guest)) return 0;
    BsDexAndroidAttachImpl candidate = {
        .native_owner = native_owner,
        .java = *java, .guest = *guest,
        .attachment_generation = native_owner->attachment_generation,
        .resources_generation = native_owner->resources_generation
    };
    if (!owner_live(&candidate)) return 0;
    *a = candidate;
    return 1;
}

int bs_dex_android_attach_impl_callbacks(
    BsDexAndroidAttachImpl *a,
    BsDexHorizonAttachmentJava *callbacks) {
    if (callbacks) *callbacks = (BsDexHorizonAttachmentJava){0};
    if (!callbacks || !owner_live(a) || !complete_java(&a->java) ||
        !complete_guest(&a->guest)) return 0;
    *callbacks = (BsDexHorizonAttachmentJava){
        .user = a,
        .package_witness = a->java.package_witness,
        .verify_main_java_loop = verify_loop,
        .execute_attach = execute,
        .verify_type = verify_type,
        .verify_attachment = verify_attached,
        .detach = detach
    };
    return 1;
}

int bs_dex_android_attach_impl_clear(BsDexAndroidAttachImpl *a) {
    if (!a || a->active || a->token) return 0;
    *a = (BsDexAndroidAttachImpl){0};
    return 1;
}
