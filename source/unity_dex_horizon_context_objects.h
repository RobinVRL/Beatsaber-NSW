/* Horizon-backed native data owners for original Quest Context dependencies.
 *
 * The original GameActivity.onCreate asks Context.getFilesDir/getObbDir/
 * getExternalFilesDirs, ApplicationInfo.nativeLibraryDir and Resources
 * Configuration. These native objects carry ONLY independently verified
 * filesystem, applet/window and Horizon Settings values. They are NOT JNI
 * java.io.File, Resources, Configuration, Intent or Context instances.
 *
 * They must not be passed as Java jobjects to BsDexAndroidContextHost or
 * used to assert Activity.attach or UnityPlayer.currentActivity.
 */
#ifndef BS_UNITY_DEX_HORIZON_CONTEXT_OBJECTS_H
#define BS_UNITY_DEX_HORIZON_CONTEXT_OBJECTS_H

#include "unity_dex_quest_package_host.h"
#include "unity_dex_horizon_java_runtime.h"
#include <stddef.h>
#include <stdint.h>

typedef enum BsDexHorizonContextFileKind {
    BS_HCTX_FILES_DIR = 1,
    BS_HCTX_OBB_DIR = 2,
    BS_HCTX_EXTERNAL_FILES_DIR = 3
} BsDexHorizonContextFileKind;

typedef struct BsDexHorizonContextConfig {
    uint32_t window_width_px;
    uint32_t window_height_px;
    char horizon_language_tag[9];
    uint64_t observation_number;
} BsDexHorizonContextConfig;

typedef struct BsDexHorizonLaunchReceipt {
    /* Values from the actually delivered launch event, not the APK filter. */
    const char *action;
    const char *package_name;
    const char *activity_class;
    int launcher_category;
    int vr_category;
    void *source_token;           /* live native launch source */
} BsDexHorizonLaunchReceipt;

/* Read-only facts about the actual Horizon process that owns this applet.
 * applet_type is the native libnx AppletType numeric value, not an Android
 * Activity class or an Intent action. argv records *availability only*:
 * envGetArgv does not supply a trusted Android Intent, component, or bounds
 * suitable for interpreting arbitrary startup bytes as launch metadata.
 * Zero-valued ids or fields are faithfully reported as observed. */
typedef struct BsDexHorizonStartupObservation {
    int applet_type;
    uint64_t applet_resource_user_id;
    int loader_has_argv;
    int loader_has_info;
    int loader_is_nso;
} BsDexHorizonStartupObservation;

typedef struct BsDexHorizonContextNative {
    void *user;
    int (*on_owner_thread)(void *user);
    int (*native_activity_base_ready)(void *user);
    int (*read_window_pixels)(void *user,
                              uint32_t *width, uint32_t *height);
    int (*read_horizon_language)(void *user, char tag[9]);
    /* Optional: must reverify the exact delivered event against a live
     * external launch source on EVERY call. No Horizon native Android Intent
     * source exists in the current loader, so production leaves this NULL. */
    int (*verify_launch_receipt)(void *user,
                                 const BsDexHorizonLaunchReceipt *receipt);
    /* Optional OS startup observation. No field authorizes a Java Intent. */
    int (*observe_startup)(void *user, BsDexHorizonStartupObservation *out);
} BsDexHorizonContextNative;

typedef struct BsDexHorizonContextObjects BsDexHorizonContextObjects;

typedef struct BsDexHorizonContextFile {
    const BsDexHorizonContextObjects *owner;
    BsDexHorizonContextFileKind kind;
    uint64_t generation;
    char absolute_path[768];
} BsDexHorizonContextFile;

struct BsDexHorizonContextObjects {
    BsDexHorizonContextNative native;
    const BsDexQuestPackageHost *package; /* borrowed original APK facts */
    BsDexHorizonContextFile files, obb, external_files;
    BsDexHorizonContextConfig configuration;
    BsDexHorizonLaunchReceipt delivered_launch;
    char launch_action[64];
    char launch_package_name[128];
    char launch_activity_class[256];
    uint64_t generation;
    int initialized;
    int launch_observed;
};

/* Names of the observed staged SD directories must be passed explicitly.
 * This does not mkdir, write, or fabricate an Android file. It requires an
 * observed native Activity base, NWindow and Horizon language source. */
int bs_dex_horizon_context_objects_init(
    BsDexHorizonContextObjects *out, const BsDexQuestPackageHost *package,
    const BsDexHorizonContextNative *native,
    const char *files_dir, const char *obb_dir,
    const char *external_files_dir);

/* Production native callbacks use real Horizon Settings system language,
 * NWindow dimensions, and the previously observed main thread/Activity base
 * lifetime. No Android Java Context/Resources/Intent is exposed. */
typedef struct BsDexHorizonContextProduction {
    const BsDexHorizonJavaRuntime *native_activity;
    const BsDexHorizonServices *horizon;
} BsDexHorizonContextProduction;

BsDexHorizonContextNative bs_dex_horizon_context_production_callbacks(
    BsDexHorizonContextProduction *production);

/* Reobserve real applet identity and loader argument availability. This
 * works without a delivered Android launch; it deliberately never returns
 * a BsDexHorizonLaunchReceipt or constructs a Java object. */
int bs_dex_horizon_context_observe_startup(
    const BsDexHorizonContextObjects *owner,
    BsDexHorizonStartupObservation *out);

/* Recheck native file type and live Activity generation on each read.
 * getAbsolutePath is backed by the stored absolute native path; metadata is
 * not a fully constructed JVM java.io.File until a Java runtime backs it. */
const BsDexHorizonContextFile *bs_dex_horizon_context_file(
    const BsDexHorizonContextObjects *owner, BsDexHorizonContextFileKind kind);
int bs_dex_horizon_context_get_absolute_path(
    const BsDexHorizonContextFile *file, const char **path);
int bs_dex_horizon_context_native_library_dir(
    const BsDexHorizonContextObjects *owner, const char **path);
int bs_dex_horizon_context_read_configuration(
    BsDexHorizonContextObjects *owner, BsDexHorizonContextConfig *snapshot);

/* Records only an independently witnessed REAL native launch receipt. This
 * may be useful if a future launcher exposes explicit Android launch args.
 * No native launch observation currently exists in production; these return
 * zero until a real verifier is connected. Does not create Java Intent. */
int bs_dex_horizon_context_bind_launch(
    BsDexHorizonContextObjects *owner,
    const BsDexHorizonLaunchReceipt *receipt);
int bs_dex_horizon_context_read_launch(
    const BsDexHorizonContextObjects *owner,
    BsDexHorizonLaunchReceipt *receipt);

/* Deliberately unavailable: native-backed paths/config are insufficient for
 * a Java Activity.attach readback, Resources, AssetManager, or Looper. */
int bs_dex_horizon_context_java_attachment_available(
    const BsDexHorizonContextObjects *owner);
int bs_dex_horizon_context_objects_release(BsDexHorizonContextObjects *owner);

#endif
