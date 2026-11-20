/* Verified original Quest 1.46 Unity Java Activity preinitialization contract.
 * This module does not emulate Java bytecode or write UnityPlayer static fields.
 * An independently implemented Java/DEX executor must execute the ORIGINAL
 * bytecode and provide readback before its checkpoint can be accepted.
 */
#ifndef BS_UNITY_JAVA_PREINIT_H
#define BS_UNITY_JAVA_PREINIT_H

#include <stddef.h>
#include <stdint.h>

#define BS_UNITY_JAVA_DEX_SIZE UINT32_C(135120) /* Quest 1.40.8 */
#define BS_UNITY_JAVA_DEX_CRC32 UINT32_C(0x817442b4) /* Quest 1.40.8 */

/* Offsets are absolute offsets into the original classes.dex. DEX PCs are
 * counted in 16-bit code units from code_item + 16, not byte addresses. */
#define BS_UJP_GAME_ONCREATE_CODE UINT32_C(0x22edac)
#define BS_UJP_GAME_SURFACE_CODE UINT32_C(0x237964)
#define BS_UJP_GAME_PLAYER_CTOR_CODE UINT32_C(0x237488)
#define BS_UJP_UNITY_PLAYER_CTOR_CODE UINT32_C(0x238ac8)
#define BS_UJP_UNITY_CONTEXT_WRITE_PC UINT32_C(0x90)
#define BS_UJP_UNITY_ACTIVITY_WRITE_PC UINT32_C(0x9a)
#define BS_UJP_GAME_NATIVE_INIT_PC UINT32_C(0xec)

/* Each prerequisite is independently checked by a genuine Java host. The
 * first 14 accept an actual object/value in BsUnityJavaPreinitHost. The rest
 * are verified executable services required to interpret the constructor.
 * An absent service must return zero, never a placeholder success. */
typedef enum BsUnityJavaPreinitRequirement {
    BS_UJP_CONTEXT = 0,
    BS_UJP_ACTIVITY,
    BS_UJP_RESOURCES,
    BS_UJP_CONFIGURATION,
    BS_UJP_ASSET_MANAGER,
    BS_UJP_APPLICATION_INFO,
    BS_UJP_APP_METADATA,
    BS_UJP_NATIVE_LIBRARY_DIR,
    BS_UJP_INTENT,
    BS_UJP_PACKAGE_MANAGER,
    BS_UJP_CLASS_LOADER,
    BS_UJP_WINDOW,
    BS_UJP_NATIVE_SURFACE,
    BS_UJP_UI_THREAD,
    BS_UJP_VM_ATTACHED,
    BS_UJP_SURFACEVIEW_FACTORY,
    BS_UJP_FRAMELAYOUT_FACTORY,
    BS_UJP_HANDLER_CONSTRUCTOR,
    BS_UJP_THREAD_RUNTIME,
    BS_UJP_DEFAULT_EXCEPTION_HANDLER,
    BS_UJP_NATIVE_LIBRARY_LOADER,
    BS_UJP_UNITY_JNI_LOADER,
    BS_UJP_VIEW_CALLBACK_REGISTRATION,
    BS_UJP_NATIVE_HOST_BINDING,
    BS_UJP_PLAYER_CLASS_RUNTIME,
    BS_UJP_REQUIREMENT_COUNT
} BsUnityJavaPreinitRequirement;

#define BS_UJP_BLOCK_DEX (UINT64_C(1) << 48)
#define BS_UJP_BLOCK_EXECUTOR (UINT64_C(1) << 49)
#define BS_UJP_BLOCK_CHECKPOINT (UINT64_C(1) << 50)
#define BS_UJP_BLOCK_READBACK (UINT64_C(1) << 51)
#define BS_UJP_BLOCK_CONTEXT_IDENTITY (UINT64_C(1) << 52)

typedef struct BsUnityJavaPreinitHost {
    void *context;              /* actual UnityPlayerGameActivity instance */
    void *activity;             /* identical to context, subclass of Activity */
    void *resources;            /* live android.content.res.Resources */
    void *configuration;        /* Resources.getConfiguration() */
    void *asset_manager;        /* Activity.getAssets() */
    void *application_info;     /* Context.getApplicationInfo() */
    void *app_metadata;         /* ApplicationInfo.metaData Bundle */
    const char *native_library_dir; /* ApplicationInfo.nativeLibraryDir */
    void *intent;               /* Activity.getIntent() */
    void *package_manager;      /* Activity.getPackageManager() */
    void *class_loader;         /* Activity.getClassLoader() */
    void *window;               /* observed Window/SurfaceView factory owner */
    void *native_surface;       /* live host verified native Surface attachment */
    void *ui_thread;            /* Thread.currentThread(), attached to JNI */
    void *user;
    /* Return exactly 1 if the actual Java VM has validated this object or
     * provided this service, 0 if absent, and a negative value on failure.
     * For BS_UJP_ACTIVITY require UnityPlayerGameActivity subclass identity;
     * for BS_UJP_NATIVE_SURFACE require the live NWindow-backed Java surface.
     * For BS_UJP_APP_METADATA read the real Bundle including the
     * `unity.run-without-focus` key; for BS_UJP_NATIVE_LIBRARY_DIR verify
     * ApplicationInfo.nativeLibraryDir, without guessing its contents.
     * For *_LOADER verify original System.loadLibrary/Unity native loading.
     * Only the first 14 requirements receive non-NULL values. */
    int (*verify)(void *user, BsUnityJavaPreinitRequirement requirement,
                  const void *observed);
} BsUnityJavaPreinitHost;

