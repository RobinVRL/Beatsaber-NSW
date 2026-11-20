#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "unity_dex_quest_arsc_resolver.h"
#include "unity_apk_stored_entry.h"
#include "unity_dex_quest_package_host.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#if defined(BS_ARSC_TRACE)
#include <stdio.h>
#define ARSC_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define ARSC_TRACE(...) ((void)0)
#endif

#define RES_STRING_POOL_TYPE 0x0001u
#define RES_TABLE_TYPE 0x0002u
#define RES_PACKAGE_TYPE 0x0200u
#define RES_TYPE_TYPE 0x0201u
#define RES_TYPE_SPEC_TYPE 0x0202u
#define RES_STRING_UTF8_FLAG 0x100u
#define RES_STRING_SORTED_FLAG 0x1u
#define RES_VALUE_STRING 0x03u
#define RES_VALUE_REFERENCE 0x01u
#define RES_VALUE_INT_DEC 0x10u
#define RES_VALUE_INT_HEX 0x11u
#define RES_VALUE_INT_BOOLEAN 0x12u

typedef struct Chunk {
    size_t at, size, header_size;
    uint16_t type;
} Chunk;

typedef struct Pool {
    Chunk chunk;
    size_t count, offsets_at, strings_at, strings_end;
    int utf8;
} Pool;

typedef struct RawResource {
    uint8_t type;
    uint32_t data, key_index;
} RawResource;

typedef struct ResourceChoice {
    RawResource value;
    int found, ambiguous, missing_qualifier, density_variants;
    int matching_density;
    uint64_t rank;
} ResourceChoice;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int span(size_t at, size_t count, size_t limit) {
    return at <= limit && count <= limit - at;
}
static int chunk_at(const uint8_t *b, size_t limit,
                    size_t at, Chunk *out) {
    if (!b || !out || (at & 3u) || !span(at, 8, limit)) return 0;
    size_t header = rd16(b + at + 2);
    size_t length = rd32(b + at + 4);
    if (header < 8 || header > length || (length & 3u) ||
        !span(at, length, limit)) return 0;
    *out = (Chunk){.at = at, .header_size = header,
                   .size = length, .type = rd16(b + at)};
    return 1;
}

