#include "unity_lifecycle.h"
#include "vulkan_bridge.h"
#include "jni_bridge.h"
#include "guest_tls.h"
#include "elf_dependency_order.h"
#include "platform_bridge.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#ifndef BS_QUEST_ROOT
#define BS_QUEST_ROOT "sdmc:/switch/beat-saber-1408/"
#endif
#define BS_LIBMAIN_SIZE 6696u
#define BS_LIBMAIN_CRC32 UINT32_C(0x20c44ede)

static bool s_native_loader_done;
static bool s_il2cpp_onload_done;
static bool s_gameactivity_registration_done;

static bool bs_native_loader_matches_source(void) {
    FILE *file = fopen(BS_QUEST_ROOT "libmain.so", "rb");
    if (!file) {
        debugPrintf("[unity] libmain JNI_OnLoad blocked: private source unreadable\n");
        return false;
    }
    unsigned char buffer[4096];
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t total = 0;
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, file)) != 0) {
        total += n;
        if (total > BS_LIBMAIN_SIZE) break;
        crc = crc32(crc, buffer, (uInt)n);
    }
    int failed_read = ferror(file);
    fclose(file);
    if (failed_read || total != BS_LIBMAIN_SIZE ||
        (uint32_t)crc != BS_LIBMAIN_CRC32) {
        debugPrintf("[unity] libmain JNI_OnLoad blocked: Quest 1.46 pinned "
                    "ELF size/CRC mismatch (size=%lu crc=%08lx)\n",
                    (unsigned long)total, (unsigned long)crc);
        return false;
    }
    return true;
}

bool bs_unity_start_verified_native_loader(so_module *libmain,
                                            size_t own_unresolved) {
    if (s_native_loader_done) return true;
    if (!libmain || own_unresolved || !bs_jni_vm() || !bs_jni_env() ||
        !bs_guest_tls_is_thread_attached() ||
        !bs_native_loader_matches_source()) {
        debugPrintf("[unity] native-loader JNI_OnLoad blocked: own_missing=%lu "
                    "JNI/TLS/source readiness incomplete\n",
                    (unsigned long)own_unresolved);
        return false;
    }
    /* This pin is intentionally narrow. The original 140-byte JNI_OnLoad at
     * 0x4a80 contains three indirect JNI calls: JavaVM AttachCurrentThread (slot 4),
     * JNIEnv FindClass (slot 6) and RegisterNatives (slot 215). The functions
     * registered for com/unity3d/player/NativeLoader are never called here.
     */
    const uintptr_t entry = so_find_module_export(libmain, "JNI_OnLoad");
    const uintptr_t begin = (uintptr_t)libmain->load_virtbase;
    if (!entry || entry < begin || entry - begin != 0x4a80 ||
        entry - begin >= libmain->load_size) {
        debugPrintf("[unity] native-loader JNI_OnLoad blocked: unrecognized "
                    "original entrypoint %p\n", (void *)entry);
        return false;
    }
    const size_t before = bs_jni_registration_count();
    debugPrintf("[unity] entering verified ORIGINAL libmain.so JNI_OnLoad "
                "(registration-only)\n");
    const int32_t version =
        ((BSJNIOnLoadFn)entry)(bs_jni_vm(), NULL);
    const size_t after = bs_jni_registration_count();
    if (version != BS_JNI_VERSION_1_6 || after <= before) {
        debugPrintf("[unity] original libmain.so JNI_OnLoad FAILED "
                    "result=0x%08x registered=%lu (%s)\n",
                    (unsigned)version, (unsigned long)(after - before),
                    bs_jni_last_error());
        return false;
    }
    s_native_loader_done = true;
    debugPrintf("[unity] ORIGINAL libmain.so JNI_OnLoad returned 0x%08x; "
                "captured %lu original registered native methods\n",
                (unsigned)version, (unsigned long)(after - before));
    return true;
}

/* The supplied Quest 1.46 libil2cpp.so JNI_OnLoad is exactly 72 bytes long:
 * one BL to imported __android_log_print, two stores of JavaVM and the
 * callback function address, then a constant JNI_VERSION_1_6 return.
 * The full guest file and the actual callable code bytes must both match.
 * A mismatch stops the call even if the public symbol name still exists.
 */
#define BS_IL2CPP_SIZE ((size_t)130481456)
#define BS_IL2CPP_CRC32 UINT32_C(0x44987195)
#define BS_IL2CPP_JNI_VADDR ((uintptr_t)0x34a3598)
#define BS_IL2CPP_LOG_JUMP_SLOT ((uintptr_t)0x7821548)

static bool bs_il2cpp_source_matches(void) {
    FILE *file = fopen(BS_QUEST_ROOT "libil2cpp.so", "rb");
    if (!file) {
        debugPrintf("[il2cpp] JNI_OnLoad blocked: original source unreadable\n");
        return false;
    }
    unsigned char buffer[65536];
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t bytes = 0, count;
    while ((count = fread(buffer, 1, sizeof buffer, file)) > 0) {
        if (count > BS_IL2CPP_SIZE - bytes) {
            bytes = BS_IL2CPP_SIZE + 1;
            break;
        }
        bytes += count;
        crc = crc32(crc, buffer, (uInt)count);
    }
    const int read_error = ferror(file);
    fclose(file);
    if (read_error || bytes != BS_IL2CPP_SIZE ||
        (uint32_t)crc != BS_IL2CPP_CRC32) {
        debugPrintf("[il2cpp] JNI_OnLoad blocked: source size/CRC differs "
                    "(%lu, %08lx)\n", (unsigned long)bytes,
                    (unsigned long)crc);
        return false;
    }
    return true;
}

