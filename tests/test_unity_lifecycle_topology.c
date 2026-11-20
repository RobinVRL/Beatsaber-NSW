/* Original Quest GameActivity topology and passive lifecycle host test.
 *
 * Uses actual owner-staged libgame.so bytes in a read-only mapped copy.
 * Guest calls abort in this host test. A constructor stub exercises the stop guard.
 * Build example from beat-saber-engine with host clang:
 *
 * clang -std=c11 -Wall -Wextra -Werror -Wpedantic -O2 \
 *   -Itests/lifecycle_shims -Isource \
 *   -include tests/test_gameactivity_preflight_path.h \
 *   tests/test_unity_lifecycle_topology.c source/unity_lifecycle.c \
 *   -o tests/test_unity_lifecycle_topology.exe
 *
 * The host-only elf.h and zlib.h shims do NOT participate in the Switch build.
 */
#include "unity_lifecycle.h"
#include "jni_bridge.h"
#include "platform_bridge.h"
#include "vulkan_bridge.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const struct {
    const char *name;
    uintptr_t vaddr;
} s_pins[] = {
    {"JNI_OnLoad", 0x1d4cc},
    {"GameActivity_register", 0x1bd2c},
    {"Java_com_google_androidgamesdk_GameActivity_initializeNativeCode", 0x1cdf4},
    {"android_main", 0x196d0},
    {"GameActivity_onCreate", 0x20708},
    {"GameActivityEventsInit", 0x1fc88},
    {"android_app_read_cmd", 0x202e0},
    {"android_app_pre_exec_cmd", 0x20398},
    {"android_app_post_exec_cmd", 0x20484},
    {"android_app_write_cmd", 0x20524},
    {"android_app_swap_input_buffers", 0x205f0}
};
static int s_hide_export = -1;
static unsigned s_constructor_calls;
static unsigned s_onload_calls;
static unsigned s_native_code_calls;
static bool s_hit_boundary_on_ctor = true;
static bool s_current_activity;
static bool s_java_ready;
static bool s_boundary_armed;
static bool s_unity_window_enabled;
static bool s_boundary_seen;
static uintptr_t s_vm;
static uintptr_t s_jni_slots[233];
static uintptr_t *s_jni_table = s_jni_slots;

/* Same polynomial and reflected seed as zlib.crc32(). Real output is
 * cross-checked against the exact original ELF code spans in the test. */
