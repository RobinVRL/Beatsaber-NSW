/* Standalone constructor-graph tests using all 15 genuine Quest ARM64 ELFs.
 * Does NOT execute any guest ELF or change the original on-disk libraries.
 *
 * From beat-saber-engine (Windows, installed LLVM + devkitPro headers):
 * clang -std=c11 -Wall -Wextra -Werror -Isource \
 *   -idirafter C:/devkitPro/devkitA64/aarch64-none-elf/include \
 *   tests/test_elf_dependency_order.c source/elf_dependency_order.c \
 *   source/elf_symbol_versions.c -o tests/elf_dependency_order_test.exe
 */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "elf_dependency_order.h"
#include "elf_symbol_versions.h"
#include "so_util.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof(a[0]))

static void put16(unsigned char *p, uint16_t n) {
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}
static void put32(unsigned char *p, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8));
}
static void put64(unsigned char *p, uint64_t n) {
    for (size_t i = 0; i < 8; ++i) p[i] = (unsigned char)(n >> (i * 8));
}
static uint16_t get16(const unsigned char *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint64_t get64(const unsigned char *p) {
    uint64_t n = 0;
    for (size_t i = 0; i < 8; ++i) n |= (uint64_t)p[i] << (i * 8);
    return n;
}

static void dynamic_tag(unsigned char *elf, size_t index,
                        uint64_t tag, uint64_t value) {
    assert(index < 32);
    put64(elf + 0x200 + index * 16, tag);
    put64(elf + 0x208 + index * 16, value);
}

/* Minimal real-structure (but not executable) AArch64 sectionless ET_DYN.
 * DT_HASH supplies a single STN_UNDEF symbol so the portable version parser
 * can rigorously validate DT_NEEDED, DT_SONAME and PT_DYNAMIC ranges.
 */
static void fake_elf(unsigned char elf[4096], const char *soname,
                     const char *const *needed, size_t needed_count) {
    assert(needed_count < 12);
    memset(elf, 0, 4096);
    memcpy(elf, "\177ELF", 4);
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    put16(elf + 16, 3);    /* ET_DYN */
    put16(elf + 18, 183);  /* EM_AARCH64 */
    put64(elf + 32, 64);   /* e_phoff */
    put16(elf + 52, 64);
    put16(elf + 54, 56);
    put16(elf + 56, 2);    /* PT_LOAD + PT_DYNAMIC */

    put32(elf + 64, 1);    /* PT_LOAD, vaddr 0, file offset 0 */
    put32(elf + 68, 6);    /* PF_R|PF_W */
    put64(elf + 96, 4096);
    put64(elf + 104, 4096);
    put64(elf + 112, 4096);
    put32(elf + 120, 2);   /* PT_DYNAMIC */
    put64(elf + 128, 0x200);
    put64(elf + 136, 0x200);
    put64(elf + 152, 0x200);
    put64(elf + 160, 0x200);

    dynamic_tag(elf, 0, 4, 0x500);   /* DT_HASH */
    dynamic_tag(elf, 1, 5, 0x800);   /* DT_STRTAB */
    dynamic_tag(elf, 2, 6, 0x600);   /* DT_SYMTAB */
    dynamic_tag(elf, 3, 10, 0x100); /* DT_STRSZ */
    dynamic_tag(elf, 4, 11, 24);    /* DT_SYMENT */
    put32(elf + 0x500, 1);
    put32(elf + 0x504, 1);
    size_t str_offset = 1;
    assert(strlen(soname) + 1 < 0x100 - str_offset);
    memcpy(elf + 0x800 + str_offset, soname, strlen(soname) + 1);
    dynamic_tag(elf, 5, 14, str_offset);  /* DT_SONAME */
    str_offset += strlen(soname) + 1;
    for (size_t i = 0; i < needed_count; ++i) {
        size_t len = strlen(needed[i]);
        assert(len + 1 < 0x100 - str_offset);
        memcpy(elf + 0x800 + str_offset, needed[i], len + 1);
        dynamic_tag(elf, 6 + i, 1, str_offset);  /* DT_NEEDED */
        str_offset += len + 1;
    }
    dynamic_tag(elf, 6 + needed_count, 0, 0); /* DT_NULL */
}

static void test_synthetic_graph_errors(void) {
    unsigned char a[4096], b[4096], c[4096];
    const char *need_b[] = {"libb.so"};
    const char *need_a[] = {"liba.so"};
    const char *need_system[] = {"libc.so", "liblog.so"};
    const char *need_bad[] = {"libnotinquest.so"};
    BsElfDependencyImage images[] = {
        {"staged/liba.so", a, sizeof(a), 0},
        {"staged/libb.so", b, sizeof(b), 0}
    };
    size_t order[] = {999, 999};
    BsElfDependencyError error;

    fake_elf(a, "liba.so", need_b, COUNT(need_b));
    fake_elf(b, "libb.so", NULL, 0);
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error) == 0);
    assert(order[0] == 1 && order[1] == 0);

    /* Dependencies are matched by authoritative SONAME even for aliased
     * filenames, not by a loosely matching basename.
     */
    images[1].filename = "staged/another-file.so";
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error) == 0);
    assert(order[0] == 1 && order[1] == 0);

    fake_elf(a, "liba.so", need_system, COUNT(need_system));
    order[0] = 999; order[1] = 999;
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error)
           == BS_ELF_DEP_MISSING);
    assert(!strcmp(error.dependency, "libc.so"));
    assert(error.module_index == 0);
    assert(order[0] == 999 && order[1] == 999);
    assert(bs_elf_dependency_order_images(images, 2,
             &bs_elf_dep_quest_android_policy, order, &error) == 0);
    assert(order[0] == 0 && order[1] == 1);

    fake_elf(a, "liba.so", need_bad, COUNT(need_bad));
    order[0] = 999; order[1] = 999;
    assert(bs_elf_dependency_order_images(images, 2,
             &bs_elf_dep_quest_android_policy, order, &error)
           == BS_ELF_DEP_MISSING);
    assert(!strcmp(error.dependency, "libnotinquest.so"));
    assert(order[0] == 999 && order[1] == 999);

    fake_elf(a, "liba.so", need_b, COUNT(need_b));
    fake_elf(b, "libb.so", need_a, COUNT(need_a));
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error)
           == BS_ELF_DEP_CYCLE);
    assert(error.module_index == 0);
    assert(order[0] == 999 && order[1] == 999);

    fake_elf(a, "liba.so", need_a, COUNT(need_a));
    fake_elf(b, "libb.so", NULL, 0);
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error)
           == BS_ELF_DEP_CYCLE);

    fake_elf(b, "liba.so", NULL, 0);
    fake_elf(a, "liba.so", NULL, 0);
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error)
           == BS_ELF_DEP_DUPLICATE);
    assert(!strcmp(error.dependency, "liba.so"));

    fake_elf(a, "liba.so", NULL, 0);
    fake_elf(b, "libb.so", NULL, 0);
    a[0] = '?'; /* corrupt ELF magic */
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error)
           == BS_ELF_DEP_MALFORMED);
    a[0] = 0x7f;
    dynamic_tag(a, 6, 1, 0xfffffffcULL); /* bogus DT_NEEDED strtab offset */
    dynamic_tag(a, 7, 0, 0);
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error)
           == BS_ELF_DEP_MALFORMED);

    fake_elf(a, "liba.so", NULL, 0);
    fake_elf(b, "libb.so", NULL, 0);
    images[1].filename = "staged/libb.so";
    assert(bs_elf_dependency_order_images(images, 2, NULL, order, &error) == 0);
    assert(order[0] == 0 && order[1] == 1);
    assert(bs_elf_dependency_order_images(images, 0, NULL, order, &error)
           == BS_ELF_DEP_ARGUMENT);
    assert(bs_elf_dependency_order_images(images, 65, NULL, order, &error)
           == BS_ELF_DEP_TOO_MANY);

    const char *bad_policy_name[] = {"libc.so", "libc.so"};
    const BsElfDependencyPolicy bad_policy = {
        bad_policy_name, COUNT(bad_policy_name)
    };
    assert(bs_elf_dependency_order_images(images, 2, &bad_policy,
                                           order, &error)
           == BS_ELF_DEP_ARGUMENT);

    /* A three-level graph exercises stable choice among independent nodes. */
    const char *need_c[] = {"libc-private.so"};
    fake_elf(c, "libc-private.so", NULL, 0);
    fake_elf(b, "libb.so", need_c, COUNT(need_c));
    fake_elf(a, "liba.so", need_b, COUNT(need_b));
    BsElfDependencyImage triple[] = {
        {"liba.so", a, sizeof(a), 0},
        {"libb.so", b, sizeof(b), 0},
        {"libc-private.so", c, sizeof(c), 0}
    };
    size_t three[3] = {999, 999, 999};
    assert(bs_elf_dependency_order_images(triple, COUNT(triple), NULL,
                                          three, &error) == 0);
    assert(three[0] == 2 && three[1] == 1 && three[2] == 0);
    puts("PASS: deterministic synthetic graph, cycle, malformed and allowlist tests");
}

