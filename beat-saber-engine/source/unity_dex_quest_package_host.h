/* Quest 1.46 APK-backed Android package model for the original DEX.
 *
 * The exact original AndroidManifest.xml and resources.arsc are verified
 * against staged APK bytes before exposing metadata. Native C snapshots are
 * NOT Java objects. An Android Context, PackageManager, Intent, Resources,
 * ApplicationInfo jobject, Java Bundle and Activity.attach remain unready
 * until independent live-Java witnesses verify their actual identities and
 * values. This module never publishes UnityPlayer.currentActivity.
 */
#ifndef BS_UNITY_DEX_QUEST_PACKAGE_HOST_H
#define BS_UNITY_DEX_QUEST_PACKAGE_HOST_H

#include "unity_apk_manifest.h"
#include "unity_dex_android_context_host.h"
#include <stddef.h>
#include <stdint.h>

#define BS_QUEST_RESOURCES_ARSC_SIZE 258492u
#define BS_QUEST_RESOURCES_ARSC_CRC UINT32_C(0x0b0977d4)

typedef struct BsDexQuestApplicationInfo {
    const char *package_name;       /* PackageItemInfo.packageName */
    uint32_t min_sdk_version;       /* ApplicationInfo.minSdkVersion */
    uint32_t target_sdk_version;    /* ApplicationInfo.targetSdkVersion */
    uint32_t label_resource;        /* PackageItemInfo.labelRes */
    const char *source_dir;         /* verified physical APK path, optional */
    const char *public_source_dir;  /* same observed APK, optional */
    const char *native_library_dir; /* verified staged native ELF directory */
    const char *data_dir;           /* verified staged private directory */
    size_t application_metadata_count;
} BsDexQuestApplicationInfo;

typedef struct BsDexQuestActivityInfo {
    const char *name;               /* exact launch activity class */
    const char *package_name;
    uint32_t theme_resource;        /* ActivityInfo.theme */
    uint32_t config_changes;        /* ActivityInfo.configChanges */
    int enabled;
    int exported;
    size_t activity_metadata_count;
} BsDexQuestActivityInfo;

/* Manifest intent-filter template: this is the application's advertised
 * accepted launch, NOT proof that Activity.getIntent() returned an object.
 * Java Intent.getComponent/getAction/getCategories require separate witness. */
typedef struct BsDexQuestLaunchFilter {
    const char *action;
    const char *package_name;
    const char *activity_class;
    const char *launcher_category;
    const char *vr_category; /* NULL if absent */
    int exported, enabled;
} BsDexQuestLaunchFilter;

typedef struct BsDexQuestPackageHost {
    BsApkManifest manifest;
    BsDexQuestApplicationInfo application;
    BsDexQuestActivityInfo activity;
    BsDexQuestLaunchFilter launch_filter;
    char apk_path[768];
    char native_library_dir[768];
    char data_dir[768];
    uint32_t resources_crc;
    size_t application_metadata_count;
    size_t activity_metadata_count;
    int initialized;
    int native_paths_observed;
} BsDexQuestPackageHost;

/* Load BOTH the verified manifest and original stored resources.arsc from
 * the same original Quest APK; checksum, type and size checks fail closed.
 * Does not resolve arbitrary resources or manufacture a Resources object. */
int bs_dex_quest_package_open(BsDexQuestPackageHost *host,
                              const char *staged_original_apk);
void bs_dex_quest_package_close(BsDexQuestPackageHost *host);

/* Observe native paths only when the staged directory contains actual
 * libmain.so and libunity.so regular files, and the app's data directory
 * exists. These are C paths, not Java File or String instances. */
int bs_dex_quest_package_observe_native_paths(
    BsDexQuestPackageHost *host,
    const char *staged_native_library_dir,
    const char *staged_data_dir);

/* Typed, read-only native model. NULL means unavailable/unverified. */
const BsDexQuestApplicationInfo *bs_dex_quest_package_application(
    const BsDexQuestPackageHost *host);
