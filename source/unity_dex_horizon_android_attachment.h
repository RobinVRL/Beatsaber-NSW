/* Original Quest Activity.attach dependency graph on Horizon.
 *
 * The owned objects in this module are C DATA OWNERS, with precise class-like
 * kinds and generation. They are never JNI jobjects. The Quest APK manifest,
 * its resource-table identity, actual native NWindow/assets, on-disk File
 * paths and Horizon Configuration can be backed today. Android Java
 * ContextImpl, Intent, Resources, AssetManager, Looper, Handler and real
 * Activity.attach still require independent executable Java services.
 *
 * Prepared native objects may be queried without an Android attachment.
 * The Activity host attach callback must return 0 unless a real Java backend
 * constructs, retains, independently verifies and attaches every required
 * Java object. There is no synthetic "attached" completion path.
 */
#ifndef BS_UNITY_DEX_HORIZON_ANDROID_ATTACHMENT_H
#define BS_UNITY_DEX_HORIZON_ANDROID_ATTACHMENT_H

#include "unity_dex_horizon_activity_host.h"
#include "unity_dex_horizon_context_objects.h"
#include "unity_dex_horizon_main_looper.h"
#include "unity_dex_horizon_services.h"
#include <stddef.h>
#include <stdint.h>

typedef enum BsDexHorizonAttachmentKind {
    BS_HA_APPLICATION_INFO = 1,
    BS_HA_ACTIVITY_INFO,
    BS_HA_APPLICATION_METADATA,
    BS_HA_ACTIVITY_METADATA,
    BS_HA_LAUNCH_FILTER,
    BS_HA_LAUNCH_RECEIPT,
    BS_HA_FILES_DIR,
    BS_HA_OBB_DIR,
    BS_HA_EXTERNAL_FILES_DIR,
    BS_HA_NATIVE_LIBRARY_DIR,
    BS_HA_CONFIGURATION,
    BS_HA_RESOURCE_TABLE,
    BS_HA_NATIVE_ASSET_MANAGER,
    BS_HA_NATIVE_WINDOW,
    BS_HA_NATIVE_LOOPER,
    BS_HA_OBJECT_COUNT
} BsDexHorizonAttachmentKind;

typedef struct BsDexHorizonAndroidAttachment BsDexHorizonAndroidAttachment;

/* Borrowed-by-value typed handle; generations invalidate old snapshots.
 * Its payload belongs to the module, not to a guest Java heap. */
typedef struct BsDexHorizonAttachmentRef {
    const BsDexHorizonAndroidAttachment *owner;
    uint64_t generation;
    BsDexHorizonAttachmentKind kind;
} BsDexHorizonAttachmentRef;

/* Fresh, independent physical NWindow and NDK AAssetManager observations;
 * production implementations validate the live applet owner/asset bytes.
 * These callbacks must be real platform operations, not pointer-only checks. */
typedef struct BsDexHorizonAttachmentNative {
    void *user;
    int (*window)(void *user, void **actual_nwindow);
    int (*asset_manager)(void *user, void **actual_ndk_manager);
} BsDexHorizonAttachmentNative;

/* Only actual guest/Android framework execution may supply this backend.
 * execute_attach must construct ContextImpl and the Java class instances,
 * run the real Activity.attach side effects, retain their Java ownership,
 * then independently read the actual Activity getters into observed. The
 * returned token denotes that real Java attachment until detach succeeds.
 *
 * verify_main_java_loop must establish the observed live Java Looper AND
 * Handler corresponding to the previously verified native owner thread.
 * verify_attachment must reread the Activity/Window/Context state on EVERY
 * call. verify_type checks real class identity and object liveness, never a
 * native kind marker or pointer match alone.
 */
typedef struct BsDexHorizonAttachmentJava {
    void *user;
    BsDexQuestJavaWitness package_witness;
    int (*verify_main_java_loop)(void *user, void *activity,
                                 const BsDexHorizonMainLooper *native_owner);
    int (*execute_attach)(void *user, void *activity, void *nwindow,
                          const BsDexHorizonAndroidAttachment *native_model,
                          const BsDexHorizonLaunchReceipt *delivered,
                          BsDexQuestJavaOwners *package_java,
                          BsDexAndroidContextAttachment *observed,
                          void **attached_token, const char **exception);
    int (*verify_type)(void *user, const char *descriptor, void *object);
    int (*verify_attachment)(void *user, void *activity, void *nwindow,
                              void *attached_token,
                              const BsDexAndroidContextAttachment *observed);
    int (*detach)(void *user, void *activity, void *nwindow,
                  void *attached_token);
} BsDexHorizonAttachmentJava;

typedef struct BsDexHorizonAttachmentSnapshot BsDexHorizonAttachmentSnapshot;

