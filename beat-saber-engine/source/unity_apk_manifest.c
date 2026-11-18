#include "unity_apk_manifest.h"
#include "unity_apk_stored_entry.h"
#include <limits.h>
#include <string.h>

#define APK_NO_STRING UINT32_MAX
#define APK_XML UINT16_C(0x0003)
#define APK_STRING_POOL UINT16_C(0x0001)
#define APK_XML_START UINT16_C(0x0102)
#define APK_XML_END UINT16_C(0x0103)
#define APK_MAX_ATTRIBUTES 48
#define APK_MAX_XML_DEPTH 40

typedef struct Pool {
    const uint8_t *all;
    size_t total;
    size_t begin, size, header_size, strings_start;
    uint32_t count;
    int utf8;
} Pool;

typedef struct Attribute {
    char name[80];
    BsApkManifestValue value;
} Attribute;

static int have(size_t size, size_t off, size_t n) {
    return off <= size && n <= size - off;
}

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_u32(const uint8_t *b, size_t size,
                    size_t off, uint32_t *out) {
    if (!b || !out || !have(size, off, 4)) return 0;
    *out = u32(b + off);
    return 1;
}

static int pool_open(Pool *pool, const uint8_t *data,
                     size_t size, size_t at, size_t chunk) {
    if (!pool || !data || !have(size, at, chunk) || chunk < 28 ||
        u16(data + at) != APK_STRING_POOL) return 0;
    const uint16_t header = u16(data + at + 2);
    const uint32_t count = u32(data + at + 8);
    const uint32_t strings_start = u32(data + at + 20);
    const uint32_t flags = u32(data + at + 16);
    if (header < 28 || header > chunk || strings_start >= chunk ||
        count > 100000 || count > (chunk - header) / 4 ||
        strings_start < header + (size_t)count * 4) return 0;
    *pool = (Pool){
        .all = data, .total = size, .begin = at,
        .size = chunk, .header_size = header,
        .strings_start = strings_start, .count = count,
        .utf8 = !!(flags & 0x100u)
    };
    return 1;
}

static int utf8_length(const Pool *pool, size_t *cursor,
                       uint32_t *out) {
    const size_t end = pool->begin + pool->size;
    if (*cursor >= end) return 0;
    uint8_t b = pool->all[(*cursor)++];
    if (b & 0x80) {
        if (*cursor >= end) return 0;
        *out = (uint32_t)(b & 0x7fu) << 8 | pool->all[(*cursor)++];
    } else *out = b;
    return 1;
}

static int utf16_length(const Pool *pool, size_t *cursor,
                        uint32_t *out) {
    const size_t end = pool->begin + pool->size;
    if (!have(end, *cursor, 2)) return 0;
    uint16_t first = u16(pool->all + *cursor);
    *cursor += 2;
    if (first & 0x8000u) {
        if (!have(end, *cursor, 2)) return 0;
        *out = (uint32_t)(first & 0x7fffu) << 16 |
               u16(pool->all + *cursor);
        *cursor += 2;
    } else *out = first;
    return 1;
}

/* The original Quest manifest's identifiers and metadata are ASCII. A non-
 * ASCII string is not misdecoded into a different identity; it fails the
 * bounded package parser. Display labels with resource IDs are deliberately
 * left to the separate resources.arsc reader. */
static int pool_string(const Pool *pool, uint32_t idx,
                       char *out, size_t capacity) {
    if (!pool || !pool->all || !out || !capacity || idx >= pool->count)
        return 0;
    const size_t off = pool->begin + pool->header_size + (size_t)idx * 4;
    uint32_t string_off = 0;
    if (!read_u32(pool->all, pool->total, off, &string_off) ||
        string_off >= pool->size - pool->strings_start) return 0;
    size_t cursor = pool->begin + pool->strings_start + string_off;
    const size_t end = pool->begin + pool->size;
    uint32_t count = 0, bytes = 0;
    if (pool->utf8) {
        if (!utf8_length(pool, &cursor, &count) ||
            !utf8_length(pool, &cursor, &bytes) ||
            count > bytes || bytes >= capacity ||
            !have(end, cursor, (size_t)bytes + 1) ||
            pool->all[cursor + bytes] != 0) return 0;
        for (uint32_t i = 0; i < bytes; ++i)
            if (pool->all[cursor + i] < 0x20u ||
                pool->all[cursor + i] >= 0x7fu) return 0;
        memcpy(out, pool->all + cursor, bytes);
        out[bytes] = '\0';
        return 1;
    }
    if (!utf16_length(pool, &cursor, &count) || count >= capacity ||
        !have(end, cursor, (size_t)count * 2 + 2) ||
        u16(pool->all + cursor + (size_t)count * 2) != 0) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t ch = u16(pool->all + cursor + (size_t)i * 2);
        if (ch < 0x20u || ch >= 0x7fu) return 0;
        out[i] = (char)ch;
    }
    out[count] = '\0';
    return 1;
}

