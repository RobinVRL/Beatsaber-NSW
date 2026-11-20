#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "unity_apk_dex_loader.h"
#include "unity_java_preinit.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_EOCD_SIG UINT32_C(0x06054b50)
#define ZIP_CENTRAL_SIG UINT32_C(0x02014b50)
#define ZIP_LOCAL_SIG UINT32_C(0x04034b50)
#define ZIP_TAIL_MAX (65535u + 22u)

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Deliberately accepts only ZIP32 APKs whose offsets fit this C runtime's
 * seek type. All lengths and offsets are bounded against actual file size. */
static int read_at(FILE *f, uint64_t total, uint64_t offset,
                   void *dest, size_t length) {
    if (!dest || offset > total || length > total - offset ||
        offset > LONG_MAX || fseek(f, (long)offset, SEEK_SET) != 0)
        return 0;
    return fread(dest, 1, length, f) == length;
}

void bs_unity_apk_free_original_dex(BsUnityOriginalDexImage *image) {
    if (!image) return;
    free(image->data);
    image->data = NULL;
    image->size = 0;
}

int bs_unity_apk_load_original_dex(const char *apk_path,
                                   BsUnityOriginalDexImage *out) {
    /* Never discard an existing allocation by overwriting its owner. */
    if (!out || out->data || out->size) return 0;
    if (!apk_path || !*apk_path) return 0;

    FILE *f = fopen(apk_path, "rb");
    if (!f) return 0;
    uint8_t *tail = NULL;
    uint8_t *dex = NULL;
    int valid = 0;
    do {
        if (fseek(f, 0, SEEK_END) != 0) break;
        long end = ftell(f);
        if (end < 22 || (uint64_t)end > UINT32_MAX) break;
        const uint64_t total = (uint64_t)end;
        const size_t tail_size = (size_t)(total < ZIP_TAIL_MAX ?
                                          total : ZIP_TAIL_MAX);
        tail = malloc(tail_size);
        if (!tail || !read_at(f, total, total - tail_size,
                              tail, tail_size)) break;

        /* The final ZIP record can have a comment; require its declared
         * length to end exactly at EOF, excluding decoy signatures. */
        size_t eocd = tail_size;
        for (size_t i = tail_size - 22; ; --i) {
            if (le32(tail + i) == ZIP_EOCD_SIG &&
                i + 22u + le16(tail + i + 20) == tail_size) {
                eocd = i;
                break;
            }
            if (i == 0) break;
        }
        if (eocd == tail_size) break;
        const uint8_t *e = tail + eocd;
        const uint16_t n = le16(e + 10);
        const uint64_t cd_size = le32(e + 12);
        const uint64_t cd_start = le32(e + 16);
        const uint64_t cd_end = cd_start + cd_size;
        if (le16(e + 4) != 0 || le16(e + 6) != 0 ||
            le16(e + 8) != n || n == UINT16_MAX ||
            cd_size == UINT32_MAX || cd_start == UINT32_MAX ||
            cd_end > total - tail_size + eocd) break;

        const char entry_name[] = "classes.dex";
        uint64_t cursor = cd_start, original_local = 0;
        uint32_t original_crc = 0;
        unsigned found = 0;
        for (uint16_t index = 0; index < n; ++index) {
            uint8_t c[46];
            if (cursor > cd_end || sizeof c > cd_end - cursor ||
                !read_at(f, total, cursor, c, sizeof c) ||
                le32(c) != ZIP_CENTRAL_SIG) break;
            const uint16_t name_len = le16(c + 28);
            const uint16_t extra_len = le16(c + 30);
            const uint16_t comment_len = le16(c + 32);
            const uint64_t next = cursor + sizeof c + name_len +
                                  extra_len + comment_len;
            if (next > cd_end || next <= cursor) break;
            if (name_len == sizeof entry_name - 1) {
                char name[sizeof entry_name];
                if (!read_at(f, total, cursor + sizeof c,
                             name, name_len)) break;
                name[name_len] = '\0';
                if (!strcmp(name, entry_name)) {
                    ++found;
                    /* The ORIGINAL classes.dex is STORE method (0), not
                     * encrypted. A different format is not accepted as an
                     * equivalent replacement for this pinned APK. */
                    if (found > 1 || (le16(c + 8) & 1u) != 0 ||
                        le16(c + 10) != 0 ||
                        le32(c + 20) != BS_UNITY_JAVA_DEX_SIZE ||
                        le32(c + 24) != BS_UNITY_JAVA_DEX_SIZE ||
                        le32(c + 16) != BS_UNITY_JAVA_DEX_CRC32 ||
                        le32(c + 42) == UINT32_MAX) break;
                    original_local = le32(c + 42);
                    original_crc = le32(c + 16);
                }
            }
            cursor = next;
        }
        if (found != 1 || !original_crc || cursor != cd_end) break;

        uint8_t local[30];
        if (!read_at(f, total, original_local, local, sizeof local) ||
            le32(local) != ZIP_LOCAL_SIG ||
            (le16(local + 6) & 1u) != 0 || le16(local + 8) != 0 ||
            le16(local + 26) != sizeof entry_name - 1) break;
        const uint64_t data_at = original_local + sizeof local +
                                 le16(local + 26) + le16(local + 28);
        if (data_at > cd_start ||
            BS_UNITY_JAVA_DEX_SIZE > cd_start - data_at) break;
        char local_name[sizeof entry_name];
        if (!read_at(f, total, original_local + sizeof local,
                     local_name, sizeof entry_name - 1)) break;
        local_name[sizeof entry_name - 1] = '\0';
        if (strcmp(local_name, entry_name)) break;

        dex = malloc(BS_UNITY_JAVA_DEX_SIZE);
        if (!dex || !read_at(f, total, data_at, dex,
                              BS_UNITY_JAVA_DEX_SIZE)) break;
        BsUnityJavaPreinitReport report = {0};
        if (!bs_unity_java_preinit_verify_dex(
                dex, BS_UNITY_JAVA_DEX_SIZE, &report) ||
            report.dex_crc32 != original_crc) break;

        out->data = dex;
        out->size = BS_UNITY_JAVA_DEX_SIZE;
        dex = NULL;
        valid = 1;
    } while (0);

    free(tail);
    free(dex);
    fclose(f);
    return valid;
}
