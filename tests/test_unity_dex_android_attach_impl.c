/* Isolated guest-executor fixture: verifies exact original attachment bridge
 * state changes and rollback. The real-APK owner tests run separately. */
#include "unity_dex_android_attach_impl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    BsDexHorizonActivityHost host;
    BsDexHorizonMainLooper loop;
    BsDexHorizonAndroidAttachment attachment;
    BsDexHorizonJavaResources resources;
    BsDexAndroidFrameworkContext native;
    BsDexAndroidAttachImpl adapter;
    BsDexHorizonAttachmentJava callbacks;
    BsDexFrameworkJavaAttachment graph;
    BsDexAndroidContextAttachment expected;
    int window, java_window, source, alternate_source, token;
    int observed, launched, java_loop, java_getters, guest_live;
    int guest_attaches, guest_verifications, guest_detaches;
    int fail_detach, partial_fail, fail_java, changed_resource;
    int changed_source, wrong_get_window;
} Fixture;

static Fixture *active_fixture;
static const char *const raised_exception =
    "Ljava/lang/IllegalStateException;";

/* The adapter calls only independently backed published APIs. This fixture
 * replaces their implementations to exercise the adapter's own guard rails. */
int bs_dex_android_framework_context_observe(
    const BsDexAndroidFrameworkContext *owner,
    BsDexFrameworkNativeContext *out) {
    Fixture *f = active_fixture;
    if (out) *out = (BsDexFrameworkNativeContext){0};
    if (!out || !owner || !f->observed ||
        owner != &f->native) return 0;
    *out = (BsDexFrameworkNativeContext){
        .nwindow = &f->window, .owner_thread_id = 42,
        .asset_manager = &f->resources, .alooper = &f->loop
    };
    return 1;
}

int bs_dex_android_framework_context_launch(
    const BsDexAndroidFrameworkContext *owner,
    BsDexHorizonLaunchReceipt *out) {
    Fixture *f = active_fixture;
    if (out) *out = (BsDexHorizonLaunchReceipt){0};
    if (!out || !f->launched || owner != &f->native) return 0;
    *out = (BsDexHorizonLaunchReceipt){
        .action = "android.intent.action.MAIN",
        .package_name = "com.beatgames.beatsaber",
        .activity_class = "com.unity3d.player.UnityPlayerGameActivity",
        .launcher_category = 1, .vr_category = 1,
        .source_token = f->changed_source ?
            (void *)&f->alternate_source : (void *)&f->source
    };
    return 1;
}

int bs_dex_horizon_main_looper_java_ready(
    const BsDexHorizonMainLooper *loop) {
    return active_fixture->java_loop &&
        loop == &active_fixture->loop;
}

int bs_dex_android_framework_context_verify_java(
    const BsDexAndroidFrameworkContext *native,
    const BsDexFrameworkJavaAttachment *graph,
    const BsDexFrameworkJavaExecutor *executor,
    BsDexAndroidContextAttachment *out) {
    Fixture *f = active_fixture;
    if (out) *out = (BsDexAndroidContextAttachment){0};
    if (!out || native != &f->native || !graph || !executor ||
        !f->guest_live || !f->java_getters || f->fail_java ||
        graph->activity != &f->host.activity) return 0;
    *out = f->expected;
    if (f->changed_resource) out->resources = &f->loop;
    return 1;
}

static int always_class(void *user, void *object, const char *name) {
    (void)user;
    return object && name && !strcmp(name, "Landroid/view/Window;") &&
        object == &active_fixture->java_window;
}
static int always_getter(void *user, void *object, const char *c,
                         const char *m, const char *p, void **out) {
    (void)user;
    if (out) *out = NULL;
    Fixture *f = active_fixture;
    if (!out || !f || !f->guest_live || !f->java_getters ||
        object != &f->host.activity || !c || !m || !p ||
        strcmp(c, "Landroid/app/Activity;") ||
        strcmp(m, "getWindow") ||
        strcmp(p, "()Landroid/view/Window;")) return 0;
    *out = f->wrong_get_window ? (void *)&f->window :
        (void *)&f->java_window;
    return 1;
}
static int always_path(void *user, void *obj, const char *path) {
    (void)user; return obj && path;
}
static int always_asset(void *user, void *obj, void *ndk) {
    (void)user; return obj && ndk;
}
static int always_config(
    void *user, void *java,
    const BsDexHorizonObservedConfiguration *native) {
    (void)user; return java && native;
}
static int always_receipt(
    void *user, void *java, const BsDexHorizonLaunchReceipt *delivered) {
    (void)user; return java && delivered && delivered->source_token;
}
static int always_attached(void *user,
                           const BsDexFrameworkJavaAttachment *java,
                           const BsDexFrameworkNativeContext *native) {
    (void)user; return java && native;
}
static int package_class(void *user, void *obj, const char *name) {
    return always_class(user, obj, name);
}
static int package_info(void *user, void *object,
                        const BsDexQuestApplicationInfo *model) {
    (void)user; return object && model;
}
static int package_activity_info(void *user, void *object,
                                 const BsDexQuestActivityInfo *model) {
    (void)user; return object && model;
}
static int package_intent(void *user, void *intent, void *component,
                          const BsDexQuestLaunchFilter *filter) {
    (void)user; return intent && component && filter;
}
static int package_manager(void *user, void *pm, void *info, void *component) {
    (void)user; return pm && info && component;
}
static int package_bundle(void *user, void *bundle,
                          const BsApkManifest *model, int activity_scope) {
    (void)user; return bundle && model && (activity_scope == 0 || activity_scope == 1);
}
static int package_string(void *user, void *string, const char *expected) {
    (void)user; return string && expected;
}

