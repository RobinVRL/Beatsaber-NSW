/* Bounded Android Java View/SurfaceView/SurfaceHolder adapter for the
 * verified original Quest DEX. Native surfaces are ALWAYS observed via a
 * live Horizon NWindow / JNI Surface token, never invented by this host.
 *
 * The caller owns this host and keeps it alive until the DEX VM is destroyed.
 * The interpreter passes its opaque VM-owned Java references to the platform
 * invoke callback. This module does not write UnityPlayer static fields or
 * create an Android Activity and does not start a Vulkan device.
 */
#ifndef BS_UNITY_DEX_ANDROID_SURFACE_HOST_H
#define BS_UNITY_DEX_ANDROID_SURFACE_HOST_H

#include "unity_dex_vm.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BS_DEX_ANDROID_MAX_VIEWS 12
#define BS_DEX_ANDROID_MAX_HOLDER_CALLBACKS 16

/* Every service returns 1 only AFTER performing the named real operation;
 * 0 means unavailable/failed. None is an authorization to claim guest
 * Activity creation or Android Java class initializers have happened.
 *
 * construct_view must register the actual View/SurfaceView in the native
 * host's UI hierarchy with the supplied actual Context and VM reference.
 * For a SurfaceView it must return the live NWindow that belongs to that
 * specific View; using an unrelated default window is prohibited.
 * remove_view rolls back construction and detaches the view from its parent.
 * On Switch the JNI functions below must validate the exact NWindow that
 * construct_view supplied. Geometry is measured in pixels, not Android dp.
 *
 * Callbacks and formats must be delivered to a functioning event/UI service.
 * A callback returning success without retaining/delivering events is not a
 * valid implementation. No Android SurfaceHolder.Callback is dispatched
 * merely because it has been registered.
 */
typedef struct BsDexAndroidSurfaceServices {
    void *user;
    int (*context_matches_activity)(void *user, void *actual_context);
    int (*on_ui_thread)(void *user);
    int (*construct_view)(void *user, void *actual_context,
                          uintptr_t dex_view_identity, int is_surface_view,
                          void **observed_nwindow);
    int (*remove_view)(void *user, uintptr_t dex_view_identity,
                       void *observed_nwindow);
    int (*get_window_dimensions)(void *user, void *observed_nwindow,
                                 uint32_t *width, uint32_t *height);
    void *(*surface_from_nwindow)(void *user, void *observed_nwindow);
    void *(*verified_nwindow_from_surface)(void *user, void *surface);
    void (*revoke_surface)(void *user, void *surface);
    /* Must prove the actual Java object implements SurfaceHolder.Callback;
     * string-based class-name guessing or accepting every non-null ref fails. */
    int (*is_holder_callback)(void *user, BsDexVM *vm, BsDexValue callback);
    int (*register_holder_callback)(void *user, uintptr_t dex_view_identity,
                                    BsDexValue callback);
    int (*set_holder_format)(void *user, uintptr_t dex_view_identity,
                             int32_t format);
    int (*set_z_order_on_top)(void *user, uintptr_t dex_view_identity,
                              int is_on_top);
    /* For a DEX-owned subclass, return the constructed Java view for this
     * identity. The VM independently checks its concrete class before binding. */
    void *(*constructed_java_view)(void *user, uintptr_t dex_view_identity);
} BsDexAndroidSurfaceServices;

typedef struct BsDexAndroidSurfaceView {
    BsDexValue receiver;
    BsDexValue context;
    void *native_window;
    void *jni_surface;
    void *activity_context;
    uint32_t width_px;
    uint32_t height_px;
    int32_t last_format;
    BsDexValue holder_callbacks[BS_DEX_ANDROID_MAX_HOLDER_CALLBACKS];
    size_t callback_count;
    unsigned reserved : 1;
    unsigned initialized : 1;
    unsigned is_surface_view : 1;
    unsigned format_set : 1;
    unsigned z_order_set : 1;
    unsigned z_order_on_top : 1;
    unsigned detach_pending : 1;
} BsDexAndroidSurfaceView;

typedef struct BsDexAndroidSurfaceHolder {
    struct BsDexAndroidSurfaceHost *owner;
    BsDexAndroidSurfaceView *view;
} BsDexAndroidSurfaceHolder;

typedef struct BsDexAndroidSurfaceHost {
    BsDexAndroidSurfaceServices services;
    BsDexVM *attached_vm;
    BsDexAndroidSurfaceView views[BS_DEX_ANDROID_MAX_VIEWS];
    BsDexAndroidSurfaceHolder holders[BS_DEX_ANDROID_MAX_VIEWS];
    size_t view_count;
    /* Counts attest only backend operations, not full Android lifecycle. */
    size_t observed_surface_constructors;
    size_t observed_base_view_constructors;
    size_t observed_holder_callbacks;
} BsDexAndroidSurfaceHost;

void bs_dex_android_surface_host_init(BsDexAndroidSurfaceHost *host,
                                      const BsDexAndroidSurfaceServices *services);
/* Revoke JNI Surface tokens and detach native Views; return 0 if teardown
 * cannot be completed by the real platform. Destroy the DEX VM first. */
int bs_dex_android_surface_host_reset(BsDexAndroidSurfaceHost *host);

/* Composable with BsDexHost.new_platform_object/invoke_platform:
 *
 * BsDexHost dex_host = {
 *    .user = &combined,
 *    .new_platform_object = my_dispatch_new,
 *    .invoke_platform = my_dispatch_invoke,
 *    ...
 * };
 * my_dispatch_invoke calls bs_dex_android_surface_invoke_platform for
 * recognized platform classes, then calls another host for other classes.
 * These callbacks return 0 for unknown methods (no fabricated success).
 */
int bs_dex_android_surface_new_platform_object(void *user, BsDexVM *vm,
                                               const char *class_descriptor,
                                               BsDexValue *actual_object,
                                               const char **exception);
int bs_dex_android_surface_invoke_platform(
    void *user, BsDexVM *vm, const char *declaring_class,
    const char *method_name, const char *prototype,
    uint8_t invoke_opcode, const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception);

/* Existing JNI bridge/Horizon NWindow facilities, enabled on Switch. These
 * functions are safe to assign to the last four surface-service callbacks
 * and return failure on non-Switch host tests. construct_view is deliberately
 * not provided: the integrator must create/bind a real UI child view. */
int bs_dex_android_horizon_window_dimensions(void *user, void *window,
                                              uint32_t *width, uint32_t *height);
void *bs_dex_android_jni_surface_from_nwindow(void *user, void *window);
void *bs_dex_android_jni_verified_window(void *user, void *surface);
void bs_dex_android_jni_revoke_surface(void *user, void *surface);

#ifdef __cplusplus
}
#endif
#endif
