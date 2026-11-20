/* Original Quest Unity/GameActivity bootstrap coordination. MIT license. */
#ifndef BS_UNITY_LIFECYCLE_H
#define BS_UNITY_LIFECYCLE_H
#include "so_util.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "gameactivity_java_driver.h"

/* Zero-initialize for one mapped module set and keep it alive until teardown.
 * Retry on the same guest thread after a missing prerequisite is supplied. */
typedef struct BsUnityStartProgress {
    so_module *modules;
    size_t constructors_completed;
    unsigned jni_completed;
    bool waiting_for_activity;
    bool activity_probe_run;
    bool terminal_failure;
    bool native_code_started;
} BsUnityStartProgress;

typedef struct BsUnityStartCapabilities {
    BsUnityStartProgress *progress;
    size_t unresolved_relocations;
    const size_t *constructor_order;
    size_t constructor_order_count;
    bool bionic_abi_verified;
    bool game_activity_ready;
    bool assets_ready;
    bool media_ndk_ready;
    bool vulkan_preflight_ready;
    bool unity_window_graphics;
    /* The ORIGINAL initializeNativeCode creates NativeCode and its own
     * android_app worker. A caller-provided native-only android_app must
     * never be passed directly to original android_main. */
    BsGameActivityJavaDriver *java_driver;
    const BsGameActivityJavaLaunch *java_launch;
    void *game_activity_app; /* Legacy diagnostic only: NEVER passed to android_main. */
    /* Optional DEX preinit after the other JNI hooks and ELF constructors. */
    void (*probe_activity_constructor)(void);
    /* Optional: original UnityPlayer constructor natives (initJni,
     * nativeUnityPlayerForGameActivityInitialized) that Java runs in
     * onCreateSurfaceView after loadNative and before initializeNativeCode.
     * Returns 0 on success; nonzero is a terminal startup failure. */
    int (*before_native_code)(void);
} BsUnityStartCapabilities;

/* The exact Quest 1.46 libmain.so JNI_OnLoad only calls JavaVM GetEnv and
 * JNIEnv FindClass/RegisterNatives. Its full original ELF is CRC-checked
 * before allowing this limited native registration bootstrap. */
bool bs_unity_start_verified_native_loader(so_module *libmain,
                                            size_t own_unresolved);

/* Independently verified short native entrypoint from the supplied original
 * libil2cpp.so. It stores the real JavaVM for later IL2CPP initialization
 * and calls the imported Android logger. Other IL2CPP imports are NOT
 * implicitly marked ready by this limited JNI_OnLoad invocation.
 */
bool bs_unity_start_verified_il2cpp_onload(so_module *libil2cpp);

/* Read-only preflight for the ORIGINAL Quest libgame.so JNI_OnLoad.
 * The pinned JNI_OnLoad calls GameActivity_register, which performs Java
 * reflection/registration. The readiness probe demands independently
 * verified Java Activity method behavior, rather than metadata handles.
 * This function never invokes guest libgame code.
 *
 * Pass the unresolved relocation count for libgame.so specifically (not the
 * total across all 15 modules). The caller must first finalize the module's
 * executable mapping and release its temporary ELF image.
 */
enum BsGameActivityPreflightBlocker {
    BS_GA_BLOCK_MODULE             = 1u << 0,
    BS_GA_BLOCK_SOURCE             = 1u << 1,
    BS_GA_BLOCK_CODE               = 1u << 2,
    BS_GA_BLOCK_LOCAL_PLT          = 1u << 3,
    BS_GA_BLOCK_UNRESOLVED_IMPORTS = 1u << 4,
    BS_GA_BLOCK_JNI_VM             = 1u << 5,
    BS_GA_BLOCK_GUEST_TLS          = 1u << 6,
    BS_GA_BLOCK_JNI_SLOTS          = 1u << 7,
    BS_GA_BLOCK_JAVA_ACTIVITY      = 1u << 8,
    BS_GA_BLOCK_ANDROID_LOG_PLT    = 1u << 9
};

/* Bits in missing_jni_slots index this table of original JNIEnv slots:
 * 6 FindClass, 15 ExceptionOccurred, 16 ExceptionDescribe,
 * 21 NewGlobalRef, 23 DeleteLocalRef, 33 GetMethodID, 94 GetFieldID,
 * 113 GetStaticMethodID, 215 RegisterNatives.
 */
typedef struct BsGameActivityPreflight {
    uint32_t blockers;
    uint16_t missing_jni_slots;
    size_t own_unresolved_relocations;
    bool source_verified;
    bool executable_verified;
    bool local_plt_verified;
    bool android_log_plt_verified;
    bool java_host_verified;
} BsGameActivityPreflight;

/* Returns false until the actual Java GameActivity contract has been
 * implemented and independently validated. Logs precise failed checks.
 * The result structure is zeroed then filled, including on early failure.
 */
bool bs_unity_preflight_original_gameactivity(const so_module *libgame,
                                              size_t own_unresolved,
                                              BsGameActivityPreflight *result);

/* Narrow registration-only bootstrap of the PINNED original libgame.so
 * JNI_OnLoad. It records the 24 real native method addresses in JNIEnv and
 * installs the original cached method IDs without creating an Activity,
 * starting android_main or reporting Java/UI lifecycle availability.
 * Requires all actual JNI registration lookups/slots to be audited, zero
 * strong missing imports, source/code/PLT validation and thread TLS. */
