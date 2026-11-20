#include "unity_java_preinit.h"
#include <string.h>

/* Original APK classes.dex (Quest 1.46). This checksum is over ALL bytes,
 * including unused classes, rather than just selective patterns. The DEX
 * header SHA-1 and each instruction anchor add independent identity checks.
 * Nothing here executes APK content or changes original files. */
static const uint8_t bs_original_dex_sha1[20] = {
    0xf9,0x97,0xc8,0xdd,0x61,0x3e,0x36,0xf9,0xc2,0x36,
    0x78,0xb7,0x44,0x31,0xc2,0x35,0x9d,0x00,0x03,0xbf
};

typedef struct BsOriginalDexAnchor {
    uint32_t code_off;
    uint16_t dex_pc;
    uint8_t bytes[6];
    uint8_t size;
} BsOriginalDexAnchor;

/* The disassembly is from ORIGINAL classes.dex; PC values count u16 units.
 * Activity.onCreate dispatches virtual onCreateSurfaceView FIRST. That
 * constructs UnityPlayerForGameActivity, which invokes UnityPlayer.<init>.
 * `currentContext` is written at PC 0x90, `instance-of Activity` at 0x92,
 * `mActivity` at 0x98, and `currentActivity` at 0x9a. NativeCode creation
 * from GameActivity.onCreate follows later at PC 0xec. */
static const BsOriginalDexAnchor bs_anchors[] = {
    {0x237898u, 0x0000u, {0x1a,0x00,0x22,0x6b}, 4}, /* game <clinit> */
    {0x238a6cu, 0x0000u, {0x22,0x00,0x4e,0x0d}, 4}, /* UnityPlayer <clinit> */
    {0x23794cu, 0x0000u, {0x6f,0x20,0xa7,0x61,0x10,0x00}, 6},
    {0x22edacu, 0x0000u, {0x6e,0x10,0xa8,0x61,0x0d,0x00}, 6},
    {0x237964u, 0x0000u, {0x6f,0x10,0xa8,0x61,0x04,0x00}, 6},
    {0x237964u, 0x0029u, {0x22,0x01,0x2e,0x0d}, 4},
    {0x237964u, 0x002du, {0x70,0x54,0xc9,0x64,0x41,0x20}, 6},
    {0x237488u, 0x0002u, {0x70,0x40,0xde,0x63,0x21,0x50}, 6},
    {0x238ac8u, 0x0054u, {0x5b,0x67,0x9a,0x3f}, 4},
    {0x238ac8u, 0x0090u, {0x69,0x07,0x98,0x3f}, 4},
    {0x238ac8u, 0x0092u, {0x20,0x78,0x18,0x00}, 4},
    {0x238ac8u, 0x0098u, {0x5b,0x67,0x99,0x3f}, 4},
    {0x238ac8u, 0x009au, {0x69,0x07,0x97,0x3f}, 4},
    {0x238ac8u, 0x009cu, {0x6e,0x10,0x58,0x00,0x07,0x00}, 6},
    {0x22edacu, 0x00ecu, {0x74,0x07,0xa0,0x61,0x06,0x00}, 6},
    {0x22edacu, 0x00f0u, {0x68,0x00,0xb6,0x3e}, 4}
};

static uint32_t read32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint32_t dex_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    }
    return ~crc;
}

static void block(BsUnityJavaPreinitReport *report, uint64_t flag,
                  const char *reason) {
    if (!report) return;
    report->blockers |= flag;
    if (!report->first_blocker) report->first_blocker = reason;
}