bool bs_unity_start_verified_il2cpp_onload(so_module *libil2cpp) {
    if (s_il2cpp_onload_done) return true;
    if (!libil2cpp || !bs_jni_vm() || !bs_jni_env() ||
        !bs_guest_tls_is_thread_attached()) {
        debugPrintf("[il2cpp] JNI_OnLoad blocked: JNI/TLS unavailable\n");
        return false;
    }
    const uintptr_t runtime = (uintptr_t)libil2cpp->load_virtbase;
    const uintptr_t entry = so_find_module_export(libil2cpp, "JNI_OnLoad");
    static const uint32_t expected_code[] = {
        0xa9bf4ffeu, 0xaa0003f3u, 0x90fef201u, 0x910e9021u,
        0xb0fef162u, 0x91247042u, 0x52800080u, 0x94fb7e97u,
        0xf0023fc8u, 0xf0023fc9u, 0x528000c0u, 0xf907f113u,
        0xd503201fu, 0x100000a8u, 0x72a00020u, 0xf907e128u,
        0xa8c14ffeu, 0xd65f03c0u
    };
    if (!runtime || entry != runtime + BS_IL2CPP_JNI_VADDR ||
        BS_IL2CPP_LOG_JUMP_SLOT + sizeof(uintptr_t) > libil2cpp->load_size ||
        BS_IL2CPP_JNI_VADDR + sizeof expected_code > libil2cpp->load_size ||
        memcmp((const void *)entry, expected_code, sizeof expected_code)) {
        debugPrintf("[il2cpp] JNI_OnLoad blocked: original entrypoint or "
                    "72-byte AArch64 control flow differs\n");
        return false;
    }
    /* Validate the real relocated PLT target for the ONLY imported function
     * called by this exact 72-byte entrypoint. This works even while other
     * unrelated IL2CPP imports remain unresolved and uncallable.
     */
    uintptr_t logger = 0;
    for (int i = 0; i < bs_platform_symbols_count; ++i) {
        if (!strcmp(bs_platform_symbols[i].symbol, "__android_log_print")) {
            logger = bs_platform_symbols[i].func;
            break;
        }
    }
    const uintptr_t actual_logger = *(const uintptr_t *)
        (runtime + BS_IL2CPP_LOG_JUMP_SLOT);
    if (!logger || actual_logger != logger) {
        debugPrintf("[il2cpp] JNI_OnLoad blocked: imported Android logger "
                    "GOT not ABI-linked (got=%p required=%p)\n",
                    (void *)actual_logger, (void *)logger);
        return false;
    }
    if (!bs_il2cpp_source_matches()) return false;
    debugPrintf("[il2cpp] entering ORIGINAL Quest 1.46 libil2cpp.so "
                "JNI_OnLoad (verified 72-byte VM registration)\n");
    const int32_t version = ((BSJNIOnLoadFn)entry)(bs_jni_vm(), NULL);
    if (version != BS_JNI_VERSION_1_6) {
        debugPrintf("[il2cpp] original JNI_OnLoad returned invalid version "
                    "0x%08x\n", (unsigned)version);
        return false;
    }
    s_il2cpp_onload_done = true;
    debugPrintf("[il2cpp] ORIGINAL JNI_OnLoad returned JNI 1.6; "
                "VM registered, IL2CPP gameplay initialization pending\n");
    return true;
}

/* This is the original Quest 1.46.0 GameActivity implementation inside
 * libgame.so, not Android's generic NativeActivity or a substitute event loop.
 * The exact source ELF also pins the direct call graph: JNI_OnLoad vaddr
 * 0x1d4cc -> JavaVM GetEnv -> GameActivity_register@plt vaddr 0x27210,
 * whose R_AARCH64_JUMP_SLOT at 0x2c5b8 resolves to vaddr 0x1bd2c.
 * GameActivity_register is 4296 bytes long and uses many Java method/field
 * lookups; there is no safe abbreviated invocation of its first instructions.
 */
#define BS_GAME_SIZE ((size_t)152576)
#define BS_GAME_CRC32 UINT32_C(0x41828fc1)
#define BS_GAME_ONLOAD_VADDR ((uintptr_t)0x1d4cc)
#define BS_GAME_ONLOAD_SIZE ((size_t)120)
#define BS_GAME_ONLOAD_CRC32 UINT32_C(0xc013f770)
#define BS_GAME_REGISTER_VADDR ((uintptr_t)0x1bd2c)
#define BS_GAME_REGISTER_SIZE ((size_t)4296)
#define BS_GAME_REGISTER_CRC32 UINT32_C(0x3e404d70)
#define BS_GAME_REGISTER_JUMP_SLOT ((uintptr_t)0x2c5b8)
#define BS_GAME_ANDROID_LOG_JUMP_SLOT ((uintptr_t)0x2c680)

static bool bs_gameactivity_source_matches(void) {
    FILE *file = fopen(BS_QUEST_ROOT "libgame.so", "rb");
    if (!file) {
        debugPrintf("[gameactivity] preflight: original libgame.so unreadable\n");
        return false;
    }
    unsigned char buffer[4096];
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t total = 0;
    size_t n;
    bool size_overrun = false;
    while ((n = fread(buffer, 1, sizeof buffer, file)) != 0) {
        if (n > BS_GAME_SIZE - total) {
            size_overrun = true;
            break;
        }
        total += n;
        crc = crc32(crc, buffer, (uInt)n);
    }
    const int failed_read = ferror(file);
    fclose(file);
    if (failed_read || size_overrun || total != BS_GAME_SIZE ||
        (uint32_t)crc != BS_GAME_CRC32) {
        debugPrintf("[gameactivity] preflight: libgame.so source mismatch "
                    "(size=%lu crc=%08lx overflow=%d)\n",
                    (unsigned long)total, (unsigned long)crc,
                    (int)size_overrun);
        return false;
    }
    return true;
}