static int guest_loop(void *user, void *activity,
                      const BsDexHorizonMainLooper *loop) {
    Fixture *f = user;
    return f->java_loop && activity == &f->host.activity && loop == &f->loop;
}

static int guest_attach(void *user, void *activity,
                        const BsDexFrameworkNativeContext *native,
                        const BsDexHorizonLaunchReceipt *launch,
                        BsDexFrameworkJavaAttachment *graph,
                        void **token, const char **exception) {
    Fixture *f = user;
    assert(activity == &f->host.activity);
    assert(native->nwindow == &f->window && native->owner_thread_id == 42);
    assert(launch->source_token == &f->source);
    assert(!f->guest_live);
    ++f->guest_attaches;
    f->guest_live = 1;
    *token = &f->token;
    *graph = f->graph;
    if (f->partial_fail) {
        *exception = raised_exception;
        return -1;
    }
    return 1;
}

static int guest_verify(void *user, void *activity, void *token,
                        const BsDexFrameworkJavaAttachment *graph,
                        const BsDexFrameworkNativeContext *native,
                        const BsDexHorizonLaunchReceipt *launch) {
    Fixture *f = user;
    ++f->guest_verifications;
    return f->guest_live && activity == &f->host.activity &&
        token == &f->token && graph->activity == activity &&
        native->nwindow == &f->window &&
        launch->source_token == &f->source;
}

static int guest_detach(void *user, void *activity, void *token,
                        const BsDexFrameworkJavaAttachment *graph) {
    Fixture *f = user;
    if (!f->guest_live || activity != &f->host.activity ||
        token != &f->token || f->fail_detach) return 0;
    (void)graph;
    ++f->guest_detaches;
    f->guest_live = 0;
    return 1;
}

static void init_fixture(Fixture *f) {
    memset(f, 0, sizeof *f);
    active_fixture = f;
    f->observed = f->launched = f->java_loop = f->java_getters = 1;
    f->attachment.activity_host = &f->host;
    f->attachment.looper = &f->loop;
    f->attachment.generation = 8;
    f->resources.attachment = &f->attachment;
    f->resources.generation = 9;
    f->native = (BsDexAndroidFrameworkContext){
        .initialized = 1,
        .attachment = &f->attachment, .resources = &f->resources,
        .attachment_generation = 8, .resources_generation = 9
    };
    f->graph.activity = &f->host.activity;
    f->graph.context_impl = &f->token;
    f->graph.window = &f->java_window;
    f->expected = (BsDexAndroidContextAttachment){
        .activity = &f->host.activity,
        .intent = &f->source, .resources = &f->resources,
        .assets = &f->window, .application_info = &f->token,
        .metadata_bundle = &f->token, .package_manager = &f->window,
        .metadata_field_observed = 1, .files_dir = &f->source,
        .obb_dir = &f->source, .native_library_dir = &f->source
    };
    BsDexFrameworkJavaExecutor java = {
        .verify_class = always_class,
        .invoke_noarg = always_getter,
        .verify_file_path = always_path,
        .verify_native_asset_manager = always_asset,
        .verify_configuration = always_config,
        .verify_receipt = always_receipt,
        .verify_attachment = always_attached,
        .package_witness = {
            .verify_class = package_class,
            .verify_app_info = package_info,
            .verify_activity_info = package_activity_info,
            .verify_launch_intent = package_intent,
            .verify_package_manager = package_manager,
            .verify_bundle = package_bundle,
            .verify_string_value = package_string
        }
    };
    BsDexAndroidAttachGuest guest = {
        .user = f, .verify_main_java_loop = guest_loop,
        .attach = guest_attach, .verify_attached = guest_verify,
        .detach = guest_detach
    };
    assert(bs_dex_android_attach_impl_init(
        &f->adapter, &f->native, &java, &guest));
    assert(bs_dex_android_attach_impl_callbacks(&f->adapter, &f->callbacks));
}