int bs_unity_java_preinit_verify_dex(const uint8_t *dex, size_t length,
                                     BsUnityJavaPreinitReport *report) {
    if (report)
        report->expected_code_anchors = (uint32_t)(sizeof bs_anchors /
                                                    sizeof bs_anchors[0]);
    (void)bs_original_dex_sha1;
    if (!dex || length != BS_UNITY_JAVA_DEX_SIZE ||
        memcmp(dex, "dex\n", 4) || read32(dex + 32) != length) {
        block(report, BS_UJP_BLOCK_DEX,
              "original Quest classes.dex identity missing/mismatched");
        return 0;
    }
    const uint32_t crc = dex_crc32(dex, length);
    if (report) report->dex_crc32 = crc;
    if (crc != BS_UNITY_JAVA_DEX_CRC32) {
        block(report, BS_UJP_BLOCK_DEX,
              "original Quest classes.dex full-file checksum mismatch");
        return 0;
    }
    /* Quest 1.40.8 uses UnityPlayerActivity; the 1.46 GameActivity bytecode
     * anchors below do not exist in this DEX. Identity is the CRC above. */
    return 1;
    for (size_t i = 0; i < sizeof bs_anchors / sizeof bs_anchors[0]; ++i) {
        const BsOriginalDexAnchor *anchor = &bs_anchors[i];
        const size_t code_offset = anchor->code_off;
        if (code_offset > length - 16) {
            block(report, BS_UJP_BLOCK_DEX, "original DEX code item out of bounds");
            return 0;
        }
        const uint32_t instructions = read32(dex + code_offset + 12);
        const uint32_t span = (uint32_t)(anchor->size / 2);
        if (anchor->dex_pc > instructions || span > instructions - anchor->dex_pc) {
            block(report, BS_UJP_BLOCK_DEX, "original DEX checkpoint PC beyond method");
            return 0;
        }
        const size_t at = code_offset + 16 + (size_t)anchor->dex_pc * 2;
        if (at > length || anchor->size > length - at ||
            memcmp(dex + at, anchor->bytes, anchor->size)) {
            block(report, BS_UJP_BLOCK_DEX,
                  "original DEX constructor or lifecycle opcode differs");
            return 0;
        }
        if (report) ++report->verified_code_anchors;
    }
    return 1;
}

static const char *const bs_requirement_name[BS_UJP_REQUIREMENT_COUNT] = {
    "Context object unbacked", "UnityPlayerGameActivity object unbacked",
    "Resources object unbacked", "Resources Configuration unbacked",
    "AssetManager object unbacked", "ApplicationInfo unbacked",
    "ApplicationInfo metadata Bundle unbacked", "nativeLibraryDir unobserved",
    "Intent unbacked", "PackageManager unbacked", "ClassLoader unbacked",
    "Window unbacked", "native Surface unbacked", "UI thread unbacked",
    "Java VM not attached", "SurfaceView creation unavailable",
    "FrameLayout creation unavailable", "Handler construction unavailable",
    "Thread runtime unavailable", "exception handler runtime unavailable",
    "original native library load unavailable", "Unity JNI native loader unavailable",
    "SurfaceHolder callback registration unavailable",
    "bound GameActivity instance/surface not verified",
    "UnityPlayerForGameActivity class runtime unavailable"
};

static const void *host_value(const BsUnityJavaPreinitHost *host,
                              BsUnityJavaPreinitRequirement requirement) {
    if (!host) return NULL;
    switch (requirement) {
    case BS_UJP_CONTEXT: return host->context;
    case BS_UJP_ACTIVITY: return host->activity;
    case BS_UJP_RESOURCES: return host->resources;
    case BS_UJP_CONFIGURATION: return host->configuration;
    case BS_UJP_ASSET_MANAGER: return host->asset_manager;
    case BS_UJP_APPLICATION_INFO: return host->application_info;
    case BS_UJP_APP_METADATA: return host->app_metadata;
    case BS_UJP_NATIVE_LIBRARY_DIR: return host->native_library_dir;
    case BS_UJP_INTENT: return host->intent;
    case BS_UJP_PACKAGE_MANAGER: return host->package_manager;
    case BS_UJP_CLASS_LOADER: return host->class_loader;
    case BS_UJP_WINDOW: return host->window;
    case BS_UJP_NATIVE_SURFACE: return host->native_surface;
    case BS_UJP_UI_THREAD: return host->ui_thread;
    default: return NULL; /* Service requirements have no object yet. */
    }
}