static int decode_value(const Pool *pool, uint32_t raw_idx,
                        uint8_t value_type, uint32_t value_data,
                        BsApkManifestValue *out) {
    if (!pool || !out) return 0;
    *out = (BsApkManifestValue){0};
    if (raw_idx != APK_NO_STRING) {
        out->kind = BS_APK_VALUE_STRING;
        return pool_string(pool, raw_idx, out->text, sizeof out->text);
    }
    if (value_type == 0x03u) {
        out->kind = BS_APK_VALUE_STRING;
        return pool_string(pool, value_data, out->text,
                           sizeof out->text);
    }
    if (value_type == 0x12u) {
        out->kind = BS_APK_VALUE_BOOLEAN;
        out->raw = value_data != 0;
        return 1;
    }
    if (value_type == 0x10u || value_type == 0x11u) {
        out->kind = BS_APK_VALUE_INTEGER;
        out->raw = value_data;
        return 1;
    }
    if (value_type == 0x01u) {
        out->kind = BS_APK_VALUE_RESOURCE_ID;
        out->raw = value_data;
        return 1;
    }
    /* Unsupported typed values are not guessed. They remain explicitly
     * ABSENT and cannot satisfy any required manifest field. */
    return 1;
}

static int read_start(const Pool *pool, const uint8_t *data,
                      size_t size, size_t at, size_t chunk,
                      char *name, size_t cap, Attribute attrs[APK_MAX_ATTRIBUTES],
                      size_t *attr_count) {
    if (!pool || !pool->all || !data || !name || !attr_count ||
        chunk < 36 || !have(size, at, chunk)) return 0;
    const uint32_t idx = u32(data + at + 20);
    const uint16_t attr_start = u16(data + at + 24);
    const uint16_t attr_size = u16(data + at + 26);
    const uint16_t count = u16(data + at + 28);
    if (!pool_string(pool, idx, name, cap) ||
        attr_size < 20 || count > APK_MAX_ATTRIBUTES ||
        attr_start < 20 ||
        16u + (size_t)attr_start > chunk ||
        (size_t)count * attr_size >
            chunk - (16u + (size_t)attr_start)) return 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t p = at + 16u + attr_start + i * attr_size;
        const uint32_t name_index = u32(data + p + 4);
        const uint32_t raw_index = u32(data + p + 8);
        if (u16(data + p + 12) != 8 ||
            !pool_string(pool, name_index, attrs[i].name,
                         sizeof attrs[i].name) ||
            !decode_value(pool, raw_index, data[p + 15],
                          u32(data + p + 16), &attrs[i].value)) return 0;
    }
    *attr_count = count;
    return 1;
}

static const BsApkManifestValue *field(const Attribute *attrs,
                                        size_t count, const char *key) {
    const BsApkManifestValue *result = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(attrs[i].name, key)) continue;
        if (result) return NULL; /* duplicate attribute is ambiguous */
        result = &attrs[i].value;
    }
    return result;
}

static int copy_text(char *dst, size_t dst_size,
                     const BsApkManifestValue *value) {
    if (!dst || !value || value->kind != BS_APK_VALUE_STRING) return 0;
    const size_t len = strlen(value->text);
    if (!len || len >= dst_size) return 0;
    memcpy(dst, value->text, len + 1);
    return 1;
}

static int integer(const BsApkManifestValue *value, uint32_t *out) {
    if (!value || !out || value->kind != BS_APK_VALUE_INTEGER) return 0;
    *out = value->raw;
    return 1;
}

static int boolean(const BsApkManifestValue *value, int *out) {
    if (!value || !out || value->kind != BS_APK_VALUE_BOOLEAN) return 0;
    *out = value->raw != 0;
    return 1;
}

