/* Native child-view registration for the ORIGINAL Quest SurfaceView.<init>.
 *
 * A libnx applet NWindow is a parent display surface, not an Android View or
 * an independent child SurfaceView. This module records child registrations
 * only when a separate UI provider performs and independently verifies them.
 * The production provider is currently absent on Horizon: initialization
 * succeeds, but construct_view and Java methods return unavailable.
 *
 * The backend owns the registration records. The UI provider owns actual
 * children and their NWindows; the SurfaceHost owns JNI Surface tokens. The
 * caller destroys the VM, then resets SurfaceHost, then resets this backend.
 */
#ifndef BS_UNITY_DEX_HORIZON_SURFACE_VIEW_BACKEND_H
#define BS_UNITY_DEX_HORIZON_SURFACE_VIEW_BACKEND_H

#include "unity_dex_android_surface_host.h"
#include "unity_dex_horizon_activity_host.h"
#include "unity_dex_horizon_services.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN BS_DEX_ANDROID_MAX_VIEWS

/* The provider must own an independently registered child in the Activity's
 * real UI hierarchy, not the applet default window or a borrowed window from
 * another view. A successful create must return both an owned child handle and
 * its own live NWindow (for SurfaceView). For a plain View, return NULL window.
 * A failed create must undo any partial side effects before returning zero.
 *
 * verify_attached/verify_detached must independently query the actual UI
 * hierarchy. Removal must detach callbacks and release child resources, and
 * repeated removal after completion must be safe during recovery. Java view
 * callbacks require actual class identity/lifetime evidence, never a C token.
 * Callback registration must retain a guest callback for real event dispatch;
 * format and Z-order must be changed by the real UI/compositor service.
 */
typedef struct BsDexHorizonSurfaceViewChildOps {
    void *user;
    int (*create)(void *user, void *context, void *parent_window,
                  uintptr_t dex_identity, int is_surface_view,
                  void **child_handle, void **child_nwindow);
    int (*verify_attached)(void *user, void *context, void *parent_window,
                           uintptr_t dex_identity, void *child_handle,
                           void *child_nwindow);
    int (*remove)(void *user, void *context, void *parent_window,
                  uintptr_t dex_identity, void *child_handle,
                  void *child_nwindow);
    int (*verify_detached)(void *user, void *context, void *parent_window,
                           uintptr_t dex_identity, void *child_handle,
                           void *child_nwindow);
    void *(*observed_java_view)(void *user, uintptr_t dex_identity,
                                void *child_handle);
    int (*verify_java_view)(void *user, uintptr_t dex_identity,
                            void *child_handle, void *java_view);
    int (*is_holder_callback)(void *user, BsDexVM *vm, BsDexValue callback);
    int (*register_holder_callback)(void *user, void *child_handle,
                                    uintptr_t dex_identity, BsDexValue callback);
    int (*set_holder_format)(void *user, void *child_handle,
                             uintptr_t dex_identity, int32_t format);
    int (*set_z_order_on_top)(void *user, void *child_handle,
                              uintptr_t dex_identity, int is_on_top);
} BsDexHorizonSurfaceViewChildOps;

/* Injectable ROOT observations for native-only host tests. These must be
 * independently backed on any non-test platform. Production initialization
 * derives all four directly from the live Activity and Horizon services. */
typedef struct BsDexHorizonSurfaceViewRootHooks {
    void *user;
    int (*on_ui_thread)(void *user);
    int (*matches_attached_activity)(void *user, void *context);
    int (*get_parent_window)(void *user, void *context, void **parent);
    int (*valid_parent_window)(void *user, void *parent);
} BsDexHorizonSurfaceViewRootHooks;

typedef struct BsDexHorizonSurfaceViewChild {
    uintptr_t identity;
    void *context;
    void *parent_window;
    void *child_handle;
    void *child_nwindow;
    void *java_view;
    unsigned active : 1;
    unsigned is_surface_view : 1;
    unsigned removal_pending : 1;
    unsigned remove_completed : 1;
} BsDexHorizonSurfaceViewChild;

typedef struct BsDexHorizonSurfaceViewBackend {
    BsDexHorizonActivityHost *activity;  /* borrowed */
    BsDexHorizonServices *horizon;       /* borrowed */
    BsDexHorizonSurfaceViewRootHooks root;
    BsDexHorizonSurfaceViewChildOps child_ops;
    BsDexHorizonSurfaceViewChild children[BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN];
    unsigned production : 1;
} BsDexHorizonSurfaceViewBackend;

/* Uses observed applet thread and verified attached Activity/NWindow.
 * No default native child factory exists; child_ops may be NULL, making the
 * adapter deterministically unavailable until real UI services are added. */
int bs_dex_horizon_surface_view_backend_init(
    BsDexHorizonSurfaceViewBackend *backend,
    BsDexHorizonActivityHost *activity, BsDexHorizonServices *horizon,
    const BsDexHorizonSurfaceViewChildOps *child_ops);

/* For isolated host tests of a real provider's contract. This API makes no
 * claim about any physical Switch Activity or Java runtime. */
int bs_dex_horizon_surface_view_backend_init_with_hooks(
    BsDexHorizonSurfaceViewBackend *backend,
    const BsDexHorizonSurfaceViewRootHooks *root,
    const BsDexHorizonSurfaceViewChildOps *child_ops);

/* Independently testable native ownership/registration subset. Success
 * proves the provider registered and independently verified a child, but
 * does not prove Java View.<init>, Java class metadata, or JNI Surface. */
int bs_dex_horizon_surface_view_register_native(
    BsDexHorizonSurfaceViewBackend *backend, void *context,
    uintptr_t dex_identity, int is_surface_view, void **observed_nwindow);
int bs_dex_horizon_surface_view_remove_native(
    BsDexHorizonSurfaceViewBackend *backend, uintptr_t dex_identity,
    void *observed_nwindow);
int bs_dex_horizon_surface_view_validate_native(
    BsDexHorizonSurfaceViewBackend *backend, uintptr_t dex_identity,
    void *observed_nwindow);
/* Retry removal of any children left attached after earlier failures. */
int bs_dex_horizon_surface_view_backend_reset(
    BsDexHorizonSurfaceViewBackend *backend);

/* BsDexAndroidSurfaceHost integration. The JNI roundtrip uses the existing
 * libnx/JNI helpers and is verified again by the SurfaceHost. The production
 * constructor cannot complete without both real child and Java View backing.
 * The caller must keep backend alive as long as the SurfaceHost is alive. */
BsDexAndroidSurfaceServices bs_dex_horizon_surface_view_services(
    BsDexHorizonSurfaceViewBackend *backend);

#ifdef __cplusplus
}
#endif
#endif
