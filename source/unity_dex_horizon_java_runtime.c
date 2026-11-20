#include "unity_dex_horizon_java_runtime.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#include "util.h"

#define BS_HJ_HOOK_MAGIC UINT32_C(0x484a484b)
typedef struct BsHorizonHookToken {
    uint32_t magic;
    BsDexHorizonJavaRuntime *owner;
    AppletHookCookie cookie;
} BsHorizonHookToken;

/* libnx invokes applet hooks when the existing application calls
 * appletMainLoop(). These callbacks only record real native applet events.
 * They DO NOT dispatch Android Activity.onPause/onResume/onDestroy. */
static void horizon_applet_hook(AppletHookType event, void *param) {
    BsDexHorizonJavaRuntime *runtime = param;
    BsDexHorizonJavaAppletEvent recorded = 0;
    switch (event) {
    case AppletHookType_OnFocusState:
        recorded = BS_HJ_APPLET_FOCUS;
        break;
    case AppletHookType_OnOperationMode:
        recorded = BS_HJ_APPLET_OPERATION_MODE;
        break;
    case AppletHookType_OnPerformanceMode:
        recorded = BS_HJ_APPLET_PERFORMANCE_MODE;
        break;
    case AppletHookType_OnExitRequest:
        recorded = BS_HJ_APPLET_EXIT_REQUEST;
        break;
    case AppletHookType_OnResume:
        recorded = BS_HJ_APPLET_RESUME;
        break;
    default:
        return;
    }
    (void)bs_dex_horizon_java_runtime_record_applet_event(runtime, recorded);
}

static int switch_on_main_thread(void *user) {
    return bs_dex_horizon_services_on_applet_thread(user);
}

static int switch_native_ready(void *user) {
    return bs_dex_horizon_services_native_ready(user);
}

static void *switch_window(void *user) {
    const BsDexHorizonServices *services = user;
    return services ? services->observed.nwindow : NULL;
}

static int switch_valid_window(void *user, void *window) {
    return bs_dex_horizon_services_valid_nwindow(user, window);
}

static int switch_subscribe(void *user, BsDexHorizonJavaRuntime *runtime,
                            void **token) {
    if (token) *token = NULL;
    if (!runtime || !token || !switch_on_main_thread(user) ||
        !switch_native_ready(user)) return 0;
    BsHorizonHookToken *hook = calloc(1, sizeof *hook);
    if (!hook) return 0;
    hook->magic = BS_HJ_HOOK_MAGIC;
    hook->owner = runtime;
    /* libnx appletHook installs the new callback in its list HEAD and saves
     * the PREVIOUS head in cookie. The cookie callback/param do not equal the
     * newly installed ones; checking them wrongly rejected every live hook.
     * See the linked libnx appletHook/appleUnhook list splice contract. */
    appletHook(&hook->cookie, horizon_applet_hook, runtime);
    *token = hook;
    return 1;
}

static int switch_unsubscribe(void *user, void *token) {
    if (!token || !switch_on_main_thread(user)) return 0;
    BsHorizonHookToken *hook = token;
    if (hook->magic != BS_HJ_HOOK_MAGIC || !hook->owner ||
        hook->owner->native.user != user) return 0;
    appletUnhook(&hook->cookie);
    hook->magic = 0;
    free(hook);
    return 1;
}
#endif

static int hooks_valid(const BsDexHorizonJavaNativeHooks *hooks) {
    return hooks && hooks->on_main_thread && hooks->native_ready &&
        hooks->current_window && hooks->valid_window && hooks->subscribe &&
        hooks->unsubscribe;
}

int bs_dex_horizon_java_runtime_init_with_hooks(
    BsDexHorizonJavaRuntime *runtime,
    const BsDexHorizonJavaNativeHooks *hooks) {
    if (!runtime || !hooks_valid(hooks)) return 0;
    *runtime = (BsDexHorizonJavaRuntime){0};
    runtime->native = *hooks;
    return 1;
}

int bs_dex_horizon_java_runtime_init(
    BsDexHorizonJavaRuntime *runtime, BsDexHorizonServices *services) {
    if (!runtime || !services) return 0;
#ifdef __SWITCH__
    const BsDexHorizonJavaNativeHooks hooks = {
        .user = services,
        .on_main_thread = switch_on_main_thread,
        .native_ready = switch_native_ready,
        .current_window = switch_window,
        .valid_window = switch_valid_window,
        .subscribe = switch_subscribe,
        .unsubscribe = switch_unsubscribe
    };
    return bs_dex_horizon_java_runtime_init_with_hooks(runtime, &hooks);
#else
    /* A non-Switch executable has no native libnx applet registration API. */
    return 0;
#endif
}

