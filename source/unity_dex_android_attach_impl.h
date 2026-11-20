/* Checked bridge between a real guest Android Activity.attach executor and
 * the existing original-Quest native/Java attachment contracts.
 *
 * This module performs no Java object construction by itself. A guest
 * executor must actually run Activity.attach and supply owned Java objects,
 * then this adapter re-executes their getters through the framework verifier.
 * A manifest intent-filter is never substituted for a delivered Intent.
 *
 * Production does not currently provide the executable ContextImpl, Window,
 * Intent or Java Resources/Handler owners needed by Guest.attach. In their
 * absence this bridge cannot authorize onCreate or JNI currentActivity.
 */
#ifndef BS_UNITY_DEX_ANDROID_ATTACH_IMPL_H
#define BS_UNITY_DEX_ANDROID_ATTACH_IMPL_H

#include "unity_dex_android_framework_context.h"

typedef struct BsDexAndroidAttachGuest {
    void *user;
    /* Independently inspect the real Java Looper/Handler and owner thread. */
    int (*verify_main_java_loop)(void *user, void *activity,
                                 const BsDexHorizonMainLooper *native_owner);
    /* Execute genuine guest ContextImpl, Window, Intent, Resources ownership
     * and Activity.attach. Return a non-NULL opaque token for EVERY outstanding
     * side effect, including a partial failure, so detach can undo it.
     * Missing token on failure means the guest cleaned up its own side effects.
     * graph contains real strong Java owners retained through detach. */
    int (*attach)(void *user, void *activity,
                  const BsDexFrameworkNativeContext *native,
                  const BsDexHorizonLaunchReceipt *delivered,
                  BsDexFrameworkJavaAttachment *graph,
                  void **token, const char **exception);
    /* Reinspect the guest's attached Activity/ContextImpl/Window fields. This
     * is called on every verification, not cached from the attach result. */
    int (*verify_attached)(void *user, void *activity, void *token,
                           const BsDexFrameworkJavaAttachment *graph,
                           const BsDexFrameworkNativeContext *native,
                           const BsDexHorizonLaunchReceipt *delivered);
    /* Undo the actual guest attach side effects and release strong Java owners.
     * A failure must leave the token/owners alive for later retry. */
    int (*detach)(void *user, void *activity, void *token,
                  const BsDexFrameworkJavaAttachment *graph);
} BsDexAndroidAttachGuest;

typedef struct BsDexAndroidAttachImpl {
    BsDexAndroidFrameworkContext *native_owner; /* borrowed */
    BsDexFrameworkJavaExecutor java;            /* borrowed user, live executor */
    BsDexAndroidAttachGuest guest;              /* borrowed user, live executor */
    uint64_t attachment_generation;
    uint64_t resources_generation;
    void *activity;
    void *window;                               /* observed native NWindow */
    void *token;                                /* owned by guest */
    BsDexFrameworkJavaAttachment graph;         /* borrowed strong Java owners */
    BsDexAndroidContextAttachment readback;
    /* Independent copy of the delivered event; cannot change behind the
     * adapter as native owners re-observe startup/launch state. */
    BsDexHorizonLaunchReceipt delivered;
    char launch_action[64];
    char launch_package_name[128];
    char launch_activity_class[256];
    int active;
} BsDexAndroidAttachImpl;

/* Bind observed source and real guest executor. This publishes no attachment;
 * it is valid to prepare before any delivered launch/Java Resources exist. */
int bs_dex_android_attach_impl_init(
    BsDexAndroidAttachImpl *adapter,
    BsDexAndroidFrameworkContext *native_owner,
    const BsDexFrameworkJavaExecutor *java,
    const BsDexAndroidAttachGuest *guest);

/* Supply callback table to bs_dex_horizon_android_attachment_set_java().
 * The native owner, original APK, verified Java class executor and guest
 * callbacks must remain live until successful detach and clear. */
int bs_dex_android_attach_impl_callbacks(
    BsDexAndroidAttachImpl *adapter,
    BsDexHorizonAttachmentJava *callbacks);

/* Return the actual Java android.view.Window from guest Activity.getWindow().
 * Only available after complete attachment; rechecks native launch source,
 * Java owner, original getter execution and class identity on every access.
 * Borrowed Java object stays owned by the guest until detach. This is NOT the
 * native NWindow. The current BsDexAndroidContextAttachment has no Window
 * slot; ContextHost integration may call this accessor and wrap its result
 * as a VM reference after independently validating the Activity receiver.
 * *java_window is cleared on every failure. */
int bs_dex_android_attach_impl_observe_java_window(
    BsDexAndroidAttachImpl *adapter, void *activity, void **java_window);

/* Fails while any guest side effects remain attached. */
int bs_dex_android_attach_impl_clear(BsDexAndroidAttachImpl *adapter);

#endif
