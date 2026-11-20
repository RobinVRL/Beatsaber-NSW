#include "unity_dex_quest_arsc_resolver.h"
#include "unity_apk_stored_entry.h"
#include "unity_dex_quest_package_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void reject_mutation(uint8_t *bytes, size_t size, size_t offset,
                            const uint8_t *replacement, size_t n) {
    assert(offset <= size && n <= size - offset && n <= 8);
    uint8_t saved[8] = {0};
    memcpy(saved, bytes + offset, n);
    memcpy(bytes + offset, replacement, n);
    BsQuestArscResolver parsed = {0};
    if (bs_quest_arsc_parse(bytes, size, &parsed)) {
        fprintf(stderr, "Unexpectedly accepted mutation at 0x%zx (length=%zu)\n", offset, n);
        bs_quest_arsc_release(&parsed);
        assert(0);
    }
    assert(!parsed.bytes && !parsed.size);
    memcpy(bytes + offset, saved, n);
}

static void mutate32_reject(uint8_t *bytes, size_t size,
                            size_t offset, uint32_t value) {
    uint8_t changed[4] = {0};
    wr32(changed, value);
    reject_mutation(bytes, size, offset, changed, sizeof changed);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    BsQuestArscResolver resolver = {0};
    assert(!bs_quest_arsc_open_apk("missing-Quest.apk", &resolver));
    assert(bs_quest_arsc_open_apk(argv[1], &resolver));
    assert(resolver.bytes && resolver.size == BS_QUEST_RESOURCES_ARSC_SIZE);
    assert(resolver.crc32 == BS_QUEST_RESOURCES_ARSC_CRC);
    assert(resolver.package_id == 0x7fu);
    assert(!bs_quest_arsc_open_apk(argv[1], &resolver));
    assert(resolver.type_id_offset == 0);

    char text[4096] = {0};
    char type[48] = {0};
    char name[128] = {0};
    assert(bs_quest_arsc_get_name(
        &resolver, 0x7f0d0021u, type, sizeof type, name, sizeof name));
    assert(!strcmp(type, "string") && !strcmp(name, "app_name"));
    assert(bs_quest_arsc_get_string(
        &resolver, 0x7f0d0021u, NULL, text, sizeof text));
    assert(!strcmp(text, "Beat Saber"));
    assert(bs_quest_arsc_get_name(
        &resolver, 0x7f040001u, type, sizeof type, name, sizeof name));
    assert(!strcmp(type, "bool"));
    assert(!strcmp(name, "abc_config_actionMenuItemAllCaps"));
    int boolean = -1;
    assert(bs_quest_arsc_get_boolean(
        &resolver, 0x7f040001u, NULL, &boolean));
    assert(boolean == 1);
    assert(!bs_quest_arsc_get_boolean(
        &resolver, 0x7f040000u, NULL, &boolean));
    assert(boolean == 0); /* orientation variation unresolved */
    BsQuestArscConfiguration landscape = {.orientation = 2};
    assert(bs_quest_arsc_get_boolean(
        &resolver, 0x7f040000u, &landscape, &boolean));
    assert(boolean == 1);
    BsQuestArscConfiguration portrait = {.orientation = 1};
    assert(bs_quest_arsc_get_boolean(
        &resolver, 0x7f040000u, &portrait, &boolean));
    assert(boolean == 0);

    assert(bs_quest_arsc_get_name(
        &resolver, 0x7f090000u, type, sizeof type, name, sizeof name));
    assert(!strcmp(type, "integer") &&
           !strcmp(name, "abc_config_activityDefaultDur"));
    int32_t integer = 0;
    assert(bs_quest_arsc_get_integer(
        &resolver, 0x7f090000u, NULL, &integer));
    assert(integer == 220);
    assert(!bs_quest_arsc_get_integer(
        &resolver, 0x7f040001u, NULL, &integer));
    assert(integer == 0);
    assert(!bs_quest_arsc_get_boolean(
        &resolver, 0x7f090000u, NULL, &boolean));
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7f090000u, NULL, text, sizeof text));
    assert(!text[0]);

    /* The original string table contains 86 locale variants of this real
     * AndroidX appcompat resource. Only supplied language chooses one. */
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7f0d0005u, NULL, text, sizeof text));
    BsQuestArscConfiguration german = {0};
    assert(bs_quest_arsc_set_observed_language(&german, "de-DE"));
    assert(bs_quest_arsc_get_string(
        &resolver, 0x7f0d0005u, &german, text, sizeof text));
    assert(!strcmp(text, "Zur Startseite"));
    BsQuestArscConfiguration japanese = {0};
    assert(bs_quest_arsc_set_observed_language(&japanese, "ja-JP"));
    assert(bs_quest_arsc_get_string(
        &resolver, 0x7f0d0005u, &japanese, text, sizeof text));
    assert(!strcmp(text, "ホームに戻る"));
    BsQuestArscConfiguration english = {0};
    assert(bs_quest_arsc_set_observed_language(&english, "en-US"));
    assert(bs_quest_arsc_get_string(
        &resolver, 0x7f0d0021u, &english, text, sizeof text));
    assert(!strcmp(text, "Beat Saber"));
    assert(!bs_quest_arsc_set_observed_language(&english, "de-Latn-DE"));
    assert(!bs_quest_arsc_set_observed_language(&english, "untrusted"));
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7f0d0005u, &german, text, 4));
    assert(!text[0]);

    /* Resource IDs that require an unobserved density never borrow a
     * random mdpi/xhdpi variant. Actual native NWindow pixels alone are
     * insufficient to infer Android densityDpi. */
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7f0c0000u, NULL, text, sizeof text));
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7f0c0000u, &german, text, sizeof text));
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7f030000u, &german, text, sizeof text));
    assert(!bs_quest_arsc_get_string(
        &resolver, 0x7e0d0021u, &english, text, sizeof text));
    assert(!bs_quest_arsc_get_name(
        &resolver, 0x7f1f0000u, type, sizeof type, name, sizeof name));

    BsApkStoredEntry original = {0};
    assert(bs_apk_read_stored_entry(
        argv[1], "resources.arsc", BS_QUEST_ARSC_MAX_BYTES, &original));
    assert(original.size == resolver.size &&
           !memcmp(original.bytes, resolver.bytes, original.size));
    BsQuestArscResolver copy = {0};
    assert(bs_quest_arsc_parse(original.bytes, original.size, &copy));
    assert(copy.bytes != original.bytes);
    bs_quest_arsc_release(&copy);
    uint8_t *b = original.bytes;
    size_t total = original.size;
    mutate32_reject(b, total, 4, (uint32_t)(total + 4));
    mutate32_reject(b, total, 8, UINT32_MAX);
    /* Global pool starts at 12; break its index-to-strings offset. */
    mutate32_reject(b, total, 12 + 20, UINT32_MAX);
    mutate32_reject(b, total, 12 + rd16(b + 14),
                    UINT32_MAX); /* first string index */
    const size_t first_utf8 =
        12 + rd32(b + 12 + 20) +
        rd32(b + 12 + rd16(b + 12 + 2));
    const uint8_t illegal_utf8[] = {0xff};
    reject_mutation(b, total, first_utf8 + 2,
                    illegal_utf8, sizeof illegal_utf8);
    /* First package type name is the actual UTF-16 string "anim". An
     * unpaired high surrogate is invalid even if byte lengths fit. */
    /* Package starts after global string pool. */
    const size_t pkg = 12 + rd32(b + 12 + 4);
    assert(pkg < total && rd16(b + pkg) == 0x0200u);
    const size_t type_pool = pkg + rd32(b + pkg + 268);
    const size_t first_utf16 =
        type_pool + rd32(b + type_pool + 20) +
        rd32(b + type_pool + rd16(b + type_pool + 2));
    const uint8_t orphan_surrogate[] = {0x00, 0xd8};
    reject_mutation(b, total, first_utf16 + 2,
                    orphan_surrogate, sizeof orphan_surrogate);
    mutate32_reject(b, total, pkg + 268, UINT32_MAX);
    mutate32_reject(b, total, pkg + 276, UINT32_MAX);
    mutate32_reject(b, total, pkg + 8, 256u);
    /* Find the actual first type chunk, mutate entry count and its entry
     * offset. These must be rejected during parse rather than at getString. */
    size_t first_type = 0, first_spec = 0;
    for (size_t pos = pkg + rd16(b + pkg + 2); pos < total;) {
        uint16_t kind = rd16(b + pos);
        uint32_t size = rd32(b + pos + 4);
        if (kind == 0x0202 && !first_spec) first_spec = pos;
        if (kind == 0x0201 && !first_type) first_type = pos;
        assert(size >= 8 && pos + size <= total);
        pos += size;
    }
    assert(first_type && first_spec);
    mutate32_reject(b, total, first_spec + 12, UINT32_MAX);
    mutate32_reject(b, total, first_type + 12, UINT32_MAX);
    mutate32_reject(b, total, first_type + 16, UINT32_MAX);
    mutate32_reject(b, total, first_type + 20, UINT32_MAX);
    mutate32_reject(b, total, first_type + rd16(b + first_type + 2),
                    UINT32_MAX - 1u); /* 0xffffffff means valid NO_ENTRY */
    const size_t first_entry = first_type + rd32(b + first_type + 16) +
        rd32(b + first_type + rd16(b + first_type + 2));
    mutate32_reject(b, total, first_entry + 4, UINT32_MAX);
    mutate32_reject(b, total, first_entry + rd16(b + first_entry) + 4,
                    UINT32_MAX); /* string pool index out of bounds */
    /* A truncated root or misplaced string payload must fail cleanly. */
    assert(!bs_quest_arsc_parse(b, total - 1, &copy));
    assert(!copy.bytes);
    bs_apk_free_stored_entry(&original);
    bs_quest_arsc_release(&resolver);
    assert(!resolver.bytes && !resolver.size);
    puts("PASS: original Quest ARSC type/UTF pools/locales/typed values and malformed bounds");
    return 0;
}
