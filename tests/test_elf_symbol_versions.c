/* Standalone host tests: original staged Quest OpenSSL ELFs + corrupt fixtures.
 * Build from beat-saber-engine/ with Clang:
 *   clang -std=c11 -Wall -Wextra -Werror -Isource \
 *     tests/test_elf_symbol_versions.c source/elf_symbol_versions.c -o elf_versions_test
 * No Unity/Quest code is executed; ELF file images are only parsed as data.
 */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "elf_symbol_versions.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static void rebase_program_headers(uint8_t *image, uint64_t bias) {
    const size_t phoff = (size_t)get64(image + 32);
    for (size_t i = 0; i < get16(image + 56); ++i) {
        uint8_t *ph = image + phoff + i * 56;
        put64(ph + 16, get64(ph + 16) + bias);
    }
}

static uint32_t elf_hash(const char *name) {
    uint32_t h = 0;
    for (; *name; ++name) {
        h = (h << 4) + (uint8_t)*name;
        uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static void dynamic_entry(uint8_t *image, unsigned index,
                          uint64_t tag, uint64_t value) {
    put64(image + 0x200 + index * 16, tag);
    put64(image + 0x208 + index * 16, value);
}

/* Build a tiny sectionless ET_DYN, intentionally forcing DT_HASH fallback. */
static void make_fixture(uint8_t *image, bool provider) {
    const char *name = "BIO_ctrl";
    const char *version = "OPENSSL_1_1_0";
    const char *soname = "libcrypto.so";
    const unsigned name_off = 1;
    const unsigned version_off = name_off + (unsigned)strlen(name) + 1;
    const unsigned soname_off = version_off + (unsigned)strlen(version) + 1;
    memset(image, 0, 4096);
    memcpy(image, "\177ELF", 4);
    image[4] = 2; image[5] = 1; image[6] = 1;
    put16(image + 16, 3);    /* ET_DYN */
    put16(image + 18, 183);  /* EM_AARCH64 */
    put64(image + 32, 64);   /* e_phoff */
    put16(image + 52, 64);   /* e_ehsize */
    put16(image + 54, 56);   /* e_phentsize */
    put16(image + 56, 2);
    /* PT_LOAD: all dynamic records and the dummy defined symbol are in range. */
    put32(image + 64, 1);
    put32(image + 68, 7);
    put64(image + 64 + 32, 4096);
    put64(image + 64 + 40, 4096);
    put64(image + 64 + 48, 0x1000);
    /* PT_DYNAMIC */
    put32(image + 120, 2);
    put64(image + 120 + 8, 0x200);
    put64(image + 120 + 16, 0x200);
    put64(image + 120 + 32, 0x120);
    put64(image + 120 + 40, 0x120);
    unsigned n = 0;
    dynamic_entry(image, n++, 4, 0x300);         /* DT_HASH */
    dynamic_entry(image, n++, 5, 0x600);         /* DT_STRTAB */
    dynamic_entry(image, n++, 6, 0x400);         /* DT_SYMTAB */
    dynamic_entry(image, n++, 10, 100);          /* DT_STRSZ */
    dynamic_entry(image, n++, 11, 24);           /* DT_SYMENT */
    dynamic_entry(image, n++, 0x6ffffff0u, 0x800); /* DT_VERSYM */
    if (provider) {
        dynamic_entry(image, n++, 14, soname_off);
        dynamic_entry(image, n++, 0x6ffffffcu, 0xa00);
        dynamic_entry(image, n++, 0x6ffffffdu, 1);
    } else {
        dynamic_entry(image, n++, 0x6ffffffeu, 0x900);
        dynamic_entry(image, n++, 0x6fffffffu, 1);
        dynamic_entry(image, n++, 1, soname_off); /* DT_NEEDED */
    }
    dynamic_entry(image, n, 0, 0);
    put32(image + 0x300, 1);  /* SysV hash nbucket */
    put32(image + 0x304, 2);  /* SysV hash nchain / dynsym count */
    put32(image + 0x308, 1);
    put32(image + 0x400 + 24, name_off);
    image[0x400 + 24 + 4] = 0x12; /* STB_GLOBAL, STT_FUNC */
    if (provider) {
        put16(image + 0x400 + 24 + 6, 1);
        put64(image + 0x400 + 24 + 8, 0xb00);
        put64(image + 0x400 + 24 + 16, 16);
    }
    memcpy(image + 0x600 + name_off, name, strlen(name) + 1);
    memcpy(image + 0x600 + version_off, version, strlen(version) + 1);
    memcpy(image + 0x600 + soname_off, soname, strlen(soname) + 1);
    put16(image + 0x802, 2);
    if (provider) {
        put16(image + 0xa00, 1);     /* vd_version */
        put16(image + 0xa04, 2);     /* vd_ndx */
        put16(image + 0xa06, 1);     /* vd_cnt */
        put32(image + 0xa08, elf_hash(version));
        put32(image + 0xa0c, 20);   /* vd_aux */
        put32(image + 0xa14, version_off); /* vda_name */
    } else {
        put16(image + 0x900, 1);     /* vn_version */
        put16(image + 0x902, 1);     /* vn_cnt */
        put32(image + 0x904, soname_off);
        put32(image + 0x908, 16);    /* vn_aux */
        put32(image + 0x910, elf_hash(version)); /* vna_hash */
        put16(image + 0x916, 2);     /* vna_other */
        put32(image + 0x918, version_off);
    }
}

static void test_fixture(void) {
    uint8_t importing[4096], providing[4096];
    make_fixture(importing, false);
    make_fixture(providing, true);
    BsElfVersionView from, to;
    BsElfVersionRequirement req;
    BsElfVersionExport exp;
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == 1);
    assert(bs_elf_version_view_init(&to, providing, sizeof(providing)) == 1);
    assert(from.needed_count == 1);
    const char *needed = NULL;
    assert(bs_elf_version_dependency(&from, 0, &needed) == 1);
    assert(strcmp(needed, "libcrypto.so") == 0);
    assert(bs_elf_version_dependency(&from, 1, &needed) == 0);
    assert(bs_elf_version_import(&from, 1, &req) == 1);
    assert(req.explicit_version && strcmp(req.version_name, "OPENSSL_1_1_0") == 0);
    assert(strcmp(req.needed_soname, "libcrypto.so") == 0);
    assert(bs_elf_version_allows_platform_shim(&from, &req) == 0);
    /* Same symbol name must never authorize a platform shim for a third-party
     * GNU version, even if a platform function happens to export that name. */
    BsElfVersionRequirement forged = req;
    forged.version_name = "LIBC";
    forged.version_hash = elf_hash("LIBC");
    assert(bs_elf_version_allows_platform_shim(&from, &forged) == 0);
    forged.needed_soname = "libc.so"; /* not in fixture DT_NEEDED */
    assert(bs_elf_version_allows_platform_shim(&from, &forged) == 0);
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 1);
    assert(exp.symbol_index == 1 && exp.symbol_value == 0xb00);
    assert(bs_elf_version_find_export(&to, "libdifferent.so", "BIO_ctrl", &req, &exp) == 1);
    /* SONAME is authoritative even if the staged file path is an alias. */
    req.version_hash ^= 1;
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);
    req.version_hash ^= 1;
    req.needed_soname = "libssl.so";
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);
    req.needed_soname = "libcrypto.so";
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "other", &req, &exp) == -1);

    put16(providing + 0x802, 0x8002); /* non-default GNU version */
    assert(bs_elf_version_view_init(&to, providing, sizeof(providing)) == 1);
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 1);
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", NULL, &exp) == 0);
    put16(providing + 0x802, 2);
    providing[0x400 + 24 + 5] = 2; /* STV_HIDDEN */
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);
    providing[0x400 + 24 + 5] = 0;
    put32(providing + 0xa08, 0); /* incorrect version hash */
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);
    put32(providing + 0xa08, elf_hash("OPENSSL_1_1_0"));
    put16(providing + 0x400 + 24 + 6, 0); /* undefined export */
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);
    put16(providing + 0x400 + 24 + 6, 1);
    providing[0x400 + 24 + 4] = 0x16; /* STT_TLS is not an address */
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);
    providing[0x400 + 24 + 4] = 0x1a; /* STT_GNU_IFUNC must not execute resolver */
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 0);

    /* so_load rebases p_vaddr inside so_base; DT_* values are still unrebased. */
    make_fixture(importing, false);
    make_fixture(providing, true);
    const uint64_t bias = 0x7100000000ULL;
    rebase_program_headers(importing, bias);
    rebase_program_headers(providing, bias + 0x100000);
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == -1);
    assert(bs_elf_version_view_init_rebased(&from, importing, sizeof(importing),
                                            bias) == 1);
    assert(bs_elf_version_view_init_rebased(&to, providing, sizeof(providing),
                                            bias + 0x100000) == 1);
    assert(bs_elf_version_import(&from, 1, &req) == 1);
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl", &req, &exp) == 1);

    /* Exercise the sectionless DT_GNU_HASH count fallback independently. */
    make_fixture(providing, true);
    dynamic_entry(providing, 0, 0x6ffffef5u, 0x340);
    put32(providing + 0x340, 1); /* nbuckets */
    put32(providing + 0x344, 1); /* symoffset */
    put32(providing + 0x348, 1); /* bloom size */
    put32(providing + 0x358, 1); /* bucket 0 = dynsym #1 */
    put32(providing + 0x35c, 1); /* final chain entry */
    assert(bs_elf_version_view_init(&to, providing, sizeof(providing)) == 1);
    assert(to.symbol_count == 2);
    assert(bs_elf_version_find_export(&to, "libcrypto.so", "BIO_ctrl",
                                      &req, &exp) == 1);
    put32(providing + 0x344, 3); /* bucket points before symoffset */
    assert(bs_elf_version_view_init(&to, providing, sizeof(providing)) == -1);

    make_fixture(importing, false);
    put16(importing + 0x802, 3); /* unknown VERNEED index */
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == 1);
    assert(bs_elf_version_import(&from, 1, &req) == -1);
    make_fixture(importing, false);
    put32(importing + 0x908, 0xffffff00); /* malformed vn_aux */
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == -1);
    make_fixture(importing, false);
    dynamic_entry(importing, 8, 1, 9999); /* out-of-range DT_NEEDED */
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == -1);
    make_fixture(importing, false);
    dynamic_entry(importing, 9, 1, 10); /* second DT_NEEDED */
    dynamic_entry(importing, 10, 0, 0);
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == 1);
    assert(from.needed_count == 2);
    assert(bs_elf_version_dependency(&from, 0, &needed) == 1);
    assert(strcmp(needed, "libcrypto.so") == 0);
    assert(bs_elf_version_dependency(&from, 1, &needed) == 1);
    assert(strcmp(needed, "OPENSSL_1_1_0") == 0);
    assert(bs_elf_version_dependency(&from, 2, &needed) == 0);
    make_fixture(importing, false);
    importing[18] = 0; /* wrong machine */
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == -1);
    make_fixture(importing, false);
    dynamic_entry(importing, 5, 0x6ffffff0u, 0xfff); /* truncated versym */
    assert(bs_elf_version_view_init(&from, importing, sizeof(importing)) == -1);
}

