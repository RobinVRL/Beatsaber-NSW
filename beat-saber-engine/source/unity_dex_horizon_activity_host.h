/* Minimal native Activity class/identity host for the pinned Quest DEX.
 * The class ancestry is parsed from the ORIGINAL DEX, while the Activity
 * lifetime and weak references are owned by this host. All Android
 * application, UI, Context, and Window side effects come from independently
 * observed platform services. An absent service refuses construction or
 * attachment. This module does not emulate the Android system server. */
#ifndef BS_UNITY_DEX_HORIZON_ACTIVITY_HOST_H
#define BS_UNITY_DEX_HORIZON_ACTIVITY_HOST_H

#include "unity_dex_android_host.h"
#include <stddef.h>
#include <stdint.h>

typedef struct BsDexHorizonActivityServices {
    void *user;
    /* Must verify the caller is on the real, attached app UI thread. */
    int (*on_ui_thread)(void *user);
    /* Execute the native Activity-base initialization against this lifetime
     * owner. Must actually install the platform Activity event/UI state. */
    int (*construct_base)(void *user, void *activity);
    /* Perform Activity.attach and return the actual observed native Window
     * plus the Java getters read back from the same Activity. */
    int (*attach)(void *user, void *activity, void **window,
                  BsDexAndroidContextAttachment *observed,
                  const char **exception);
    /* Independently verify the Horizon NWindow and the committed Context
     * owner/Window linkage, not just equality with the supplied pointers. */
    int (*valid_nwindow)(void *user, void *window);
    int (*verify_attachment)(void *user, void *activity, void *window,
                             const BsDexAndroidContextAttachment *observed);
    /* Inspect actual Java/platform resource type and live identity. */
    int (*verify_external)(void *user, const char *descriptor, void *object);
    /* Release native UI/window linkage and event registrations. Return 1 only
     * when there is no remaining platform attachment. */
    int (*detach)(void *user, void *activity, void *window);
} BsDexHorizonActivityServices;

typedef struct BsDexHorizonActivity {
    uint64_t generation;
    void *window;
    int constructed;
    int attached;
    int alive;
} BsDexHorizonActivity;

typedef struct BsDexHorizonActivityHost {
    BsDexHorizonActivityServices platform;
    BsDexAndroidContextAttachment observed;
    BsDexHorizonActivity activity;
    uint64_t next_generation;
    size_t weak_outstanding;
    int original_classes_verified;
    /* Non-owning; caller destroys the DEX VM before this host. */
    BsDexAndroidContextHost *context;
} BsDexHorizonActivityHost;

/* Validate the original pinned DEX and the EXACT 7-step Java inheritance
 * chain to android.app.Activity, before any C object can be constructed. */
int bs_dex_horizon_activity_init(BsDexHorizonActivityHost *host,
                                  const uint8_t *dex, size_t dex_size,
                                  const BsDexHorizonActivityServices *platform);

/* Populate the constructor, attach, metadata and weak-liveness callbacks on
 * an existing composed host. Other bridge methods remain caller-owned.
 * This does not start the DEX or automatically supply SurfaceView services. */
int bs_dex_horizon_activity_bind(BsDexHorizonActivityHost *host,
                                  BsDexAndroidHost *bridge,
                                  BsDexAndroidContextHost *context);

/* Surface layer callbacks; use with BsDexAndroidSurfaceServices.user=host,
 * alongside an independently functional View/NWindow factory and dispatch. */
int bs_dex_horizon_activity_on_ui_thread(void *user);
int bs_dex_horizon_activity_context_matches(void *user, void *context);

/* Witnessed native Activity destruction (after platform detach succeeds).
 * Outstanding weak handles become empty immediately, but stay allocated
 * until the DEX VM releases them. No new Activity may be constructed while
 * old weak handles remain outstanding. */
int bs_dex_horizon_activity_invalidate(BsDexHorizonActivityHost *host);

/* Only call after the VM is destroyed and its weak handles released. Returns
 * 0 on failed native detach; the live state remains available for retry. */
int bs_dex_horizon_activity_release(BsDexHorizonActivityHost *host);

#endif
