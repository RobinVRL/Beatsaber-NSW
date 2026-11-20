/* Parsed, byte-verified Android package metadata from the ORIGINAL Quest
 * AndroidManifest.xml. Values are a read-only resource for an Android Java
 * emulation backend; parsing the manifest never creates Java objects. */
#ifndef BS_UNITY_APK_MANIFEST_H
#define BS_UNITY_APK_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define BS_APK_MAX_METADATA 40u
#define BS_UNITY_QUEST_MANIFEST_SIZE 8956u
#define BS_UNITY_QUEST_MANIFEST_CRC UINT32_C(0x7088e0f8)

typedef enum BsApkManifestValueKind {
    BS_APK_VALUE_ABSENT = 0,
    BS_APK_VALUE_STRING = 1,
    BS_APK_VALUE_INTEGER = 2,
    BS_APK_VALUE_BOOLEAN = 3,
    BS_APK_VALUE_RESOURCE_ID = 4
} BsApkManifestValueKind;

typedef struct BsApkManifestValue {
    BsApkManifestValueKind kind;
    uint32_t raw;
    char text[160];
} BsApkManifestValue;

typedef struct BsApkManifestMetadata {
    char name[128];
    BsApkManifestValue value;
    int belongs_to_activity;
} BsApkManifestMetadata;

typedef struct BsApkManifest {
    char package_name[128];
    char version_name[64];
    char activity_class[256];
    uint32_t version_code;
    uint32_t min_sdk_version;
    uint32_t target_sdk_version;
    uint32_t compile_sdk_version;
    uint32_t application_label_resource;
    uint32_t activity_theme_resource;
    uint32_t activity_config_changes;
    int activity_enabled;
    int activity_exported;
    int launches_main_action;
    int launches_launcher_category;
    int launches_oculus_vr_category;
    BsApkManifestMetadata metadata[BS_APK_MAX_METADATA];
    size_t metadata_count;
} BsApkManifest;

/* Accepts one well-formed Android binary XML document with actual observed
 * Quest launch metadata. Rejects unknown value encodings for required fields,
 * malformed pools/chunks and incomplete launch declarations. */
int bs_apk_parse_manifest(const uint8_t *data, size_t size,
                           BsApkManifest *out);

/* Requires the pinned original Manifest CRC, bytes, Quest package and
 * UnityPlayerGameActivity launch class, as read from the staged APK. */
int bs_apk_load_original_manifest(const char *apk_path,
                                   BsApkManifest *out);

/* Exact name lookup of parsed metadata; NULL for absent or duplicate names. */
const BsApkManifestMetadata *bs_apk_manifest_metadata(
    const BsApkManifest *manifest, const char *name,
    int belongs_to_activity);

#endif