static bool bs_gameactivity_span_valid(const so_module *module,
                                        uintptr_t offset, size_t length) {
    return module->load_virtbase && offset <= module->load_size &&
           length <= module->load_size - offset &&
           (uintptr_t)module->load_virtbase <= UINTPTR_MAX - offset &&
           length <= UINTPTR_MAX -
                     ((uintptr_t)module->load_virtbase + offset);
}

/* Index into the lower nine bits of BsGameActivityPreflight.missing_jni_slots.
 * Derived from the original 4296-byte GameActivity_register disassembly.
 * A table entry being populated does not mean that it implements the correct
 * Java class semantics (hence the separate hard Java-Activity blocker).
 */
static const unsigned bs_gameactivity_jni_slots[] = {
    6, 15, 16, 21, 23, 33, 94, 113, 215
};
static const char *const bs_gameactivity_jni_names[] = {
    "FindClass", "ExceptionOccurred", "ExceptionDescribe", "NewGlobalRef",
    "DeleteLocalRef", "GetMethodID", "GetFieldID", "GetStaticMethodID",
    "RegisterNatives"
};

static uint16_t bs_gameactivity_missing_jni_slots(void) {
    const void *env = bs_jni_env();
    const uint16_t all_missing =
        (uint16_t)((1u << (sizeof bs_gameactivity_jni_slots /
                          sizeof bs_gameactivity_jni_slots[0])) - 1u);
    if (!env) return all_missing;

    /* JNIEnv is a pointer TO a function-table pointer. Every unsupported
     * slot holds its own distinct fail-closed diagnostic trap, so ask the
     * bridge directly (no JNI operation is invoked). The table pointer must
     * still be live, otherwise the probe fails closed. */
    uintptr_t table = 0;
    memcpy(&table, env, sizeof table);
    if (!table) return all_missing;

    uint16_t missing = 0;
    for (size_t i = 0; i < sizeof bs_gameactivity_jni_slots /
                             sizeof bs_gameactivity_jni_slots[0]; ++i) {
        if (!bs_jni_env_slot_implemented(bs_gameactivity_jni_slots[i]))
            missing |= (uint16_t)(1u << i);
    }
    return missing;
}

bool bs_unity_preflight_original_gameactivity(const so_module *libgame,
                                              size_t own_unresolved,
                                              BsGameActivityPreflight *result) {
    if (!result) return false;
    memset(result, 0, sizeof *result);
    result->own_unresolved_relocations = own_unresolved;
    /* JNI metadata-only support is insufficient. An actual Activity-backed
     * Java host must independently confirm every required live method before
     * permitting the original guest registration entrypoint to execute. */
    result->java_host_verified = bs_jni_gameactivity_java_ready() != 0;
    if (!result->java_host_verified)
        result->blockers = BS_GA_BLOCK_JAVA_ACTIVITY;
    if (own_unresolved) result->blockers |= BS_GA_BLOCK_UNRESOLVED_IMPORTS;
    if (!bs_guest_tls_is_thread_attached())
        result->blockers |= BS_GA_BLOCK_GUEST_TLS;
    if (!bs_jni_vm() || !bs_jni_env())
        result->blockers |= BS_GA_BLOCK_JNI_VM;
    result->missing_jni_slots = bs_gameactivity_missing_jni_slots();
    if (result->missing_jni_slots)
        result->blockers |= BS_GA_BLOCK_JNI_SLOTS;

    result->source_verified = bs_gameactivity_source_matches();
    if (!result->source_verified)
        result->blockers |= BS_GA_BLOCK_SOURCE;

    if (!libgame || !libgame->load_virtbase || libgame->so_base ||
        !memchr(libgame->name, '\0', sizeof libgame->name) ||
        strcmp(strrchr(libgame->name, '/') ?
               strrchr(libgame->name, '/') + 1 : libgame->name, "libgame.so") ||
        !bs_gameactivity_span_valid(libgame, BS_GAME_ONLOAD_VADDR,
                                     BS_GAME_ONLOAD_SIZE) ||
        !bs_gameactivity_span_valid(libgame, BS_GAME_REGISTER_VADDR,
                                     BS_GAME_REGISTER_SIZE) ||
        !bs_gameactivity_span_valid(libgame, BS_GAME_REGISTER_JUMP_SLOT,
                                     sizeof(uintptr_t)) ||
        !bs_gameactivity_span_valid(libgame, BS_GAME_ANDROID_LOG_JUMP_SLOT,
                                     sizeof(uintptr_t))) {
        result->blockers |= BS_GA_BLOCK_MODULE;
    } else {
        const uintptr_t runtime = (uintptr_t)libgame->load_virtbase;
        const uintptr_t jni = so_find_module_export((so_module *)libgame,
                                                    "JNI_OnLoad");
        const uintptr_t registration = so_find_module_export(
            (so_module *)libgame, "GameActivity_register");
        if (jni == runtime + BS_GAME_ONLOAD_VADDR &&
            registration == runtime + BS_GAME_REGISTER_VADDR &&
            (uint32_t)crc32(0L, (const Bytef *)jni,
                            (uInt)BS_GAME_ONLOAD_SIZE) == BS_GAME_ONLOAD_CRC32 &&
            (uint32_t)crc32(0L, (const Bytef *)registration,
                            (uInt)BS_GAME_REGISTER_SIZE) == BS_GAME_REGISTER_CRC32)
            result->executable_verified = true;
        else
            result->blockers |= BS_GA_BLOCK_CODE;

        uintptr_t relocated = 0;
        memcpy(&relocated, (const void *)(runtime +
               BS_GAME_REGISTER_JUMP_SLOT), sizeof relocated);
        if (relocated == runtime + BS_GAME_REGISTER_VADDR)
            result->local_plt_verified = true;
        else
            result->blockers |= BS_GA_BLOCK_LOCAL_PLT;

        /* GameActivity_register's real log calls pass through this imported
         * JUMP_SLOT. A resolved relocation count alone cannot establish that
         * its target points at the expected ABI-correct native bridge.
         */
        uintptr_t expected_log = 0, actual_log = 0;
        for (int i = 0; i < bs_platform_symbols_count; ++i) {
            if (!strcmp(bs_platform_symbols[i].symbol, "__android_log_print")) {
                expected_log = bs_platform_symbols[i].func;
                break;
            }
        }
        memcpy(&actual_log, (const void *)(runtime +
               BS_GAME_ANDROID_LOG_JUMP_SLOT), sizeof actual_log);
        if (expected_log && actual_log == expected_log)
            result->android_log_plt_verified = true;
        else
            result->blockers |= BS_GA_BLOCK_ANDROID_LOG_PLT;
    }

    debugPrintf("[gameactivity] original JNI_OnLoad READ-ONLY preflight: "
                "source=%d mapped_code=%d local_PLT=%d log_PLT=%d own_missing=%lu "
                "java_host=%d blocked=0x%08x\n",
                (int)result->source_verified,
                (int)result->executable_verified,
                (int)result->local_plt_verified,
                (int)result->android_log_plt_verified,
                (unsigned long)own_unresolved,
                (int)result->java_host_verified,
                (unsigned)result->blockers);
    for (size_t i = 0; i < sizeof bs_gameactivity_jni_slots /
                             sizeof bs_gameactivity_jni_slots[0]; ++i) {
        if (result->missing_jni_slots & (uint16_t)(1u << i))
            debugPrintf("[gameactivity] JNI slot %u (%s) unsupported\n",
                        bs_gameactivity_jni_slots[i],
                        bs_gameactivity_jni_names[i]);
    }
    static const struct {
        uint32_t flag;
        const char *reason;
    } failures[] = {
        {BS_GA_BLOCK_MODULE, "libgame module missing/not finalized or invalid bounds"},
        {BS_GA_BLOCK_SOURCE, "original libgame.so file identity differs"},
        {BS_GA_BLOCK_CODE, "original JNI_OnLoad/GameActivity_register code differs"},
        {BS_GA_BLOCK_LOCAL_PLT, "GameActivity_register local jump slot invalid"},
        {BS_GA_BLOCK_UNRESOLVED_IMPORTS, "libgame has unresolved relocations"},
        {BS_GA_BLOCK_JNI_VM, "attached JNIEnv/JavaVM unavailable"},
        {BS_GA_BLOCK_GUEST_TLS, "guest thread TLS unavailable"},
        {BS_GA_BLOCK_JNI_SLOTS, "GameActivity registration JNI ABI incomplete"},
        {BS_GA_BLOCK_JAVA_ACTIVITY,
         "real Java GameActivity class/method/field metadata and lifecycle missing"},
        {BS_GA_BLOCK_ANDROID_LOG_PLT, "Android logging jump slot not bound to bridge"}
    };
    for (size_t i = 0; i < sizeof failures / sizeof failures[0]; ++i)
        if (result->blockers & failures[i].flag)
            debugPrintf("[gameactivity] blocked: %s\n", failures[i].reason);
    return result->blockers == 0;
}

