/* Host-only read-only GameActivity preflight validation against real Quest ELF.
 * Run from beat-saber-engine/:
 * clang -std=c11 -Wall -Wextra -Werror -O2 -Itests/lifecycle_shims -Isource \
 *   -include tests/test_gameactivity_preflight_path.h \
 *   tests/test_gameactivity_preflight.c source/unity_lifecycle.c source/vulkan_bridge.c \
 *   -o /tmp/test_gameactivity_preflight && /tmp/test_gameactivity_preflight
 * The actual guest functions are NEVER invoked.
 */
#include "unity_lifecycle.h"
#include "jni_bridge.h"
#include "platform_bridge.h"

#include <assert.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Host-only CRC for the pinned original ELF. Windows clang does not link
 * the Switch port's zlib, but the same polynomial verifies the exact bytes. */
uLong crc32(uLong initial, const Bytef *data, uInt length) {
    if (!data) return initial;
    uint32_t value = (uint32_t)initial ^ UINT32_C(0xffffffff);
    for (uInt i = 0; i < length; ++i) {
        value ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^
                ((value & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return (uLong)(value ^ UINT32_C(0xffffffff));
}

static uintptr_t s_jni_slots[233];
static uintptr_t *s_jni_table = s_jni_slots;
static uintptr_t s_vm_table;
static int s_vm_ready = 1;
static int s_tls_ready = 1;
static const unsigned s_required_slots[] = {
    6, 15, 16, 21, 23, 33, 94, 113, 215
};

int debugPrintf(char *message, ...) {
    (void)message;
    return 0;
}
void *bs_jni_vm(void) { return s_vm_ready ? &s_vm_table : NULL; }
void *bs_jni_env(void) { return s_vm_ready ? &s_jni_table : NULL; }
int bs_jni_gameactivity_java_ready(void) { return 0; }
int bs_jni_unityplayer_has_current_activity(void) { return 0; }
#ifdef _WIN32
int setenv(const char *name, const char *value, int overwrite) {
    assert(overwrite == 1);
    return _putenv_s(name, value);
}
#endif
int bs_jni_gameactivity_registration_abi_ready(void) { return 0; }
int bs_jni_gameactivity_natives_captured(void) { return 0; }
int bs_gameactivity_java_driver_create(BsGameActivityJavaDriver *driver,
                                        const BsGameActivityJavaLaunch *launch) {
    (void)driver; (void)launch;
    abort(); /* Preflight must refuse before calling original NativeCode. */
}
bool bs_guest_tls_is_thread_attached(void) { return s_tls_ready != 0; }
size_t bs_jni_registration_count(void) { return 0; }
const char *bs_jni_last_error(void) { return "test: guest calls disabled"; }
int32_t bs_jni_call_onload_if_ready(const char *lib, BSJNIOnLoadFn fn,
                                    const BSJNIReadiness *ready) {
    (void)lib; (void)fn; (void)ready;
    abort();  /* Any accidental guest call fails the test immediately. */
}
void so_execute_init_array(so_module *module, bool (*should_stop)(void)) {
    (void)should_stop;
    (void)module;
    abort();  /* Never invoke original constructors in this test. */
}

DynLibFunction bs_platform_symbols[] = {
    {"__android_log_print", (uintptr_t)1}
};
const int bs_platform_symbols_count = 1;

uintptr_t so_find_module_export(so_module *module, const char *name) {
    const uintptr_t base = (uintptr_t)module->load_virtbase;
    if (!strcmp(name, "JNI_OnLoad")) return base + 0x1d4cc;
    if (!strcmp(name, "GameActivity_register")) return base + 0x1bd2c;
    return 0;
}

static void load_original_libgame(so_module *module) {
    FILE *file = fopen(BS_QUEST_ROOT "libgame.so", "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long bytes = ftell(file);
    assert(bytes == 152576L);
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *image = malloc((size_t)bytes);
    assert(image);
    assert(fread(image, 1, (size_t)bytes, file) == (size_t)bytes);
    fclose(file);

    const Elf64_Ehdr *hdr = (const Elf64_Ehdr *)image;
    assert(!memcmp(hdr->e_ident, ELFMAG, SELFMAG));
    assert(hdr->e_ident[EI_CLASS] == ELFCLASS64);
    assert(hdr->e_machine == EM_AARCH64);
    assert(hdr->e_phentsize == sizeof(Elf64_Phdr));
    assert(hdr->e_phoff + (size_t)hdr->e_phnum * sizeof(Elf64_Phdr)
           <= (size_t)bytes);
    const Elf64_Phdr *programs = (const Elf64_Phdr *)(image + hdr->e_phoff);
    size_t map_size = 0;
    for (size_t i = 0; i < hdr->e_phnum; ++i) {
        const Elf64_Phdr *p = &programs[i];
        if (p->p_type != PT_LOAD) continue;
        assert(p->p_offset + p->p_filesz <= (size_t)bytes);
        assert(p->p_memsz >= p->p_filesz);
        assert(p->p_vaddr + p->p_memsz < 1024 * 1024);
        if ((size_t)(p->p_vaddr + p->p_memsz) > map_size)
            map_size = (size_t)(p->p_vaddr + p->p_memsz);
    }
    unsigned char *mapped = calloc(1, map_size);
    assert(mapped);
    for (size_t i = 0; i < hdr->e_phnum; ++i) {
        const Elf64_Phdr *p = &programs[i];
        if (p->p_type == PT_LOAD)
            memcpy(mapped + p->p_vaddr, image + p->p_offset,
                   (size_t)p->p_filesz);
    }
    free(image);
    memset(module, 0, sizeof *module);
    strcpy(module->name, "libgame.so");
    module->load_virtbase = mapped;
    module->load_size = map_size;
    /* Represent the resolver's real local symbol JUMP_SLOT relocation. */
    const uintptr_t register_address = (uintptr_t)mapped + 0x1bd2c;
    memcpy(mapped + 0x2c5b8, &register_address, sizeof register_address);
    const uintptr_t log_address = bs_platform_symbols[0].func;
    memcpy(mapped + 0x2c680, &log_address, sizeof log_address);
}

static BsGameActivityPreflight probe(so_module *module, size_t missing) {
    BsGameActivityPreflight result;
    assert(!bs_unity_preflight_original_gameactivity(module, missing, &result));
    return result;
}

int main(void) {
    so_module game;
    load_original_libgame(&game);
    const uintptr_t fallback = (uintptr_t)0x1111;
    s_jni_slots[28] = fallback;
    s_jni_slots[29] = fallback;
    for (size_t i = 0; i < sizeof s_required_slots /
                             sizeof s_required_slots[0]; ++i)
        s_jni_slots[s_required_slots[i]] = (uintptr_t)0x2222;

    BsGameActivityPreflight r = probe(&game, 0);
    assert(r.source_verified && r.executable_verified && r.local_plt_verified &&
           r.android_log_plt_verified);
    assert(r.own_unresolved_relocations == 0);
    assert(r.missing_jni_slots == 0);
    assert(r.blockers == BS_GA_BLOCK_JAVA_ACTIVITY);

    r = probe(&game, 4);
    assert(r.blockers == (BS_GA_BLOCK_JAVA_ACTIVITY |
                          BS_GA_BLOCK_UNRESOLVED_IMPORTS));
    assert(r.own_unresolved_relocations == 4);

    s_jni_slots[16] = fallback;
    s_jni_slots[94] = fallback;
    s_jni_slots[113] = fallback;
    r = probe(&game, 0);
    assert(r.missing_jni_slots == ((1u << 2) | (1u << 6) | (1u << 7)));
    assert(r.blockers == (BS_GA_BLOCK_JAVA_ACTIVITY | BS_GA_BLOCK_JNI_SLOTS));
    for (size_t i = 0; i < sizeof s_required_slots /
                             sizeof s_required_slots[0]; ++i)
        s_jni_slots[s_required_slots[i]] = (uintptr_t)0x2222;

    s_vm_ready = 0;
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_JNI_VM);
    assert(r.missing_jni_slots == (1u << 9) - 1u);
    s_vm_ready = 1;
    s_tls_ready = 0;
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_GUEST_TLS);
    s_tls_ready = 1;

    unsigned char *mapped = game.load_virtbase;
    mapped[0x1d4cc] ^= 1;
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_CODE);
    assert(!r.executable_verified);
    mapped[0x1d4cc] ^= 1;

    mapped[0x1bd2c] ^= 1;
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_CODE);
    mapped[0x1bd2c] ^= 1;

    uintptr_t bad = 0;
    memcpy(mapped + 0x2c5b8, &bad, sizeof bad);
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_LOCAL_PLT);
    assert(!r.local_plt_verified);
    uintptr_t good = (uintptr_t)mapped + 0x1bd2c;
    memcpy(mapped + 0x2c5b8, &good, sizeof good);

    memcpy(mapped + 0x2c680, &bad, sizeof bad);
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_ANDROID_LOG_PLT);
    assert(!r.android_log_plt_verified);
    good = bs_platform_symbols[0].func;
    memcpy(mapped + 0x2c680, &good, sizeof good);

    game.so_base = (void *)1;
    r = probe(&game, 0);
    assert(r.blockers & BS_GA_BLOCK_MODULE);
    game.so_base = NULL;
    r = probe(NULL, 0);
    assert(r.blockers & BS_GA_BLOCK_MODULE);

    r = probe(&game, 0);
    assert(r.blockers == BS_GA_BLOCK_JAVA_ACTIVITY);
    free(game.load_virtbase);
    puts("PASS: original libgame source, mapped code, local PLT, JNI slot and refusal tests");
    return 0;
}
