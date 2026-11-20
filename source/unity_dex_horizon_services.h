/* Production Horizon observations for the pinned Quest Activity host.
 * Native services can verify the owning applet thread, a live libnx NWindow,
 * mapped Quest AssetManager bytes, and provisioned SD directories. Android
 * Activity.<init>, Activity.attach, Intent, Resources, Java Looper, and Java
 * view hierarchy are NOT supplied by libnx or the current JNI shim.
 *
 * This adapter refuses those Java services until they exist. It cannot grant
 * Java readiness or advance original DEX construction by itself. */
#ifndef BS_UNITY_DEX_HORIZON_SERVICES_H
#define BS_UNITY_DEX_HORIZON_SERVICES_H

#include "unity_dex_horizon_activity_host.h"

typedef struct BsDexHorizonNativeObservations {
    /* Snapshot owned by the observing applet thread; window and JNI tokens
     * are borrowed. No fabricated Java objects are allocated or retained. */
    void *nwindow;
    void *jni_asset_manager;
    void *native_asset_manager;
    void *observed_jni_surface;
    uint64_t applet_thread_id;
    uint32_t width_px, height_px;
    int applet_main_thread_observed;
    int jni_thread_attached;
    int native_window_valid;
    int native_asset_opened;
    int files_dir_exists;
    int obb_dir_exists;
    int external_files_dir_exists;
    int native_jni_surface_verified;
} BsDexHorizonNativeObservations;

typedef struct BsDexHorizonServices {
    BsDexHorizonNativeObservations observed;
} BsDexHorizonServices;

/* Call on the same libnx main applet/event-loop thread that runs startup,
 * after SD staging and bs_jni_init(). Rereads current thread, window,
 * assets and directories from production services, and records only observed
 * successes. Returns 1 when every native prerequisite is backed. Returns 0
 * on non-Switch host builds and on incomplete native observations. */
int bs_dex_horizon_services_observe(BsDexHorizonServices *services);

/* Independently verify the CURRENT libnx NWindow and owner thread against
 * the snapshot. An applet focus/window change can revoke these observations. */
int bs_dex_horizon_services_on_applet_thread(const BsDexHorizonServices *services);
int bs_dex_horizon_services_valid_nwindow(const BsDexHorizonServices *services,
                                          void *window);

/* Optional borrowed native JNI Surface token: verify its libnx NWindow via
 * the current JNI environment. Never creates a Java SurfaceView or a child
 * view. Returns 0 for invalid/null/stale/non-Switch surfaces. */
int bs_dex_horizon_services_observe_surface(BsDexHorizonServices *services,
                                            void *jni_surface);

/* A complete native snapshot is independent of Android Java readiness.
 * No Java object may be fabricated from it. */
int bs_dex_horizon_services_native_ready(const BsDexHorizonServices *services);

/* Produces the production BsDexHorizonActivityServices callback contract.
 * Native on_ui_thread and valid_nwindow are backed. Base constructor,
 * Activity.attach, Java type verification and attachment verification return
 * 0 until corresponding executable Java/Horizon services are implemented.
 * detach returns success only if no Java attachment was ever created. */
BsDexHorizonActivityServices bs_dex_horizon_services_callbacks(
    BsDexHorizonServices *services);

#endif