bool bs_unity_start_verified_gameactivity_registration(so_module *libgame,
                                                       size_t own_unresolved) {
    if (s_gameactivity_registration_done) return true;
    /* The complete original JNI_OnLoad / GameActivity_register entrypoint
     * (including its internal EventsInit) was independently disassembled:
     * only JavaVM GetEnv and exact registration/lookups are performed here.
     * The 24 JNI native methods are installed as addresses but never called.
     * Running this entrypoint does NOT construct Java GameActivity or begin
     * Android's native_app_glue worker thread. */
    if (!libgame || own_unresolved || !bs_guest_tls_is_thread_attached() ||
        !bs_jni_vm() || !bs_jni_env() ||
        !bs_jni_gameactivity_registration_abi_ready()) {
        debugPrintf("[gameactivity] original registration-only JNI_OnLoad "
                    "blocked: missing=%lu or JNI metadata/thread not ready\n",
                    (unsigned long)own_unresolved);
        return false;
    }
    BsGameActivityPreflight p = {0};
    (void)bs_unity_preflight_original_gameactivity(libgame, own_unresolved, &p);
    /* The ONLY preflight failure permitted for registration-only invocation
     * is the unimplemented Java Activity runtime, which is not entered by
     * the pinned registration code. All ELF/JNI/TLS failures are fatal. */
    if (p.blockers & ~((uint32_t)BS_GA_BLOCK_JAVA_ACTIVITY)) {
        debugPrintf("[gameactivity] original JNI_OnLoad registration refused: "
                    "blockers=0x%08x\n", (unsigned)p.blockers);
        return false;
    }
    BsGameActivityTopology topology = {0};
    if (!bs_unity_preflight_gameactivity_topology(libgame, &topology)) {
        debugPrintf("[gameactivity] original JNI_OnLoad refused: native "
                    "entrypoint topology differs\n");
        return false;
    }
    const uintptr_t entry = so_find_module_export(libgame, "JNI_OnLoad");
    if (entry != (uintptr_t)libgame->load_virtbase + BS_GAME_ONLOAD_VADDR)
        return false;
    const size_t before = bs_jni_registration_count();
    debugPrintf("[gameactivity] entering original Quest 1.46 libgame "
                "registration-only JNI_OnLoad (no Java Activity)\n");
    const int32_t version = ((BSJNIOnLoadFn)entry)(bs_jni_vm(), NULL);
    const size_t after = bs_jni_registration_count();
    if (version != BS_JNI_VERSION_1_6 || after < before ||
        after - before != 24 || !bs_jni_gameactivity_natives_captured()) {
        debugPrintf("[gameactivity] registration failed version=0x%08x "
                    "captured=%lu metadata=%d (%s)\n",
                    (unsigned)version, (unsigned long)(after - before),
                    bs_jni_gameactivity_natives_captured(), bs_jni_last_error());
        return false;
    }
    s_gameactivity_registration_done = true;
    debugPrintf("[gameactivity] original JNI_OnLoad registered 24 native "
                "methods, Java lifecycle remains separate\n");
    return true;
}