static int execute(Fixture *f, BsDexQuestJavaOwners *package,
                   BsDexAndroidContextAttachment *out, void **token,
                   const char **exception) {
    BsDexHorizonLaunchReceipt delivered = {0};
    assert(bs_dex_android_framework_context_launch(&f->native, &delivered));
    return f->callbacks.execute_attach(
        f->callbacks.user, &f->host.activity, &f->window,
        &f->attachment, &delivered, package, out, token, exception);
}

static void reject_and_detach(Fixture *f) {
    BsDexQuestJavaOwners package = {0};
    BsDexAndroidContextAttachment out = {0};
    const char *exception = NULL;
    void *token = NULL;
    assert(execute(f, &package, &out, &token, &exception) == 0);
    assert(token == &f->token);
    assert(!out.activity && !package.launch_intent);
    assert(!bs_dex_android_attach_impl_clear(&f->adapter));
    assert(f->callbacks.detach(f->callbacks.user, &f->host.activity,
                               &f->window, token) == 1);
}

int main(void) {
    Fixture f = {0};
    init_fixture(&f);
    assert(f.callbacks.verify_main_java_loop(
        f.callbacks.user, &f.host.activity, &f.loop));
    BsDexQuestJavaOwners package = {0};
    BsDexAndroidContextAttachment out = {0};
    void *token = NULL;
    const char *exception = NULL;
    f.launched = 0;
    assert(!f.callbacks.execute_attach(f.callbacks.user, &f.host.activity,
        &f.window, &f.attachment, NULL, &package, &out, &token, &exception));
    assert(!f.guest_attaches && !token && !exception);
    f.launched = 1;
    f.java_loop = 0;
    assert(!f.callbacks.verify_main_java_loop(
        f.callbacks.user, &f.host.activity, &f.loop));
    assert(!execute(&f, &package, &out, &token, &exception));
    assert(!f.guest_attaches);
    f.java_loop = 1;
    f.fail_java = 1;
    reject_and_detach(&f);
    f.fail_java = 0;
    f.partial_fail = 1;
    assert(execute(&f, &package, &out, &token, &exception) == -1);
    assert(token == &f.token && exception == raised_exception);
    f.fail_detach = 1;
    assert(!f.callbacks.detach(f.callbacks.user, &f.host.activity,
                               &f.window, token));
    assert(!bs_dex_android_attach_impl_clear(&f.adapter));
    f.fail_detach = 0;
    assert(f.callbacks.detach(f.callbacks.user, &f.host.activity,
                              &f.window, token));
    f.partial_fail = 0;
    assert(execute(&f, &package, &out, &token, &exception) == 1);
    assert(token == &f.token && !exception);
    assert(out.activity == &f.host.activity &&
           out.resources == &f.resources);
    assert(f.callbacks.verify_type(f.callbacks.user,
        "Landroid/view/Window;", &f.java_window) == 1);
    void *observed_java_window = NULL;
    assert(&f.java_window != &f.window);
    assert(bs_dex_android_attach_impl_observe_java_window(
        &f.adapter, &f.host.activity, &observed_java_window));
    assert(observed_java_window == &f.java_window);
    f.wrong_get_window = 1;
    assert(!bs_dex_android_attach_impl_observe_java_window(
        &f.adapter, &f.host.activity, &observed_java_window));
    assert(!observed_java_window);
    f.wrong_get_window = 0;
    assert(f.callbacks.verify_attachment(f.callbacks.user,
        &f.host.activity, &f.window, token, &out));
    int inspections = f.guest_verifications;
    assert(f.callbacks.verify_attachment(f.callbacks.user,
        &f.host.activity, &f.window, token, &out));
    assert(f.guest_verifications > inspections);
    f.changed_source = 1;
    assert(!f.callbacks.verify_attachment(f.callbacks.user,
        &f.host.activity, &f.window, token, &out));
    assert(!bs_dex_android_attach_impl_observe_java_window(
        &f.adapter, &f.host.activity, &observed_java_window));
    assert(!observed_java_window);
    f.changed_source = 0;
    assert(bs_dex_android_attach_impl_observe_java_window(
        &f.adapter, &f.host.activity, &observed_java_window));
    f.changed_resource = 1;
    assert(!f.callbacks.verify_attachment(f.callbacks.user,
        &f.host.activity, &f.window, token, &out));
    f.changed_resource = 0;
    f.observed = 0;
    assert(!f.callbacks.verify_attachment(f.callbacks.user,
        &f.host.activity, &f.window, token, &out));
    assert(f.callbacks.detach(f.callbacks.user, &f.host.activity,
                              &f.window, token));
    assert(!bs_dex_android_attach_impl_observe_java_window(
        &f.adapter, &f.host.activity, &observed_java_window));
    assert(!observed_java_window);
    f.observed = 1;
    assert(bs_dex_android_attach_impl_clear(&f.adapter));
    assert(!f.adapter.native_owner);
    puts("PASS: guest Activity.attach bridge observes live receipt/Java graph, rechecks getters and preserves rollback tokens");
    return 0;
}