static int on_ui_thread(void *user) {
    BsDexHorizonJavaRuntime *runtime = user;
    return runtime && hooks_valid(&runtime->native) &&
        runtime->native.on_main_thread(runtime->native.user) == 1;
}

static int window_valid(void *user, void *window) {
    BsDexHorizonJavaRuntime *runtime = user;
    if (!on_ui_thread(runtime) || !window ||
        !runtime->native.native_ready(runtime->native.user)) return 0;
    return window == runtime->native.current_window(runtime->native.user) &&
        runtime->native.valid_window(runtime->native.user, window) == 1;
}

/* The original Quest DEX verifies the UnityPlayerGameActivity concrete class
 * through its seven-class chain in bs_dex_horizon_activity_init. Until
 * Activity.attach is reached, the original DEX uses this base only as an
 * instance with a live UI owner (including external WeakReference identity).
 * This constructor owns a REAL applet event subscription and native Window
 * observation. It neither claims nor supplies framework-only Java fields:
 * later Context/Looper/Handler methods must stop at their missing service. */
static int construct_minimal_base(void *user, void *actual_activity) {
    BsDexHorizonJavaRuntime *runtime = user;
    if (!runtime) return 0;
    runtime->last_construct_gate = 1;
    if (!on_ui_thread(runtime)) return 0;
    runtime->last_construct_gate = 2;
    if (!runtime->verified_host ||
        !runtime->verified_host->original_classes_verified ||
        actual_activity != &runtime->verified_host->activity ||
        !runtime->verified_host->activity.constructed ||
        !runtime->verified_host->activity.alive ||
        runtime->verified_host->activity.attached ||
        !runtime->verified_host->activity.generation ||
        runtime->verified_host->activity.generation !=
            runtime->verified_host->next_generation ||
        runtime->native_activity || runtime->event_token ||
        runtime->native_base_constructed)
        return 0;
    runtime->last_construct_gate = 3;
    if (runtime->native.native_ready(runtime->native.user) != 1)
        return 0;
    runtime->last_construct_gate = 4;

    void *window = runtime->native.current_window(runtime->native.user);
    if (!window || !window_valid(runtime, window)) return 0;
    runtime->last_construct_gate = 5;
    void *token = NULL;
    int registered = runtime->native.subscribe(runtime->native.user,
                                               runtime, &token);
    runtime->last_construct_gate = 6;
#ifdef __SWITCH__
    debugPrintf("[java:horizon-hook] subscribe rc=%d token=%d "
                "window_valid_after=%d\n", registered, token != NULL,
                window_valid(runtime, window));
#endif
    if (registered != 1 || !token ||
        !window_valid(runtime, window)) {
        /* A partial subscription must be removed, even if the native
         * backend returned failure after creating its token. */
        if (token && runtime->native.unsubscribe(runtime->native.user,
                                                 token) != 1) {
            /* Preserve the live event cookie for a later detach retry. */
            runtime->native_activity = actual_activity;
            runtime->native_window = window;
            runtime->activity_generation =
                runtime->verified_host->activity.generation;
            runtime->event_token = token;
        }
        return 0;
    }
    runtime->last_construct_gate = 7;
    runtime->native_activity = actual_activity;
    runtime->native_window = window;
    runtime->activity_generation =
        runtime->verified_host->activity.generation;
    runtime->event_token = token;
    runtime->native_base_constructed = 1;
    runtime->event_count = 0;
    runtime->last_native_event = 0;
    runtime->exit_requested = 0;
    runtime->last_construct_gate = 8;
    return 1;
}

int bs_dex_horizon_java_runtime_native_base_ready(
    const BsDexHorizonJavaRuntime *runtime) {
    return runtime && runtime->native_base_constructed &&
        runtime->event_token && runtime->native_activity &&
        runtime->verified_host &&
        runtime->native_activity == &runtime->verified_host->activity &&
        runtime->verified_host->original_classes_verified &&
        runtime->verified_host->activity.constructed &&
        runtime->verified_host->activity.alive &&
        runtime->verified_host->activity.generation ==
            runtime->activity_generation &&
        !runtime->exit_requested &&
        window_valid((void *)runtime, runtime->native_window);
}