uLong crc32(uLong initial, const Bytef *data, uInt length) {
    if (!data) return initial;
    uint32_t crc = (uint32_t)initial ^ UINT32_C(0xffffffff);
    for (uInt i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^
                  ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return (uLong)(crc ^ UINT32_C(0xffffffff));
}

int debugPrintf(char *message, ...) { (void)message; return 0; }
#ifdef _WIN32
int setenv(const char *name, const char *value, int overwrite) {
    assert(overwrite == 1);
    return _putenv_s(name, value);
}
#endif
void *bs_jni_vm(void) { return &s_vm; }
void *bs_jni_env(void) { return &s_jni_table; }
int bs_jni_gameactivity_java_ready(void) { return s_java_ready; }
int bs_jni_unityplayer_has_current_activity(void) { return s_current_activity; }
int bs_jni_gameactivity_registration_abi_ready(void) { return 0; }
int bs_jni_gameactivity_natives_captured(void) { return 0; }
int bs_gameactivity_java_driver_create(BsGameActivityJavaDriver *driver,
                                        const BsGameActivityJavaLaunch *launch) {
    (void)driver; (void)launch;
    assert(s_current_activity && s_onload_calls == 7);
    ++s_native_code_calls;
    return 0;
}
bool bs_guest_tls_is_thread_attached(void) { return true; }
size_t bs_jni_registration_count(void) { return 0; }
const char *bs_jni_last_error(void) { return "guest calls disabled by test"; }
int32_t bs_jni_call_onload_if_ready(const char *name, BSJNIOnLoadFn entry,
                                    const BSJNIReadiness *ready) {
    assert(entry && ready && !s_hit_boundary_on_ctor);
    if (!strcmp(name, "libUnityOpenXR.so")) assert(s_current_activity);
    ++s_onload_calls;
    return BS_JNI_VERSION_1_6;
}
void so_execute_init_array(so_module *module, bool (*should_stop)(void)) {
    (void)module;
    assert(s_boundary_armed != s_unity_window_enabled);
    assert(!should_stop());
    ++s_constructor_calls;
    if (s_hit_boundary_on_ctor) {
        s_boundary_seen = true;
        assert(should_stop());
    }
}
void bs_vulkan_arm_graphics_boundary(bool armed) { s_boundary_armed = armed; }
void bs_vulkan_enable_unity_window(void) {
    s_boundary_armed = false;
    s_unity_window_enabled = true;
}
bool bs_vulkan_graphics_boundary_seen(void) { return s_boundary_seen; }
DynLibFunction bs_platform_symbols[] = {
    {"__android_log_print", (uintptr_t)0x444444}
};
const int bs_platform_symbols_count = 1;

uintptr_t so_find_module_export(so_module *module, const char *name) {
    for (size_t i = 0; i < sizeof s_pins / sizeof s_pins[0]; ++i) {
        if (strcmp(s_pins[i].name, name) == 0)
            return (int)i == s_hide_export ? 0 :
                   (uintptr_t)module->load_virtbase + s_pins[i].vaddr;
    }
    return 0;
}

static unsigned char *load_original_libgame(so_module *out) {
    FILE *file = fopen(BS_QUEST_ROOT "libgame.so", "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long count = ftell(file);
    assert(count == 152576L);
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *image = malloc((size_t)count);
    assert(image);
    assert(fread(image, 1, (size_t)count, file) == (size_t)count);
    fclose(file);

    assert(crc32(0, image, (uInt)count) == 0x41828fc1UL);
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)image;
    assert(header->e_ident[0] == 0x7f &&
           header->e_ident[1] == 'E' &&
           header->e_ident[2] == 'L' &&
           header->e_ident[3] == 'F');
    assert(header->e_machine == EM_AARCH64);
    assert(header->e_phentsize == sizeof(Elf64_Phdr));
    assert(header->e_phoff < (size_t)count);
    assert(header->e_phnum <=
           ((size_t)count - (size_t)header->e_phoff) /
            sizeof(Elf64_Phdr));
    const Elf64_Phdr *segments =
        (const Elf64_Phdr *)(image + header->e_phoff);
    size_t mapped_size = 0;
    for (size_t i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        if (segment->p_type != PT_LOAD) continue;
        assert(segment->p_offset <= (uint64_t)count);
        assert(segment->p_filesz <= (uint64_t)count - segment->p_offset);
        assert(segment->p_memsz >= segment->p_filesz);
        assert(segment->p_vaddr + segment->p_memsz < 1024u * 1024u);
        if (mapped_size < segment->p_vaddr + segment->p_memsz)
            mapped_size = (size_t)(segment->p_vaddr + segment->p_memsz);
    }
    assert(mapped_size > 0);
    unsigned char *mapped = calloc(1, mapped_size);
    assert(mapped);
    for (size_t i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr *segment = &segments[i];
        if (segment->p_type != PT_LOAD) continue;
        memcpy(mapped + segment->p_vaddr, image + segment->p_offset,
               (size_t)segment->p_filesz);
    }
    free(image);
    memset(out, 0, sizeof *out);
    memcpy(out->name, "libgame.so", sizeof "libgame.so");
    /* Mirror so_finalize's observable state. load_base is donated, and both
     * dynamic symbol/string tables now point into the executable mapping.
     * The preflight must never read the donated source pointer. */
    out->load_base = (void *)(uintptr_t)0x1000u;
    out->load_virtbase = mapped;
    out->load_size = mapped_size;
    out->syms = (Elf64_Sym *)(mapped + 0x2000u);
    out->dynstrtab = (char *)(mapped + 0x3000u);
    out->num_syms = 300;
    uintptr_t target = (uintptr_t)mapped + 0x1bd2c;
    memcpy(mapped + 0x2c5b8, &target, sizeof target);
    target = bs_platform_symbols[0].func;
    memcpy(mapped + 0x2c680, &target, sizeof target);
    return mapped;
}

static void test_original_code(so_module *game) {
    assert(bs_gameactivity_entrypoint_count() ==
           sizeof s_pins / sizeof s_pins[0]);
    for (size_t i = 0; i < bs_gameactivity_entrypoint_count(); ++i)
        assert(!strcmp(bs_gameactivity_entrypoint_name(i), s_pins[i].name));
    assert(!bs_gameactivity_entrypoint_name(bs_gameactivity_entrypoint_count()));

    BsGameActivityTopology result;
    assert(!bs_unity_preflight_gameactivity_topology(game, NULL));
    assert(bs_unity_preflight_gameactivity_topology(game, &result));
    assert(result.blockers == 0 && result.entrypoint_mismatch == 0);
    assert(result.source_verified && result.mapped_image_verified);
    assert(result.entrypoints_verified == bs_gameactivity_entrypoint_count());

    unsigned char *mapped = game->load_virtbase;
    mapped[0x1cdf4] ^= 0x40;
    assert(!bs_unity_preflight_gameactivity_topology(game, &result));
    assert(result.blockers & BS_GA_TOPO_BYTES);
    assert(result.entrypoint_mismatch == (UINT32_C(1) << 2));
    mapped[0x1cdf4] ^= 0x40;

    s_hide_export = 4;
    assert(!bs_unity_preflight_gameactivity_topology(game, &result));
    assert(result.blockers & BS_GA_TOPO_SYMBOL);
    assert(result.entrypoint_mismatch == (UINT32_C(1) << 4));
    s_hide_export = -1;

    game->so_base = (void *)1;
    assert(!bs_unity_preflight_gameactivity_topology(game, &result));
    assert(result.blockers & BS_GA_TOPO_MODULE);
    game->so_base = NULL;

    Elf64_Sym *mapped_syms = game->syms;
    game->syms = (Elf64_Sym *)game->load_base;
    assert(!bs_unity_preflight_gameactivity_topology(game, &result));
    assert(result.blockers & BS_GA_TOPO_MODULE);
    game->syms = mapped_syms;

    game->load_size = 0x20000u;
    assert(!bs_unity_preflight_gameactivity_topology(game, &result));
    assert(result.blockers & BS_GA_TOPO_MODULE);
    game->load_size = 0x31000u;
    assert(bs_unity_preflight_gameactivity_topology(game, &result));
}

static void test_passive_state_machine(void) {
    BsGameActivityTrace trace;
    bs_gameactivity_trace_reset(&trace);
    assert(!bs_gameactivity_trace_running(&trace));
    assert(!bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_RESUME));
    assert(trace.rejected_commands == 1);
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_START));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_INIT_WINDOW));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_WINDOW_RESIZED));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_WINDOW_REDRAW_NEEDED));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_CONTENT_RECT_CHANGED));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_WINDOW_INSETS_CHANGED));
    assert(trace.window_events == 5);
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_CONFIG_CHANGED));
    assert(trace.configuration_events == 1);
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_LOW_MEMORY));
    assert(trace.low_memory_events == 1);
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_RESUME));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_SAVE_STATE));
    assert(trace.save_state_requests == 1);
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_GAINED_FOCUS));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_TEXT_INPUT_STATE_CHANGED));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_KEY_EVENTS));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_MOTION_EVENTS));
    assert(trace.input_events == 3);
    assert(bs_gameactivity_trace_running(&trace));
    assert(!bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_START));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_PAUSE));
    assert(!bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_KEY_EVENTS));
    assert(!bs_gameactivity_trace_running(&trace));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_STOP));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_START));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_RESUME));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_TERM_WINDOW));
    assert(!bs_gameactivity_trace_running(&trace));
    assert(bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_DESTROY));
    assert(!bs_gameactivity_trace_command(&trace, BS_GA_APP_CMD_START));
    assert(trace.stage == BS_GA_TRACE_DESTROYED);
    assert(!bs_gameactivity_trace_running(NULL));
    assert(!bs_gameactivity_trace_command(NULL, BS_GA_APP_CMD_START));
}

