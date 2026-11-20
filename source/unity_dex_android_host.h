/* Composable, fail-closed Android method dispatcher for the original Quest
 * classes.dex interpreter. Platform Activity construction and attachment
 * require a backend.
 *
 * A real platform integrator must supply independently observed Java objects,
 * a real UI thread and actual Android class-identity/assignability evidence.
 * Host-test fixtures model those services but never grant NRO readiness. */
#ifndef BS_UNITY_DEX_ANDROID_HOST_H
#define BS_UNITY_DEX_ANDROID_HOST_H

#include "unity_dex_android_context_host.h"
#include "unity_dex_android_surface_host.h"
struct BsDexAndroidTextInputHost;
struct BsDexHorizonJavaHandlerHost;

typedef struct BsDexAndroidHost {
    BsDexAndroidContextHost *context;
    BsDexAndroidSurfaceHost *surface;
    struct BsDexAndroidTextInputHost *text_input;
    struct BsDexHorizonJavaHandlerHost *handler;
    /* Optional platform handler adapter. Keeping calls indirect lets host
     * fixtures omit the Horizon module without creating false stubs. */
    int (*handler_new)(struct BsDexHorizonJavaHandlerHost *, BsDexVM *,
                       const char *, BsDexValue *, const char **);
    int (*handler_invoke)(struct BsDexHorizonJavaHandlerHost *, BsDexVM *,
                          const char *, const char *, const char *, uint8_t,
                          const BsDexValue *, size_t, BsDexValue *,
                          const char **);
    int (*handler_instance_of)(struct BsDexHorizonJavaHandlerHost *, BsDexVM *,
                                BsDexValue, const char *, int *);
    /* Optional executable Java Resources method adapter. It receives only
     * the exact original DEX invocation and must independently verify the
     * real attached Java Resources receiver and typed data owner. */
    void *resource_user;
    int (*resource_invoke)(void *, BsDexVM *, const char *, const char *,
                           const char *, uint8_t, const BsDexValue *, size_t,
                           BsDexValue *, const char **);
    void *user;
    /* Return 1 ONLY after checking actual Java runtime class metadata for
     * this live object's assignability. No string/pointer-only guessing. */
    int (*observed_instance_of)(void *user, void *live_object,
                                const char *class_descriptor,
                                int *result, const char **exception);
    /* Load the original game library or verify its completed load. */
    int (*load_game_library)(void *user);
    /* Run the platform base constructor on a new instance of the requested
     * concrete subclass. Leave *activity NULL on failure, and set exception
     * only when the platform actually throws. Ownership stays with the
     * backend. The dispatcher releases rejected objects through
     * discard_activity. Keep accepted objects alive until VM teardown. */
    int (*construct_activity_base)(void *user, const char *subclass,
                                   void **activity, const char **exception);
    void (*discard_activity)(void *user, void *activity);
    /* Called AFTER the original DEX constructor has returned. Perform the
     * platform Activity.attach with real Context/Window/launch Intent and
     * then obtain its actual Java object getters into 'observed'. A synthetic
     * GameActivity JNI proxy is not a constructed Activity. On failure the
     * backend retains ownership so discard_activity can release it. */
    int (*attach_and_observe_activity)(
        void *user, void *activity, BsDexAndroidContextAttachment *observed,
        const char **exception);

    /* A native Java weak-reference backend. The token owns ONLY a weak
     * handle; it must never retain a JNI global or other strong reference to
     * the referent. get must independently check liveness on every call and
     * return bs_dex_null() if the referent was cleared or collected. The
     * returned non-null VM reference must be the ORIGINAL live referent.
     * All four methods are required as a set; the interpreter does not hold
     * a native strong owner. The host must outlive VM destruction because
     * release is invoked by the VM even for a previously cleared handle. */
    int (*weak_reference_create)(void *user, BsDexVM *vm,
                                 BsDexValue referent, void **weak_handle,
                                 const char **exception);
    int (*weak_reference_get)(void *user, BsDexVM *vm,
                              void *weak_handle, BsDexValue *live_or_null,
                              const char **exception);
    int (*weak_reference_clear)(void *user, BsDexVM *vm,
                                void *weak_handle, const char **exception);
    void (*weak_reference_release)(void *user, BsDexVM *vm,
                                   void *weak_handle);
    /* Real monotonic nanosecond timestamp used to seed the VM's faithful
     * java.util.Random() constructor. No constant or simulated clock is
     * permitted on the production guest path. */
    int (*monotonic_nanoseconds)(void *user, uint64_t *nanoseconds);
    /* Independently observe a live owner-thread Android-compatible main
     * event queue, with the current OS thread ID sampled at the same time.
     * This only authorizes the VM's Looper.getMainLooper/getThread and
     * Thread.currentThread identity model, not a constructed Handler. */
    int (*observe_main_looper)(void *user, uintptr_t *queue_identity,
                               uint64_t *owner_thread_id,
                               uint64_t *current_thread_id);
} BsDexAndroidHost;

/* Enforce Java's constructor -> Activity.attach -> onCreate ordering. This
 * method never synthesizes Java objects or marks JNI readiness. After a
 * successful original constructor, execute the real attach backend, verify
 * the returned Java objects, and commit the Context snapshot atomically.
 * Returns 1 on witnessed attachment, 0 for a missing/failed service, -1 for
 * an observed Java exception. The VM must remain alive for later onCreate. */
int bs_dex_android_host_complete_attachment(
    BsDexAndroidHost *bridge, BsDexVM *vm, const char **exception);

/* Populate an interpreter callback table. It remains unusable for onCreate
 * until validate_activity independently confirms the real subclass/UI thread.
 * A successful bind is NOT proof of service readiness. */
int bs_dex_android_host_callbacks(BsDexAndroidHost *bridge, BsDexHost *out);

#endif