bool bs_unity_start_verified_gameactivity_registration(so_module *libgame,
                                                       size_t own_unresolved);

/* A separate, stronger READ-ONLY source/entrypoint/topology audit. It pins
 * exact code bytes for the original libgame.so JNI_OnLoad, GameActivity
 * registration, initializeNativeCode, onCreate, android_main, event hooks and
 * native-app-glue command handlers. All symbol addresses refer to a finalized
 * executable mapping, with dynsym/dynstr rebased by successful so_finalize().
 * Passing this audit never means Java or XR is ready.
 *
 * entrypoint_mismatch has one bit per index returned by
 * bs_gameactivity_entrypoint_name(). Exports with the correct name but wrong
 * address/size/bytes are rejected. An absent source sets BS_GA_TOPO_SOURCE.
 */
enum BsGameActivityTopologyBlocker {
    BS_GA_TOPO_MODULE  = 1u << 0,
    BS_GA_TOPO_SOURCE  = 1u << 1,
    BS_GA_TOPO_SYMBOL  = 1u << 2,
    BS_GA_TOPO_BYTES   = 1u << 3
};
typedef struct BsGameActivityTopology {
    uint32_t blockers;
    uint32_t entrypoint_mismatch;
    size_t entrypoints_checked;
    size_t entrypoints_verified;
    bool source_verified;
    bool mapped_image_verified;
} BsGameActivityTopology;
size_t bs_gameactivity_entrypoint_count(void);
const char *bs_gameactivity_entrypoint_name(size_t index);
bool bs_unity_preflight_gameactivity_topology(const so_module *libgame,
                                              BsGameActivityTopology *result);

/* Passive command sequencing for the GameActivity native_app_glue protocol.
 * The numeric values are pinned to the supplied original Quest libgame.so
 * GameActivity command writer, including command 6 for keyboard visibility,
 * command 17 for window insets and command 18 for input-state changes.
 * This is an isolated diagnostic trace,
 * NEVER a fabricated 'struct android_app' and NEVER an active event source.
 * No Java, ALooper, ANativeWindow or guest callbacks are invoked.
 */
enum BsGameActivityGlueCommand {
    BS_GA_APP_CMD_UNUSED_INPUT_CHANGED = 0,
    BS_GA_APP_CMD_INIT_WINDOW = 1,
    BS_GA_APP_CMD_TERM_WINDOW = 2,
    BS_GA_APP_CMD_WINDOW_RESIZED = 3,
    BS_GA_APP_CMD_WINDOW_REDRAW_NEEDED = 4,
    BS_GA_APP_CMD_CONTENT_RECT_CHANGED = 5,
    BS_GA_APP_CMD_SOFTWARE_KB_VIS_CHANGED = 6,
    BS_GA_APP_CMD_GAINED_FOCUS = 7,
    BS_GA_APP_CMD_LOST_FOCUS = 8,
    BS_GA_APP_CMD_CONFIG_CHANGED = 9,
    BS_GA_APP_CMD_LOW_MEMORY = 10,
    BS_GA_APP_CMD_START = 11,
    BS_GA_APP_CMD_RESUME = 12,
    BS_GA_APP_CMD_SAVE_STATE = 13,
    BS_GA_APP_CMD_PAUSE = 14,
    BS_GA_APP_CMD_STOP = 15,
    BS_GA_APP_CMD_DESTROY = 16,
    BS_GA_APP_CMD_WINDOW_INSETS_CHANGED = 17,
    BS_GA_APP_CMD_TEXT_INPUT_STATE_CHANGED = 18,
    BS_GA_APP_CMD_KEY_EVENTS = 19,
    BS_GA_APP_CMD_MOTION_EVENTS = 20
};
enum BsGameActivityTraceStage {
    BS_GA_TRACE_CREATED = 0,
    BS_GA_TRACE_STARTED,
    BS_GA_TRACE_RESUMED,
    BS_GA_TRACE_PAUSED,
    BS_GA_TRACE_STOPPED,
    BS_GA_TRACE_DESTROYED
};
typedef struct BsGameActivityTrace {
    enum BsGameActivityTraceStage stage;
    bool window_attached;
    bool input_focused;
    unsigned accepted_commands;
    unsigned rejected_commands;
    unsigned configuration_events;
    unsigned window_events;
    unsigned low_memory_events;
    unsigned save_state_requests;
    unsigned input_events;
} BsGameActivityTrace;
void bs_gameactivity_trace_reset(BsGameActivityTrace *trace);
bool bs_gameactivity_trace_command(BsGameActivityTrace *trace, int8_t command);
/* Only a coherent sequence, NOT proof that any real android_app exists. */
bool bs_gameactivity_trace_running(const BsGameActivityTrace *trace);

/* Original Unity and libgame entrypoints, gated on concrete runtime services.
 * This does not fabricate a GameActivity, Unity lifecycle or an XR runtime.
 * Returns false if any imported ABI or required real device service is absent.
 */
bool bs_unity_start_original_game(so_module *modules, size_t module_count,
                                  const BsUnityStartCapabilities *caps);
#endif