static uint8_t *read_file(const char *filename, size_t *size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open staged Quest library %s\n", filename);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) || ftell(f) <= 0) { fclose(f); return NULL; }
    *size = (size_t)ftell(f);
    rewind(f);
    uint8_t *image = malloc(*size);
    if (!image) { fclose(f); return NULL; }
    if (fread(image, 1, *size, f) != *size) {
        free(image);
        image = NULL;
    }
    fclose(f);
    return image;
}

static size_t validate_quest_dependency(const char *consumer_path,
                                        const char *provider_path) {
    size_t csize = 0, psize = 0;
    uint8_t *consumer_image = read_file(consumer_path, &csize);
    uint8_t *provider_image = read_file(provider_path, &psize);
    assert(consumer_image && provider_image);
    BsElfVersionView consumer, provider;
    assert(bs_elf_version_view_init(&consumer, consumer_image, csize) == 1);
    assert(bs_elf_version_view_init(&provider, provider_image, psize) == 1);
    assert(provider.soname != NULL);
    size_t matches = 0;
    int needed_by_consumer = 0;
    for (size_t d = 0; d < consumer.needed_count; ++d) {
        const char *needed = NULL;
        assert(bs_elf_version_dependency(&consumer, d, &needed) == 1);
        if (strcmp(needed, provider.soname) == 0)
            needed_by_consumer = 1;
    }
    for (size_t i = 0; i < consumer.symbol_count; ++i) {
        BsElfVersionRequirement req;
        if (bs_elf_version_import(&consumer, i, &req) != 1) continue;
        if (!req.explicit_version || strcmp(req.needed_soname, provider.soname))
            continue;
        assert(needed_by_consumer &&
               "versioned imports must name a direct DT_NEEDED dependency");
        BsElfVersionExport exp;
        assert(strncmp(req.version_name, "OPENSSL_", 8) == 0);
        assert(bs_elf_version_allows_platform_shim(&consumer, &req) == 0);
        assert(bs_elf_version_find_export(&provider, provider_path,
                                          req.symbol_name, &req, &exp) == 1);
        assert(exp.symbol_index < provider.symbol_count);
        BsElfVersionRequirement wrong = req;
        wrong.version_name = "OPENSSL_NONEXISTENT_0";
        assert(bs_elf_version_find_export(&provider, provider_path,
                                          req.symbol_name, &wrong, &exp) == 0);
        ++matches;
    }
    free(consumer_image);
    free(provider_image);
    return matches;
}