const BsDexQuestActivityInfo *bs_dex_quest_package_activity(
    const BsDexQuestPackageHost *host);
const BsDexQuestLaunchFilter *bs_dex_quest_package_launch_filter(
    const BsDexQuestPackageHost *host);
/* Returns 1 only for an existing exact-name, correctly typed manifest
 * value, selecting application (0) or Activity (1) metadata. Never coerces
 * a RESOURCE_ID to a boolean/string. A missing Bundle.getBoolean key has a
 * Java default of false, but only after a real Java Bundle is verified. */
int bs_dex_quest_package_metadata(
    const BsDexQuestPackageHost *host, const char *name, int activity,
    BsApkManifestValue *result);

/* Read typed native Bundle source values with Android Bundle defaults.
 * These do NOT invoke a Java Bundle or establish a Java jobject. Missing
 * getString returns success with NULL; missing getBoolean returns false.
 * Existing keys with incompatible types fail closed instead of coercing. */
int bs_dex_quest_package_bundle_get_string(
    const BsDexQuestPackageHost *host, const char *name, int activity,
    const char **string_or_null);
int bs_dex_quest_package_bundle_get_boolean(
    const BsDexQuestPackageHost *host, const char *name, int activity,
    int *value);

/* A real Java bridge may supply these object handles only AFTER
 * independently reading actual platform objects and their fields. The
 * witness callbacks must inspect live Java fields, no pointer-only checks.
 * To accept Intent, inspect the delivered Activity launch Intent, action,
 * component and its categories. The manifest filter is not that proof.
 * verify_bundle must inspect every typed manifest entry for the given
 * application (0) / Activity (1) scope, using Android Bundle semantics.
 */
typedef struct BsDexQuestJavaWitness {
    void *user;
    int (*verify_class)(void *user, void *object, const char *descriptor);
    int (*verify_app_info)(void *user, void *java_application_info,
                           const BsDexQuestApplicationInfo *model);
    int (*verify_activity_info)(void *user, void *java_activity_info,
                                const BsDexQuestActivityInfo *model);
    int (*verify_launch_intent)(void *user, void *java_intent,
                                void *java_component,
                                const BsDexQuestLaunchFilter *model);
    int (*verify_package_manager)(void *user, void *java_package_manager,
                                  void *java_activity_info,
                                  void *java_component);
    int (*verify_bundle)(void *user, void *java_bundle,
                         const BsApkManifest *manifest, int activity_scope);
    int (*verify_string_value)(void *user, void *java_string,
                               const char *expected_utf8);
} BsDexQuestJavaWitness;

typedef struct BsDexQuestJavaOwners {
    void *launch_intent;
    void *component;
    void *package_manager;
    void *application_info;
    void *application_metadata_bundle;
    void *activity_info;
    void *activity_metadata_bundle;
    void *native_library_dir_string;
} BsDexQuestJavaOwners;

/* Verifies a complete, observed Java package/launch owner graph. A fixture
 * may model these callbacks in tests; production requires a live Java VM.
 * An incomplete verification never publishes a partial Java object graph. */
int bs_dex_quest_package_verify_java_owners(
    const BsDexQuestPackageHost *host,
    const BsDexQuestJavaOwners *candidate,
    const BsDexQuestJavaWitness *witness,
    BsDexQuestJavaOwners *accepted);

/* Stage ONLY independently attested package-related object handles into a
 * Context attachment candidate. It leaves activity, Resources, AssetManager,
 * File objects and attachment_evidenced untouched, so
 * bs_dex_android_context_bind_attachment still requires actual attach. */
int bs_dex_quest_package_stage_context(
    const BsDexQuestPackageHost *host,
    const BsDexQuestJavaOwners *candidate_java,
    const BsDexQuestJavaWitness *witness,
    BsDexAndroidContextAttachment *candidate);

#endif