struct BsDexHorizonAndroidAttachment {
    const BsDexQuestPackageHost *package;            /* borrowed */
    BsDexHorizonContextObjects *context;             /* borrowed, live */
    BsDexHorizonJavaRuntime *runtime;                /* borrowed, live */
    BsDexHorizonMainLooper *looper;                  /* borrowed, live */
    BsDexHorizonAttachmentNative native;
    BsDexHorizonAttachmentJava java;
    BsDexHorizonActivityHost *activity_host;         /* borrowed, after bind */
    BsDexHorizonActivityServices previous;          /* original constructor */
    BsDexHorizonAttachmentSnapshot *snapshot;       /* owned typed C data */
    BsDexAndroidContextAttachment actual_readback;   /* only actual Java */
    void *attached_token;                            /* owned by Java backend */
    void *attached_window;
    uint64_t generation;
    int adapter_installed;
    int java_attached;
    int java_verified;
};

/* Build/own a typed native service graph after original native Activity
 * construction. No Java backend is required for this step. The graph owns a
 * full copy of verified manifest metadata, native paths and configuration;
 * external owners must remain alive through release. */
int bs_dex_horizon_android_attachment_prepare(
    BsDexHorizonAndroidAttachment *adapter,
    const BsDexQuestPackageHost *package,
    BsDexHorizonContextObjects *context,
    BsDexHorizonJavaRuntime *runtime,
    BsDexHorizonMainLooper *looper,
    const BsDexHorizonAttachmentNative *native);
int bs_dex_horizon_android_attachment_release(
    BsDexHorizonAndroidAttachment *adapter);

/* Exact, generation-checked native object readback. These never produce Java
 * references. Every access rechecks native owner/window/assets/files.
 * Launch receipt exists only after a witnessed delivered launch event. */
int bs_dex_horizon_android_attachment_ref(
    const BsDexHorizonAndroidAttachment *adapter,
    BsDexHorizonAttachmentKind kind,
    BsDexHorizonAttachmentRef *out);
int bs_dex_horizon_android_attachment_application(
    BsDexHorizonAttachmentRef ref,
    const BsDexQuestApplicationInfo **value);
int bs_dex_horizon_android_attachment_activity(
    BsDexHorizonAttachmentRef ref,
    const BsDexQuestActivityInfo **value);
/* Manifest intent-filter template (accepted launch), not a delivered Intent. */
int bs_dex_horizon_android_attachment_launch_filter(
    BsDexHorizonAttachmentRef ref,
    const BsDexQuestLaunchFilter **value);
int bs_dex_horizon_android_attachment_metadata(
    BsDexHorizonAttachmentRef ref, const char *key,
    BsApkManifestValue *value);
int bs_dex_horizon_android_attachment_path(
    BsDexHorizonAttachmentRef ref, const char **absolute_path);
int bs_dex_horizon_android_attachment_configuration(
    BsDexHorizonAttachmentRef ref, BsDexHorizonContextConfig *value);
int bs_dex_horizon_android_attachment_resource_table(
    BsDexHorizonAttachmentRef ref, size_t *size, uint32_t *crc);
int bs_dex_horizon_android_attachment_native_handle(
    BsDexHorizonAttachmentRef ref, void **handle);
int bs_dex_horizon_android_attachment_launch(
    BsDexHorizonAttachmentRef ref, BsDexHorizonLaunchReceipt *delivered);

/* Install on an ORIGINAL DEX class-verified BsDexHorizonActivityHost only
 * AFTER bs_dex_horizon_java_runtime_bind_verified_host has succeeded and
 * BEFORE any Java attach is attempted. Original native Activity constructor,
 * owner-thread and NWindow/detach callbacks are forwarded unchanged.
 * Changes host.platform IN MEMORY; no existing implementation files edited.
 */
int bs_dex_horizon_android_attachment_install(
    BsDexHorizonAndroidAttachment *adapter,
    BsDexHorizonActivityHost *verified_host);

/* Register independently implemented Java services. NULL/missing services
 * or absent native delivered launch/Java Handler+Looper cause attach to
 * return 0 with an empty readback and no claimed attachment. */
int bs_dex_horizon_android_attachment_set_java(
    BsDexHorizonAndroidAttachment *adapter,
    const BsDexHorizonAttachmentJava *java);
int bs_dex_horizon_android_attachment_java_attached(
    const BsDexHorizonAndroidAttachment *adapter);

/* Production native observation callbacks from the existing libnx Horizon
 * services. No fake NDK AssetManager or NWindow is ever allocated. */
BsDexHorizonAttachmentNative
bs_dex_horizon_android_attachment_production_native(
    BsDexHorizonServices *horizon);

#endif
