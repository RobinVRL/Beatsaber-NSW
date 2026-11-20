#include "unity_apk_manifest.h"
#include "unity_apk_stored_entry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    BsApkManifest manifest = {0};
    assert(bs_apk_load_original_manifest(argv[1], &manifest));
    assert(!strcmp(manifest.package_name, "com.beatgames.beatsaber"));
    assert(!strcmp(manifest.version_name, "1.46.0_34060"));
    assert(manifest.version_code == 3491);
    assert(manifest.min_sdk_version == 32 &&
           manifest.target_sdk_version == 32 &&
           manifest.compile_sdk_version == 32);
    assert(!strcmp(manifest.activity_class,
                   "com.unity3d.player.UnityPlayerGameActivity"));
    assert(manifest.activity_exported && manifest.activity_enabled);
    assert(manifest.launches_main_action &&
           manifest.launches_launcher_category &&
           manifest.launches_oculus_vr_category);
    assert(manifest.activity_theme_resource == 0x7f0e00ffu);
    assert(manifest.application_label_resource == 0x7f0d0021u);
    assert(manifest.activity_config_changes == 1073758207u);
    const BsApkManifestMetadata *supported = bs_apk_manifest_metadata(
        &manifest, "com.oculus.supportedDevices", 0);
    assert(supported && supported->value.kind == BS_APK_VALUE_STRING);
    assert(!strcmp(supported->value.text,
                   "quest2|questpro|quest3|quest3s"));
    const BsApkManifestMetadata *focus = bs_apk_manifest_metadata(
        &manifest, "com.oculus.vr.focusaware", 1);
    assert(focus && focus->value.kind == BS_APK_VALUE_BOOLEAN &&
           focus->value.raw == 1);
    assert(!bs_apk_manifest_metadata(&manifest, "missing-meta", 0));

    BsApkStoredEntry entry = {0};
    assert(bs_apk_read_stored_entry(argv[1], "AndroidManifest.xml",
                                    20000, &entry));
    assert(entry.size == BS_UNITY_QUEST_MANIFEST_SIZE &&
           entry.crc32 == BS_UNITY_QUEST_MANIFEST_CRC);
    BsApkManifest parsed = {0};
    assert(bs_apk_parse_manifest(entry.bytes, entry.size, &parsed));
    assert(!strcmp(parsed.package_name, manifest.package_name));

    /* Reject mutated root and XML framing without modifying published data. */
    entry.bytes[0] = 0xff;
    assert(!bs_apk_parse_manifest(entry.bytes, entry.size, &parsed));
    assert(!strcmp(parsed.package_name, manifest.package_name));
    entry.bytes[0] = 3;
    entry.bytes[4] ^= 0x04;
    assert(!bs_apk_parse_manifest(entry.bytes, entry.size, &parsed));
    assert(!strcmp(parsed.package_name, manifest.package_name));
    bs_apk_free_stored_entry(&entry);
    assert(!bs_apk_load_original_manifest("missing-original-apk", &manifest));
    assert(!manifest.package_name[0] && !manifest.metadata_count);
    puts("PASS: original Quest binary AndroidManifest package/activity/SDK/"
         " launch intent and typed metadata; malformed input rejected");
    return 0;
}