/* The Eden regression was 12/12 missing libmain imports. All are explicitly
 * @LIBC rather than unversioned! There is deliberately no guest libc.so or
 * libdl.so; the curated native platform bridge provides their named symbols.
 */
static void validate_real_libmain_bionic_shims(void) {
    const char *const expected[] = {
        "__cxa_finalize", "__cxa_atexit", "__register_atfork", "malloc",
        "memcpy", "free", "dlsym", "dlclose", "snprintf", "dlopen",
        "dlerror", "__stack_chk_fail"
    };
    const char *path = "private-sd/switch/beat-saber-quest/libmain.so";
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    assert(data);
    BsElfVersionView view;
    assert(bs_elf_version_view_init(&view, data, size) == 1);
    size_t total = 0;
    bool found[sizeof(expected)/sizeof(expected[0])] = {false};
    for (size_t i = 1; i < view.symbol_count; ++i) {
        BsElfVersionRequirement req;
        if (bs_elf_version_import(&view, i, &req) != 1) continue;
        assert(req.explicit_version);
        assert(strcmp(req.version_name, "LIBC") == 0);
        assert(req.version_hash == elf_hash("LIBC"));
        assert(bs_elf_version_allows_platform_shim(&view, &req) == 1);
        bool known = false;
        for (size_t k = 0; k < sizeof(expected)/sizeof(expected[0]); ++k) {
            if (!strcmp(req.symbol_name, expected[k])) {
                assert(!found[k]);
                found[k] = true;
                known = true;
                break;
            }
        }
        assert(known && "unexpected Bionic import in genuine libmain.so");
        ++total;

        BsElfVersionRequirement wrong = req;
        wrong.version_hash ^= 1;
        assert(bs_elf_version_allows_platform_shim(&view, &wrong) == 0);
        wrong = req;
        wrong.version_name = "LIBC_P";
        assert(bs_elf_version_allows_platform_shim(&view, &wrong) == 0);
        wrong = req;
        wrong.needed_soname = "libcrypto.so";
        assert(bs_elf_version_allows_platform_shim(&view, &wrong) == 0);
    }
    assert(total == sizeof(expected)/sizeof(expected[0]));
    for (size_t k = 0; k < total; ++k) assert(found[k]);
    /* Verify staged PHDR rebasing does not invalidate a Bionic requirement. */
    rebase_program_headers(data, UINT64_C(0x7100000000));
    assert(bs_elf_version_view_init_rebased(&view, data, size,
                                           UINT64_C(0x7100000000)) == 1);
    size_t rebased = 0;
    for (size_t i = 1; i < view.symbol_count; ++i) {
        BsElfVersionRequirement req;
        if (bs_elf_version_import(&view, i, &req) == 1) {
            assert(bs_elf_version_allows_platform_shim(&view, &req) == 1);
            ++rebased;
        }
    }
    assert(rebased == 12);
    /* Regression guard: all 12 original versioned imports still have an
     * actual exact-name platform-bridge implementation in this source tree. */
    size_t bridge_size = 0;
    uint8_t *bridge = read_file("source/platform_bridge.c", &bridge_size);
    assert(bridge && bridge_size);
    char *bridge_text = malloc(bridge_size + 1);
    assert(bridge_text);
    memcpy(bridge_text, bridge, bridge_size);
    bridge_text[bridge_size] = 0;
    for (size_t k = 0; k < total; ++k) {
        char needle[128];
        snprintf(needle, sizeof(needle), "{\"%s\",", expected[k]);
        assert(strstr(bridge_text, needle) &&
               "real libmain import missing from platform-bridge lookup");
    }
    free(bridge_text);
    free(bridge);
    free(data);
    puts("PASS: all 12 genuine libmain @LIBC imports can use exact-name Bionic shims");
}

