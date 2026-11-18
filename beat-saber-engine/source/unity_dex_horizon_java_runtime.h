/* Quest UnityPlayerGameActivity native base lifetime on Horizon.
 *
 * The ORIGINAL DEX executes every subclass constructor from
 * UnityPlayerGameActivity through GameActivity and AndroidX. The terminal
 * android.app.Activity.<init>()V is a framework method with no Quest DEX
 * implementation. For the original pre-attach path, the smallest observable
 * base service is a live, class-verified native Activity instance on the
 * applet's actual main thread, with its own working applet event subscription.
 *
 * This host supplies exactly that service. It does not synthesize Android
 * ContextImpl, Intent, Resources, Java Looper/Handler, FragmentController,
 * Window objects, or an Activity.attach. All those operations remain blocked
 * until separate independently implemented Java services are installed.
 *
 * Lifetime: native observations > this runtime > verified original DEX
 * BsDexHorizonActivityHost > VM. Release the VM and Activity before freeing
 * the runtime. libnx applet hooks must be unregistered before runtime exit.
 */
#ifndef BS_UNITY_DEX_HORIZON_JAVA_RUNTIME_H
#define BS_UNITY_DEX_HORIZON_JAVA_RUNTIME_H

#include "unity_dex_horizon_activity_host.h"
#include "unity_dex_horizon_services.h"
#include <stdint.h>

typedef struct BsDexHorizonJavaRuntime BsDexHorizonJavaRuntime;

/* Exact observed native applet events. None indicates a Java lifecycle event
 * or authorizes Android onCreate/SurfaceView callback dispatch. */
typedef enum BsDexHorizonJavaAppletEvent {
    BS_HJ_APPLET_FOCUS = 1,
    BS_HJ_APPLET_OPERATION_MODE,
    BS_HJ_APPLET_PERFORMANCE_MODE,
    BS_HJ_APPLET_EXIT_REQUEST,
    BS_HJ_APPLET_RESUME
} BsDexHorizonJavaAppletEvent;

/* Native event ownership adapter. Production implementation calls libnx
 * appletHook/appletUnhook, checks the actual main thread, NWindow, staged APK
 * assets and directory backing. Injected adapters are for isolated host
 * tests and independently backed alternative native applet implementations.
 *
 * subscribe must register a REAL event callback with an event pump, keep
 * 'observer' alive and return an opaque non-NULL token. unsubscribe must
 * actually remove the callback before returning success. The callback may
 * use bs_dex_horizon_java_runtime_record_applet_event for native telemetry.
 */
typedef struct BsDexHorizonJavaNativeHooks {
    void *user;
    int (*on_main_thread)(void *user);
    int (*native_ready)(void *user);
    void *(*current_window)(void *user);
    int (*valid_window)(void *user, void *window);
    int (*subscribe)(void *user, BsDexHorizonJavaRuntime *observer,
                     void **token);
    int (*unsubscribe)(void *user, void *token);
} BsDexHorizonJavaNativeHooks;

struct BsDexHorizonJavaRuntime {
    BsDexHorizonJavaNativeHooks native;
    BsDexHorizonActivityHost *verified_host; /* borrowed original DEX host */
    void *native_activity;
    void *native_window;
    void *event_token;
    uint64_t activity_generation;
    uint64_t event_count;
    uint32_t last_native_event;
    int native_base_constructed;
    int exit_requested;
    /* Last genuinely attempted constructor gate, for guest log diagnosis.
     * 0=not called, 1=called, 2=UI, 3=class owner, 4=native preflight,
     * 5=window, 6=hook returned, 7=hook verified, 8=constructed. */
    uint32_t last_construct_gate;
};

/* Initialize production callbacks that use the verified
 * BsDexHorizonServices snapshot. Real applet events are delivered only
 * after the outer application calls appletMainLoop(). */
int bs_dex_horizon_java_runtime_init(
    BsDexHorizonJavaRuntime *runtime, BsDexHorizonServices *services);

/* Only for an independently executable native event backend. The adapter's
 * callbacks must represent real operations; this entry is used by host
 * regression fixtures. */
int bs_dex_horizon_java_runtime_init_with_hooks(
    BsDexHorizonJavaRuntime *runtime,
    const BsDexHorizonJavaNativeHooks *hooks);

/* Build BsDexHorizonActivityServices for bs_dex_horizon_activity_init().
 * The original Activity metadata has to be checked by that host against the
 * exact Quest DEX before calling bind_verified_host below. */
BsDexHorizonActivityServices bs_dex_horizon_java_runtime_callbacks(
    BsDexHorizonJavaRuntime *runtime);

/* No newly allocated Java class identity is inferred. This only accepts an
 * independently DEX-verified Activity host using this exact callback table. */
int bs_dex_horizon_java_runtime_bind_verified_host(
    BsDexHorizonJavaRuntime *runtime, BsDexHorizonActivityHost *host);

/* Called on the owning event-loop thread by a real applet hook. Readable
 * telemetry only; it does not dispatch Android/JNI Java lifecycle methods. */
int bs_dex_horizon_java_runtime_record_applet_event(
    BsDexHorizonJavaRuntime *runtime, BsDexHorizonJavaAppletEvent event);

/* True only while the same native Activity generation and event subscription
 * remain live on the same observed NWindow and applet main thread. */
int bs_dex_horizon_java_runtime_native_base_ready(
    const BsDexHorizonJavaRuntime *runtime);

/* Requires completed event unregistration and no Activity attachment.
 * Does not call bs_dex_horizon_activity_release(); caller owns that host. */
int bs_dex_horizon_java_runtime_shutdown(BsDexHorizonJavaRuntime *runtime);

#endif