static unsigned char *read_original(const char *filename, size_t *size) {
    char path[300];
    snprintf(path, sizeof(path),
             "../import/quest-engine/APK/lib/arm64-v8a/%s", filename);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Missing original ELF: %s\n", path); return NULL; }
    assert(fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f);
    assert(length >= 64);
    assert(fseek(f, 0, SEEK_SET) == 0);
    *size = (size_t)length;
    unsigned char *bytes = malloc(*size);
    assert(bytes);
    assert(fread(bytes, 1, *size, f) == *size);
    fclose(f);
    return bytes;
}

/* Exactly the in-memory transformation performed by so_load: every p_vaddr
 * becomes load_virtbase+p_vaddr, while PT_DYNAMIC tag values remain raw.
 */
static void emulate_so_load_phdr_rebase(unsigned char *elf, uint64_t bias) {
    size_t phoff = (size_t)get64(elf + 32);
    size_t count = get16(elf + 56);
    for (size_t i = 0; i < count; ++i) {
        unsigned char *header = elf + phoff + i * 56;
        put64(header + 16, get64(header + 16) + bias);
    }
}

static void test_all_15_real_original_quest_modules(void) {
    static const char *const names[] = {
        "libmain.so", "libc++_shared.so", "libunity.so", "libil2cpp.so",
        "libgame.so", "lib_burst_generated.so", "libopenxr_loader.so",
        "libUnityOpenXR.so", "libOVRPlugin.so", "libOVRMetricsTool.so",
        "libovrplatformloader.so", "libhaptics_sdk.so", "libenet.so",
        "libcrypto.so", "libssl.so"
    };
    /* Stable Kahn output for the *actual* original 15 DT_NEEDED lists:
     *  libunity -> libmain
     *  libgame -> libc++_shared
     *  libssl -> libcrypto
     *  libenet -> libssl + libcrypto
     */
    static const size_t expected[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 12
    };
    enum { N = 15 };
    assert(COUNT(names) == N && COUNT(expected) == N);
    unsigned char *bytes[N] = {0};
    BsElfDependencyImage images[N];
    BsElfDependencyError error;
    size_t order[N] = {0};
    for (size_t i = 0; i < N; ++i) {
        size_t length = 0;
        bytes[i] = read_original(names[i], &length);
        assert(bytes[i]);
        images[i] = (BsElfDependencyImage) {names[i], bytes[i], length, 0};
    }
    assert(bs_elf_dependency_order_images(images, N, NULL, order, &error)
           == BS_ELF_DEP_MISSING);
    assert(!strcmp(error.dependency, "liblog.so"));
    BsElfDependencyStatus status = bs_elf_dependency_order_images(
        images, N, &bs_elf_dep_quest_android_policy, order, &error);
    if (status != BS_ELF_DEP_OK)
        fprintf(stderr, "Unexpected genuine dependency failure: %s "
                "module=%zu required=%s\n",
                bs_elf_dependency_status_string(status),
                error.module_index, error.dependency);
    assert(status == BS_ELF_DEP_OK);
    for (size_t i = 0; i < N; ++i) assert(order[i] == expected[i]);

    /* Independently inspect each original DT_NEEDED record and prove that
     * precisely the real inter-module edges were ordered correctly.
     */
    const size_t edges[][2] = {
        {2, 0},   /* libunity -> libmain */
        {4, 1},   /* libgame -> libc++_shared */
        {12, 13}, /* libenet -> libcrypto */
        {12, 14}, /* libenet -> libssl */
        {14, 13}  /* libssl -> libcrypto */
    };
    static const char *const android_externals[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so",
        "libandroid.so", "libstdc++.so", "libEGL.so",
        "libGLESv3.so", "libz.so", "libmediandk.so"
    };
    bool observed_edges[N][N] = {{false}};
    bool observed_external[COUNT(android_externals)] = {false};
    size_t original_dt_needed_count = 0;
    size_t external_records = 0;
    for (size_t i = 0; i < N; ++i) {
        BsElfVersionView view;
        assert(bs_elf_version_view_init(&view,
                     images[i].image, images[i].image_size) == 1);
        original_dt_needed_count += view.needed_count;
        for (size_t d = 0; d < view.needed_count; ++d) {
            const char *needed = NULL;
            assert(bs_elf_version_dependency(&view, d, &needed) == 1);
            size_t provider = N;
            for (size_t j = 0; j < N; ++j)
                if (!strcmp(needed, names[j])) { provider = j; break; }
            if (provider != N) {
                assert(!observed_edges[i][provider]);
                observed_edges[i][provider] = true;
                continue;
            }
            bool known = false;
            for (size_t e = 0; e < COUNT(android_externals); ++e) {
                if (!strcmp(needed, android_externals[e])) {
                    observed_external[e] = true;
                    known = true;
                    ++external_records;
                    break;
                }
            }
            assert(known && "unknown external DT_NEEDED in original Quest ELF");
        }
    }
    size_t edge_count = 0;
    for (size_t i = 0; i < N; ++i)
        for (size_t j = 0; j < N; ++j) {
            bool expected_edge = false;
            for (size_t e = 0; e < COUNT(edges); ++e)
                if (edges[e][0] == i && edges[e][1] == j)
                    expected_edge = true;
            assert(observed_edges[i][j] == expected_edge);
            edge_count += observed_edges[i][j];
        }
    assert(edge_count == COUNT(edges));
    for (size_t e = 0; e < COUNT(android_externals); ++e)
        assert(observed_external[e] && "expected Quest Android external absent");
    assert(original_dt_needed_count == edge_count + external_records);
    size_t location[N];
    for (size_t position = 0; position < N; ++position)
        location[order[position]] = position;
    for (size_t e = 0; e < COUNT(edges); ++e)
        assert(location[edges[e][1]] < location[edges[e][0]]);
    printf("PASS: source-backed graph has %zu direct internal edges and "
           "%zu external DT_NEEDED records in 10 explicit SONAME classes\n",
           edge_count, external_records);

    /* Removing a REQUIRED library must fail even with Android external
     * allowance; a permissive policy must not hide libcrypto.so.
     */
    BsElfDependencyImage missing_crypto[N - 1];
    size_t index = 0;
    for (size_t i = 0; i < N; ++i)
        if (strcmp(names[i], "libcrypto.so"))
            missing_crypto[index++] = images[i];
    assert(index == N - 1);
    assert(bs_elf_dependency_order_images(missing_crypto, N - 1,
        &bs_elf_dep_quest_android_policy, order, &error) == BS_ELF_DEP_MISSING);
    assert(!strcmp(error.dependency, "libcrypto.so"));

    /* Preserve actual file buffers but rebase their PHDR fields exactly as
     * so_load does. The adapter reads a real array of struct so_module.
     */
    so_module staged[N];
    memset(staged, 0, sizeof(staged));
    for (size_t i = 0; i < N; ++i) {
        const uint64_t bias = UINT64_C(0x7100000000) + i * UINT64_C(0x1000000);
        emulate_so_load_phdr_rebase(bytes[i], bias);
        snprintf(staged[i].name, sizeof staged[i].name,
                 "sdmc:/switch/beat-saber-quest/%s", names[i]);
        staged[i].so_base = bytes[i];
        staged[i].so_size = images[i].image_size;
        staged[i].load_virtbase = (void *)(uintptr_t)bias;
    }
    status = bs_elf_dependency_order_modules(staged, N,
        &bs_elf_dep_quest_android_policy, order, &error);
    if (status != BS_ELF_DEP_OK)
        fprintf(stderr, "Staged so_module dependency failure: %s "
                "module=%zu required=%s\n",
                bs_elf_dependency_status_string(status),
                error.module_index, error.dependency);
    assert(status == BS_ELF_DEP_OK);
    for (size_t i = 0; i < N; ++i) assert(order[i] == expected[i]);
    /* Interface must fail before use-after-free of temporary ELF file. */
    void *original_image = staged[0].so_base;
    staged[0].so_base = NULL;
    assert(bs_elf_dependency_order_modules(staged, N,
        &bs_elf_dep_quest_android_policy, order, &error)
           == BS_ELF_DEP_ARGUMENT);
    staged[0].so_base = original_image;

    printf("PASS: 15 ORIGINAL Quest ELF64 dependency graph and rebased "
           "so_module adapter; constructor order:");
    for (size_t i = 0; i < N; ++i)
        printf(" %s", names[expected[i]]);
    puts("");
    for (size_t i = 0; i < N; ++i) free(bytes[i]);
}

int main(void) {
    test_synthetic_graph_errors();
    test_all_15_real_original_quest_modules();
    return 0;
}
