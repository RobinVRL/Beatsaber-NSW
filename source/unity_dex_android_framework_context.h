/* Original Quest ContextImpl / Activity.attach boundary.
 *
 * This adapter exposes executable *native* Context services backed by the
 * pinned original APK, live AAssetManager, NWindow, paths, and main ALooper.
 * It also verifies independently executed Java Context getters against those
 * facts. No native C handle is treated as an Android Java object.
 *
 * In particular, a manifest intent-filter is never a delivered launch. The
 * Java verification path requires a witnessed delivered receipt, genuine
 * Activity.attach side effects, original Java method execution and class
 * readback, so production correctly returns zero while they are unavailable.
 */
#ifndef BS_UNITY_DEX_ANDROID_FRAMEWORK_CONTEXT_H
#define BS_UNITY_DEX_ANDROID_FRAMEWORK_CONTEXT_H

#include "unity_dex_horizon_java_resources.h"

typedef struct BsDexFrameworkNativeContext {
    /* Borrowed data from the live, generation-checked attachment owner. */
    const BsDexQuestApplicationInfo *application;
    const BsDexQuestActivityInfo *activity;
    const char *files_dir;
    const char *obb_dir;
    const char *external_files_dir;
    const char *native_library_dir;
    BsDexHorizonObservedConfiguration configuration;
    void *asset_manager; /* real NDK AAssetManager, NOT Java AssetManager */
    void *nwindow;       /* real NWindow, NOT Java Window */
    void *alooper;       /* real ALooper, NOT android.os.Looper */
    uint64_t owner_thread_id;
} BsDexFrameworkNativeContext;

typedef struct BsDexFrameworkJavaAttachment {
    void *activity;      /* original instantiated UnityPlayerGameActivity */
    void *context_impl;  /* actual android.app.ContextImpl */
    void *window;        /* Java Window attached to native NWindow */
    void *main_looper;   /* actual Java Looper of native owner thread */
    void *handler;       /* actual Java Handler of that Looper */
    void *class_loader;  /* original APK ClassLoader */
    void *files_dir;     /* java.io.File */
    void *obb_dir;       /* java.io.File */
    BsDexQuestJavaOwners package_java;
} BsDexFrameworkJavaAttachment;

/* These callbacks must execute and inspect real guest framework methods.
 * verify_attachment inspects actual ContextImpl/Activity/Window registration
 * on every request; it cannot rely only on input pointer equality.
 * invoke_noarg must DISPATCH the indicated Java method, not return a staged
 * pointer by descriptor. Exceptions and unimplemented Java calls return 0.
 * verify_receipt inspects the actual Intent's action, component and category
 * values against the independently reverified native delivery receipt.
 */
typedef struct BsDexFrameworkJavaExecutor {
    void *user;
    BsDexQuestJavaWitness package_witness;
    int (*verify_class)(void *user, void *object, const char *descriptor);
    int (*invoke_noarg)(void *user, void *receiver,
                        const char *declaring_descriptor,
                        const char *method, const char *prototype,
                        void **observed_return);
    int (*verify_file_path)(void *user, void *java_file,
                            const char *observed_absolute_path);
    int (*verify_native_asset_manager)(void *user, void *java_assets,
                                       void *ndk_asset_manager);
    int (*verify_configuration)(void *user, void *java_configuration,
                                const BsDexHorizonObservedConfiguration *native);
    int (*verify_receipt)(void *user, void *java_intent,
                          const BsDexHorizonLaunchReceipt *delivered);
    int (*verify_attachment)(void *user,
                             const BsDexFrameworkJavaAttachment *java,
                             const BsDexFrameworkNativeContext *native);
} BsDexFrameworkJavaExecutor;

typedef struct BsDexAndroidFrameworkContext {
    BsDexHorizonAndroidAttachment *attachment; /* borrowed */
    BsDexHorizonJavaResources *resources;     /* borrowed */
    uint64_t attachment_generation;
    uint64_t resources_generation;
    uint64_t context_generation;
    int initialized;
} BsDexAndroidFrameworkContext;

/* Establish native service identity. No launch receipt or Java readiness is
 * fabricated by init; all native data is re-observed on EVERY query. */
int bs_dex_android_framework_context_init(
    BsDexAndroidFrameworkContext *out,
    BsDexHorizonAndroidAttachment *attachment,
    BsDexHorizonJavaResources *resources);
int bs_dex_android_framework_context_observe(
    const BsDexAndroidFrameworkContext *owner,
    BsDexFrameworkNativeContext *native);
/* Exact typed manifest data, application or Activity metadata scope. */
int bs_dex_android_framework_context_metadata(
    const BsDexAndroidFrameworkContext *owner,
    const char *name, int activity_scope, BsApkManifestValue *value);
/* Actual native APK asset open/read is forwarded to the existing audited
 * AAssetManager owner, including its safe path and stream lifetime checks. */
int bs_dex_android_framework_context_open_asset(
    BsDexAndroidFrameworkContext *owner, const char *relative_name,
    BsDexHorizonAssetStream *stream, uint64_t *length);
/* Returns zero until the host actually delivered an independently witnessed
 * launch; success always rechecks the live external source. */
int bs_dex_android_framework_context_launch(
    const BsDexAndroidFrameworkContext *owner,
    BsDexHorizonLaunchReceipt *delivered);

/* Validate the Java graph produced by a REAL external framework executor
 * AFTER Activity.attach. This does NOT invoke Activity.attach itself or
 * publish Activity/JNI readiness. out remains zeroed on every failure.
 * Success attests an independently executed Context/Activity getter graph;
 * the caller retains all Java references and must continue checking them. */
int bs_dex_android_framework_context_verify_java(
    const BsDexAndroidFrameworkContext *owner,
    const BsDexFrameworkJavaAttachment *candidate,
    const BsDexFrameworkJavaExecutor *executor,
    BsDexAndroidContextAttachment *out);

void bs_dex_android_framework_context_clear(
    BsDexAndroidFrameworkContext *owner);

#endif
