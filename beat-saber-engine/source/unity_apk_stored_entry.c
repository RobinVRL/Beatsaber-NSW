#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "unity_apk_stored_entry.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_END UINT32_C(0x06054b50)
#define ZIP_CENTRAL UINT32_C(0x02014b50)
#define ZIP_LOCAL UINT32_C(0x04034b50)
#define ZIP_MAX_TRAILER (65535u + 22u)

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_at(FILE *f, uint64_t total, uint64_t off,
                   void *out, size_t len) {
    if (!f || !out || off > total || len > total - off ||
        off > LONG_MAX || fseek(f, (long)off, SEEK_SET) != 0)
        return 0;
    return fread(out, 1, len, f) == len;
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                    (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ UINT32_MAX;
}

void bs_apk_free_stored_entry(BsApkStoredEntry *entry) {
    if (!entry) return;
    free(entry->bytes);
    *entry = (BsApkStoredEntry){0};
}

int bs_apk_read_stored_entry(const char *apk_path, const char *entry_name,
                             size_t max_bytes, BsApkStoredEntry *out) {
    if (!out || out->bytes || out->size || out->crc32 ||
        !apk_path || !*apk_path || !entry_name || !*entry_name ||
        !max_bytes || max_bytes > UINT32_MAX)
        return 0;
    size_t target_len = strlen(entry_name);
    if (target_len > UINT16_MAX) return 0;
    FILE *fp = fopen(apk_path, "rb");
    if (!fp) return 0;

    int success = 0;
    uint8_t *tail = NULL;
    uint8_t *name = NULL;
    uint8_t *payload = NULL;
    do {
        if (fseek(fp, 0, SEEK_END) != 0) break;
        long end = ftell(fp);
        if (end < 22) break;
        const uint64_t file_size = (uint64_t)end;
        const size_t tail_size = (size_t)(file_size > ZIP_MAX_TRAILER ?
                                          ZIP_MAX_TRAILER : file_size);
        tail = malloc(tail_size);
        if (!tail || !read_at(fp, file_size, file_size - tail_size,
                              tail, tail_size)) break;
        size_t eocd = tail_size;
        for (size_t i = tail_size - 22; ; --i) {
            if (rd32(tail + i) == ZIP_END &&
                i + 22u + rd16(tail + i + 20) == tail_size) {
                eocd = i;
                break;
            }
            if (i == 0) break;
        }
        if (eocd == tail_size) break;
        const uint8_t *e = tail + eocd;
        const unsigned n = rd16(e + 10);
        const uint64_t cd_off = rd32(e + 16);
        const uint64_t cd_size = rd32(e + 12);
        const uint64_t cd_end = cd_off + cd_size;
        /* No multi-disk or ZIP64; reject mismatched central count too. */
        if (rd16(e + 4) || rd16(e + 6) || rd16(e + 8) != n ||
            !n || n == UINT16_MAX || cd_off == UINT32_MAX ||
            cd_size == UINT32_MAX ||
            cd_end > file_size - tail_size + eocd) break;

        uint64_t cursor = cd_off;
        uint64_t selected_local = 0;
        uint32_t selected_crc = 0;
        uint32_t selected_size = 0;
        uint16_t selected_flags = 0;
        unsigned found = 0;
        int bad = 0;
        for (unsigned i = 0; i < n; ++i) {
            uint8_t c[46];
            if (cursor > cd_end || sizeof c > cd_end - cursor ||
                !read_at(fp, file_size, cursor, c, sizeof c) ||
                rd32(c) != ZIP_CENTRAL) {
                bad = 1;
                break;
            }
            uint16_t name_len = rd16(c + 28);
            uint16_t extra_len = rd16(c + 30);
            uint16_t comment_len = rd16(c + 32);
            const uint64_t next = cursor + sizeof c + name_len +
                                  extra_len + comment_len;
            if (next > cd_end || next <= cursor) {
                bad = 1;
                break;
            }
            if (name_len == target_len) {
                name = malloc(name_len ? name_len : 1);
                if (!name || !read_at(fp, file_size, cursor + sizeof c,
                                      name, name_len)) {
                    bad = 1;
                    break;
                }
                if (memcmp(name, entry_name, name_len) == 0) {
                    ++found;
                    /* Encrypted, nonstored, oversized, ambiguous and split
                     * target entries cannot represent verified APK data. */
                    uint32_t compressed = rd32(c + 20);
                    selected_size = rd32(c + 24);
                    selected_crc = rd32(c + 16);
                    selected_flags = rd16(c + 8);
                    selected_local = rd32(c + 42);
                    if (found != 1 || (selected_flags & 1u) ||
                        rd16(c + 10) != 0 ||
                        rd16(c + 34) != 0 ||
                        selected_size != compressed ||
                        selected_size > max_bytes ||
                        !selected_size || selected_local == UINT32_MAX ||
                        compressed == UINT32_MAX) {
                        bad = 1;
                        break;
                    }
                }
                free(name);
                name = NULL;
            }
            cursor = next;
        }
        if (bad || found != 1 || cursor != cd_end) break;

        uint8_t local[30];
        if (!read_at(fp, file_size, selected_local, local, sizeof local) ||
            rd32(local) != ZIP_LOCAL ||
            (rd16(local + 6) & 1u) || rd16(local + 8) != 0 ||
            rd16(local + 26) != target_len ||
            (rd16(local + 6) & ~UINT16_C(0x0008)) !=
                (selected_flags & ~UINT16_C(0x0008))) break;
        if (rd32(local + 14) && rd32(local + 14) != selected_crc) break;
        if (rd32(local + 18) && rd32(local + 18) != selected_size) break;
        if (rd32(local + 22) && rd32(local + 22) != selected_size) break;
        const uint64_t data_at = selected_local + sizeof local +
                                 rd16(local + 26) + rd16(local + 28);
        if (data_at > cd_off || selected_size > cd_off - data_at) break;
        name = malloc(target_len);
        if (!name || !read_at(fp, file_size,
                              selected_local + sizeof local,
                              name, target_len) ||
            memcmp(name, entry_name, target_len) != 0) break;
        free(name);
        name = NULL;
        payload = malloc(selected_size);
        if (!payload || !read_at(fp, file_size, data_at,
                                 payload, selected_size) ||
            crc32_bytes(payload, selected_size) != selected_crc) break;
        out->bytes = payload;
        out->size = selected_size;
        out->crc32 = selected_crc;
        payload = NULL;
        success = 1;
    } while (0);
    free(tail);
    free(name);
    free(payload);
    fclose(fp);
    return success;
}