int bs_dex_horizon_java_runtime_record_applet_event(
    BsDexHorizonJavaRuntime *runtime, BsDexHorizonJavaAppletEvent event) {
    if (!runtime || !runtime->event_token || !runtime->native_activity ||
        !on_ui_thread(runtime) ||
        event < BS_HJ_APPLET_FOCUS || event > BS_HJ_APPLET_RESUME)
        return 0;
    ++runtime->event_count;
    runtime->last_native_event = (uint32_t)event;
    if (event == BS_HJ_APPLET_EXIT_REQUEST) runtime->exit_requested = 1;
    return 1;
}

static int unavailable_attach(void *user, void *activity, void **window,
                              BsDexAndroidContextAttachment *readback,
                              const char **exception) {
    (void)user;
    (void)activity;
    if (window) *window = NULL;
    if (readback) *readback = (BsDexAndroidContextAttachment){0};
    if (exception) *exception = NULL;
    /* Native applet registration is NOT Android Activity.attach(). Java
     * ContextImpl, Intent and Resources remain unavailable. */
    return 0;
}

static int unavailable_attachment_proof(
    void *user, void *activity, void *window,
    const BsDexAndroidContextAttachment *readback) {
    (void)user;
    (void)activity;
    (void)window;
    (void)readback;
    return 0;
}

static int unavailable_java_class(void *user, const char *descriptor,
                                  void *object) {
    (void)user;
    (void)descriptor;
    (void)object;
    return 0;
}

static int detach_base(void *user, void *activity, void *window) {
    BsDexHorizonJavaRuntime *runtime = user;
    if (!runtime || !runtime->verified_host ||
        activity != &runtime->verified_host->activity ||
        window || !on_ui_thread(runtime)) return 0;
    if (runtime->event_token) {
        if (runtime->native.unsubscribe(runtime->native.user,
                                        runtime->event_token) != 1)
            return 0;
    }
    runtime->event_token = NULL;
    runtime->native_activity = NULL;
    runtime->native_window = NULL;
    runtime->activity_generation = 0;
    runtime->native_base_constructed = 0;
    runtime->exit_requested = 0;
    return 1;
}

BsDexHorizonActivityServices bs_dex_horizon_java_runtime_callbacks(
    BsDexHorizonJavaRuntime *runtime) {
    BsDexHorizonActivityServices callbacks = {0};
    if (!runtime || !hooks_valid(&runtime->native)) return callbacks;
    callbacks.user = runtime;
    callbacks.on_ui_thread = on_ui_thread;
    callbacks.construct_base = construct_minimal_base;
    callbacks.attach = unavailable_attach;
    callbacks.valid_nwindow = window_valid;
    callbacks.verify_attachment = unavailable_attachment_proof;
    callbacks.verify_external = unavailable_java_class;
    callbacks.detach = detach_base;
    return callbacks;
}

int bs_dex_horizon_java_runtime_bind_verified_host(
    BsDexHorizonJavaRuntime *runtime, BsDexHorizonActivityHost *host) {
    if (!runtime || !host || !hooks_valid(&runtime->native) ||
        !host->original_classes_verified || host->activity.constructed ||
        host->activity.alive || host->weak_outstanding ||
        runtime->verified_host || runtime->event_token ||
        host->platform.user != runtime ||
        host->platform.construct_base != construct_minimal_base ||
        host->platform.on_ui_thread != on_ui_thread ||
        host->platform.detach != detach_base) return 0;
    runtime->verified_host = host;
    return 1;
}

int bs_dex_horizon_java_runtime_shutdown(BsDexHorizonJavaRuntime *runtime) {
    if (!runtime || runtime->event_token || runtime->native_activity ||
        runtime->native_base_constructed) return 0;
    if (runtime->verified_host &&
        (runtime->verified_host->activity.constructed ||
         runtime->verified_host->activity.alive ||
         runtime->verified_host->weak_outstanding)) return 0;
    runtime->verified_host = NULL;
    runtime->native = (BsDexHorizonJavaNativeHooks){0};
    runtime->native_window = NULL;
    runtime->activity_generation = 0;
    return 1;
}