static int utf8_codepoint(const uint8_t *in, size_t length,
                          size_t *used, uint32_t *point) {
    if (!length || !in || !used || !point) return 0;
    uint32_t c = in[0], min = 0;
    size_t n = 1;
    if (c < 0x80) {
        if (!c) return 0; /* Android length strings are NUL-terminated. */
    } else if ((c & 0xe0u) == 0xc0u) {
        n = 2; c &= 0x1fu; min = 0x80;
    } else if ((c & 0xf0u) == 0xe0u) {
        n = 3; c &= 0x0fu; min = 0x800;
    } else if ((c & 0xf8u) == 0xf0u) {
        n = 4; c &= 0x07u; min = 0x10000;
    } else return 0;
    if (n > length) return 0;
    for (size_t i = 1; i < n; ++i) {
        if ((in[i] & 0xc0u) != 0x80u) return 0;
        c = (c << 6) | (in[i] & 0x3fu);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return 0;
    *used = n;
    *point = c;
    return 1;
}

/* Return actual Unicode code point, enforcing strict UTF-16 surrogate
 * pairing. Android pool entries are length-prefixed UTF-16 units. */
static int utf16_point(const uint8_t *in, size_t units,
                       size_t *consumed, uint32_t *point) {
    if (!in || !units || !consumed || !point) return 0;
    uint32_t a = rd16(in);
    *consumed = 1;
    if (a >= 0xdc00 && a <= 0xdfff) return 0;
    if (a >= 0xd800 && a <= 0xdbff) {
        if (units < 2) return 0;
        uint32_t b = rd16(in + 2);
        if (b < 0xdc00 || b > 0xdfff) return 0;
        *point = 0x10000u + ((a - 0xd800u) << 10) + (b - 0xdc00u);
        *consumed = 2;
    } else *point = a;
    return *point != 0;
}

static size_t put_utf8(uint32_t point, char *out) {
    if (point < 0x80) {
        if (out) out[0] = (char)point;
        return 1;
    }
    if (point < 0x800) {
        if (out) {
            out[0] = (char)(0xc0u | (point >> 6));
            out[1] = (char)(0x80u | (point & 0x3fu));
        }
        return 2;
    }
    if (point < 0x10000) {
        if (out) {
            out[0] = (char)(0xe0u | (point >> 12));
            out[1] = (char)(0x80u | ((point >> 6) & 0x3fu));
            out[2] = (char)(0x80u | (point & 0x3fu));
        }
        return 3;
    }
    if (out) {
        out[0] = (char)(0xf0u | (point >> 18));
        out[1] = (char)(0x80u | ((point >> 12) & 0x3fu));
        out[2] = (char)(0x80u | ((point >> 6) & 0x3fu));
        out[3] = (char)(0x80u | (point & 0x3fu));
    }
    return 4;
}

static int decode_length8(const uint8_t *b, size_t end, size_t *pos,
                          uint32_t *length) {
    if (!b || !pos || !length || *pos >= end) return 0;
    uint32_t a = b[(*pos)++];
    if (a & 0x80u) {
        if (*pos >= end) return 0;
        a = ((a & 0x7fu) << 8) | b[(*pos)++];
    }
    *length = a;
    return 1;
}
static int decode_length16(const uint8_t *b, size_t end, size_t *pos,
                           uint32_t *length) {
    if (!span(*pos, 2, end)) return 0;
    uint32_t a = rd16(b + *pos);
    *pos += 2;
    if (a & 0x8000u) {
        if (!span(*pos, 2, end)) return 0;
        a = ((a & 0x7fffu) << 16) | rd16(b + *pos);
        *pos += 2;
    }
    *length = a;
    return 1;
}

static int pool_info(const uint8_t *b, size_t limit,
                     Chunk ch, Pool *pool) {
    if (!pool || ch.type != RES_STRING_POOL_TYPE ||
        ch.header_size < 28) return 0;
    size_t count = rd32(b + ch.at + 8);
    size_t style_count = rd32(b + ch.at + 12);
    uint32_t flags = rd32(b + ch.at + 16);
    size_t strings = rd32(b + ch.at + 20);
    size_t styles = rd32(b + ch.at + 24);
    if ((flags & ~(RES_STRING_UTF8_FLAG | RES_STRING_SORTED_FLAG)) ||
        style_count || styles || !count ||
        count > BS_QUEST_ARSC_MAX_BYTES / 4 ||
        !span(ch.header_size, count * 4, ch.size) ||
        strings < ch.header_size + count * 4 ||
        strings >= ch.size ||
        !span(ch.at, ch.size, limit)) return 0;
    *pool = (Pool){.chunk = ch, .count = count,
                   .offsets_at = ch.at + ch.header_size,
                   .strings_at = ch.at + strings,
                   .strings_end = ch.at + ch.size,
                   .utf8 = !!(flags & RES_STRING_UTF8_FLAG)};
    return 1;
}

/* Validate the actual length-prefix encoding and Unicode scalar values.
 * out==NULL performs full validation without allocating/copying a string. */
static int pool_string(const uint8_t *b, const Pool *pool, size_t index,
                       char *out, size_t capacity) {
    if (out && capacity) out[0] = '\0';
    if (!b || !pool || index >= pool->count ||
        (out && !capacity)) return 0;
    size_t offset = rd32(b + pool->offsets_at + index * 4);
    if (offset > pool->strings_end - pool->strings_at) return 0;
    size_t pos = pool->strings_at + offset;
    size_t bytes_done = 0;
    if (pool->utf8) {
        uint32_t utf16_units = 0, byte_length = 0;
        if (!decode_length8(b, pool->strings_end, &pos, &utf16_units) ||
            !decode_length8(b, pool->strings_end, &pos, &byte_length) ||
            pos >= pool->strings_end ||
            (size_t)byte_length >= pool->strings_end - pos ||
            b[pos + byte_length] != 0) return 0;
        size_t cursor = 0, decoded_units = 0;
        while (cursor < byte_length) {
            size_t used = 0;
            uint32_t point = 0;
            if (!utf8_codepoint(b + pos + cursor,
                                byte_length - cursor, &used, &point))
                return 0;
            cursor += used;
            decoded_units += point > 0xffffu ? 2u : 1u;
            if (decoded_units > utf16_units) return 0;
        }
        if (decoded_units != utf16_units) return 0;
        if (out) {
            if ((size_t)byte_length >= capacity) return 0;
            memcpy(out, b + pos, byte_length);
            out[byte_length] = 0;
        }
        return 1;
    }
    uint32_t units = 0;
    if (!decode_length16(b, pool->strings_end, &pos, &units) ||
        pos >= pool->strings_end ||
        (size_t)units >= (pool->strings_end - pos) / 2 ||
        rd16(b + pos + (size_t)units * 2) != 0) return 0;
    size_t cursor = 0;
    while (cursor < units) {
        size_t consumed = 0;
        uint32_t point = 0;
        if (!utf16_point(b + pos + cursor * 2, units - cursor,
                         &consumed, &point)) return 0;
        cursor += consumed;
        size_t extra = put_utf8(point, NULL);
        if (bytes_done > SIZE_MAX - extra) return 0;
        if (out) {
            if (extra >= capacity || bytes_done > capacity - extra - 1)
                return 0;
            (void)put_utf8(point, out + bytes_done);
        }
        bytes_done += extra;
    }
    if (out) out[bytes_done] = 0;
    return 1;
}

static int validate_pool(const uint8_t *b, const Pool *pool) {
    for (size_t i = 0; i < pool->count; ++i)
        if (!pool_string(b, pool, i, NULL, 0)) {
            ARSC_TRACE("ARSC bad pool at 0x%zx index %zu utf8=%d\n",
                       pool->chunk.at, i, pool->utf8);
            return 0;
        }
    return 1;
}

static int validate_package_name(const uint8_t *utf16) {
    if (!utf16) return 0;
    size_t offset = 0;
    while (offset < 128) {
        if (rd16(utf16 + 2 * offset) == 0) return offset > 0;
        size_t consumed = 0;
        uint32_t point = 0;
        if (!utf16_point(utf16 + 2 * offset, 128 - offset,
                         &consumed, &point)) return 0;
        offset += consumed;
    }
    return 0;
}

static int validate_value(const uint8_t *b, size_t off,
                          size_t end, const Pool *global) {
    if (!span(off, 8, end) || rd16(b + off) != 8 ||
        b[off + 2] != 0) return 0;
    if (b[off + 3] == RES_VALUE_STRING &&
        rd32(b + off + 4) >= global->count) return 0;
    return 1;
}

static int validate_type(const uint8_t *b, Chunk chunk,
                         size_t expected, const Pool *global,
                         const Pool *keys) {
    size_t here = chunk.at;
    size_t entries_count = rd32(b + here + 12);
    size_t entries_start = rd32(b + here + 16);
    size_t config_size = rd32(b + here + 20);
    if (chunk.header_size < 48 || b[here + 9] != 0 ||
        rd16(b + here + 10) != 0 ||
        config_size < 28 || config_size > 64 ||
        chunk.header_size < 20 + config_size ||
        entries_count != expected ||
        entries_count > BS_QUEST_ARSC_MAX_BYTES / 4 ||
        entries_start < chunk.header_size + entries_count * 4 ||
        entries_start > chunk.size ||
        (entries_start & 3u)) return 0;
    size_t end = here + chunk.size;
    for (size_t i = 0; i < entries_count; ++i) {
        uint32_t relative = rd32(b + here + chunk.header_size + i * 4);
        if (relative == UINT32_MAX) continue;
        if (relative & 3u ||
            relative > chunk.size - entries_start) return 0;
        size_t entry = here + entries_start + relative;
        if (!span(entry, 8, end)) return 0;
        size_t size = rd16(b + entry);
        uint16_t flags = rd16(b + entry + 2);
        if (rd32(b + entry + 4) >= keys->count) return 0;
        /* PUBLIC (2) and WEAK (4) are valid; COMPLEX (1) selects map. */
        if (flags & ~UINT16_C(0x0007)) return 0;
        if (flags & 1u) {
            if (size < 16 || !span(entry, size, end)) return 0;
            size_t maps = rd32(b + entry + 12);
            if (maps > (end - entry - size) / 12) return 0;
            for (size_t j = 0; j < maps; ++j)
                if (!validate_value(b, entry + size + 12 * j + 4,
                                     end, global)) return 0;
        } else if (size < 8 || !span(entry, size + 8, end) ||
                   !validate_value(b, entry + size, end, global))
            return 0;
    }
    return 1;
}

static int validate_table(BsQuestArscResolver *resolver) {
    const uint8_t *b = resolver->bytes;
    const size_t size = resolver->size;
    Chunk table = {0};
    if (!chunk_at(b, size, 0, &table) ||
        table.type != RES_TABLE_TYPE ||
        table.header_size != 12 || table.size != size ||
        rd32(b + 8) != 1) return 0;
    size_t i = table.header_size;
    Chunk global = {0}, pkg = {0};
    Pool global_pool = {0};
    if (!chunk_at(b, size, i, &global) ||
        !pool_info(b, size, global, &global_pool) ||
        !validate_pool(b, &global_pool)) return 0;
    ARSC_TRACE("ARSC global pool valid\n");
    resolver->global_pool_offset = i;
    i += global.size;
    if (!chunk_at(b, size, i, &pkg) ||
        pkg.type != RES_PACKAGE_TYPE || pkg.header_size < 284 ||
        pkg.size != size - i) return 0;
    resolver->package_offset = i;
    resolver->package_id = rd32(b + i + 8);
    if (!resolver->package_id || resolver->package_id > 255 ||
        !validate_package_name(b + i + 12)) return 0;
    resolver->type_id_offset =
        pkg.header_size >= 288 ? rd32(b + i + 284) : 0;
    if (resolver->type_id_offset >= 255) return 0;
    const size_t type_offset = rd32(b + i + 268);
    const size_t key_offset = rd32(b + i + 276);
    if ((type_offset & 3u) || (key_offset & 3u) ||
        type_offset < pkg.header_size || key_offset < pkg.header_size ||
        type_offset >= pkg.size || key_offset >= pkg.size ||
        type_offset == key_offset) return 0;
    Chunk type_pool_ch = {0}, key_pool_ch = {0};
    Pool types = {0}, keys = {0};
    if (!chunk_at(b, size, i + type_offset, &type_pool_ch) ||
        !chunk_at(b, size, i + key_offset, &key_pool_ch) ||
        !span(type_pool_ch.at, type_pool_ch.size, i + pkg.size) ||
        !span(key_pool_ch.at, key_pool_ch.size, i + pkg.size) ||
        !(type_pool_ch.at + type_pool_ch.size <= key_pool_ch.at ||
          key_pool_ch.at + key_pool_ch.size <= type_pool_ch.at) ||
        !pool_info(b, size, type_pool_ch, &types) ||
        !pool_info(b, size, key_pool_ch, &keys) ||
        !validate_pool(b, &types) ||
        !validate_pool(b, &keys)) return 0;
    ARSC_TRACE("ARSC package pools valid\n");
    resolver->type_pool_offset = type_pool_ch.at;
    resolver->key_pool_offset = key_pool_ch.at;
    uint32_t spec_counts[256] = {0};
    unsigned type_chunks = 0;
    for (size_t pos = i + pkg.header_size; pos < i + pkg.size; ) {
        Chunk ch = {0};
        if (!chunk_at(b, i + pkg.size, pos, &ch)) return 0;
        if (ch.type == RES_STRING_POOL_TYPE) {
            if (pos != type_pool_ch.at && pos != key_pool_ch.at)
                return 0;
        } else if (ch.type == RES_TYPE_SPEC_TYPE) {
            if (ch.header_size < 16) return 0;
            unsigned id = b[pos + 8];
            size_t count = rd32(b + pos + 12);
            if (!id || id + resolver->type_id_offset > 255 ||
                id > types.count || spec_counts[id] ||
                count > BS_QUEST_ARSC_MAX_BYTES / 4 ||
                !span(ch.header_size, count * 4, ch.size) ||
                ch.header_size + count * 4 != ch.size) return 0;
            spec_counts[id] = (uint32_t)count;
        } else if (ch.type == RES_TYPE_TYPE) {
            if (ch.header_size < 24) return 0;
            unsigned id = b[pos + 8];
            if (!id || !spec_counts[id] ||
                !validate_type(b, ch, spec_counts[id],
                               &global_pool, &keys)) {
                ARSC_TRACE("ARSC bad type at 0x%zx id=%u spec=%u\n",
                           pos, id, spec_counts[id]);
                return 0;
            }
            ++type_chunks;
        } else return 0; /* Reject unsupported resource overlay chunks. */
        pos += ch.size;
    }
    return type_chunks != 0;
}

int bs_quest_arsc_parse(const uint8_t *bytes, size_t size,
                        BsQuestArscResolver *out) {
    if (!out || out->bytes || !bytes || size < 32 ||
        size > BS_QUEST_ARSC_MAX_BYTES) return 0;
    uint8_t *copy = malloc(size);
    if (!copy) return 0;
    memcpy(copy, bytes, size);
    BsQuestArscResolver candidate = {.bytes = copy, .size = size};
    if (!validate_table(&candidate)) {
        free(copy);
        return 0;
    }
    *out = candidate;
    return 1;
}

int bs_quest_arsc_open_apk(const char *original_apk_path,
                            BsQuestArscResolver *out) {
    if (!out || out->bytes || !original_apk_path) return 0;
    BsApkStoredEntry stored = {0};
    if (!bs_apk_read_stored_entry(original_apk_path, "resources.arsc",
                                  BS_QUEST_ARSC_MAX_BYTES, &stored))
        return 0;
    int valid = stored.size == BS_QUEST_RESOURCES_ARSC_SIZE &&
        stored.crc32 == BS_QUEST_RESOURCES_ARSC_CRC &&
        bs_quest_arsc_parse(stored.bytes, stored.size, out);
    if (valid) out->crc32 = stored.crc32;
    bs_apk_free_stored_entry(&stored);
    return valid;
}

void bs_quest_arsc_release(BsQuestArscResolver *resolver) {
    if (!resolver) return;
    free(resolver->bytes);
    *resolver = (BsQuestArscResolver){0};
}

int bs_quest_arsc_set_observed_language(
    BsQuestArscConfiguration *configuration, const char *tag) {
    if (!configuration || !tag) return 0;
    const size_t length = strlen(tag);
    if (length < 2 || length >= sizeof configuration->language_tag)
        return 0;
    size_t separator = 0;
    while (separator < length && tag[separator] != '-')
        ++separator;
    if (separator != 2 && separator != 3) return 0;
    for (size_t i = 0; i < separator; ++i)
        if (!isalpha((unsigned char)tag[i])) return 0;
    if (separator != length) {
        size_t region = length - separator - 1;
        if (region != 2 && region != 3) return 0;
        for (size_t i = separator + 1; i < length; ++i)
            if ((region == 2 && !isalpha((unsigned char)tag[i])) ||
                (region == 3 && !isdigit((unsigned char)tag[i])))
                return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        char c = tag[i];
        configuration->language_tag[i] =
            (i < separator) ? (char)tolower((unsigned char)c) :
            (char)toupper((unsigned char)c);
    }
    configuration->language_tag[length] = 0;
    return 1;
}

static int decoded_locale(const uint8_t *input, char base, char output[4]) {
    if (!input || !output) return 0;
    output[0] = output[1] = output[2] = output[3] = 0;
    if (!input[0] && !input[1]) return 1;
    if (input[0] & 0x80u) {
        output[0] = (char)(base + (input[1] & 0x1fu));
        output[1] = (char)(base +
            ((input[1] >> 5) & 0x7u) + ((input[0] & 3u) << 3));
        output[2] = (char)(base + ((input[0] >> 2) & 0x1fu));
        for (size_t i = 0; i < 3; ++i) {
            if (output[i] < base ||
                output[i] > (base == 'a' ? 'z' : '9')) return 0;
        }
    } else {
        if (!((input[0] >= (uint8_t)'a' &&
               input[0] <= (uint8_t)'z') ||
              (input[0] >= (uint8_t)'A' &&
               input[0] <= (uint8_t)'Z')) ||
            !((input[1] >= (uint8_t)'a' &&
               input[1] <= (uint8_t)'z') ||
              (input[1] >= (uint8_t)'A' &&
               input[1] <= (uint8_t)'Z'))) return 0;
        output[0] = (char)input[0];
        output[1] = (char)input[1];
    }
    return 1;
}

/* Return 1 for an observed exact qualifier match, 0 for observed mismatch,
 * -1 for a qualifier whose value the host has NOT independently measured.
 * This is intentionally stricter than Android's full resource chooser. */
static int evaluate_config(const uint8_t *b, size_t type_at,
                           const BsQuestArscConfiguration *wanted,
                           uint64_t *rank, int *has_density,
                           int *exact_density) {
    size_t config = type_at + 20;
    size_t config_size = rd32(b + config);
    char lang[4] = {0}, region[4] = {0};
    if (!decoded_locale(b + config + 8, 'a', lang) ||
        !decoded_locale(b + config + 10, '0', region))
        return -1;
    /* Uppercase the two-letter ISO region (the 3-digit form stays numeric). */
    if (strlen(region) == 2)
        for (size_t i = 0; i < 2; ++i)
            region[i] = (char)toupper((unsigned char)region[i]);
    if (*region && !*lang) return -1;
    const char *native_tag = wanted ? wanted->language_tag : "";
    if (wanted && !memchr(wanted->language_tag, 0,
                          sizeof wanted->language_tag)) return -1;
    char observed_lang[4] = {0}, observed_region[4] = {0};
    if (native_tag[0]) {
        const char *sep = strchr(native_tag, '-');
        size_t n = sep ? (size_t)(sep - native_tag) : strlen(native_tag);
        if ((n != 2 && n != 3) || n >= sizeof observed_lang) return -1;
        memcpy(observed_lang, native_tag, n);
        if (sep) {
            n = strlen(sep + 1);
            if ((n != 2 && n != 3) ||
                n >= sizeof observed_region) return -1;
            memcpy(observed_region, sep + 1, n);
        }
    }
    uint64_t score = 0;
    if (*lang) {
        if (!*observed_lang) return -1;
        if (strcmp(observed_lang, lang)) return 0;
        score += 1000000u;
        if (*region) {
            if (!*observed_region) return -1;
            if (strcmp(observed_region, region)) return 0;
            score += 400000u;
        }
    }
    uint8_t orientation = b[config + 12];
    uint16_t density = rd16(b + config + 14);
    uint16_t sdk = rd16(b + config + 24);
    *has_density = density != 0;
    *exact_density = density && wanted && wanted->density_dpi &&
        density == wanted->density_dpi;
    if (orientation) {
        if (!wanted || !wanted->orientation) return -1;
        if (orientation != wanted->orientation) return 0;
        score += 20000u;
    }
    if (density) {
        if (!wanted || !wanted->density_dpi) return -1;
        if (density != wanted->density_dpi) return 0;
        score += 200000u;
    }
    if (sdk) {
        if (!wanted || !wanted->sdk_version) return -1;
        if (sdk > wanted->sdk_version) return 0;
        score += sdk;
    }
    /* No guessed MCC/MNC, touchscreen, input, DP, layout, script, color
     * mode or locale variant. If such a qualifier could affect this entry,
     * fail closed even when an unqualified value exists. */
    for (size_t i = 4; i < config_size; ++i) {
        if (i == 8 || i == 9 || i == 10 || i == 11 ||
            i == 12 || i == 14 || i == 15 || i == 24 || i == 25)
            continue;
        if (b[config + i]) return -1;
    }
    *rank = score;
    return 1;
}

static int selected_raw(const BsQuestArscResolver *r, uint32_t id,
                        const BsQuestArscConfiguration *config,
                        RawResource *out) {
    if (out) *out = (RawResource){0};
    if (!r || !r->bytes || !out ||
        (id >> 24) != r->package_id) return 0;
    const unsigned stored_type = (id >> 16) & 0xffu;
    if (!stored_type || stored_type <= r->type_id_offset) return 0;
    const unsigned type = stored_type - r->type_id_offset;
    const unsigned entry_id = id & 0xffffu;
    const uint8_t *b = r->bytes;
    Chunk pkg = {0};
    if (!chunk_at(b, r->size, r->package_offset, &pkg)) return 0;
    ResourceChoice choice = {0};
    int saw_entry = 0;
    for (size_t pos = pkg.at + pkg.header_size;
         pos < pkg.at + pkg.size;) {
        Chunk ch = {0};
        if (!chunk_at(b, r->size, pos, &ch)) return 0;
        if (ch.type == RES_TYPE_TYPE && b[pos + 8] == type &&
            entry_id < rd32(b + pos + 12)) {
            uint32_t relative = rd32(b + pos + ch.header_size + 4 * entry_id);
            if (relative != UINT32_MAX) {
                saw_entry = 1;
                uint64_t rank = 0;
                int has_density = 0, exact_density = 0;
                int matched = evaluate_config(b, pos, config, &rank,
                                              &has_density, &exact_density);
                if (has_density) choice.density_variants = 1;
                if (matched < 0) choice.missing_qualifier = 1;
                if (matched > 0) {
                    size_t record = pos + rd32(b + pos + 16) + relative;
                    if (rd16(b + record + 2) & 1u) {
                        /* Complex array/style/map entries need their own
                         * parser and cannot pretend to be scalar values. */
                        choice.missing_qualifier = 1;
                    } else {
                        size_t value = record + rd16(b + record);
                        RawResource candidate = {
                            .type = b[value + 3],
                            .data = rd32(b + value + 4),
                            .key_index = rd32(b + record + 4)
                        };
                        if (!choice.found || rank > choice.rank) {
                            choice.value = candidate;
                            choice.rank = rank;
                            choice.found = 1;
                            choice.ambiguous = 0;
                        } else if (rank == choice.rank) {
                            choice.ambiguous = 1;
                        }
                        if (exact_density) choice.matching_density = 1;
                    }
                }
            }
        }
        pos += ch.size;
    }
    if (!saw_entry || !choice.found || choice.ambiguous ||
        choice.missing_qualifier ||
        (choice.density_variants && !choice.matching_density))
        return 0;
    *out = choice.value;
    return 1;
}

static int resolved_raw(const BsQuestArscResolver *r, uint32_t id,
                        const BsQuestArscConfiguration *config,
                        RawResource *out) {
    uint32_t visited[12] = {0};
    for (unsigned depth = 0; depth < 12; ++depth) {
        for (unsigned i = 0; i < depth; ++i)
            if (visited[i] == id) return 0;
        visited[depth] = id;
        if (!selected_raw(r, id, config, out)) return 0;
        if (out->type != RES_VALUE_REFERENCE) return 1;
        id = out->data;
        if (!id || (id >> 24) != r->package_id) return 0;
    }
    return 0;
}

int bs_quest_arsc_get_string(const BsQuestArscResolver *r, uint32_t id,
                             const BsQuestArscConfiguration *config,
                             char *utf8, size_t capacity) {
    if (utf8 && capacity) utf8[0] = 0;
    if (!utf8 || !capacity || capacity > BS_QUEST_ARSC_MAX_STRING_BYTES)
        return 0;
    RawResource value = {0};
    if (!resolved_raw(r, id, config, &value) ||
        value.type != RES_VALUE_STRING) return 0;
    Chunk ch = {0};
    Pool pool = {0};
    if (!chunk_at(r->bytes, r->size, r->global_pool_offset, &ch) ||
        !pool_info(r->bytes, r->size, ch, &pool))
        return 0;
    return pool_string(r->bytes, &pool, value.data, utf8, capacity);
}

int bs_quest_arsc_get_boolean(const BsQuestArscResolver *r, uint32_t id,
                              const BsQuestArscConfiguration *config,
                              int *value) {
    if (value) *value = 0;
    RawResource raw = {0};
    if (!value || !resolved_raw(r, id, config, &raw) ||
        raw.type != RES_VALUE_INT_BOOLEAN) return 0;
    *value = raw.data != 0;
    return 1;
}

int bs_quest_arsc_get_integer(const BsQuestArscResolver *r, uint32_t id,
                              const BsQuestArscConfiguration *config,
                              int32_t *value) {
    if (value) *value = 0;
    RawResource raw = {0};
    if (!value || !resolved_raw(r, id, config, &raw) ||
        (raw.type != RES_VALUE_INT_DEC &&
         raw.type != RES_VALUE_INT_HEX)) return 0;
    *value = (int32_t)raw.data;
    return 1;
}

int bs_quest_arsc_get_name(const BsQuestArscResolver *r, uint32_t id,
                           char *type_name, size_t type_capacity,
                           char *entry_name, size_t entry_capacity) {
    if (type_name && type_capacity) type_name[0] = 0;
    if (entry_name && entry_capacity) entry_name[0] = 0;
    if (!r || !r->bytes || !type_name || !type_capacity ||
        !entry_name || !entry_capacity ||
        (id >> 24) != r->package_id) return 0;
    const unsigned resource_type = (id >> 16) & 0xffu;
    if (!resource_type || resource_type <= r->type_id_offset) return 0;
    const unsigned type_index = resource_type - r->type_id_offset;
    const unsigned entry_index = id & 0xffffu;
    Chunk types_ch = {0}, keys_ch = {0}, pkg = {0};
    Pool types = {0}, keys = {0};
    if (!chunk_at(r->bytes, r->size, r->type_pool_offset, &types_ch) ||
        !chunk_at(r->bytes, r->size, r->key_pool_offset, &keys_ch) ||
        !chunk_at(r->bytes, r->size, r->package_offset, &pkg) ||
        !pool_info(r->bytes, r->size, types_ch, &types) ||
        !pool_info(r->bytes, r->size, keys_ch, &keys) ||
        type_index > types.count) return 0;
    uint32_t name_index = UINT32_MAX;
    for (size_t pos = pkg.at + pkg.header_size;
         pos < pkg.at + pkg.size;) {
        Chunk ch = {0};
        if (!chunk_at(r->bytes, r->size, pos, &ch)) return 0;
        if (ch.type == RES_TYPE_TYPE &&
            r->bytes[pos + 8] == type_index &&
            entry_index < rd32(r->bytes + pos + 12)) {
            uint32_t relative = rd32(r->bytes + pos + ch.header_size +
                                      4 * entry_index);
            if (relative != UINT32_MAX) {
                size_t entry = pos + rd32(r->bytes + pos + 16) + relative;
                uint32_t key = rd32(r->bytes + entry + 4);
                if (name_index != UINT32_MAX && key != name_index)
                    return 0;
                name_index = key;
            }
        }
        pos += ch.size;
    }
    if (name_index == UINT32_MAX || name_index >= keys.count ||
        !pool_string(r->bytes, &types, type_index - 1,
                     type_name, type_capacity) ||
        !pool_string(r->bytes, &keys, name_index,
                     entry_name, entry_capacity)) {
        type_name[0] = 0;
        entry_name[0] = 0;
        return 0;
    }
    return 1;
}