static void validate_newer_bionic_shims(void) {
    static const struct { const char *library; size_t allowed, rejected; } cases[] = {
        {"libunity.so", 2, 0},
        {"libopenxr_loader.so", 4, 0},
        {"libOVRPlugin.so", 6, 0},
        {"libovrplatformloader.so", 1, 0},
        {"libcrypto.so", 1, 0},
    };
    for (size_t k = 0; k < sizeof(cases)/sizeof(cases[0]); ++k) {
        char path[256];
        snprintf(path, sizeof(path), "private-sd/switch/beat-saber-quest/%s",
                 cases[k].library);
        size_t size;
        uint8_t *data = read_file(path, &size);
        assert(data);
        BsElfVersionView view;
        assert(bs_elf_version_view_init(&view, data, size) == 1);
        size_t allowed = 0, rejected = 0;
        for (size_t i = 1; i < view.symbol_count; ++i) {
            BsElfVersionRequirement req;
            if (bs_elf_version_import(&view, i, &req) != 1 ||
                !req.explicit_version || strncmp(req.version_name, "LIBC_", 5))
                continue;
            assert(bs_elf_version_allows_platform_shim(&view, &req) == 1);
            ++allowed;
            BsElfVersionRequirement wrong = req;
            wrong.symbol_name = "malloc";
            assert(bs_elf_version_allows_platform_shim(&view, &wrong) == 0);
            wrong = req;
            wrong.version_hash ^= 1;
            assert(bs_elf_version_allows_platform_shim(&view, &wrong) == 0);
            wrong = req;
            wrong.needed_soname = "libm.so";
            assert(bs_elf_version_allows_platform_shim(&view, &wrong) == 0);
        }
        assert(allowed == cases[k].allowed && rejected == cases[k].rejected);
        free(data);
    }
}