static int append_metadata(BsApkManifest *out, const Attribute *attrs,
                           size_t count, int in_activity) {
    if (!out || out->metadata_count == BS_APK_MAX_METADATA) return 0;
    const BsApkManifestValue *name = field(attrs, count, "name");
    const BsApkManifestValue *value = field(attrs, count, "value");
    if (!name || !value || value->kind == BS_APK_VALUE_ABSENT) return 0;
    BsApkManifestMetadata *dst = &out->metadata[out->metadata_count];
    if (!copy_text(dst->name, sizeof dst->name, name)) return 0;
    for (size_t i = 0; i < out->metadata_count; ++i) {
        const BsApkManifestMetadata *prior = &out->metadata[i];
        if (prior->belongs_to_activity == in_activity &&
            !strcmp(prior->name, dst->name)) return 0;
    }
    dst->value = *value;
    dst->belongs_to_activity = in_activity;
    ++out->metadata_count;
    return 1;
}

const BsApkManifestMetadata *bs_apk_manifest_metadata(
    const BsApkManifest *manifest, const char *name,
    int belongs_to_activity) {
    if (!manifest || !name || !*name) return NULL;
    const BsApkManifestMetadata *found = NULL;
    for (size_t i = 0; i < manifest->metadata_count; ++i) {
        const BsApkManifestMetadata *entry = &manifest->metadata[i];
        if (entry->belongs_to_activity != !!belongs_to_activity ||
            strcmp(entry->name, name)) continue;
        if (found) return NULL;
        found = entry;
    }
    return found;
}

