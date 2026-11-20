#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "unity_dex_quest_package_host.h"
#include "unity_apk_stored_entry.h"
#include "unity_apk_dex_loader.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define BS_QUEST_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#define BS_QUEST_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#else
#define BS_QUEST_ISREG(mode) S_ISREG(mode)
#define BS_QUEST_ISDIR(mode) S_ISDIR(mode)
#endif

#define QUEST_MAIN_ACTION "android.intent.action.MAIN"
#define QUEST_LAUNCHER "android.intent.category.LAUNCHER"
#define QUEST_VR "com.oculus.intent.category.VR"

#define JAVA_INTENT "Landroid/content/Intent;"
#define JAVA_COMPONENT "Landroid/content/ComponentName;"
#define JAVA_MANAGER "Landroid/content/pm/PackageManager;"
#define JAVA_APPINFO "Landroid/content/pm/ApplicationInfo;"
#define JAVA_ACTIVITYINFO "Landroid/content/pm/ActivityInfo;"
#define JAVA_BUNDLE "Landroid/os/Bundle;"
#define JAVA_STRING "Ljava/lang/String;"

static int observed_regular_file(const char *path) {
    struct stat metadata;
    return path && stat(path, &metadata) == 0 &&
           BS_QUEST_ISREG(metadata.st_mode);
}

static int observed_directory(const char *path) {
    struct stat metadata;
    return path && stat(path, &metadata) == 0 &&
           BS_QUEST_ISDIR(metadata.st_mode);
}

static int exact_path_copy(char *dst, size_t cap, const char *path) {
    if (!dst || !cap || !path) return 0;
    size_t len = strlen(path);
    if (!len || len >= cap) return 0;
    memcpy(dst, path, len + 1);
    return 1;
}