static void validate_all_quest_images(void) {
    const char *libraries[] = {
        "libmain.so", "libc++_shared.so", "libunity.so", "libil2cpp.so",
        "libgame.so", "lib_burst_generated.so", "libopenxr_loader.so",
        "libUnityOpenXR.so", "libOVRPlugin.so", "libOVRMetricsTool.so",
        "libovrplatformloader.so", "libhaptics_sdk.so", "libenet.so",
        "libcrypto.so", "libssl.so"
    };
    for (size_t i = 0; i < sizeof(libraries)/sizeof(libraries[0]); ++i) {
        char path[256];
        snprintf(path, sizeof(path), "private-sd/switch/beat-saber-quest/%s",
                 libraries[i]);
        size_t size;
        uint8_t *file = read_file(path, &size);
        assert(file);
        BsElfVersionView view;
        assert(bs_elf_version_view_init(&view, file, size) == 1);
        /* Exercise the precise in-memory file-image transformation so_load
         * performs before the dependency resolution pass. */
        uint64_t bias = 0x7100000000ULL;
        rebase_program_headers(file, bias);
        assert(bs_elf_version_view_init_rebased(&view, file, size, bias) == 1);
        free(file);
    }
}

int main(void) {
    test_fixture();
    validate_all_quest_images();
    validate_real_libmain_bionic_shims();
    validate_newer_bionic_shims();
    const char *base = "private-sd/switch/beat-saber-quest/";
    char ssl[256], enet[256], crypto[256];
    snprintf(ssl, sizeof(ssl), "%slibssl.so", base);
    snprintf(enet, sizeof(enet), "%slibenet.so", base);
    snprintf(crypto, sizeof(crypto), "%slibcrypto.so", base);
    const size_t ssl_crypto = validate_quest_dependency(ssl, crypto);
    const size_t enet_crypto = validate_quest_dependency(enet, crypto);
    const size_t enet_ssl = validate_quest_dependency(enet, ssl);
    assert(ssl_crypto == 433);
    assert(enet_crypto == 38);
    assert(enet_ssl == 38);
    printf("GNU ELF version matching passed: %zu libssl->crypto + "
           "%zu libenet->crypto + %zu libenet->ssl (509 real versioned imports); "
           "plus malformed/visibility tests\n",
           ssl_crypto, enet_crypto, enet_ssl);
    return 0;
}