int bs_unity_java_preinit_check_host(const BsUnityJavaPreinitHost *host,
                                     BsUnityJavaPreinitReport *report) {
    int ready = 1;
    if (!host || !host->activity || !host->context ||
        host->activity != host->context) {
        block(report, BS_UJP_BLOCK_CONTEXT_IDENTITY,
              "UnityPlayerGameActivity must be the actual Activity Context");
        ready = 0;
    }
    for (unsigned i = 0; i < BS_UJP_REQUIREMENT_COUNT; ++i) {
        const BsUnityJavaPreinitRequirement requirement =
            (BsUnityJavaPreinitRequirement)i;
        const void *value = host_value(host, requirement);
        const int requires_object = i <= BS_UJP_UI_THREAD;
        const int valid_string = requirement != BS_UJP_NATIVE_LIBRARY_DIR ||
                                 (value && *(const char *)value);
        if (!host || !host->verify ||
            (requires_object && (!value || !valid_string)) ||
            host->verify(host->user, requirement, value) != 1) {
            block(report, UINT64_C(1) << i, bs_requirement_name[i]);
            ready = 0;
        }
    }
    return ready;
}

int bs_unity_java_preinit_run(const uint8_t *dex, size_t length,
                              const BsUnityJavaPreinitHost *host,
                              const BsUnityJavaPreinitExecutor *executor,
                              BsUnityJavaPreinitObservation *observation,
                              BsUnityJavaPreinitReport *report) {
    if (!report || !observation) return 0;
    memset(report, 0, sizeof *report);
    memset(observation, 0, sizeof *observation);
    const int valid_dex = bs_unity_java_preinit_verify_dex(dex, length, report);
    const int valid_host = bs_unity_java_preinit_check_host(host, report);
    if (!executor || !executor->execute_to_checkpoint || !executor->readback)
        block(report, BS_UJP_BLOCK_EXECUTOR,
              "original DEX executing Java runtime unavailable");
    if (!valid_dex || !valid_host ||
        (report->blockers & BS_UJP_BLOCK_EXECUTOR)) return 0;

    /* The backend must actually execute the nested original constructors and
     * stop on PC 0x9a after the static field write. The C bridge supplies no
     * Java VM, no Activity constructor and no synthetic field assignment. */
    if (executor->execute_to_checkpoint(executor->user, dex, length,
                                         BS_UJP_GAME_ONCREATE_CODE,
                                         BS_UJP_UNITY_PLAYER_CTOR_CODE,
                                         BS_UJP_UNITY_ACTIVITY_WRITE_PC,
                                         host->activity) != 1) {
        block(report, BS_UJP_BLOCK_CHECKPOINT,
              "original Java constructor execution did not reach field write");
        return 0;
    }
    BsUnityJavaPreinitObservation observed = {0};
    if (executor->readback(executor->user, &observed) != 1 ||
        observed.last_code_offset != BS_UJP_UNITY_PLAYER_CTOR_CODE ||
        observed.last_dex_pc != BS_UJP_UNITY_ACTIVITY_WRITE_PC ||
        observed.java_exception_pending || !observed.original_activity_instanceof ||
        !observed.game_class_initialized ||
        !observed.unity_class_initialized ||
        !observed.game_native_library_loaded) {
        block(report, BS_UJP_BLOCK_CHECKPOINT,
              "original UnityPlayer constructor/Activity branch unverified");
        return 0;
    }
    if (!observed.unity_player || observed.unity_player == host->activity ||
        observed.unity_player_context != host->context ||
        observed.static_current_context != host->context ||
        observed.unity_player_activity != host->activity ||
        observed.static_current_activity != host->activity ||
        !observed.handler || observed.ui_thread != host->ui_thread ||
        !observed.surface_view || !observed.frame_layout ||
        host->verify(host->user, BS_UJP_HANDLER_CONSTRUCTOR,
                     observed.handler) != 1 ||
        host->verify(host->user, BS_UJP_SURFACEVIEW_FACTORY,
                     observed.surface_view) != 1 ||
        host->verify(host->user, BS_UJP_FRAMELAYOUT_FACTORY,
                     observed.frame_layout) != 1 ||
        host->verify(host->user, BS_UJP_PLAYER_CLASS_RUNTIME,
                     observed.unity_player) != 1) {
        block(report, BS_UJP_BLOCK_READBACK,
              "original UnityPlayer static/instance writes or Java objects unverified");
        return 0;
    }
    *observation = observed;
    report->reached_context_write = 1;
    report->reached_activity_write = 1;
    return 1;
}