static int acceptable_apk_path(const char *path) {
    if (!path || strstr(path, "/../") || strstr(path, "\\..\\") ||
        strstr(path, "/./") || strstr(path, "\\.\\")) return 0;
    size_t n = strlen(path);
    return !strncmp(path, "sdmc:/", 6) || path[0] == '/' ||
        (n >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                    (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
}

static int verify_original_dex_in_archive(const char *path) {
    BsUnityOriginalDexImage image = {0};
    if (!bs_unity_apk_load_original_dex(path, &image)) return 0;
    int valid = image.data && image.size != 0;
    bs_unity_apk_free_original_dex(&image);
    return valid;
}

static int observed_native_library(const char *directory, const char *name) {
    char path[850];
    if (!directory || !name) return 0;
    size_t len = strlen(directory);
    int n = snprintf(path, sizeof path, "%s%s%s", directory,
                     (len && (directory[len - 1] == '/' ||
                              directory[len - 1] == '\\')) ? "" : "/",
                     name);
    if (n <= 0 || (size_t)n >= sizeof path || !observed_regular_file(path))
        return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint8_t elf[20] = {0};
    size_t read = fread(elf, 1, sizeof elf, file);
    fclose(file);
    /* Real staged Quest ARM64 ELF file, not an arbitrary path or zero-size
     * placeholder. The existing Quest loader independently pins full ELF
     * identity and relocations before any guest entry is invoked. */
    return read == sizeof elf && elf[0] == 0x7fu && elf[1] == 'E' &&
        elf[2] == 'L' && elf[3] == 'F' && elf[4] == 2u &&
        elf[5] == 1u && elf[18] == 0xb7u && elf[19] == 0u;
}

static int verify_resources_table(const char *apk_path, uint32_t *crc) {
    BsApkStoredEntry entry = {0};
    if (!bs_apk_read_stored_entry(apk_path, "resources.arsc", 512u * 1024u,
                                   &entry)) return 0;
    const uint8_t *p = entry.bytes;
    uint32_t chunk_bytes = p && entry.size >= 8 ?
        (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
        ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24) : 0;
    const int ok = entry.size == BS_QUEST_RESOURCES_ARSC_SIZE &&
        entry.crc32 == BS_QUEST_RESOURCES_ARSC_CRC &&
        p && entry.size >= 12 &&
        p[0] == 0x02u && p[1] == 0x00u && /* RES_TABLE_TYPE */
        p[2] == 0x0cu && p[3] == 0x00u &&
        chunk_bytes == entry.size;
    if (ok && crc) *crc = entry.crc32;
    bs_apk_free_stored_entry(&entry);
    return ok;
}

int bs_dex_quest_package_open(BsDexQuestPackageHost *host,
                              const char *staged_original_apk) {
    if (!host) return 0;
    *host = (BsDexQuestPackageHost){0};
    if (!staged_original_apk ||
        !acceptable_apk_path(staged_original_apk) ||
        !exact_path_copy(host->apk_path, sizeof host->apk_path,
                         staged_original_apk) ||
        !observed_regular_file(staged_original_apk) ||
        !bs_apk_load_original_manifest(staged_original_apk,
                                       &host->manifest) ||
        !verify_resources_table(staged_original_apk, &host->resources_crc) ||
        !verify_original_dex_in_archive(staged_original_apk)) {
        *host = (BsDexQuestPackageHost){0};
        return 0;
    }
    size_t application_metadata = 0, activity_metadata = 0;
    for (size_t i = 0; i < host->manifest.metadata_count; ++i) {
        const BsApkManifestMetadata *entry = &host->manifest.metadata[i];
        if (entry->value.kind == BS_APK_VALUE_ABSENT) {
            *host = (BsDexQuestPackageHost){0};
            return 0;
        }
        if (entry->belongs_to_activity) ++activity_metadata;
        else ++application_metadata;
    }
    host->application_metadata_count = application_metadata;
    host->activity_metadata_count = activity_metadata;
    host->application = (BsDexQuestApplicationInfo){
        .package_name = host->manifest.package_name,
        .min_sdk_version = host->manifest.min_sdk_version,
        .target_sdk_version = host->manifest.target_sdk_version,
        .label_resource = host->manifest.application_label_resource,
        .source_dir = host->apk_path,
        .public_source_dir = host->apk_path,
        .application_metadata_count = application_metadata
    };
    host->activity = (BsDexQuestActivityInfo){
        .name = host->manifest.activity_class,
        .package_name = host->manifest.package_name,
        .theme_resource = host->manifest.activity_theme_resource,
        .config_changes = host->manifest.activity_config_changes,
        .enabled = host->manifest.activity_enabled,
        .exported = host->manifest.activity_exported,
        .activity_metadata_count = activity_metadata
    };
    host->launch_filter = (BsDexQuestLaunchFilter){
        .action = host->manifest.launches_main_action ? QUEST_MAIN_ACTION : NULL,
        .package_name = host->manifest.package_name,
        .activity_class = host->manifest.activity_class,
        .launcher_category = host->manifest.launches_launcher_category ?
            QUEST_LAUNCHER : NULL,
        .vr_category = host->manifest.launches_oculus_vr_category ?
            QUEST_VR : NULL,
        .enabled = host->manifest.activity_enabled,
        .exported = host->manifest.activity_exported
    };
    host->initialized = 1;
    return 1;
}

void bs_dex_quest_package_close(BsDexQuestPackageHost *host) {
    if (host) *host = (BsDexQuestPackageHost){0};
}

int bs_dex_quest_package_observe_native_paths(
    BsDexQuestPackageHost *host,
    const char *staged_native_library_dir,
    const char *staged_data_dir) {
    if (!host || !host->initialized || !staged_native_library_dir ||
        !staged_data_dir || host->native_paths_observed) return 0;
    char native_dir[sizeof host->native_library_dir];
    char data_dir[sizeof host->data_dir];
    if (!exact_path_copy(native_dir, sizeof native_dir,
                         staged_native_library_dir) ||
        !exact_path_copy(data_dir, sizeof data_dir, staged_data_dir) ||
        !observed_directory(native_dir) ||
        !observed_directory(data_dir) ||
        !observed_native_library(native_dir, "libmain.so") ||
        !observed_native_library(native_dir, "libunity.so"))
        return 0;
    memcpy(host->native_library_dir, native_dir, strlen(native_dir) + 1);
    memcpy(host->data_dir, data_dir, strlen(data_dir) + 1);
    host->application.native_library_dir = host->native_library_dir;
    host->application.data_dir = host->data_dir;
    host->native_paths_observed = 1;
    return 1;
}

const BsDexQuestApplicationInfo *bs_dex_quest_package_application(
    const BsDexQuestPackageHost *host) {
    return host && host->initialized ? &host->application : NULL;
}

const BsDexQuestActivityInfo *bs_dex_quest_package_activity(
    const BsDexQuestPackageHost *host) {
    return host && host->initialized ? &host->activity : NULL;
}

const BsDexQuestLaunchFilter *bs_dex_quest_package_launch_filter(
    const BsDexQuestPackageHost *host) {
    return host && host->initialized ? &host->launch_filter : NULL;
}

int bs_dex_quest_package_metadata(
    const BsDexQuestPackageHost *host, const char *name, int activity,
    BsApkManifestValue *result) {
    if (result) *result = (BsApkManifestValue){0};
    if (!host || !host->initialized || !result || !name ||
        (activity != 0 && activity != 1)) return 0;
    const BsApkManifestMetadata *value = bs_apk_manifest_metadata(
        &host->manifest, name, activity);
    if (!value || value->value.kind == BS_APK_VALUE_ABSENT) return 0;
    *result = value->value;
    return 1;
}

int bs_dex_quest_package_bundle_get_string(
    const BsDexQuestPackageHost *host, const char *name, int activity,
    const char **string_or_null) {
    if (string_or_null) *string_or_null = NULL;
    if (!string_or_null || !host || !host->initialized || !name ||
        (activity != 0 && activity != 1)) return 0;
    const BsApkManifestMetadata *entry = bs_apk_manifest_metadata(
        &host->manifest, name, activity);
    if (!entry) return 1; /* Android Bundle.getString: absent -> null. */
    if (entry->value.kind != BS_APK_VALUE_STRING) return 0;
    *string_or_null = entry->value.text;
    return 1;
}

int bs_dex_quest_package_bundle_get_boolean(
    const BsDexQuestPackageHost *host, const char *name, int activity,
    int *value) {
    if (value) *value = 0;
    if (!value || !host || !host->initialized || !name ||
        (activity != 0 && activity != 1)) return 0;
    const BsApkManifestMetadata *entry = bs_apk_manifest_metadata(
        &host->manifest, name, activity);
    if (!entry) return 1; /* Android Bundle.getBoolean: absent -> false. */
    if (entry->value.kind != BS_APK_VALUE_BOOLEAN) return 0;
    *value = entry->value.raw != 0;
    return 1;
}

static int observed_java(const BsDexQuestJavaWitness *witness,
                         void *object, const char *descriptor) {
    return object && witness->verify_class(
        witness->user, object, descriptor) == 1;
}

int bs_dex_quest_package_verify_java_owners(
    const BsDexQuestPackageHost *host,
    const BsDexQuestJavaOwners *candidate,
    const BsDexQuestJavaWitness *witness,
    BsDexQuestJavaOwners *accepted) {
    if (accepted) *accepted = (BsDexQuestJavaOwners){0};
    if (!host || !host->initialized || !host->native_paths_observed ||
        !candidate || !witness || !accepted ||
        !witness->verify_class || !witness->verify_app_info ||
        !witness->verify_activity_info || !witness->verify_launch_intent ||
        !witness->verify_package_manager || !witness->verify_bundle ||
        !witness->verify_string_value) return 0;

    if (!observed_java(witness, candidate->launch_intent, JAVA_INTENT) ||
        !observed_java(witness, candidate->component, JAVA_COMPONENT) ||
        !observed_java(witness, candidate->package_manager, JAVA_MANAGER) ||
        !observed_java(witness, candidate->application_info, JAVA_APPINFO) ||
        !observed_java(witness, candidate->activity_info, JAVA_ACTIVITYINFO) ||
        !observed_java(witness, candidate->application_metadata_bundle, JAVA_BUNDLE) ||
        !observed_java(witness, candidate->activity_metadata_bundle, JAVA_BUNDLE) ||
        !observed_java(witness, candidate->native_library_dir_string, JAVA_STRING))
        return 0;

    /* Independently inspect the actual delivered Intent, PackageManager's
     * getActivityInfo(component, GET_META_DATA), and both Bundle scopes.
     * Native manifest values cannot stand in for read-back Java fields. */
    if (witness->verify_launch_intent(witness->user,
            candidate->launch_intent, candidate->component,
            &host->launch_filter) != 1 ||
        witness->verify_package_manager(witness->user,
            candidate->package_manager, candidate->activity_info,
            candidate->component) != 1 ||
        witness->verify_app_info(witness->user,
            candidate->application_info, &host->application) != 1 ||
        witness->verify_activity_info(witness->user,
            candidate->activity_info, &host->activity) != 1 ||
        witness->verify_bundle(witness->user,
            candidate->application_metadata_bundle, &host->manifest, 0) != 1 ||
        witness->verify_bundle(witness->user,
            candidate->activity_metadata_bundle, &host->manifest, 1) != 1 ||
        witness->verify_string_value(witness->user,
            candidate->native_library_dir_string,
            host->native_library_dir) != 1)
        return 0;
    *accepted = *candidate;
    return 1;
}

int bs_dex_quest_package_stage_context(
    const BsDexQuestPackageHost *host,
    const BsDexQuestJavaOwners *candidate_java,
    const BsDexQuestJavaWitness *witness,
    BsDexAndroidContextAttachment *candidate) {
    if (!candidate || candidate->intent || candidate->application_info ||
        candidate->package_manager || candidate->metadata_bundle ||
        candidate->metadata_field_observed || candidate->native_library_dir)
        return 0;
    BsDexQuestJavaOwners accepted = {0};
    if (!bs_dex_quest_package_verify_java_owners(
            host, candidate_java, witness, &accepted)) return 0;
    candidate->intent = accepted.launch_intent;
    candidate->application_info = accepted.application_info;
    candidate->package_manager = accepted.package_manager;
    candidate->metadata_bundle = accepted.application_metadata_bundle;
    candidate->metadata_field_observed = 1;
    candidate->native_library_dir = accepted.native_library_dir_string;
    /* Never set Context/Activity/Resources/AssetManager/Java File evidence:
     * the real Activity.attach, getters and type verifier own those. */
    return 1;
}