static void test_no_constructor_execution(so_module *game) {
    /* Even a caller claiming EVERY capability is ready may not enter the
     * original guest on symbolic Java Activity handles. */
    so_module modules[15] = {0};
    size_t order[15] = {0};
    for (size_t i = 0; i < 15; ++i) {
        memcpy(modules[i].name, "libsynthetic.so", sizeof "libsynthetic.so");
        order[i] = i;
    }
    modules[4] = *game;
    s_jni_slots[28] = s_jni_slots[29] = (uintptr_t)0x1111;
    const unsigned required[] = {6, 15, 16, 21, 23, 33, 94, 113, 215};
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i)
        s_jni_slots[required[i]] = (uintptr_t)0x2222;
    BsGameActivityJavaDriver driver = {0};
    const BsGameActivityJavaLaunch launch = {
        .activity = &s_vm,
        .asset_manager = &s_vm,
        .configuration = &s_vm,
        .first_path = "files/",
        .second_path = "obb/",
        .third_path = "external-files/"
    };
    BsUnityStartProgress progress = {0};
    BsUnityStartCapabilities caps = {
        .progress = &progress,
        .unresolved_relocations = 0,
        .constructor_order = order,
        .constructor_order_count = 15,
        .bionic_abi_verified = true,
        .game_activity_ready = true,
        .assets_ready = true,
        .media_ndk_ready = true,
        .vulkan_preflight_ready = true,
        .java_driver = &driver,
        .java_launch = &launch
    };
    assert(!bs_unity_start_original_game(modules, 15, &caps));
    assert(s_constructor_calls == 0);
    /* JNI registration-only may be entered on an independently checked
     * original ELF; these host stubs lack a validated JNI function table and
     * must refuse before calling the ARM64 guest entrypoint. */
    assert(!bs_unity_start_verified_gameactivity_registration(game, 0));

    s_java_ready = true;
    assert(!bs_unity_start_original_game(modules, 15, &caps));
    assert(s_boundary_seen && s_constructor_calls == 1);
    assert(!s_unity_window_enabled);

    /* An independent bootstrap can complete native constructors and the
     * JNI prefix while the Activity is pending. A retry enters only the
     * remaining hooks. */
    progress = (BsUnityStartProgress){0};
    s_boundary_seen = false;
    s_hit_boundary_on_ctor = false;
    s_constructor_calls = 0;
    caps.unity_window_graphics = true;
    const char *const jni_module_names[] = {
        "libmain.so", "libil2cpp.so", "libunity.so", "libOVRPlugin.so",
        "libgame.so", "libovrplatformloader.so", "libUnityOpenXR.so"
    };
    for (size_t i = 0; i < sizeof jni_module_names / sizeof jni_module_names[0]; ++i)
        strcpy(modules[i].name, jni_module_names[i]);

    const size_t unity_size = 0x1754158u + sizeof(uintptr_t) + 1;
    unsigned char *unity = calloc(unity_size, 1);
    assert(unity);
    modules[2].load_virtbase = (void *)unity;
    modules[2].load_size = unity_size;
    strcpy((char *)unity + 0x166160u, "com/unity3d/player/UnityPlayer");
    strcpy((char *)unity + 0x16617fu,
           "com/unity3d/player/UnityPlayerForActivityOrService");
    uintptr_t first = (uintptr_t)unity + 0x166160u;
    uintptr_t second = (uintptr_t)unity + 0x16617fu;
    memcpy(unity + 0x1754150u, &first, sizeof first);
    memcpy(unity + 0x1754158u, &second, sizeof second);

    assert(!bs_unity_start_original_game(modules, 15, &caps));
    assert(s_unity_window_enabled && !s_boundary_armed && !s_boundary_seen);
    assert(progress.waiting_for_activity && s_constructor_calls == 15);
    assert(progress.jni_completed == 6 && s_onload_calls == 6);
    assert(!bs_unity_start_original_game(modules, 15, &caps));
    assert(s_constructor_calls == 15 && s_onload_calls == 6);
    s_current_activity = true;
    assert(bs_unity_start_original_game(modules, 15, &caps));
    assert(progress.jni_completed == 7 && s_onload_calls == 7);
    assert(s_constructor_calls == 15 && s_native_code_calls == 1);
    assert(bs_unity_start_original_game(modules, 15, &caps));
    assert(s_constructor_calls == 15 && s_onload_calls == 7 &&
           s_native_code_calls == 1);
    free(unity);
}

int main(void) {
    so_module game;
    unsigned char *mapped = load_original_libgame(&game);
    test_original_code(&game);
    test_passive_state_machine();
    test_no_constructor_execution(&game);
    free(mapped);
    puts("PASS: original GameActivity topology, passive lifecycle, graphics "
         "stop and JNI bootstrap resume");
    return 0;
}