/* Source-backed topology from the exact Quest 1.46 (152576 byte) libgame.so.
 * Each span is checked in both the genuine ELF file by the isolated audit
 * and the relocated executable image here. These CRCs are for code bytes,
 * not the mutable GOT or the Java object's state.
 *
 * Java_GameActivity_initializeNativeCode at 0x1cdf4 is a 116-byte wrapper.
 * Its final branch enters the native-app-glue initializer at 0x1ce68, which
 * allocates 632 bytes for NativeCode, acquires an ALooper and creates a pipe.
 * JNI_OnLoad calls the 4296-byte GameActivity_register body. A successful
 * checksum does NOT make either function safe to invoke without real Java.
 */
typedef struct BsGameActivityPin {
    const char *symbol;
    uintptr_t original_vaddr;
    size_t original_size;
    uint32_t original_crc32;
} BsGameActivityPin;
static const BsGameActivityPin s_gameactivity_entrypoints[] = {
    {"JNI_OnLoad", 0x1d4ccu, 120u, UINT32_C(0xc013f770)},
    {"GameActivity_register", 0x1bd2cu, 4296u, UINT32_C(0x3e404d70)},
    {"Java_com_google_androidgamesdk_GameActivity_initializeNativeCode",
     0x1cdf4u, 116u, UINT32_C(0x2dacc7ec)},
    {"android_main", 0x196d0u, 408u, UINT32_C(0x0a884bbb)},
    {"GameActivity_onCreate", 0x20708u, 620u, UINT32_C(0x3d137d92)},
    {"GameActivityEventsInit", 0x1fc88u, 1624u, UINT32_C(0xbdccbd0b)},
    {"android_app_read_cmd", 0x202e0u, 184u, UINT32_C(0xadca85b7)},
    {"android_app_pre_exec_cmd", 0x20398u, 236u, UINT32_C(0x172bc294)},
    {"android_app_post_exec_cmd", 0x20484u, 156u, UINT32_C(0x0b801613)},
    {"android_app_write_cmd", 0x20524u, 144u, UINT32_C(0xed135930)},
    {"android_app_swap_input_buffers", 0x205f0u, 136u,
     UINT32_C(0xd39e8a5e)}
};
_Static_assert(sizeof s_gameactivity_entrypoints /
                   sizeof s_gameactivity_entrypoints[0] <= 32,
               "preflight entrypoint mismatch bitmap capacity");

size_t bs_gameactivity_entrypoint_count(void) {
    return sizeof s_gameactivity_entrypoints /
           sizeof s_gameactivity_entrypoints[0];
}

const char *bs_gameactivity_entrypoint_name(size_t index) {
    return index < bs_gameactivity_entrypoint_count()
        ? s_gameactivity_entrypoints[index].symbol : NULL;
}

bool bs_unity_preflight_gameactivity_topology(const so_module *libgame,
                                              BsGameActivityTopology *result) {
    if (!result) return false;
    memset(result, 0, sizeof *result);
    result->entrypoints_checked = bs_gameactivity_entrypoint_count();
    result->source_verified = bs_gameactivity_source_matches();
    if (!result->source_verified) result->blockers |= BS_GA_TOPO_SOURCE;

    /* load_virtbase is reserved during so_load(), BEFORE it is mapped.
     * Accessing that reservation at this stage faults. so_finalize() rebases
     * dynsym/dynstr into the live executable mapping after it succeeds; use
     * that observable state to reject an unfinalized source reservation. */
    if (!libgame || !libgame->load_virtbase || !libgame->load_base ||
        libgame->so_base || libgame->num_syms <= 0 ||
        !libgame->syms || !libgame->dynstrtab ||
        libgame->load_base == libgame->load_virtbase ||
        libgame->load_size < sizeof(Elf64_Sym) ||
        (uintptr_t)libgame->load_virtbase >
            UINTPTR_MAX - libgame->load_size ||
        (uintptr_t)libgame->syms < (uintptr_t)libgame->load_virtbase ||
        (uintptr_t)libgame->syms - (uintptr_t)libgame->load_virtbase >
            libgame->load_size - sizeof(Elf64_Sym) ||
        (uintptr_t)libgame->dynstrtab < (uintptr_t)libgame->load_virtbase ||
        (uintptr_t)libgame->dynstrtab - (uintptr_t)libgame->load_virtbase >=
            libgame->load_size ||
        !memchr(libgame->name, '\0', sizeof libgame->name) ||
        strcmp(strrchr(libgame->name, '/') ?
               strrchr(libgame->name, '/') + 1 : libgame->name, "libgame.so")) {
        result->blockers |= BS_GA_TOPO_MODULE;
        return false;
    }

    for (size_t i = 0; i < bs_gameactivity_entrypoint_count(); ++i) {
        const BsGameActivityPin *pin = &s_gameactivity_entrypoints[i];
        const uint32_t bad = UINT32_C(1) << i;
        if (!bs_gameactivity_span_valid(libgame, pin->original_vaddr,
                                         pin->original_size)) {
            result->blockers |= BS_GA_TOPO_MODULE;
            result->entrypoint_mismatch |= bad;
            debugPrintf("[gameactivity:topology] original %s exceeds LOAD span\n",
                        pin->symbol);
            continue;
        }
        const uintptr_t mapped =
            (uintptr_t)libgame->load_virtbase + pin->original_vaddr;
        const uintptr_t export = so_find_module_export((so_module *)libgame,
                                                       pin->symbol);
        if (export != mapped) {
            result->blockers |= BS_GA_TOPO_SYMBOL;
            result->entrypoint_mismatch |= bad;
            debugPrintf("[gameactivity:topology] original %s export expected=%p "
                        "actual=%p\n", pin->symbol, (void *)mapped,
                        (void *)export);
            continue;
        }
        const uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)mapped,
                                              (uInt)pin->original_size);
        if (crc != pin->original_crc32) {
            result->blockers |= BS_GA_TOPO_BYTES;
            result->entrypoint_mismatch |= bad;
            debugPrintf("[gameactivity:topology] original %s bytes mismatch "
                        "crc=%08lx expected=%08lx\n", pin->symbol,
                        (unsigned long)crc,
                        (unsigned long)pin->original_crc32);
            continue;
        }
        ++result->entrypoints_verified;
    }
    result->mapped_image_verified =
        result->entrypoints_verified == result->entrypoints_checked;
    debugPrintf("[gameactivity:topology] verified %lu/%lu ORIGINAL exported "
                "native entrypoints, source=%d mismatches=0x%08lx "
                "blockers=0x%08lx; Java execution remains gated\n",
                (unsigned long)result->entrypoints_verified,
                (unsigned long)result->entrypoints_checked,
                (int)result->source_verified,
                (unsigned long)result->entrypoint_mismatch,
                (unsigned long)result->blockers);
    return result->blockers == 0;
}