typedef struct BsUnityJavaPreinitReport {
    uint64_t blockers;             /* requirement i uses bit 1ULL << i */
    uint32_t dex_crc32;
    uint32_t verified_code_anchors;
    uint32_t expected_code_anchors;
    const char *first_blocker;    /* stable static string */
    int reached_context_write;
    int reached_activity_write;
} BsUnityJavaPreinitReport;

/* This callback must use a real interpreter/runtime for the verified DEX.
 * It invokes UnityPlayerGameActivity.onCreate(Bundle) and suspends its Java
 * stack immediately AFTER the nested UnityPlayer.<init> bytecode at PC 0x9a,
 * before getRequestedOrientation/UnityPlayer.initialize and any graphics.
 * The executor must preserve its continuation for later Java lifecycle.
 * Returning 1 means the original opcode was executed, not merely decoded.
 * The bridge cannot independently trust callbacks that fabricate execution.
 */
typedef struct BsUnityJavaPreinitObservation BsUnityJavaPreinitObservation;

typedef struct BsUnityJavaPreinitExecutor {
    void *user;
    int (*execute_to_checkpoint)(void *user, const uint8_t *dex, size_t size,
                                  uint32_t oncreate_code_offset,
                                  uint32_t constructor_code_offset,
                                  uint32_t checkpoint_dex_pc,
                                  void *activity);
    /* Inspect the *actual suspended Java frame* and static field table.
     * This is a separate readback of interpreter state, not a requested
     * output value synthesized by execute_to_checkpoint. */
    int (*readback)(void *user, BsUnityJavaPreinitObservation *out);
} BsUnityJavaPreinitExecutor;

typedef struct BsUnityJavaPreinitObservation {
    uint32_t last_code_offset;
    uint32_t last_dex_pc;
    void *unity_player;         /* live UnityPlayerForGameActivity receiver */
    void *unity_player_context; /* instance field mContext */
    void *unity_player_activity;/* instance field mActivity */
    void *static_current_context; /* UnityPlayer.currentContext */
    void *static_current_activity;/* UnityPlayer.currentActivity */
    void *handler;              /* constructed at UnityPlayer.<init> PC 0x05 */
    void *ui_thread;            /* captured at UnityPlayer.<init> PC 0x4e */
    void *frame_layout;         /* original FrameLayout from onCreateSurfaceView */
    void *surface_view;         /* original InputEnabledSurfaceView */
    int game_class_initialized; /* original GameActivity .<clinit> ran */
    int unity_class_initialized;/* original UnityPlayer .<clinit> ran */
    int game_native_library_loaded; /* original System.loadLibrary("game") */
    int original_activity_instanceof; /* original PC 0x92 took Activity branch */
    int java_exception_pending;
} BsUnityJavaPreinitObservation;

/* Read-only. Requires exact original DEX identity and instruction anchors.
 * A result of zero means an APK/DEX mismatch; it never initializes Java. */
int bs_unity_java_preinit_verify_dex(const uint8_t *dex, size_t length,
                                     BsUnityJavaPreinitReport *report);

/* Strict prerequisite inspection; verification callbacks must query actual
 * runtime state. Each missing requirement sets a distinct report bit. */
int bs_unity_java_preinit_check_host(const BsUnityJavaPreinitHost *host,
                                     BsUnityJavaPreinitReport *report);

/* Executes original bytecode ONLY through a supplied verified Java runtime,
 * checks its stopped PC and independently reads the original static fields.
 * Never calls bs_jni_unityplayer_observe_current_activity/context itself:
 * only the owner of the real Java field-write interpreter hook may publish
 * those observations to the JNI bridge. This is a prerequisite/checkpoint
 * result, NOT proof that all constructors, onCreate or NativeCode finished. */
int bs_unity_java_preinit_run(const uint8_t *dex, size_t length,
                              const BsUnityJavaPreinitHost *host,
                              const BsUnityJavaPreinitExecutor *executor,
                              BsUnityJavaPreinitObservation *observation,
                              BsUnityJavaPreinitReport *report);

#endif