int bs_apk_parse_manifest(const uint8_t *data, size_t size,
                           BsApkManifest *out) {
    if (!data || !out || size < 8 || size > 1024 * 1024 ||
        u16(data) != APK_XML || u16(data + 2) < 8 ||
        u32(data + 4) != size) return 0;
    BsApkManifest result = {0};
    Pool pool = {0};
    char parents[APK_MAX_XML_DEPTH][72] = {{0}};
    size_t depth = 0;
    size_t at = u16(data + 2);
    int found_manifest = 0, found_application = 0, found_sdk = 0;
    int found_activity = 0;
    int in_main_filter = 0, in_launcher_filter = 0, in_vr_filter = 0;
    while (at < size) {
        if (!have(size, at, 8)) return 0;
        const uint16_t type = u16(data + at);
        const uint16_t header = u16(data + at + 2);
        const uint32_t chunk = u32(data + at + 4);
        if (header < 8 || chunk < header || !have(size, at, chunk))
            return 0;
        if (type == APK_STRING_POOL) {
            if (pool.all || !pool_open(&pool, data, size, at, chunk))
                return 0;
        } else if (type == APK_XML_START) {
            if (!pool.all || depth >= APK_MAX_XML_DEPTH || header < 16)
                return 0;
            char tag[72];
            Attribute attrs[APK_MAX_ATTRIBUTES] = {0};
            size_t count = 0;
            if (!read_start(&pool, data, size, at, chunk,
                            tag, sizeof tag, attrs, &count)) return 0;
            const char *parent = depth ? parents[depth - 1] : "";
            const char *grandparent = depth > 1 ? parents[depth - 2] : "";
            if (!strcmp(tag, "manifest")) {
                if (depth || found_manifest ||
                    !copy_text(result.package_name,
                               sizeof result.package_name,
                               field(attrs, count, "package")) ||
                    !copy_text(result.version_name,
                               sizeof result.version_name,
                               field(attrs, count, "versionName")) ||
                    !integer(field(attrs, count, "versionCode"),
                             &result.version_code) ||
                    !integer(field(attrs, count, "compileSdkVersion"),
                             &result.compile_sdk_version)) return 0;
                found_manifest = 1;
            } else if (!strcmp(tag, "uses-sdk") &&
                       !strcmp(parent, "manifest")) {
                if (found_sdk ||
                    !integer(field(attrs, count, "minSdkVersion"),
                             &result.min_sdk_version) ||
                    !integer(field(attrs, count, "targetSdkVersion"),
                             &result.target_sdk_version)) return 0;
                found_sdk = 1;
            } else if (!strcmp(tag, "application") &&
                       !strcmp(parent, "manifest")) {
                if (found_application) return 0;
                found_application = 1;
                const BsApkManifestValue *label = field(attrs, count, "label");
                if (label && label->kind == BS_APK_VALUE_RESOURCE_ID)
                    result.application_label_resource = label->raw;
            } else if (!strcmp(tag, "activity") &&
                       !strcmp(parent, "application")) {
                const BsApkManifestValue *activity_name =
                    field(attrs, count, "name");
                if (!activity_name || activity_name->kind != BS_APK_VALUE_STRING)
                    return 0;
                if (!strcmp(activity_name->text,
                            "com.unity3d.player.UnityPlayerGameActivity")) {
                    if (found_activity ||
                        !copy_text(result.activity_class,
                                   sizeof result.activity_class,
                                   activity_name) ||
                        !boolean(field(attrs, count, "enabled"),
                                 &result.activity_enabled) ||
                        !boolean(field(attrs, count, "exported"),
                                 &result.activity_exported) ||
                        !integer(field(attrs, count, "configChanges"),
                                 &result.activity_config_changes)) return 0;
                    const BsApkManifestValue *theme =
                        field(attrs, count, "theme");
                    if (theme && theme->kind == BS_APK_VALUE_RESOURCE_ID)
                        result.activity_theme_resource = theme->raw;
                    found_activity = 1;
                }
            } else if (!strcmp(tag, "meta-data") &&
                       (!strcmp(parent, "application") ||
                        !strcmp(parent, "activity"))) {
                if (!append_metadata(&result, attrs, count,
                                      !strcmp(parent, "activity"))) return 0;
            } else if ((!strcmp(tag, "action") ||
                        !strcmp(tag, "category")) &&
                       !strcmp(parent, "intent-filter") &&
                       !strcmp(grandparent, "activity")) {
                const BsApkManifestValue *name = field(attrs, count, "name");
                if (!name || name->kind != BS_APK_VALUE_STRING) return 0;
                if (!strcmp(tag, "action") &&
                    !strcmp(name->text, "android.intent.action.MAIN"))
                    in_main_filter = 1;
                if (!strcmp(tag, "category") &&
                    !strcmp(name->text, "android.intent.category.LAUNCHER"))
                    in_launcher_filter = 1;
                if (!strcmp(tag, "category") &&
                    !strcmp(name->text, "com.oculus.intent.category.VR"))
                    in_vr_filter = 1;
            }
            memcpy(parents[depth], tag, strlen(tag) + 1);
            ++depth;
        } else if (type == APK_XML_END) {
            if (!pool.all || header < 16 || chunk < 24 || !depth)
                return 0;
            char tag[72];
            if (!pool_string(&pool, u32(data + at + 20),
                             tag, sizeof tag) ||
                strcmp(tag, parents[depth - 1])) return 0;
            if (!strcmp(tag, "intent-filter") && depth >= 2 &&
                !strcmp(parents[depth - 2], "activity")) {
                if (in_main_filter && in_launcher_filter) {
                    result.launches_main_action = 1;
                    result.launches_launcher_category = 1;
                    result.launches_oculus_vr_category = in_vr_filter;
                }
                in_main_filter = in_launcher_filter = in_vr_filter = 0;
            }
            --depth;
        }
        at += chunk;
    }
    if (at != size || depth || !pool.all || !found_manifest ||
        !found_sdk || !found_application || !found_activity ||
        !result.version_code || !result.target_sdk_version ||
        !result.activity_enabled || !result.activity_exported ||
        !result.launches_main_action ||
        !result.launches_launcher_category) return 0;
    *out = result;
    return 1;
}

int bs_apk_load_original_manifest(const char *apk_path,
                                   BsApkManifest *out) {
    if (!out) return 0;
    *out = (BsApkManifest){0};
    if (!apk_path) return 0;
    BsApkStoredEntry entry = {0};
    if (!bs_apk_read_stored_entry(apk_path, "AndroidManifest.xml",
                                   1024 * 1024, &entry)) return 0;
    int ok = entry.size == BS_UNITY_QUEST_MANIFEST_SIZE &&
             entry.crc32 == BS_UNITY_QUEST_MANIFEST_CRC &&
             bs_apk_parse_manifest(entry.bytes, entry.size, out) &&
             !strcmp(out->package_name, "com.beatgames.beatsaber") &&
             !strcmp(out->version_name, "1.46.0_34060") &&
             out->version_code == 3491 &&
             out->min_sdk_version == 32 && out->target_sdk_version == 32 &&
             !strcmp(out->activity_class,
                     "com.unity3d.player.UnityPlayerGameActivity");
    bs_apk_free_stored_entry(&entry);
    if (!ok) *out = (BsApkManifest){0};
    return ok;
}