void bs_gameactivity_trace_reset(BsGameActivityTrace *trace) {
    if (trace) memset(trace, 0, sizeof *trace);
}

bool bs_gameactivity_trace_command(BsGameActivityTrace *trace, int8_t command) {
    if (!trace) return false;
    bool okay = false;
    if (trace->stage == BS_GA_TRACE_DESTROYED) goto done;

    switch (command) {
        case BS_GA_APP_CMD_INIT_WINDOW:
            if (!trace->window_attached) {
                trace->window_attached = true;
                ++trace->window_events;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_TERM_WINDOW:
            if (trace->window_attached) {
                trace->window_attached = false;
                trace->input_focused = false;
                ++trace->window_events;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_WINDOW_RESIZED:
        case BS_GA_APP_CMD_WINDOW_REDRAW_NEEDED:
        case BS_GA_APP_CMD_CONTENT_RECT_CHANGED:
        case BS_GA_APP_CMD_WINDOW_INSETS_CHANGED:
            if (trace->window_attached) {
                ++trace->window_events;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_CONFIG_CHANGED:
            ++trace->configuration_events;
            okay = true;
            break;
        case BS_GA_APP_CMD_LOW_MEMORY:
            ++trace->low_memory_events;
            okay = true;
            break;
        case BS_GA_APP_CMD_SAVE_STATE:
            if (trace->stage != BS_GA_TRACE_CREATED &&
                trace->stage != BS_GA_TRACE_STOPPED) {
                ++trace->save_state_requests;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_SOFTWARE_KB_VIS_CHANGED:
            /* A trace has no original Java keyboard-visibility payload. */
            okay = true;
            break;
        case BS_GA_APP_CMD_TEXT_INPUT_STATE_CHANGED:
        case BS_GA_APP_CMD_KEY_EVENTS:
        case BS_GA_APP_CMD_MOTION_EVENTS:
            if (trace->input_focused &&
                trace->stage == BS_GA_TRACE_RESUMED) {
                ++trace->input_events;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_GAINED_FOCUS:
            if (trace->window_attached &&
                trace->stage == BS_GA_TRACE_RESUMED &&
                !trace->input_focused) {
                trace->input_focused = true;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_LOST_FOCUS:
            if (trace->input_focused) {
                trace->input_focused = false;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_START:
            if (trace->stage == BS_GA_TRACE_CREATED ||
                trace->stage == BS_GA_TRACE_STOPPED) {
                trace->stage = BS_GA_TRACE_STARTED;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_RESUME:
            if (trace->stage == BS_GA_TRACE_STARTED ||
                trace->stage == BS_GA_TRACE_PAUSED) {
                trace->stage = BS_GA_TRACE_RESUMED;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_PAUSE:
            if (trace->stage == BS_GA_TRACE_RESUMED) {
                trace->stage = BS_GA_TRACE_PAUSED;
                trace->input_focused = false;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_STOP:
            if (trace->stage == BS_GA_TRACE_PAUSED ||
                trace->stage == BS_GA_TRACE_STARTED) {
                trace->stage = BS_GA_TRACE_STOPPED;
                trace->input_focused = false;
                okay = true;
            }
            break;
        case BS_GA_APP_CMD_DESTROY:
            trace->stage = BS_GA_TRACE_DESTROYED;
            trace->window_attached = false;
            trace->input_focused = false;
            okay = true;
            break;
        default:
            /* Unknown commands, including unrelated input/window messages,
             * are NOT accepted as evidence of lifecycle progress. */
            break;
    }
done:
    if (okay) ++trace->accepted_commands;
    else ++trace->rejected_commands;
    return okay;
}

bool bs_gameactivity_trace_running(const BsGameActivityTrace *trace) {
    return trace && trace->stage == BS_GA_TRACE_RESUMED &&
           trace->window_attached && trace->input_focused;
}

static so_module *find_module(so_module *modules, size_t count,
                              const char *basename) {
    for (size_t i = 0; i < count; ++i) {
        const char *path = modules[i].name;
        if (!memchr(path, '\0', sizeof modules[i].name)) continue;
        const char *sep = strrchr(path, '/');
        if (strcmp(sep ? sep + 1 : path, basename) == 0)
            return &modules[i];
    }
    return NULL;
}

bool bs_unity_start_original_game(so_module *modules, size_t module_count,
                                  const BsUnityStartCapabilities *caps) {
    if (!modules || !caps || !caps->progress || module_count != 15) {
        debugPrintf("[unity] full bootstrap blocked: incomplete Quest module set\n");
        return false;
    }
    BsUnityStartProgress *progress = caps->progress;
    if (progress->modules && progress->modules != modules) return false;
    if (progress->terminal_failure || bs_vulkan_graphics_boundary_seen())
        return false;
    if (progress->native_code_started) return true;
    progress->modules = modules;
    progress->waiting_for_activity = false;
#define BS_REQUIRE(test, reason) \
    do { if (!(test)) { \
        debugPrintf("[unity] full bootstrap blocked: %s\n", reason); \
        return false; \
    } } while (0)
    BS_REQUIRE(caps->unresolved_relocations == 0,
               "unresolved Android/cross-module imports or TLSDESC");
    BS_REQUIRE(bs_guest_tls_is_thread_attached(), "guest AArch64 thread TLS");
    BS_REQUIRE(bs_jni_vm() && bs_jni_env(), "JNI JavaVM/JNIEnv tables");
    BS_REQUIRE(caps->bionic_abi_verified, "Bionic startup ABI/thread probes");
    BS_REQUIRE(caps->game_activity_ready, "real GameActivity lifecycle");
    BS_REQUIRE(caps->assets_ready, "original AssetManager/assets");
    BS_REQUIRE(caps->media_ndk_ready, "Android MediaNDK decoder interfaces");
    BS_REQUIRE(caps->vulkan_preflight_ready, "graphics driver preflight (GLES context or Vulkan entrypoints)");
    /* The original XR plugin performs its own loader/runtime preinit. A
     * Vulkan-backed session and tracked input are later milestones: the
     * guarded launch ends before their Vulkan instance can be created. */
    BS_REQUIRE(caps->java_driver && caps->java_launch,
               "Java GameActivity/NativeCode lifecycle host");
    BS_REQUIRE(caps->java_launch->activity &&
               caps->java_launch->asset_manager &&
               caps->java_launch->configuration &&
               caps->java_launch->first_path &&
               caps->java_launch->second_path &&
               caps->java_launch->third_path,
               "actual Java Activity, assets, Configuration and paths");

    /* Validate every original GameActivity entrypoint before constructors.
     * The Java bridge must independently verify its backed lifecycle host;
     * the native-only event transport is never treated as a Java receiver. */
    so_module *game = find_module(modules, module_count, "libgame.so");
    BS_REQUIRE(game, "original libgame.so absent");
    BsGameActivityTopology topology;
    BS_REQUIRE(bs_unity_preflight_gameactivity_topology(game, &topology),
               "original GameActivity/native_app_glue code topology");
    BsGameActivityPreflight activity;
    BS_REQUIRE(bs_unity_preflight_original_gameactivity(game, 0, &activity),
               "original GameActivity JNI needs real Java methods, callbacks "
               "and backed android_app creation");

    /* Native ELF DT_NEEDED determines constructor ordering. The validated
     * graph was captured BEFORE so_free_temp discarded original PT_DYNAMIC;
     * reject any missing/duplicate index BEFORE calling even one constructor.
     */
    BS_REQUIRE(caps->constructor_order &&
               caps->constructor_order_count == module_count &&
               module_count <= BS_ELF_DEP_MAX_MODULES,
               "verified original DT_NEEDED constructor order");
    bool seen[BS_ELF_DEP_MAX_MODULES] = {false};
    for (size_t i = 0; i < module_count; ++i) {
        size_t id = caps->constructor_order[i];
        BS_REQUIRE(id < module_count && !seen[id],
                   "invalid or repeated dependency-order module");
        seen[id] = true;
    }
    /* The pinned Quest libcrypto.so constructor at original VA 0xf8c60
     * checks OPENSSL_armcap before attempting an ARM SIGILL/MRS feature
     * probe. Horizon cannot provide the Android/Linux SIGILL probe contract,
     * and Eden's Dynarmic aborts on that raw system-register read. Force
     * OpenSSL's documented portable-software path instead of advertising
     * unverified hardware AES/SHA capabilities. This changes no guest ELF
     * code, bypasses no ABI/Java/graphics readiness check, and is valid on
     * real Switch hardware as well as Eden (with lower crypto throughput). */
    BS_REQUIRE(setenv("OPENSSL_armcap", "0", 1) == 0 &&
               getenv("OPENSSL_armcap") &&
               strcmp(getenv("OPENSSL_armcap"), "0") == 0,
               "unable to disable unsafe OpenSSL ARM signal-based CPU probe");
    debugPrintf("[crypto] pinned OpenSSL ARM CPU probe uses verified portable "
                "path (OPENSSL_armcap=0)\n");
    /* Diagnostic runs stop at the first original Vulkan graphics call.
     * Window graphics runs enter the driver after the same startup checks. */
    if (!progress->constructors_completed) {
        if (caps->unity_window_graphics)
            bs_vulkan_enable_unity_window();
        else
            bs_vulkan_arm_graphics_boundary(true);
    }
    for (size_t i = progress->constructors_completed; i < module_count; ++i) {
        so_module *module = &modules[caps->constructor_order[i]];
        debugPrintf("[unity] executing original constructors in "
                    "DT_NEEDED order: %s\n", module->name);
        so_execute_init_array(module, bs_vulkan_graphics_boundary_seen);
        if (bs_vulkan_graphics_boundary_seen()) return false;
        ++progress->constructors_completed;
    }
    /* Original libunity JNI_OnLoad loads two class-name addresses through
     * pinned R_AARCH64_RELATIVE slots at 0x1754150/0x1754158. The second
     * became null in the observed Eden guest trace after 12 JNI native
     * registrations. Read the mapped slots before entering any JNI callback;
     * unrelocated or overwritten names must not be passed to FindClass. */
    so_module *unity = find_module(modules, module_count, "libunity.so");
    BS_REQUIRE(unity && unity->load_virtbase &&
               unity->load_size > 0x1754158u + sizeof(uintptr_t),
               "original Unity JNI class name relocation spans invalid");
    static const struct {
        size_t got_offset, string_offset;
        const char *expected_name;
    } unity_java_names[] = {
        {0x1754150u, 0x166160u, "com/unity3d/player/UnityPlayer"},
        {0x1754158u, 0x16617fu,
         "com/unity3d/player/UnityPlayerForActivityOrService"}
    };
    for (size_t i = 0; i < sizeof unity_java_names / sizeof unity_java_names[0]; ++i) {
        const uintptr_t expected = (uintptr_t)unity->load_virtbase +
                                   unity_java_names[i].string_offset;
        uintptr_t actual = 0;
        memcpy(&actual, (const uint8_t *)unity->load_virtbase +
                        unity_java_names[i].got_offset, sizeof actual);
        bool relocation_matches = actual == expected &&
            unity_java_names[i].string_offset +
                strlen(unity_java_names[i].expected_name) + 1 <= unity->load_size &&
            strcmp((const char *)expected, unity_java_names[i].expected_name) == 0;
        debugPrintf("[unity:jni-class] original Java name %lu slot=0x%lx "
                    "relocated=%d actual=%p expected=%p\n",
                    (unsigned long)i,
                    (unsigned long)unity_java_names[i].got_offset,
                    relocation_matches, (void *)actual, (void *)expected);
        BS_REQUIRE(relocation_matches,
                   "original Unity Java class R_AARCH64_RELATIVE corrupted");
    }
    /* Separate original JNI_OnLoad entrypoints are kept in their actual
     * library modules. A matching native loader that was already initialized
     * above must never be entered twice.
     */
    /* Parse the original ELF dynamic symbol names, including GNU versions:
     * libunity.so's entry is JNI_OnLoad@@libunity, which does not match a
     * plain end-anchored readelf string search. The real seven-entry set is
     * checked by tests/test_original_jni_graphics_boundary.py; omitting Unity
     * would skip its native JavaVM initialization. */
    static const char *const jni_order[] = {
        "libmain.so", "libil2cpp.so", "libgame.so", "libunity.so",
        "libOVRPlugin.so", "libovrplatformloader.so", "libUnityOpenXR.so"
    };
    BSJNIReadiness jni_ready = {
        .unresolved_imports = 0,
        .missing_needed_libraries = 0,
        .platform_services_ready = 1
    };
    for (; progress->jni_completed < sizeof jni_order / sizeof jni_order[0];
           ++progress->jni_completed) {
        size_t i = progress->jni_completed;
        if (s_native_loader_done && !strcmp(jni_order[i], "libmain.so")) continue;
        if (s_il2cpp_onload_done && !strcmp(jni_order[i], "libil2cpp.so")) continue;
        if (s_gameactivity_registration_done &&
            !strcmp(jni_order[i], "libgame.so")) continue;
        so_module *module = find_module(modules, module_count, jni_order[i]);
        BS_REQUIRE(module, "required JNI module absent");
        const uintptr_t entry = so_find_module_export(module, "JNI_OnLoad");
        BS_REQUIRE(entry, "original JNI_OnLoad export absent");
        if (!strcmp(jni_order[i], "libUnityOpenXR.so") &&
            !bs_jni_unityplayer_has_current_activity() &&
            !progress->activity_probe_run && s_gameactivity_registration_done &&
            caps->probe_activity_constructor) {
            progress->activity_probe_run = true;
            caps->probe_activity_constructor();
            if (bs_vulkan_graphics_boundary_seen()) return false;
        }
        if (!strcmp(jni_order[i], "libUnityOpenXR.so") &&
            !bs_jni_unityplayer_has_current_activity()) {
            progress->waiting_for_activity = true;
            debugPrintf("[unity] JNI startup waiting for original "
                        "UnityPlayer.currentActivity before %s "
                        "(%u/%u hooks complete)\n", jni_order[i],
                        progress->jni_completed,
                        (unsigned)(sizeof jni_order / sizeof jni_order[0]));
            return false;
        }
        debugPrintf("[unity] entering original JNI_OnLoad: %s\n", jni_order[i]);
        progress->terminal_failure = true;
        int32_t version = bs_jni_call_onload_if_ready(
            jni_order[i], (BSJNIOnLoadFn)entry, &jni_ready);
        if (bs_vulkan_graphics_boundary_seen()) return false;
        BS_REQUIRE(version == BS_JNI_VERSION_1_6, "original JNI_OnLoad failed");
        progress->terminal_failure = false;
    }
    debugPrintf("[unity] original JNI startup completed %u hooks\n",
                progress->jni_completed);
    /* Exact Quest chain (independently disassembled):
     * initializeNativeCode -> original 632-byte NativeCode ->
     * GameActivity_onCreate -> pthread_create(original worker) ->
     * android_main(real 384-byte android_app).
     * Calling android_main ourselves bypasses initialization of the actual
     * android_app, onAppCmd, mutexes, pipe, looper and owned guest thread.
     * Its worker must be created and owned by the original libgame code. */
    progress->terminal_failure = true;
    if (caps->before_native_code) {
        int pre = caps->before_native_code();
        if (bs_vulkan_graphics_boundary_seen()) return false;
        BS_REQUIRE(pre == 0, "UnityPlayer constructor natives failed");
    }
    BS_REQUIRE(bs_gameactivity_java_driver_create(
                   caps->java_driver, caps->java_launch) == 0,
               "original initializeNativeCode failed to create NativeCode");
    progress->terminal_failure = false;
    progress->native_code_started = true;
    debugPrintf("[unity] original GameActivity.NativeCode initialized; "
                "native worker owns original android_main\n");
    return true;
#undef BS_REQUIRE
}
