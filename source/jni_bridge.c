/*
 * Unity 6000.3.19f1 Android JNI bootstrap, for ORIGINAL Quest ELF libraries.
 * No Java bytecode, Unity calls, FMOD emulation, or game-specific method hooks.
 *
 * JNI 1.6 C ABI: (JNIEnv*) -> (JNINativeInterface*) -> indexed callbacks;
 * likewise JavaVM* -> JNIInvokeInterface*. Offsets follow NDK jni.h.
 *
 * Copyright (c) 2026 Beat Saber Quest Switch port contributors.
 * MIT license; see ../LICENSE.
 */
#include "jni_bridge.h"

#include <inttypes.h>
#include <float.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SWITCH__)
#include <switch/display/native_window.h>
#endif

#define BS_JNI_ENV_SLOTS 233 /* JNI 1.6: slot 232 = GetObjectRefType. */
#define BS_JNI_VM_SLOTS    8 /* JNI 1.6: slot 7 = AttachCurrentThreadAsDaemon. */
#define BS_JNI_MAX_METHOD_IDS 128
#define BS_JNI_MAX_OBJECTS    2048
#define BS_JNI_MAX_REFS       8192
#define BS_JNI_STRING_BYTES   512
#define BS_JNI_OBSERVED_FIELDS_MAX 32
#define BS_JNI_OBSERVED_LOCALES_MAX 8
#define BS_JNI_LOCALE_TAG_BYTES 48
#define BS_JNI_MAGIC_CLASS UINT32_C(0x4a434c53)
#define BS_JNI_MAGIC_OBJECT UINT32_C(0x4a4f424a)

/*
 * AAPCS64 uses 64-bit code pointers and data pointers. The JNI interface is
 * a contiguous table of pointers; each slot stores the code pointer of the
 * corresponding C ABI callback. The public bridge intentionally does not
 * define JNIEnv/JavaVM names so it can coexist with NDK jni.h in callers.
 */
typedef void (*BSJniGenericFunction)(void);
_Static_assert(sizeof(BSJniGenericFunction) == sizeof(void *),
               "JNI table requires equally sized code/data pointers");
_Static_assert(sizeof(uintptr_t) == sizeof(void *),
               "JNI native function address must fit uintptr_t");

/* All tables and stable handles have process lifetime; never hand out stack refs. */
static BSJniGenericFunction g_env_functions[BS_JNI_ENV_SLOTS];
static BSJniGenericFunction g_vm_functions[BS_JNI_VM_SLOTS];
/* JNI specifies JNIEnv* is thread-specific even when all threads share the
 * same function table. The outer pointer lives in TLS, never in a global
 * singleton shared by native Bionic worker threads. */
static _Thread_local const BSJniGenericFunction *g_thread_env_table;
static const BSJniGenericFunction *g_vm_table = g_vm_functions;
static atomic_int g_initialized;
static BSJNILogFn g_logger;
static void *g_logger_user;
static _Thread_local char g_last_error[256];
static BSJNINativeRegistration g_native_methods[BS_JNI_MAX_NATIVES];
static atomic_size_t g_native_count;

/* JNI environment attachment belongs to a thread, not to a JavaVM globally. */
static _Thread_local int g_attached;
static _Thread_local uint32_t g_attach_generation;

/* Class, object, method and explicit-reference pools are shared by attached
 * JNI threads. Publish initialized entries only after allocation completes.
 * Never invoke guest code or a logger while holding this short pool lock. */
static atomic_flag g_registry_lock = ATOMIC_FLAG_INIT;
static void bs_jni_lock(void) {
    while (atomic_flag_test_and_set_explicit(
        &g_registry_lock, memory_order_acquire)) {}
}
static void bs_jni_unlock(void) {
    atomic_flag_clear_explicit(&g_registry_lock, memory_order_release);
}

typedef struct BSJNIClass {
    uint32_t magic;
    char name[BS_JNI_CLASS_BYTES];
} BSJNIClass;

typedef enum BSJNIObjectKind {
    BS_JNI_OBJECT_ACTIVITY = 1,
    BS_JNI_OBJECT_ASSET_MANAGER,
    BS_JNI_OBJECT_STRING,
    BS_JNI_OBJECT_NO_SUCH_FIELD,
    BS_JNI_OBJECT_NO_SUCH_METHOD,
    BS_JNI_OBJECT_NO_CLASS_DEF,
    BS_JNI_OBJECT_UNSUPPORTED_ERROR,
    BS_JNI_OBJECT_INDEX_ERROR,
    BS_JNI_OBJECT_CONFIGURATION,
    BS_JNI_OBJECT_INSETS,
    BS_JNI_OBJECT_LOCALE_LIST,
    BS_JNI_OBJECT_LOCALE,
    BS_JNI_OBJECT_SURFACE,
    BS_JNI_OBJECT_THROWABLE,
    BS_JNI_OBJECT_GAME_ACTIVITY_HOST,
    BS_JNI_OBJECT_MOTION_EVENT,
    BS_JNI_OBJECT_KEY_EVENT,
    BS_JNI_OBJECT_UNITY_PLAYER_HOST,
    BS_JNI_OBJECT_CLASS_LOADER,  /* the APK's dalvik.system.PathClassLoader */
    BS_JNI_OBJECT_HOST_INSTANCE, /* app class instance; text = class name */
    BS_JNI_OBJECT_FILE,          /* java.io.File; text = path */
    BS_JNI_OBJECT_ARRAY,         /* Java array; text = array class name */
    BS_JNI_OBJECT_REFLECTED      /* java.lang.reflect.Method/Constructor/Field;
                                  * text = declaring class */
} BSJNIObjectKind;

struct BSJNIKnownMember;
typedef struct BSJNIObservedCell {
    const struct BSJNIKnownMember *member;
    int32_t integer_value;
    float float_value;
} BSJNIObservedCell;

typedef struct BSJNILocaleTags {
    char language[BS_JNI_LOCALE_TAG_BYTES];
    char script[BS_JNI_LOCALE_TAG_BYTES];
    char country[BS_JNI_LOCALE_TAG_BYTES];
    char variant[BS_JNI_LOCALE_TAG_BYTES];
} BSJNILocaleTags;

typedef struct BSJNIObject {
    uint32_t magic;
    BSJNIObjectKind kind;
    char text[BS_JNI_STRING_BYTES];
    size_t observed_count;
    size_t configuration_default_count;
    BSJNIObservedCell observed[BS_JNI_OBSERVED_FIELDS_MAX];
    struct BSJNIObject *locale_list; /* Configuration only */
    size_t locale_count;             /* LocaleList only */
    struct BSJNIObject *locale_items[BS_JNI_OBSERVED_LOCALES_MAX];
    BSJNILocaleTags locale;          /* Locale only */
    void *native_window;             /* Verified Surface only */
    char throwable_class[BS_JNI_CLASS_BYTES];
    BSJNIGameActivityHostServices gameactivity_host;
    BSJNIObservedMotionEvent *motion_event; /* host-observed, never guessed */
    BSJNIObservedKeyEvent *key_event;
    /* Arrays: element storage, length, JNI element type letter. */
    void *array_data;
    int32_t array_length;
    char array_type;
    /* Reflected members: 'M' method, 'C' constructor, 'F' field. */
    char member_kind;
    uint8_t member_static;
    char member_name[128];
    char member_sig[256];
    /* Lifetime of transient objects (strings, arrays, files): live
     * reference records targeting it, whether the raw local handle was
     * deleted, and whether host state (array element, static field) keeps
     * it alive for the rest of the process. */
    uint8_t pinned;
    uint8_t raw_released;
    uint32_t refs;
    const void *local_owner; /* creating thread's JNIEnv identity */
    uint32_t local_frame;    /* its local frame depth at creation */
} BSJNIObject;

typedef enum BSJNIMethodKind {
    BS_JNI_METHOD_ASSETS = 1,
    BS_JNI_METHOD_CONTEXT,
    BS_JNI_METHOD_PACKAGE
} BSJNIMethodKind;

typedef struct BSJNIMethodID {
    uint32_t magic;
    BSJNIMethodKind method;
    char class_name[BS_JNI_CLASS_BYTES];
} BSJNIMethodID;

static BSJNIClass g_classes[BS_JNI_MAX_CLASSES];
static atomic_size_t g_class_count;
static BSJNIObject g_objects[BS_JNI_MAX_OBJECTS];
static atomic_size_t g_object_count;
static BSJNIMethodID g_methods[BS_JNI_MAX_METHOD_IDS];
static atomic_size_t g_method_count;

/* Stable symbolic targets are NOT themselves proof of a Java VM. Explicit
 * NewGlobalRef/NewLocalRef/NewWeakGlobalRef create separately typed handles,
 * rather than propagating arbitrary untrusted pointers as successful refs. */
typedef struct BSJNIReference {
    const void *target;
    const void *local_env_owner; /* TLS JNIEnv identity; NULL for globals */
    uint32_t local_generation; /* Prevent reattachment reviving local refs */
    uint8_t type; /* JNI 1=local, 2=global, 3=weak global */
    atomic_uchar active;
    uint32_t frame; /* creating thread's local frame depth (locals only) */
} BSJNIReference;

static BSJNIReference g_references[BS_JNI_MAX_REFS];
static atomic_size_t g_reference_count; /* high-water mark */
static uint32_t g_reference_free[BS_JNI_MAX_REFS];
static size_t g_reference_free_count;
/* FIFO: released slots are reused last, so a stale local handle stays
 * invalid as long as possible instead of aliasing the next new ref. */
static size_t g_reference_free_head;
static uint32_t g_object_free[BS_JNI_MAX_OBJECTS];
static size_t g_object_free_count;
/* Per-thread JNI local frame depth (PushLocalFrame/PopLocalFrame). */
static _Thread_local uint32_t g_local_frame_count;

/*
 * libgame.so (Quest 1.46, Unity 6000.3.19f1) GameActivity_register at
 * ELF VA 0x1bd2c calls GetMethodID (slot 33), GetFieldID (slot 94), and
 * GetStaticMethodID (slot 113). Its GameActivityEventsInit at 0x1fc88
 * additionally resolves Android MotionEvent/KeyEvent methods.
 *
 * These are exact name/signature/class *metadata* from that original ARM64
 * code. An ID does not imply that a Java object exists or a method can run.
 * Invocation is limited to measured value objects and explicitly installed
 * native GameActivity host callbacks. JNI registration records native pointers.
 */
typedef enum BSJNIMemberType {
    BS_JNI_INSTANCE_METHOD,
    BS_JNI_STATIC_METHOD,
    BS_JNI_INSTANCE_FIELD,
    BS_JNI_STATIC_FIELD
} BSJNIMemberType;

typedef struct BSJNIKnownMember {
    const char *class_name;
    const char *name;
    const char *signature;
    BSJNIMemberType type;
} BSJNIKnownMember;

#define BS_GAME_ACTIVITY "com/google/androidgamesdk/GameActivity"
#define BS_INSETS "androidx/core/graphics/Insets"
#define BS_CONFIG "android/content/res/Configuration"
#define BS_LOCALE_LIST "android/os/LocaleList"
#define BS_LOCALE "java/util/Locale"
#define BS_INSET_TYPE "androidx/core/view/WindowInsetsCompat$Type"
#define BS_MOTION "android/view/MotionEvent"
#define BS_KEY "android/view/KeyEvent"
#define BS_UNITY_PLAYER "com/unity3d/player/UnityPlayer"
#define BS_UNITY_PLAYER_ACTIVITY "com/unity3d/player/UnityPlayerForActivityOrService"
#define BS_UNITY_PLAYER_GAME_ACTIVITY "com/unity3d/player/UnityPlayerForGameActivity"
#define BS_IM(C, N, S) { C, N, S, BS_JNI_INSTANCE_METHOD }
#define BS_SM(C, N, S) { C, N, S, BS_JNI_STATIC_METHOD }
#define BS_IF(C, N, S) { C, N, S, BS_JNI_INSTANCE_FIELD }
#define BS_SF(C, N, S) { C, N, S, BS_JNI_STATIC_FIELD }

/* Verified against classes.dex from the original Quest APK. The subclass
 * inherits these public static fields from UnityPlayer; neither FindClass
 * nor a field ID establishes that Unity's Java constructor has assigned one. */
static const BSJNIKnownMember g_unityplayer_static_fields[] = {
    BS_SF(BS_UNITY_PLAYER, "currentActivity", "Landroid/app/Activity;"),
    BS_SF(BS_UNITY_PLAYER, "currentContext", "Landroid/content/Context;")
};

static const BSJNIKnownMember g_gameactivity_members[] = {
    BS_IM(BS_GAME_ACTIVITY, "finish", "()V"),
    BS_IM(BS_GAME_ACTIVITY, "setWindowFlags", "(II)V"),
    BS_IM(BS_GAME_ACTIVITY, "getWindowInsets",
          "(I)Landroidx/core/graphics/Insets;"),
    BS_IM(BS_GAME_ACTIVITY, "getWaterfallInsets",
          "()Landroidx/core/graphics/Insets;"),
    BS_IM(BS_GAME_ACTIVITY, "setImeEditorInfoFields", "(III)V"),

    BS_IF(BS_INSETS, "left", "I"),
    BS_IF(BS_INSETS, "right", "I"),
    BS_IF(BS_INSETS, "top", "I"),
    BS_IF(BS_INSETS, "bottom", "I"),

    BS_IF(BS_CONFIG, "colorMode", "I"),
    BS_IF(BS_CONFIG, "densityDpi", "I"),
    BS_IF(BS_CONFIG, "fontScale", "F"),
    BS_IF(BS_CONFIG, "fontWeightAdjustment", "I"),
    BS_IF(BS_CONFIG, "hardKeyboardHidden", "I"),
    BS_IF(BS_CONFIG, "keyboard", "I"),
    BS_IF(BS_CONFIG, "keyboardHidden", "I"),
    BS_IF(BS_CONFIG, "mcc", "I"),
    BS_IF(BS_CONFIG, "mnc", "I"),
    BS_IF(BS_CONFIG, "navigation", "I"),
    BS_IF(BS_CONFIG, "navigationHidden", "I"),
    BS_IF(BS_CONFIG, "orientation", "I"),
    BS_IF(BS_CONFIG, "screenHeightDp", "I"),
    BS_IF(BS_CONFIG, "screenLayout", "I"),
    BS_IF(BS_CONFIG, "screenWidthDp", "I"),
    BS_IF(BS_CONFIG, "smallestScreenWidthDp", "I"),
    BS_IF(BS_CONFIG, "touchscreen", "I"),
    BS_IF(BS_CONFIG, "uiMode", "I"),
    BS_IM(BS_CONFIG, "getLocales", "()Landroid/os/LocaleList;"),

    BS_IM(BS_LOCALE_LIST, "size", "()I"),
    BS_IM(BS_LOCALE_LIST, "get", "(I)Ljava/util/Locale;"),
    BS_IM(BS_LOCALE, "getLanguage", "()Ljava/lang/String;"),
    BS_IM(BS_LOCALE, "getScript", "()Ljava/lang/String;"),
    BS_IM(BS_LOCALE, "getCountry", "()Ljava/lang/String;"),
    BS_IM(BS_LOCALE, "getVariant", "()Ljava/lang/String;"),

    BS_SM(BS_INSET_TYPE, "captionBar", "()I"),
    BS_SM(BS_INSET_TYPE, "displayCutout", "()I"),
    BS_SM(BS_INSET_TYPE, "ime", "()I"),
    BS_SM(BS_INSET_TYPE, "mandatorySystemGestures", "()I"),
    BS_SM(BS_INSET_TYPE, "navigationBars", "()I"),
    BS_SM(BS_INSET_TYPE, "statusBars", "()I"),
    BS_SM(BS_INSET_TYPE, "systemBars", "()I"),
    BS_SM(BS_INSET_TYPE, "systemGestures", "()I"),
    BS_SM(BS_INSET_TYPE, "tappableElement", "()I"),

    BS_IM(BS_MOTION, "getDeviceId", "()I"),
    BS_IM(BS_MOTION, "getSource", "()I"),
    BS_IM(BS_MOTION, "getAction", "()I"),
    BS_IM(BS_MOTION, "getEventTime", "()J"),
    BS_IM(BS_MOTION, "getDownTime", "()J"),
    BS_IM(BS_MOTION, "getFlags", "()I"),
    BS_IM(BS_MOTION, "getMetaState", "()I"),
    BS_IM(BS_MOTION, "getActionButton", "()I"),
    BS_IM(BS_MOTION, "getButtonState", "()I"),
    BS_IM(BS_MOTION, "getClassification", "()I"),
    BS_IM(BS_MOTION, "getEdgeFlags", "()I"),
    BS_IM(BS_MOTION, "getHistorySize", "()I"),
    BS_IM(BS_MOTION, "getHistoricalEventTime", "(I)J"),
    BS_IM(BS_MOTION, "getPointerCount", "()I"),
    BS_IM(BS_MOTION, "getPointerId", "(I)I"),
    BS_IM(BS_MOTION, "getToolType", "(I)I"),
    BS_IM(BS_MOTION, "getRawX", "(I)F"),
    BS_IM(BS_MOTION, "getRawY", "(I)F"),
    BS_IM(BS_MOTION, "getXPrecision", "()F"),
    BS_IM(BS_MOTION, "getYPrecision", "()F"),
    BS_IM(BS_MOTION, "getAxisValue", "(II)F"),
    BS_IM(BS_MOTION, "getHistoricalAxisValue", "(III)F"),

    BS_IM(BS_KEY, "getDeviceId", "()I"),
    BS_IM(BS_KEY, "getSource", "()I"),
    BS_IM(BS_KEY, "getAction", "()I"),
    BS_IM(BS_KEY, "getEventTime", "()J"),
    BS_IM(BS_KEY, "getDownTime", "()J"),
    BS_IM(BS_KEY, "getFlags", "()I"),
    BS_IM(BS_KEY, "getMetaState", "()I"),
    BS_IM(BS_KEY, "getModifiers", "()I"),
    BS_IM(BS_KEY, "getRepeatCount", "()I"),
    BS_IM(BS_KEY, "getKeyCode", "()I"),
    BS_IM(BS_KEY, "getScanCode", "()I"),
    BS_IM(BS_KEY, "getUnicodeChar", "()I")
};

/* Resolved by the original GameTextInput constructor at libgame+0x22500.
 * Its connection is initially null; these IDs only describe the state and
 * connection classes until a host supplies a functioning input connection. */
#define BS_TEXT_STATE "com/google/androidgamesdk/gametextinput/State"
#define BS_TEXT_CONNECTION "com/google/androidgamesdk/gametextinput/InputConnection"
static const BSJNIKnownMember g_gametextinput_members[] = {
    BS_IM(BS_TEXT_CONNECTION, "setState", "(L" BS_TEXT_STATE ";)V"),
    BS_IM(BS_TEXT_CONNECTION, "setSoftKeyboardActive", "(ZI)V"),
    BS_IM(BS_TEXT_CONNECTION, "restartInput", "()V"),
    BS_IF(BS_TEXT_STATE, "text", "Ljava/lang/String;"),
    BS_IF(BS_TEXT_STATE, "selectionStart", "I"),
    BS_IF(BS_TEXT_STATE, "selectionEnd", "I"),
    BS_IF(BS_TEXT_STATE, "composingRegionStart", "I"),
    BS_IF(BS_TEXT_STATE, "composingRegionEnd", "I"),
};

#undef BS_IM
#undef BS_SM
#undef BS_IF

/* The actual 24 JNINativeMethod name/signature records at libgame.so ELF
 * VA 0x2b840, reached by GameActivity_register at 0x1bd2c. The original
 * function addresses are relocated by the loader; this table validates ONLY
 * Java-side method declarations and never supplies an executable function.
 * See isolated tests/jni_gameactivity_metadata_selftest.c for the independently
 * extracted 24 function vaddrs, including the text-input and surface methods.
 */
typedef struct BSJNIKnownNative {
    const char *name;
    const char *signature;
} BSJNIKnownNative;

static const BSJNIKnownNative g_gameactivity_natives[] = {
    {"initializeNativeCode",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J"},
    {"getDlError", "()Ljava/lang/String;"},
    {"terminateNativeCode", "(J)V"},
    {"onStartNative", "(J)V"},
    {"onResumeNative", "(J)V"},
    {"onSaveInstanceStateNative", "(J)[B"},
    {"onPauseNative", "(J)V"},
    {"onStopNative", "(J)V"},
    {"onConfigurationChangedNative", "(JLandroid/content/res/Configuration;)V"},
    {"onTrimMemoryNative", "(JI)V"},
    {"onWindowFocusChangedNative", "(JZ)V"},
    {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V"},
    {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V"},
    {"onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V"},
    {"onSurfaceDestroyedNative", "(J)V"},
    {"onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z"},
    {"onKeyDownNative", "(JLandroid/view/KeyEvent;)Z"},
    {"onKeyUpNative", "(JLandroid/view/KeyEvent;)Z"},
    {"onTextInputEventNative",
     "(JLcom/google/androidgamesdk/gametextinput/State;)V"},
    {"onWindowInsetsChangedNative", "(J)V"},
    {"setInputConnectionNative",
     "(JLcom/google/androidgamesdk/gametextinput/InputConnection;)V"},
    {"onContentRectChangedNative", "(JIIII)V"},
    {"onSoftwareKeyboardVisibilityChangedNative", "(JZ)V"},
    {"onEditorActionNative", "(JI)V"}
};

#define BS_JNI_GAMEACTIVITY_NATIVE_COUNT (sizeof g_gameactivity_natives / sizeof g_gameactivity_natives[0])
_Static_assert(BS_JNI_GAMEACTIVITY_NATIVE_COUNT == 24,
               "original GameActivity JNINativeMethod count changed");
_Static_assert(sizeof g_gameactivity_members / sizeof g_gameactivity_members[0] == 77,
               "original GameActivity method/field lookup inventory changed");

static int bs_jni_known_gameactivity_native(const char *name,
                                           const char *signature) {
    if (!name || !signature) return 0;
    for (size_t i = 0; i < BS_JNI_GAMEACTIVITY_NATIVE_COUNT; ++i) {
        if (!strcmp(name, g_gameactivity_natives[i].name) &&
            !strcmp(signature, g_gameactivity_natives[i].signature))
            return 1;
    }
    return 0;
}

/* Per-thread diagnostics for lookup failure; there is no Java VM/throw stack.
 * A stable synthetic throwable represents only a pending NoSuch*Error. */
static _Thread_local BSJNIObject g_missing_member_exception;
static _Thread_local int g_missing_member_pending;
/* ExceptionOccurred must return a durable Java object: unlike the thread's
 * diagnostic scratch buffer, this identity survives ExceptionClear and can
 * be held by a global reference on another attached thread. */
static _Thread_local BSJNIObject *g_live_exception;
static BSJNIObject *bs_jni_object_new(BSJNIObjectKind kind, const char *text);
static void *bs_jni_new_reference(void *env, void *obj, uint8_t kind);

static const BSJNIKnownMember *bs_jni_lookup_gameactivity_member(
    const char *class_name, const char *name, const char *signature,
    BSJNIMemberType type) {
    if (!class_name || !name || !signature)
        return NULL;
    for (size_t i = 0;
         i < sizeof g_gameactivity_members / sizeof g_gameactivity_members[0];
         ++i) {
        const BSJNIKnownMember *member = &g_gameactivity_members[i];
        if (member->type == type &&
            !strcmp(member->class_name, class_name) &&
            !strcmp(member->name, name) &&
            !strcmp(member->signature, signature))
            return member;
    }
    for (size_t i = 0; i < sizeof g_gametextinput_members /
                                sizeof g_gametextinput_members[0]; ++i) {
        const BSJNIKnownMember *member = &g_gametextinput_members[i];
        if (member->type == type &&
            !strcmp(member->class_name, class_name) &&
            !strcmp(member->name, name) &&
            !strcmp(member->signature, signature))
            return member;
    }
    return NULL;
}

/*
 * These two JNI handles represent the exact Java objects passed through
 * GameActivity: clazz (Activity) and getAssets() (AssetManager).
 * AAssetManager_fromJava in asset_fs_bridge.c maps the latter to its real
 * native file bridge. The Java handle itself is NOT an AAssetManager*.
 */
static BSJNIObject *g_activity;
static BSJNIObject *g_asset_manager;
/* Activity-thread-owned binding for the original NativeCode launch. */
static BSJNIObject *g_gameactivity_receiver;
static BSJNIObject *g_gameactivity_configuration;
static void *g_gameactivity_surface;
/* Published only after the actual Java constructor's independently observed
 * static field writes; the two writes are separate DEX operations. */
static BSJNIObject *g_unityplayer_current_activity;
static BSJNIObject *g_unityplayer_current_context;

static void bs_jni_log(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (g_logger)
        g_logger(g_logger_user, buffer);
    else
        fprintf(stderr, "%s\n", buffer);
}

static void bs_jni_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_last_error, sizeof g_last_error, format, args);
    va_end(args);
    bs_jni_log("[jni] ERROR: %s", g_last_error);
}

static void bs_jni_no_such_member(const char *kind, const char *class_name,
                                  const char *name, const char *signature) {
    if (g_missing_member_pending) return; /* Preserve first pending exception. */
    const int field = strcmp(kind, "field") == 0;
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = field
        ? BS_JNI_OBJECT_NO_SUCH_FIELD : BS_JNI_OBJECT_NO_SUCH_METHOD;
    snprintf(g_missing_member_exception.text,
             sizeof g_missing_member_exception.text,
             "NoSuch%sError: %s.%s %s",
             field ? "Field" : "Method",
             class_name ? class_name : "(invalid class)",
             name ? name : "(null)", signature ? signature : "(null)");
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_error("%s", g_missing_member_exception.text);
}

static void bs_jni_no_class_def(const char *class_name) {
    if (g_missing_member_pending) return;
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_NO_CLASS_DEF;
    snprintf(g_missing_member_exception.text,
             sizeof g_missing_member_exception.text,
             "NoClassDefFoundError: invalid class name %s",
             class_name ? class_name : "(null)");
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_error("%s", g_missing_member_exception.text);
}

/* Missing native measurements are NOT Java default fields. Propagate a
 * pending failure instead of making an unobserved zero look authoritative. */
/* ---- Lenient Android framework model --------------------------------
 * The bridge runs no Java bytecode. With lenient mode on (the default),
 * Java members the host does not model resolve to stub IDs whose calls
 * return the JNI default (0/false/null; setters are no-ops), and an
 * unbacked JNI operation logs once and returns its default instead of
 * raising a pending Java exception. Strict mode keeps the fail-closed
 * behavior used by the self-tests. */
static atomic_int g_lenient = 1;
#define BS_JNI_LENIENT_SEEN 4096
static uint32_t g_lenient_seen[BS_JNI_LENIENT_SEEN];

void bs_jni_set_lenient(int enabled) { g_lenient = enabled ? 1 : 0; }
int bs_jni_lenient(void) { return g_lenient; }

static uint32_t bs_jni_hash_str(uint32_t h, const char *s) {
    for (; s && *s; ++s) h = (h ^ (unsigned char)*s) * UINT32_C(16777619);
    return (h ^ 0x2f) * UINT32_C(16777619);
}

/* 1 the first time this (a, b, c, d) tuple is seen. Lock-free open
 * addressing; a full table just logs again. */
static int bs_jni_lenient_first(const char *a, const char *b, const char *c,
                                const char *d) {
    uint32_t h = UINT32_C(2166136261);
    h = bs_jni_hash_str(h, a);
    h = bs_jni_hash_str(h, b);
    h = bs_jni_hash_str(h, c);
    h = bs_jni_hash_str(h, d);
    if (!h) h = 1;
    for (uint32_t probe = 0; probe < 64; ++probe) {
        _Atomic uint32_t *slot = (_Atomic uint32_t *)
            &g_lenient_seen[(h + probe) % BS_JNI_LENIENT_SEEN];
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong(slot, &expected, h)) return 1;
        if (expected == h) return 0;
    }
    return 1;
}

static void bs_jni_log(const char *format, ...);
static void bs_jni_lenient_note(const char *what, const char *class_name,
                                const char *member, const char *signature) {
    if (bs_jni_lenient_first(what, class_name, member, signature))
        bs_jni_log("[jni:lenient] %s %s.%s%s -> default",
                   what ? what : "JNI", class_name ? class_name : "?",
                   member ? member : "?", signature ? signature : "");
}

static void bs_jni_missing_host_value(const char *operation,
                                       const char *class_name,
                                       const char *member) {
    if (g_lenient) {
        bs_jni_lenient_note(operation, class_name, member, NULL);
        return;
    }
    if (g_missing_member_pending) return;
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_UNSUPPORTED_ERROR;
    snprintf(g_missing_member_exception.text,
             sizeof g_missing_member_exception.text,
             "UnsupportedOperationException: %s %s.%s requires observed "
             "Android host state",
             operation ? operation : "JNI",
             class_name ? class_name : "(unknown)",
             member ? member : "(unknown)");
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_error("%s", g_missing_member_exception.text);
}

/* An attached thread's JNIEnv address is thread-local, even though all
 * threads share the immutable JNI function table. Comparing by address
 * catches accidental use of another thread's JNIEnv or detached pointers
 * without dereferencing an untrusted pointer. */
static int bs_jni_env_is_current(const void *env) {
    return g_initialized && g_attached &&
           g_thread_env_table == g_env_functions &&
           env == (const void *)&g_thread_env_table;
}

static int bs_jni_require_env(void *env, const char *operation) {
    if (bs_jni_env_is_current(env)) return 1;
    bs_jni_error("%s requires the calling thread's attached JNIEnv",
                 operation);
    return 0;
}

static void bs_jni_locale_bad_index(int32_t index, size_t count) {
    if (g_missing_member_pending) return;
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_INDEX_ERROR;
    snprintf(g_missing_member_exception.text,
             sizeof g_missing_member_exception.text,
             "IndexOutOfBoundsException: LocaleList.get(%d) size=%lu",
             index, (unsigned long)count);
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_error("%s", g_missing_member_exception.text);
}

/*
 * Every unimplemented standard JNI function points here, rather than NULL
 * or a fabricated success stub. noreturn lets a caller with ANY JNI return
 * type fail without trying to synthesize integer/FP/object return registers.
 *
 * The callback has no arguments and does not inspect the caller's registers.
 * When reached via a JNI slot, AAPCS64 enters it and aborts deterministically.
 * A more complete bridge must replace the individual slot before engine boot.
 */
static atomic_uchar g_unsupported_seen[256];
static void bs_jni_unsupported_slot(const char *table, unsigned slot,
                                    void *origin) {
    if (!g_lenient) {
        bs_jni_error("unsupported JNI operation %s slot %u from %p "
                     "(guest bootstrap stopped)", table, slot, origin);
        abort();
    }
    if (!atomic_exchange(&g_unsupported_seen[slot & 255u], 1))
        bs_jni_log("[jni:lenient] unimplemented %s slot %u from %p "
                   "returns 0", table, slot, origin);
}

/* The lenient result of an unimplemented slot: zero in both the integer
 * and the FP return register, whatever the JNI function's return type. */
#if defined(__aarch64__)
#define BS_JNI_ZERO_RETURN_REGS() \
    __asm__ volatile("mov x0, xzr\n\tfmov d0, xzr" ::: "x0", "v0")
#else
#define BS_JNI_ZERO_RETURN_REGS() do {} while (0)
#endif

/* One distinct trap per table slot so a stop names the exact JNI function
 * the original guest code reached, plus its guest return address. */
#define BS_JNI_TRAP(h, t, o) \
    __attribute__((noinline)) static void bs_jni_trap_##h##t##o(void) { \
        bs_jni_unsupported_slot("JNIEnv", (h) * 100u + (t) * 10u + (o), \
                                __builtin_return_address(0)); \
        BS_JNI_ZERO_RETURN_REGS(); \
    }
#define BS_JNI_TRAP10(h, t) \
    BS_JNI_TRAP(h, t, 0) BS_JNI_TRAP(h, t, 1) BS_JNI_TRAP(h, t, 2) \
    BS_JNI_TRAP(h, t, 3) BS_JNI_TRAP(h, t, 4) BS_JNI_TRAP(h, t, 5) \
    BS_JNI_TRAP(h, t, 6) BS_JNI_TRAP(h, t, 7) BS_JNI_TRAP(h, t, 8) \
    BS_JNI_TRAP(h, t, 9)
#define BS_JNI_TRAP100(h) \
    BS_JNI_TRAP10(h, 0) BS_JNI_TRAP10(h, 1) BS_JNI_TRAP10(h, 2) \
    BS_JNI_TRAP10(h, 3) BS_JNI_TRAP10(h, 4) BS_JNI_TRAP10(h, 5) \
    BS_JNI_TRAP10(h, 6) BS_JNI_TRAP10(h, 7) BS_JNI_TRAP10(h, 8) \
    BS_JNI_TRAP10(h, 9)
BS_JNI_TRAP100(0) BS_JNI_TRAP100(1)
BS_JNI_TRAP10(2, 0) BS_JNI_TRAP10(2, 1) BS_JNI_TRAP10(2, 2) BS_JNI_TRAP10(2, 3)
#define BS_JNI_TRAP_REF(h, t, o) bs_jni_trap_##h##t##o,
#define BS_JNI_TRAP_REF10(h, t) \
    BS_JNI_TRAP_REF(h, t, 0) BS_JNI_TRAP_REF(h, t, 1) BS_JNI_TRAP_REF(h, t, 2) \
    BS_JNI_TRAP_REF(h, t, 3) BS_JNI_TRAP_REF(h, t, 4) BS_JNI_TRAP_REF(h, t, 5) \
    BS_JNI_TRAP_REF(h, t, 6) BS_JNI_TRAP_REF(h, t, 7) BS_JNI_TRAP_REF(h, t, 8) \
    BS_JNI_TRAP_REF(h, t, 9)
#define BS_JNI_TRAP_REF100(h) \
    BS_JNI_TRAP_REF10(h, 0) BS_JNI_TRAP_REF10(h, 1) BS_JNI_TRAP_REF10(h, 2) \
    BS_JNI_TRAP_REF10(h, 3) BS_JNI_TRAP_REF10(h, 4) BS_JNI_TRAP_REF10(h, 5) \
    BS_JNI_TRAP_REF10(h, 6) BS_JNI_TRAP_REF10(h, 7) BS_JNI_TRAP_REF10(h, 8) \
    BS_JNI_TRAP_REF10(h, 9)
static void (*const g_jni_traps[240])(void) = {
    BS_JNI_TRAP_REF100(0) BS_JNI_TRAP_REF100(1)
    BS_JNI_TRAP_REF10(2, 0) BS_JNI_TRAP_REF10(2, 1)
    BS_JNI_TRAP_REF10(2, 2) BS_JNI_TRAP_REF10(2, 3)
};
_Static_assert(BS_JNI_ENV_SLOTS <= 240, "JNI trap table too small");

/* Implemented means: the slot holds neither NULL nor its own fail-closed
 * trap. Callers must use this instead of comparing neighbouring slots,
 * because every unsupported slot now has a distinct diagnostic trap. */
int bs_jni_env_slot_implemented(unsigned slot) {
    if (slot < 4 || slot >= BS_JNI_ENV_SLOTS)
        return 0;
    BSJniGenericFunction fn = g_env_functions[slot];
    return fn && fn != (BSJniGenericFunction)g_jni_traps[slot];
}

__attribute__((noinline)) static void bs_jni_vm_unsupported(void) {
    bs_jni_unsupported_slot("JavaVM", 0, __builtin_return_address(0));
    BS_JNI_ZERO_RETURN_REGS();
}

static int bs_jni_copy(char *dst, size_t cap, const char *src) {
    if (!src || !cap)
        return BS_JNI_ERR;
    size_t n = 0;
    while (n < cap && src[n])
        ++n;
    if (!n || n == cap)
        return BS_JNI_ERR;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return BS_JNI_OK;
}

/* JNI method descriptors: "(argument-types)return-type". Do not accept
 * arbitrary text that could later be confused with a registered Java method.
 */
static int bs_jni_consume_type(const char **cursor, int allow_void) {
    const char *p = *cursor;
    int array_depth = 0;
    while (*p == '[') {
        ++array_depth;
        ++p;
        if (array_depth > 255)
            return 0;
    }
    switch (*p) {
        case 'V':
            if (!allow_void || array_depth)
                return 0;
            ++p;
            break;
        case 'B': case 'C': case 'D': case 'F':
        case 'I': case 'J': case 'S': case 'Z':
            ++p;
            break;
        case 'L': {
            const char *begin = ++p;
            while (*p && *p != ';') {
                if (*p == '(' || *p == ')' || *p == '[')
                    return 0;
                ++p;
            }
            if (p == begin || *p != ';')
                return 0;
            ++p;
            break;
        }
        default:
            return 0;
    }
    *cursor = p;
    return 1;
}

static int bs_jni_valid_signature(const char *signature) {
    if (!signature || signature[0] != '(')
        return 0;
    const char *p = signature + 1;
    while (*p && *p != ')')
        if (!bs_jni_consume_type(&p, 0))
            return 0;
    if (*p++ != ')' || !bs_jni_consume_type(&p, 1))
        return 0;
    return *p == '\0';
}

/* O(1) pool membership: `p` must point exactly at an element of `base`
 * below the pool's high-water mark. Never dereferences `p` otherwise. */
static const void *bs_jni_pool_member(const void *p, const void *base,
                                      size_t elem, size_t count) {
    const uintptr_t a = (uintptr_t)p, b = (uintptr_t)base;
    if (a < b || a >= b + count * elem || (a - b) % elem) return NULL;
    return p;
}
static BSJNIReference *bs_jni_reference_record(const void *obj) {
    return (BSJNIReference *)(uintptr_t)bs_jni_pool_member(
        obj, g_references, sizeof g_references[0], g_reference_count);
}
static int bs_jni_reference_live(const BSJNIReference *ref) {
    return ref->active &&
           (ref->type != 1 ||
            (ref->local_env_owner == (const void *)&g_thread_env_table &&
             ref->local_generation == g_attach_generation));
}

static BSJNIClass *bs_jni_known_class(const void *reference) {
    const BSJNIReference *ref = bs_jni_reference_record(reference);
    if (ref) {
        if (!bs_jni_reference_live(ref)) return NULL;
        reference = ref->target;
    }
    return (BSJNIClass *)(uintptr_t)bs_jni_pool_member(
        reference, g_classes, sizeof g_classes[0], g_class_count);
}

static BSJNIObject *bs_jni_known_object(const void *reference) {
    const BSJNIReference *ref = bs_jni_reference_record(reference);
    if (ref) {
        if (!bs_jni_reference_live(ref)) return NULL;
        reference = ref->target;
    }
    /* ExceptionOccurred yields a local ref that survives ExceptionClear. */
    if (g_missing_member_exception.magic == BS_JNI_MAGIC_OBJECT &&
        reference == &g_missing_member_exception)
        return &g_missing_member_exception;
    BSJNIObject *object = (BSJNIObject *)(uintptr_t)bs_jni_pool_member(
        reference, g_objects, sizeof g_objects[0], g_object_count);
    return object && object->magic == BS_JNI_MAGIC_OBJECT ? object : NULL;
}

static BSJNIMethodID *bs_jni_known_method(const void *reference) {
    for (size_t i = 0; i < g_method_count; i++)
        if (reference == &g_methods[i])
            return &g_methods[i];
    return NULL;
}

/* ---- Host-implemented Java methods (see jni_bridge.h) ---------------- */
#define BS_JNI_MAX_HOST_METHODS 1024
typedef struct BSJNIHostMethod {
    uint32_t magic;
    char class_name[BS_JNI_CLASS_BYTES];
    char name[BS_JNI_METHOD_BYTES];
    char signature[BS_JNI_SIGNATURE_BYTES];
    char params[BS_JNI_HOST_MAX_ARGS]; /* one JNI type letter per argument */
    size_t argc;
    char result;                       /* JNI return type letter */
    int is_static;
    BSJNIHostMethodFn fn;
    void *user;
} BSJNIHostMethod;
static BSJNIHostMethod g_host_methods[BS_JNI_MAX_HOST_METHODS];
static size_t g_host_method_count;
#define BS_JNI_MAGIC_HOST_METHOD UINT32_C(0x484d4554)

static int bs_jni_host_parse_signature(BSJNIHostMethod *m) {
    const char *p = m->signature;
    if (*p++ != '(') return 0;
    m->argc = 0;
    while (*p && *p != ')') {
        if (m->argc == BS_JNI_HOST_MAX_ARGS) return 0;
        char kind = *p;
        if (*p == '[') {
            while (*p == '[') ++p;
            kind = 'L';
        }
        if (*p == 'L') {
            const char *end = strchr(p, ';');
            if (!end) return 0;
            p = end + 1;
            kind = 'L';
        } else if (strchr("ZBCSIJFD", *p)) {
            if (kind != 'L') kind = *p;
            ++p;
        } else {
            return 0;
        }
        m->params[m->argc++] = kind;
    }
    if (*p++ != ')') return 0;
    m->result = (*p == '[') ? 'L' : *p;
    return strchr("ZBCSIJFDVL", m->result) != NULL && m->result;
}

int bs_jni_register_host_method(const char *class_name, const char *name,
                                const char *signature, int is_static,
                                BSJNIHostMethodFn fn, void *user) {
    if (!class_name || !name || !signature || !fn) return BS_JNI_ERR;
    bs_jni_lock();
    for (size_t i = 0; i < g_host_method_count; ++i) {
        BSJNIHostMethod *m = &g_host_methods[i];
        if (!strcmp(m->class_name, class_name) && !strcmp(m->name, name) &&
            !strcmp(m->signature, signature)) {
            m->fn = fn; /* re-registration after bs_jni_init keeps the ID */
            m->user = user;
            m->is_static = is_static;
            bs_jni_unlock();
            return BS_JNI_OK;
        }
    }
    if (g_host_method_count == BS_JNI_MAX_HOST_METHODS) {
        bs_jni_unlock();
        return BS_JNI_ERR;
    }
    BSJNIHostMethod *m = &g_host_methods[g_host_method_count];
    memset(m, 0, sizeof *m);
    if (bs_jni_copy(m->class_name, sizeof m->class_name, class_name) ||
        bs_jni_copy(m->name, sizeof m->name, name) ||
        bs_jni_copy(m->signature, sizeof m->signature, signature) ||
        !bs_jni_host_parse_signature(m)) {
        bs_jni_unlock();
        return BS_JNI_ERR;
    }
    m->magic = BS_JNI_MAGIC_HOST_METHOD;
    m->is_static = is_static;
    m->fn = fn;
    m->user = user;
    ++g_host_method_count;
    bs_jni_unlock();
    return BS_JNI_OK;
}

static BSJNIHostMethod *bs_jni_host_method(const void *reference) {
    return (BSJNIHostMethod *)(uintptr_t)bs_jni_pool_member(
        reference, g_host_methods, sizeof g_host_methods[0],
        g_host_method_count);
}

static BSJNIAppClassResolver g_app_class_resolver;
static void *g_app_class_resolver_user;

void bs_jni_set_app_class_resolver(BSJNIAppClassResolver fn, void *user) {
    g_app_class_resolver = fn;
    g_app_class_resolver_user = user;
}

static int bs_jni_is_app_class(const char *name, char *super_out,
                               size_t super_cap) {
    return name && g_app_class_resolver &&
           g_app_class_resolver(g_app_class_resolver_user, name,
                                super_out, super_cap);
}

/* Android framework (boot classpath) superclasses for the classes this
 * bridge models. Values are the actual AOSP/libcore declarations. */
static const struct {
    const char *name, *superclass;
} g_framework_superclasses[] = {
    {"java/lang/Object", ""},
    {"java/lang/Class", "java/lang/Object"},
    {"java/lang/String", "java/lang/Object"},
    {"java/lang/ClassLoader", "java/lang/Object"},
    {"java/lang/Thread", "java/lang/Object"},
    {"java/lang/Number", "java/lang/Object"},
    {"java/lang/Throwable", "java/lang/Object"},
    {"java/lang/Exception", "java/lang/Throwable"},
    {"java/lang/Error", "java/lang/Throwable"},
    {"java/lang/RuntimeException", "java/lang/Exception"},
    {"java/lang/ReflectiveOperationException", "java/lang/Exception"},
    {"java/lang/ClassNotFoundException",
     "java/lang/ReflectiveOperationException"},
    {"java/lang/NoSuchMethodException",
     "java/lang/ReflectiveOperationException"},
    {"java/lang/IllegalStateException", "java/lang/RuntimeException"},
    {"java/lang/IllegalArgumentException", "java/lang/RuntimeException"},
    {"java/lang/NullPointerException", "java/lang/RuntimeException"},
    {"java/lang/SecurityException", "java/lang/RuntimeException"},
    {"java/lang/UnsupportedOperationException", "java/lang/RuntimeException"},
    {"java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException"},
    {"java/lang/LinkageError", "java/lang/Error"},
    {"java/lang/IncompatibleClassChangeError", "java/lang/LinkageError"},
    {"java/lang/NoSuchMethodError", "java/lang/IncompatibleClassChangeError"},
    {"java/lang/NoSuchFieldError", "java/lang/IncompatibleClassChangeError"},
    {"java/lang/NoClassDefFoundError", "java/lang/LinkageError"},
    {"java/lang/VirtualMachineError", "java/lang/Error"},
    {"java/lang/OutOfMemoryError", "java/lang/VirtualMachineError"},
    {"dalvik/system/BaseDexClassLoader", "java/lang/ClassLoader"},
    {"dalvik/system/PathClassLoader", "dalvik/system/BaseDexClassLoader"},
    {"android/content/Context", "java/lang/Object"},
    {"android/content/ContextWrapper", "android/content/Context"},
    {"android/view/ContextThemeWrapper", "android/content/ContextWrapper"},
    {"android/app/Activity", "android/view/ContextThemeWrapper"},
    {"android/content/res/AssetManager", "java/lang/Object"},
    {"android/content/res/Configuration", "java/lang/Object"},
    {"android/os/LocaleList", "java/lang/Object"},
    {"java/util/Locale", "java/lang/Object"},
    {"java/io/File", "java/lang/Object"},
    {"android/view/Surface", "java/lang/Object"},
    {"android/view/InputEvent", "java/lang/Object"},
    {"android/view/MotionEvent", "android/view/InputEvent"},
    {"android/view/KeyEvent", "android/view/InputEvent"},
};

/* 1 when `name` is a modeled class; writes its superclass ("" for
 * java/lang/Object). App DEX class_defs take precedence, as in ART's
 * PathClassLoader delegation (boot first, but the APK cannot redefine
 * java.* or android.* classes, so the two sets are disjoint). */
static int bs_jni_superclass_of(const char *name, char *out, size_t cap) {
    if (!name || !out || !cap) return 0;
    for (size_t i = 0; i < sizeof g_framework_superclasses /
                            sizeof g_framework_superclasses[0]; ++i)
        if (!strcmp(g_framework_superclasses[i].name, name)) {
            const char *parent = g_framework_superclasses[i].superclass;
            if (!parent[0]) {
                out[0] = '\0';
                return 1;
            }
            return bs_jni_copy(out, cap, parent) == BS_JNI_OK;
        }
    if (!g_app_class_resolver &&
        (!strcmp(name, "com/unity3d/player/UnityPlayerForGameActivity") ||
         !strcmp(name, "com/unity3d/player/UnityPlayerForActivityOrService")))
        /* Original DEX lineage, kept for builds without a DEX index. */
        return bs_jni_copy(out, cap, "com/unity3d/player/UnityPlayer") ==
               BS_JNI_OK;
    if (bs_jni_is_app_class(name, out, cap)) return 1;
    /* Lenient: any other boot-classpath class is modeled as a direct
     * java/lang/Object subclass with no host-backed members. */
    if (g_lenient && (!strncmp(name, "android/", 8) ||
                      !strncmp(name, "java/", 5) ||
                      !strncmp(name, "javax/", 6) ||
                      !strncmp(name, "dalvik/", 7)))
        return bs_jni_copy(out, cap, "java/lang/Object") == BS_JNI_OK;
    return 0;
}

static int bs_jni_is_framework_class(const char *name) {
    char super_name[BS_JNI_CLASS_BYTES];
    return name && !bs_jni_is_app_class(name, NULL, 0) &&
           bs_jni_superclass_of(name, super_name, sizeof super_name);
}

/* Whether `actual` is `requested` or one of its modeled superclasses. */
static int bs_jni_class_extends(const char *actual, const char *requested) {
    char current[BS_JNI_CLASS_BYTES];
    if (!actual || !requested || bs_jni_copy(current, sizeof current, actual))
        return 0;
    for (int depth = 0; depth < 32 && current[0]; ++depth) {
        if (!strcmp(current, requested)) return 1;
        char next[BS_JNI_CLASS_BYTES];
        if (!bs_jni_superclass_of(current, next, sizeof next)) return 0;
        memcpy(current, next, sizeof current);
    }
    return 0;
}

static BSJNIHostMethod *bs_jni_find_host_method(const char *class_name,
                                                const char *name,
                                                const char *signature,
                                                int is_static) {
    char klass[BS_JNI_CLASS_BYTES];
    if (!class_name || bs_jni_copy(klass, sizeof klass, class_name))
        return NULL;
    for (int depth = 0; depth < 32 && klass[0]; ++depth) {
        for (size_t i = 0; i < g_host_method_count; ++i) {
            BSJNIHostMethod *m = &g_host_methods[i];
            if (m->is_static == is_static && !strcmp(m->class_name, klass) &&
                !strcmp(m->name, name) && !strcmp(m->signature, signature))
                return m;
        }
        /* Constructors are never inherited. */
        if (!strcmp(name, "<init>")) break;
        char next[BS_JNI_CLASS_BYTES];
        if (!bs_jni_superclass_of(klass, next, sizeof next)) break;
        memcpy(klass, next, sizeof klass);
    }
    return NULL;
}

/* Unity's ReflectionHelper matches by name and parameter types; a generic
 * AndroidJavaObject return is requested as Ljava/lang/Object;, which any
 * reference return type satisfies. */
static BSJNIHostMethod *bs_jni_find_host_method_fuzzy(const char *class_name,
                                                      const char *name,
                                                      const char *signature,
                                                      int is_static) {
    BSJNIHostMethod *exact = bs_jni_find_host_method(class_name, name, signature,
                                                     is_static);
    if (exact) return exact;
    const char *close = signature ? strchr(signature, ')') : NULL;
    if (!close || strcmp(close + 1, "Ljava/lang/Object;")) return NULL;
    size_t params = (size_t)(close - signature) + 1;
    char klass[BS_JNI_CLASS_BYTES];
    if (!class_name || bs_jni_copy(klass, sizeof klass, class_name)) return NULL;
    for (int depth = 0; depth < 32 && klass[0]; ++depth) {
        for (size_t i = 0; i < g_host_method_count; ++i) {
            BSJNIHostMethod *m = &g_host_methods[i];
            if (m->is_static == is_static && !strcmp(m->class_name, klass) &&
                !strcmp(m->name, name) && !strncmp(m->signature, signature, params) &&
                (m->signature[params] == 'L' || m->signature[params] == '['))
                return m;
        }
        if (!strcmp(name, "<init>")) break;
        char next[BS_JNI_CLASS_BYTES];
        if (!bs_jni_superclass_of(klass, next, sizeof next)) break;
        memcpy(klass, next, sizeof klass);
    }
    return NULL;
}

/* Lenient stub body: the JNI default result for any return type. */
static int bs_jni_lenient_host_fn(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    memset(result, 0, sizeof *result);
    return 0;
}

static BSJNIObject *bs_jni_string_for_utf(const char *text);
/* Lenient String-returning stub: native callers routinely feed the result to
 * GetStringUTFChars without a null check, so return "" rather than null. */
static int bs_jni_lenient_string_fn(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    memset(result, 0, sizeof *result);
    result->l = bs_jni_string_for_utf("");
    return 0;
}

/* AudioManager.getProperty / Integer.parseInt: FMOD's OpenSL output sizes
 * its buffers from these (Switch audout runs at 48 kHz). */
static BSJNIObject *bs_jni_known_object(const void *ref);
static const char *bs_jni_arg_text(const BSJNIValue *args, size_t argc) {
    if (!argc || !args[0].l) return NULL;
    BSJNIObject *s = bs_jni_known_object(args[0].l);
    return s && s->kind == BS_JNI_OBJECT_STRING ? s->text : NULL;
}
static int bs_jni_audio_property_fn(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    (void)user; (void)env; (void)receiver;
    const char *key = bs_jni_arg_text(args, argc);
    memset(result, 0, sizeof *result);
    const char *value = NULL;
    if (key && strstr(key, "SAMPLE_RATE")) value = "48000";
    else if (key && strstr(key, "FRAMES_PER_BUFFER")) value = "256";
    result->l = value ? bs_jni_string_for_utf(value) : NULL;
    return 0;
}
static int bs_jni_parse_int_fn(void *user, void *env, void *receiver,
                               const BSJNIValue *args, size_t argc,
                               BSJNIValue *result) {
    (void)user; (void)env; (void)receiver;
    const char *text = bs_jni_arg_text(args, argc);
    memset(result, 0, sizeof *result);
    result->i = text ? (int32_t)strtol(text, NULL, 10) : 0;
    return 0;
}

static int bs_jni_returns_string(const char *signature) {
    const char *close = strrchr(signature, ')');
    return close && !strcmp(close + 1, "Ljava/lang/String;");
}

/* Framework values that must not default to zero. Unity derives its frame
 * interval, viewport and pacing from these Display queries; a 0 Hz refresh
 * or a 0x0 display stops it presenting. Values describe the Switch panel. */
typedef struct {
    const char *class_name, *name, *signature;
    char kind; /* 'i' int, 'f' float, 'j' long, 'z' boolean */
    double value;
} BSJNILenientValue;
static int32_t g_display_width = 1280, g_display_height = 720;
static float g_display_hz = 60.0f;
void bs_jni_set_display_metrics(int32_t width, int32_t height, float hz) {
    if (width > 0) g_display_width = width;
    if (height > 0) g_display_height = height;
    if (hz > 0) g_display_hz = hz;
}
static const BSJNILenientValue g_lenient_values[] = {
    {"android/view/Display", "getRefreshRate", "()F", 'f', -1},
    {"android/view/Display", "getWidth", "()I", 'i', -2},
    {"android/view/Display", "getHeight", "()I", 'i', -3},
    {"android/view/Display", "getAppVsyncOffsetNanos", "()J", 'j', 0},
    {"android/view/Display", "getPresentationDeadlineNanos", "()J", 'j', 11000000},
    {"android/view/Display", "isValid", "()Z", 'z', 1},
    {"android/view/Display", "getState", "()I", 'i', 2 /* STATE_ON */},
    {"android/os/PowerManager", "isInteractive", "()Z", 'z', 1},
    {"android/os/PowerManager", "isScreenOn", "()Z", 'z', 1},
};
static double bs_jni_lenient_resolve(const BSJNILenientValue *v) {
    if (v->value == -1) return g_display_hz;
    if (v->value == -2) return g_display_width;
    if (v->value == -3) return g_display_height;
    return v->value;
}
static int bs_jni_lenient_value_fn(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)env; (void)receiver; (void)args; (void)argc;
    const BSJNILenientValue *v = user;
    const double x = bs_jni_lenient_resolve(v);
    memset(result, 0, sizeof *result);
    switch (v->kind) {
    case 'f': result->f = (float)x; break;
    case 'j': result->j = (int64_t)x; break;
    case 'z': result->z = x != 0; break;
    default: result->i = (int32_t)x; break;
    }
    return 0;
}
static const BSJNILenientValue *bs_jni_lenient_value(const char *class_name,
                                                     const char *name,
                                                     const char *signature) {
    for (size_t i = 0; i < sizeof g_lenient_values / sizeof g_lenient_values[0]; ++i) {
        const BSJNILenientValue *v = &g_lenient_values[i];
        if (!strcmp(v->class_name, class_name) && !strcmp(v->name, name) &&
            !strcmp(v->signature, signature))
            return v;
    }
    return NULL;
}

/* Lenient method ID for a member the host does not model: a host method
 * on exactly `class_name` whose body returns the JNI default. Logged once
 * at resolution. NULL in strict mode or when the ID cannot be created. */
static BSJNIHostMethod *bs_jni_lenient_method(const char *class_name,
                                              const char *name,
                                              const char *signature,
                                              int is_static) {
    if (!g_lenient || !class_name || !name || !signature) return NULL;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = g_host_method_count; i-- > 0;) {
            BSJNIHostMethod *candidate = &g_host_methods[i];
            if (!strcmp(candidate->class_name, class_name) &&
                !strcmp(candidate->name, name) &&
                !strcmp(candidate->signature, signature)) {
                if (candidate->is_static != is_static) return NULL;
                if (pass)
                    bs_jni_log("[jni:lenient] stub %s %s.%s%s",
                               is_static ? "static" : "method",
                               class_name, name, signature);
                return candidate;
            }
        }
        const BSJNILenientValue *value =
            is_static ? NULL : bs_jni_lenient_value(class_name, name, signature);
        if (pass ||
            bs_jni_register_host_method(class_name, name, signature,
                                        is_static,
                                        value ? bs_jni_lenient_value_fn :
                                        !strcmp(name, "getProperty") &&
                                        strstr(class_name, "AudioManager")
                                            ? bs_jni_audio_property_fn :
                                        !strcmp(name, "parseInt") &&
                                        !strcmp(class_name, "java/lang/Integer")
                                            ? bs_jni_parse_int_fn :
                                        bs_jni_returns_string(signature)
                                            ? bs_jni_lenient_string_fn
                                            : bs_jni_lenient_host_fn,
                                        (void *)value) != BS_JNI_OK)
            return NULL;
    }
    return NULL;
}

/* Lenient field IDs. Static fields keep the last value written through
 * SetStatic*Field (a process-wide cell, like a real static); instance
 * fields read as the JNI default and ignore writes. */
#define BS_JNI_MAX_LENIENT_FIELDS 512
typedef struct BSJNILenientField {
    char class_name[BS_JNI_CLASS_BYTES];
    char name[BS_JNI_METHOD_BYTES];
    char signature[BS_JNI_SIGNATURE_BYTES];
    int is_static;
    int seeded; /* value applies to instance fields too */
    BSJNIValue value;
} BSJNILenientField;
static BSJNILenientField g_lenient_fields[BS_JNI_MAX_LENIENT_FIELDS];
static size_t g_lenient_field_count;

static BSJNILenientField *bs_jni_lenient_field(const void *id) {
    return (BSJNILenientField *)(uintptr_t)bs_jni_pool_member(
        id, g_lenient_fields, sizeof g_lenient_fields[0],
        g_lenient_field_count);
}

/* Android framework constants and device identity a Quest 2 on Android 12L
 * (SDK 32) reports. Instance entries apply to every modeled instance. */
static BSJNIObject *bs_jni_string_for_utf(const char *text);
static const struct {
    const char *class_name, *name, *signature;
    int32_t integer;
    const char *text;
} g_field_seeds[] = {
    {"android/os/Build$VERSION", "SDK_INT", "I", 32, NULL},
    {"android/os/Build$VERSION", "PREVIEW_SDK_INT", "I", 0, NULL},
    {"android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;", 0, "12"},
    {"android/os/Build$VERSION", "CODENAME", "Ljava/lang/String;", 0, "REL"},
    {"android/os/Build$VERSION", "INCREMENTAL", "Ljava/lang/String;", 0,
     "51154110092200520"},
    {"android/os/Build", "MANUFACTURER", "Ljava/lang/String;", 0, "Oculus"},
    {"android/os/Build", "MODEL", "Ljava/lang/String;", 0, "Quest 2"},
    {"android/os/Build", "BRAND", "Ljava/lang/String;", 0, "oculus"},
    {"android/os/Build", "DEVICE", "Ljava/lang/String;", 0, "hollywood"},
    {"android/os/Build", "PRODUCT", "Ljava/lang/String;", 0, "hollywood"},
    {"android/os/Build", "HARDWARE", "Ljava/lang/String;", 0, "hollywood"},
    {"android/os/Build", "BOARD", "Ljava/lang/String;", 0, "kona"},
    {"android/os/Build", "FINGERPRINT", "Ljava/lang/String;", 0,
     "oculus/hollywood/hollywood:12/SQ3A.220605.009.A1/51154110092200520:user/release-keys"},
    {"android/os/Environment", "MEDIA_MOUNTED", "Ljava/lang/String;", 0,
     "mounted"},
    {"android/content/Context", "MODE_PRIVATE", "I", 0, NULL},
    {"android/content/Context", "AUDIO_SERVICE", "Ljava/lang/String;", 0, "audio"},
    {"android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE",
     "Ljava/lang/String;", 0, "android.media.property.OUTPUT_SAMPLE_RATE"},
    {"android/media/AudioManager", "PROPERTY_OUTPUT_FRAMES_PER_BUFFER",
     "Ljava/lang/String;", 0, "android.media.property.OUTPUT_FRAMES_PER_BUFFER"},
    {"android/content/pm/ApplicationInfo", "targetSdkVersion", "I", 32, NULL},
    {"android/content/pm/ApplicationInfo", "minSdkVersion", "I", 29, NULL},
    {"android/content/pm/ApplicationInfo", "nativeLibraryDir",
     "Ljava/lang/String;", 0, "/data/app/com.beatgames.beatsaber/lib/arm64"},
    {"android/content/pm/ApplicationInfo", "sourceDir", "Ljava/lang/String;",
     0, "/switch/beat-saber-1408/beat-saber.apk"},
    {"android/content/pm/ApplicationInfo", "publicSourceDir",
     "Ljava/lang/String;", 0, "/switch/beat-saber-1408/beat-saber.apk"},
    {"android/content/pm/ApplicationInfo", "dataDir", "Ljava/lang/String;", 0,
     "/storage/emulated/0/Android/data/com.beatgames.beatsaber/files"},
    {"android/content/pm/ApplicationInfo", "packageName",
     "Ljava/lang/String;", 0, "com.beatgames.beatsaber"},
    /* Quest 1.40.8_7379 manifest versionCode; selects main.1716.*.obb. */
    {"android/content/pm/PackageInfo", "versionCode", "I", 1716, NULL},
    {"android/content/pm/PackageInfo", "versionName", "Ljava/lang/String;", 0,
     "1.40.8_7379"},
    {"android/content/pm/PackageInfo", "packageName", "Ljava/lang/String;", 0,
     "com.beatgames.beatsaber"},
};

static void bs_jni_seed_lenient_field(BSJNILenientField *f) {
    for (size_t i = 0; i < sizeof g_field_seeds / sizeof g_field_seeds[0];
         ++i) {
        if (strcmp(g_field_seeds[i].class_name, f->class_name) ||
            strcmp(g_field_seeds[i].name, f->name) ||
            strcmp(g_field_seeds[i].signature, f->signature))
            continue;
        if (g_field_seeds[i].text) {
            BSJNIObject *s = bs_jni_string_for_utf(g_field_seeds[i].text);
            if (s) s->pinned = 1;
            f->value.l = s;
        } else {
            f->value.i = g_field_seeds[i].integer;
        }
        f->seeded = 1;
        return;
    }
}

static BSJNILenientField *bs_jni_lenient_field_id(const char *class_name,
                                                  const char *name,
                                                  const char *signature,
                                                  int is_static) {
    if (!g_lenient || !class_name || !name || !signature) return NULL;
    bs_jni_lock();
    for (size_t i = 0; i < g_lenient_field_count; ++i) {
        BSJNILenientField *f = &g_lenient_fields[i];
        if (f->is_static == is_static && !strcmp(f->class_name, class_name) &&
            !strcmp(f->name, name) && !strcmp(f->signature, signature)) {
            bs_jni_unlock();
            return f;
        }
    }
    if (g_lenient_field_count == BS_JNI_MAX_LENIENT_FIELDS) {
        bs_jni_unlock();
        return NULL;
    }
    BSJNILenientField *f = &g_lenient_fields[g_lenient_field_count];
    memset(f, 0, sizeof *f);
    if (bs_jni_copy(f->class_name, sizeof f->class_name, class_name) ||
        bs_jni_copy(f->name, sizeof f->name, name) ||
        bs_jni_copy(f->signature, sizeof f->signature, signature)) {
        bs_jni_unlock();
        return NULL;
    }
    f->is_static = is_static;
    ++g_lenient_field_count;
    bs_jni_unlock();
    bs_jni_seed_lenient_field(f);
    bs_jni_log("[jni:lenient] stub %sfield %s.%s %s",
               is_static ? "static " : "", class_name, name, signature);
    return f;
}

static void bs_jni_host_runtime_exception(const BSJNIHostMethod *m) {
    if (g_missing_member_pending) return;
    memset(&g_missing_member_exception, 0, sizeof g_missing_member_exception);
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_THROWABLE;
    snprintf(g_missing_member_exception.throwable_class,
             sizeof g_missing_member_exception.throwable_class,
             "java/lang/RuntimeException");
    snprintf(g_missing_member_exception.text,
             sizeof g_missing_member_exception.text,
             "RuntimeException: host %.160s.%.96s%.160s failed",
             m->class_name, m->name, m->signature);
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_error("%s", g_missing_member_exception.text);
}

/* Arguments arrive either as a va_list (C default promotions: sub-int
 * integers as int, float as double) or as a jvalue array (8-byte unions). */
static BSJNIValue bs_jni_host_invoke(void *env, void *obj,
                                     BSJNIHostMethod *m, va_list *list,
                                     const BSJNIValue *array) {
    BSJNIValue args[BS_JNI_HOST_MAX_ARGS];
    BSJNIValue result;
    memset(args, 0, sizeof args);
    memset(&result, 0, sizeof result);
    for (size_t i = 0; i < m->argc; ++i) {
        if (array) { args[i] = array[i]; continue; }
        if (!list) break;
        switch (m->params[i]) {
            case 'J': args[i].j = va_arg(*list, int64_t); break;
            case 'F': args[i].f = (float)va_arg(*list, double); break;
            case 'D': args[i].d = va_arg(*list, double); break;
            case 'L': args[i].l = va_arg(*list, void *); break;
            case 'Z': args[i].z = (uint8_t)va_arg(*list, int); break;
            case 'B': args[i].b = (int8_t)va_arg(*list, int); break;
            case 'C': args[i].c = (uint16_t)va_arg(*list, int); break;
            case 'S': args[i].s = (int16_t)va_arg(*list, int); break;
            default: args[i].i = va_arg(*list, int32_t); break;
        }
    }
    if (g_missing_member_pending) return result;
    if (m->fn(m->user, env, obj, args, m->argc, &result) != 0)
        bs_jni_host_runtime_exception(m);
    return result;
}

static void bs_jni_host_wrong_kind(const BSJNIHostMethod *m,
                                   const char *operation) {
    bs_jni_missing_host_value(operation, m->class_name, m->name);
}

int bs_jni_is_unityplayer_host(void *receiver) {
    BSJNIObject *object = bs_jni_known_object(receiver);
    return object && object->kind == BS_JNI_OBJECT_UNITY_PLAYER_HOST;
}

static BSJNIClass *bs_jni_class_for_name(const char *name) {
    if (!name || !*name) {
        bs_jni_no_class_def(name);
        return NULL;
    }
    /* The classes below are symbolic, not loaded by a Java VM. Reject
     * malformed internal names rather than handing out false Java identities.
     */
    const size_t length = strlen(name);
    if (length >= BS_JNI_CLASS_BYTES || name[0] == '/' ||
        name[length - 1] == '/' || strstr(name, "//")) {
        bs_jni_no_class_def(name);
        return NULL;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c <= 32 || c == 127 || c == '.' || c == ';' ||
            c == '[' || c == '\\') {
            bs_jni_no_class_def(name);
            return NULL;
        }
    }
    bs_jni_lock();
    for (size_t i = 0; i < g_class_count; ++i) {
        if (strcmp(g_classes[i].name, name) == 0) {
            BSJNIClass *found = &g_classes[i];
            bs_jni_unlock();
            return found;
        }
    }
    if (g_class_count == BS_JNI_MAX_CLASSES) {
        bs_jni_unlock();
        bs_jni_error("FindClass table exhausted: %s", name);
        return NULL;
    }
    BSJNIClass *klass = &g_classes[g_class_count];
    if (bs_jni_copy(klass->name, sizeof klass->name, name) != BS_JNI_OK) {
        bs_jni_unlock();
        bs_jni_error("FindClass name is too long or invalid");
        return NULL;
    }
    klass->magic = BS_JNI_MAGIC_CLASS;
    ++g_class_count;
    bs_jni_unlock();
    bs_jni_log("[jni] symbolic FindClass %s (no Java bytecode loaded)", name);
    return klass;
}

static BSJNIObject *bs_jni_object_new(BSJNIObjectKind kind, const char *text) {
    const char *value = text ? text : "";
    size_t length = 0;
    while (length < BS_JNI_STRING_BYTES && value[length])
        ++length;
    if (length == BS_JNI_STRING_BYTES ||
        (length == 0 && kind != BS_JNI_OBJECT_STRING &&
         kind != BS_JNI_OBJECT_THROWABLE)) {
        bs_jni_error("JNI string/object contents too long");
        return NULL;
    }
    bs_jni_lock();
    BSJNIObject *object;
    if (g_object_free_count) {
        object = &g_objects[g_object_free[--g_object_free_count]];
    } else if (g_object_count == BS_JNI_MAX_OBJECTS) {
        bs_jni_unlock();
        bs_jni_error("JNI object pool exhausted");
        return NULL;
    } else {
        object = &g_objects[g_object_count];
        ++g_object_count;
    }
    memset(object, 0, sizeof *object);
    memcpy(object->text, value, length);
    object->text[length] = '\0';
    object->kind = kind;
    object->observed_count = 0;
    object->local_owner = (const void *)&g_thread_env_table;
    object->local_frame = g_local_frame_count;
    object->magic = BS_JNI_MAGIC_OBJECT;
    bs_jni_unlock();
    return object;
}

/* Transient value objects whose storage is reclaimed once the guest
 * deletes the raw local handle and no reference record targets it. */
static int bs_jni_object_transient(const BSJNIObject *object) {
    return object && object != &g_missing_member_exception &&
           !object->pinned &&
           (object->kind == BS_JNI_OBJECT_STRING ||
            object->kind == BS_JNI_OBJECT_ARRAY ||
            object->kind == BS_JNI_OBJECT_FILE ||
            object->kind == BS_JNI_OBJECT_REFLECTED);
}

/* Caller holds the registry lock. */
static void bs_jni_object_free_locked(BSJNIObject *object) {
    void *data = object->array_data;
    object->magic = 0;
    object->kind = 0;
    object->array_data = NULL;
    object->array_length = 0;
    g_object_free[g_object_free_count++] =
        (uint32_t)(object - g_objects);
    free(data);
}

static void bs_jni_object_maybe_free_locked(BSJNIObject *object) {
    if (bs_jni_object_transient(object) && object->raw_released &&
        !object->refs)
        bs_jni_object_free_locked(object);
}

/* Caller holds the registry lock. Deactivate a reference record, drop
 * its hold on the target and recycle the slot. */
static void bs_jni_reference_release_locked(BSJNIReference *ref) {
    if (!ref->active) return;
    ref->active = 0;
    BSJNIObject *object = (BSJNIObject *)(uintptr_t)bs_jni_pool_member(
        ref->target, g_objects, sizeof g_objects[0], g_object_count);
    if (object && object->magic == BS_JNI_MAGIC_OBJECT && object->refs) {
        --object->refs;
        bs_jni_object_maybe_free_locked(object);
    }
    ref->target = NULL;
    g_reference_free[(g_reference_free_head + g_reference_free_count++) %
                     BS_JNI_MAX_REFS] = (uint32_t)(ref - g_references);
}

/*
 * Immutable native-to-Java value snapshot: the integrator must supply
 * measured values, never guessed Android defaults. A Configuration or
 * Insets can be represented without pretending an Android Activity exists.
 */
void *bs_jni_object_from_observed_fields(const char *class_name,
                                          const BSJNIObservedField *fields,
                                          size_t count) {
    if (!g_initialized || !class_name || !fields || !count ||
        count > BS_JNI_OBSERVED_FIELDS_MAX) {
        bs_jni_error("observed Java field snapshot invalid arguments");
        return NULL;
    }
    BSJNIObjectKind kind;
    if (!strcmp(class_name, BS_CONFIG))
        kind = BS_JNI_OBJECT_CONFIGURATION;
    else if (!strcmp(class_name, BS_INSETS))
        kind = BS_JNI_OBJECT_INSETS;
    else {
        bs_jni_error("observed Java fields require Configuration or Insets");
        return NULL;
    }
    const BSJNIKnownMember *resolved[BS_JNI_OBSERVED_FIELDS_MAX];
    for (size_t i = 0; i < count; ++i) {
        const BSJNIObservedField *field = &fields[i];
        resolved[i] = bs_jni_lookup_gameactivity_member(
            class_name, field->name, field->signature, BS_JNI_INSTANCE_FIELD);
        if (!resolved[i] ||
            (strcmp(field->signature, "I") && strcmp(field->signature, "F")) ||
            (!strcmp(field->signature, "F") &&
             !(field->float_value <= FLT_MAX &&
               field->float_value >= -FLT_MAX))) {
            bs_jni_error("unrecognized/invalid observed Java %s field",
                         class_name);
            return NULL;
        }
        for (size_t j = 0; j < i; ++j) {
            if (resolved[j] == resolved[i]) {
                bs_jni_error("duplicate observed Java field %s.%s",
                             class_name, field->name);
                return NULL;
            }
        }
    }
    BSJNIObject *object = bs_jni_object_new(kind, class_name);
    if (!object)
        return NULL;
    for (size_t i = 0; i < count; ++i) {
        object->observed[i].member = resolved[i];
        object->observed[i].integer_value = fields[i].integer_value;
        object->observed[i].float_value = fields[i].float_value;
    }
    object->observed_count = count;
    return object;
}

static int bs_jni_valid_locale_tag(const char *text, int required) {
    if (!text || (required && !*text)) return 0;
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (++length >= BS_JNI_LOCALE_TAG_BYTES ||
            !( (*p >= 'a' && *p <= 'z') ||
               (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') ||
               *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

void *bs_jni_configuration_from_observed_locales(
    const BSJNIObservedField *fields, size_t field_count,
    const BSJNIObservedLocale *locales, size_t locale_count) {
    if (!g_initialized || !locales || !locale_count ||
        locale_count > BS_JNI_OBSERVED_LOCALES_MAX ||
        field_count > BS_JNI_OBSERVED_FIELDS_MAX ||
        (field_count && !fields) ||
        g_object_count > BS_JNI_MAX_OBJECTS - 2 - locale_count) {
        bs_jni_error("cannot publish observed Configuration locale list");
        return NULL;
    }
    for (size_t i = 0; i < locale_count; ++i) {
        if (!bs_jni_valid_locale_tag(locales[i].language, 1) ||
            !bs_jni_valid_locale_tag(locales[i].script, 0) ||
            !bs_jni_valid_locale_tag(locales[i].country, 0) ||
            !bs_jni_valid_locale_tag(locales[i].variant, 0)) {
            bs_jni_error("invalid or unobserved locale at index %lu",
                         (unsigned long)i);
            return NULL;
        }
    }
    BSJNIObject *configuration = field_count
        ? (BSJNIObject *)bs_jni_object_from_observed_fields(
            BS_CONFIG, fields, field_count)
        : bs_jni_object_new(BS_JNI_OBJECT_CONFIGURATION, BS_CONFIG);
    if (!configuration) return NULL;

    BSJNIObject *list = bs_jni_object_new(BS_JNI_OBJECT_LOCALE_LIST,
                                         BS_LOCALE_LIST);
    if (!list) return NULL; /* Capacity checked for whole transaction above. */
    for (size_t i = 0; i < locale_count; ++i) {
        BSJNIObject *locale = bs_jni_object_new(BS_JNI_OBJECT_LOCALE,
                                                BS_LOCALE);
        if (!locale) return NULL;
        memcpy(locale->locale.language, locales[i].language,
               strlen(locales[i].language) + 1);
        memcpy(locale->locale.script, locales[i].script,
               strlen(locales[i].script) + 1);
        memcpy(locale->locale.country, locales[i].country,
               strlen(locales[i].country) + 1);
        memcpy(locale->locale.variant, locales[i].variant,
               strlen(locales[i].variant) + 1);
        list->locale_items[i] = locale;
    }
    list->locale_count = locale_count;
    configuration->locale_list = list;
    return configuration;
}

void *bs_jni_gameactivity_native_proxy(
    const BSJNIGameActivityHostServices *services) {
    if (!g_initialized || !services ||
        (!services->finish && !services->set_window_flags &&
         !services->set_ime_editor_info_fields &&
         !services->get_window_insets && !services->get_waterfall_insets)) {
        bs_jni_error("GameActivity native proxy requires observed host services");
        return NULL;
    }
    BSJNIObject *proxy = bs_jni_object_new(
        BS_JNI_OBJECT_GAME_ACTIVITY_HOST, BS_GAME_ACTIVITY);
    if (!proxy) return NULL;
    proxy->gameactivity_host = *services;
    return proxy;
}

/* Host stand-in for the Java UnityPlayerForGameActivity instance that the
 * original UnityPlayerGameActivity.onCreateSurfaceView constructs. It only
 * carries class identity so original UnityPlayer natives (initJni,
 * nativeUnityPlayerForGameActivityInitialized) receive a stable receiver;
 * no Java constructor bytecode is executed. */
static BSJNIObject *g_unityplayer_host;

void *bs_jni_unityplayer_host_object(void) {
    if (!g_initialized) {
        bs_jni_error("UnityPlayer host object requires an initialized JNI VM");
        return NULL;
    }
    if (!g_unityplayer_host)
        g_unityplayer_host = bs_jni_object_new(
            BS_JNI_OBJECT_UNITY_PLAYER_HOST, BS_UNITY_PLAYER);
    return g_unityplayer_host;
}

void *bs_jni_configuration_from_host_display(
    uint32_t width, uint32_t height, const BSJNIObservedLocale *locale) {
    if (!width || !height || !locale) return NULL;

    /* AOSP Configuration.setToDefaults supplies undefined values for
     * unmeasured display metrics and input devices. Do not derive Android
     * density-independent units from Switch framebuffer pixels. */
    BSJNIObservedField fields[18] = {0};
    size_t count = 0;
    for (size_t i = 0; i < sizeof g_gameactivity_members /
                                sizeof g_gameactivity_members[0]; ++i) {
        const BSJNIKnownMember *member = &g_gameactivity_members[i];
        if (member->type != BS_JNI_INSTANCE_FIELD ||
            strcmp(member->class_name, BS_CONFIG)) continue;
        if (count == sizeof fields / sizeof fields[0]) return NULL;
        BSJNIObservedField *field = &fields[count++];
        field->name = member->name;
        field->signature = member->signature;
        if (!strcmp(field->name, "fontScale")) field->float_value = 1.f;
        if (!strcmp(field->name, "fontWeightAdjustment"))
            field->integer_value = INT32_MAX;
        if (!strcmp(field->name, "orientation"))
            field->integer_value = height > width ? 1 : width > height ? 2 : 0;
    }
    BSJNIObject *configuration = bs_jni_configuration_from_observed_locales(
        fields, count, locale, 1);
    if (configuration)
        configuration->configuration_default_count = count -
            (width != height);
    return configuration;
}

static int bs_jni_finite_motion_float(float value) {
    return value >= -FLT_MAX && value <= FLT_MAX;
}

/* Validate a whole measured input event before exposing any JNI handle or
 * mutating a reused one. A sparse axis absent from a valid snapshot is the
 * real Android MotionEvent default 0; missing pointers/history are errors. */
static int bs_jni_valid_motion_observation(
    const BSJNIObservedMotionEvent *event) {
    if (!event || !event->pointer_count ||
        event->pointer_count > BS_JNI_MAX_EVENT_POINTERS ||
        event->history_count > BS_JNI_MAX_EVENT_HISTORY ||
        event->axis_count > BS_JNI_MAX_EVENT_AXES ||
        event->historical_axis_count > BS_JNI_MAX_EVENT_HISTORICAL_AXES ||
        event->down_time_ms < 0 ||
        event->event_time_ms < event->down_time_ms ||
        !bs_jni_finite_motion_float(event->x_precision) ||
        !bs_jni_finite_motion_float(event->y_precision) ||
        event->x_precision < 0 || event->y_precision < 0)
        return 0;
    for (size_t i = 0; i < event->pointer_count; ++i) {
        const BSJNIObservedMotionPointer *pointer = &event->pointers[i];
        if (pointer->pointer_id < 0 ||
            !bs_jni_finite_motion_float(pointer->raw_x) ||
            !bs_jni_finite_motion_float(pointer->raw_y)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (pointer->pointer_id == event->pointers[j].pointer_id) return 0;
    }
    int64_t previous = event->down_time_ms;
    for (size_t i = 0; i < event->history_count; ++i) {
        const int64_t stamp = event->historical_event_times_ms[i];
        if (stamp < previous || stamp > event->event_time_ms) return 0;
        previous = stamp;
    }
    for (size_t i = 0; i < event->axis_count; ++i) {
        const BSJNIObservedMotionAxis *axis = &event->axes[i];
        if (axis->axis < 0 || axis->pointer_index < 0 ||
            (size_t)axis->pointer_index >= event->pointer_count ||
            !bs_jni_finite_motion_float(axis->value)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (axis->axis == event->axes[j].axis &&
                axis->pointer_index == event->axes[j].pointer_index) return 0;
    }
    for (size_t i = 0; i < event->historical_axis_count; ++i) {
        const BSJNIObservedHistoricalMotionAxis *axis =
            &event->historical_axes[i];
        if (axis->axis < 0 || axis->pointer_index < 0 ||
            (size_t)axis->pointer_index >= event->pointer_count ||
            axis->history_index < 0 ||
            (size_t)axis->history_index >= event->history_count ||
            !bs_jni_finite_motion_float(axis->value)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (axis->axis == event->historical_axes[j].axis &&
                axis->pointer_index == event->historical_axes[j].pointer_index &&
                axis->history_index == event->historical_axes[j].history_index)
                return 0;
    }
    return 1;
}

static int bs_jni_valid_key_observation(const BSJNIObservedKeyEvent *event) {
    return event && event->down_time_ms >= 0 &&
           event->event_time_ms >= event->down_time_ms &&
           event->repeat_count >= 0 && event->key_code >= 0 &&
           event->scan_code >= 0 && event->unicode_char >= 0 &&
           event->unicode_char <= 0x10ffff;
}

void *bs_jni_motion_event_from_observation(
    const BSJNIObservedMotionEvent *event) {
    if (!g_initialized || !bs_jni_valid_motion_observation(event)) {
        bs_jni_error("invalid/unobserved MotionEvent snapshot");
        return NULL;
    }
    BSJNIObservedMotionEvent *copy = malloc(sizeof *copy);
    if (!copy) {
        bs_jni_error("MotionEvent observation allocation failed");
        return NULL;
    }
    *copy = *event;
    BSJNIObject *object = bs_jni_object_new(BS_JNI_OBJECT_MOTION_EVENT,
                                           BS_MOTION);
    if (!object) { free(copy); return NULL; }
    object->motion_event = copy;
    return object;
}

void *bs_jni_key_event_from_observation(const BSJNIObservedKeyEvent *event) {
    if (!g_initialized || !bs_jni_valid_key_observation(event)) {
        bs_jni_error("invalid/unobserved KeyEvent snapshot");
        return NULL;
    }
    BSJNIObservedKeyEvent *copy = malloc(sizeof *copy);
    if (!copy) {
        bs_jni_error("KeyEvent observation allocation failed");
        return NULL;
    }
    *copy = *event;
    BSJNIObject *object = bs_jni_object_new(BS_JNI_OBJECT_KEY_EVENT, BS_KEY);
    if (!object) { free(copy); return NULL; }
    object->key_event = copy;
    return object;
}

int bs_jni_motion_event_update(void *event_handle,
                               const BSJNIObservedMotionEvent *event) {
    BSJNIObject *object = bs_jni_known_object(event_handle);
    if (!object || object->kind != BS_JNI_OBJECT_MOTION_EVENT ||
        !object->motion_event || !bs_jni_valid_motion_observation(event)) {
        bs_jni_error("invalid MotionEvent refresh");
        return BS_JNI_ERR;
    }
    *object->motion_event = *event;
    return BS_JNI_OK;
}

int bs_jni_key_event_update(void *event_handle,
                            const BSJNIObservedKeyEvent *event) {
    BSJNIObject *object = bs_jni_known_object(event_handle);
    if (!object || object->kind != BS_JNI_OBJECT_KEY_EVENT ||
        !object->key_event || !bs_jni_valid_key_observation(event)) {
        bs_jni_error("invalid KeyEvent refresh");
        return BS_JNI_ERR;
    }
    *object->key_event = *event;
    return BS_JNI_OK;
}

static BSJNIObject *bs_jni_string_for_utf(const char *text) {
    if (!text)
        return NULL;
    /* NewStringUTF creates a *new* Java String even when contents compare
     * equal; interning by bytes violated JNI IsSameObject identity. */
    size_t length = strlen(text);
    if (length < BS_JNI_STRING_BYTES || !g_lenient)
        return bs_jni_object_new(BS_JNI_OBJECT_STRING, text);
    /* Lenient: truncate at a UTF-8 character boundary. */
    char truncated[BS_JNI_STRING_BYTES];
    length = BS_JNI_STRING_BYTES - 1;
    while (length && ((unsigned char)text[length] & 0xc0) == 0x80) --length;
    memcpy(truncated, text, length);
    truncated[length] = '\0';
    bs_jni_log("[jni:lenient] string truncated to %zu bytes", length);
    return bs_jni_object_new(BS_JNI_OBJECT_STRING, truncated);
}

static const char *bs_jni_object_class(const void *ref) {
    BSJNIObject *object = bs_jni_known_object(ref);
    if (object) {
        switch (object->kind) {
            case BS_JNI_OBJECT_ACTIVITY:
                return "com/unity3d/player/UnityPlayerGameActivity";
            case BS_JNI_OBJECT_ASSET_MANAGER:
                return "android/content/res/AssetManager";
            case BS_JNI_OBJECT_STRING:
                return "java/lang/String";
            case BS_JNI_OBJECT_NO_SUCH_FIELD:
                return "java/lang/NoSuchFieldError";
            case BS_JNI_OBJECT_NO_SUCH_METHOD:
                return "java/lang/NoSuchMethodError";
            case BS_JNI_OBJECT_NO_CLASS_DEF:
                return "java/lang/NoClassDefFoundError";
            case BS_JNI_OBJECT_UNSUPPORTED_ERROR:
                return "java/lang/UnsupportedOperationException";
            case BS_JNI_OBJECT_INDEX_ERROR:
                return "java/lang/IndexOutOfBoundsException";
            case BS_JNI_OBJECT_CONFIGURATION:
                return BS_CONFIG;
            case BS_JNI_OBJECT_INSETS:
                return BS_INSETS;
            case BS_JNI_OBJECT_LOCALE_LIST:
                return BS_LOCALE_LIST;
            case BS_JNI_OBJECT_LOCALE:
                return BS_LOCALE;
            case BS_JNI_OBJECT_SURFACE:
                return "android/view/Surface";
            case BS_JNI_OBJECT_THROWABLE:
                return object->throwable_class;
            case BS_JNI_OBJECT_GAME_ACTIVITY_HOST:
                return BS_GAME_ACTIVITY;
            case BS_JNI_OBJECT_MOTION_EVENT:
                return BS_MOTION;
            case BS_JNI_OBJECT_KEY_EVENT:
                return BS_KEY;
            case BS_JNI_OBJECT_UNITY_PLAYER_HOST:
                return BS_UNITY_PLAYER_GAME_ACTIVITY;
            case BS_JNI_OBJECT_CLASS_LOADER:
                return "dalvik/system/PathClassLoader";
            case BS_JNI_OBJECT_HOST_INSTANCE:
                return object->text;
            case BS_JNI_OBJECT_FILE:
                return "java/io/File";
            case BS_JNI_OBJECT_ARRAY:
                return object->text;
            case BS_JNI_OBJECT_REFLECTED:
                return object->member_kind == 'F' ? "java/lang/reflect/Field" :
                       object->member_kind == 'C' ? "java/lang/reflect/Constructor" :
                                                    "java/lang/reflect/Method";
        }
    }
    if (bs_jni_known_class(ref))
        return "java/lang/Class";
    return NULL;
}

static int bs_jni_valid_version(int32_t version) {
    return version == 0x00010002 || version == 0x00010004 ||
           version == BS_JNI_VERSION_1_6;
}

/* JNIEnv slot 4 */
static int32_t bs_jni_get_version(void *env) {
    if (!bs_jni_require_env(env, "GetVersion")) return BS_JNI_ERR;
    return BS_JNI_VERSION_1_6;
}

/* JNIEnv slot 6. This creates a SYMBOLIC class identity for registration.
 * No class methods, static fields, JNI reflection or bytecode are invented.
 */
static void *bs_jni_find_class(void *env, const char *name) {
    if (!name)
        bs_jni_log("[jni:trace] FindClass(NULL) origin=%p env=%p "
                   "attached=%d pending=%d",
                   __builtin_return_address(0), env, g_attached,
                   g_missing_member_pending);
    if (!bs_jni_require_env(env, "FindClass")) return NULL;
    if (g_missing_member_pending) return NULL;
    return bs_jni_class_for_name(name);
}
/* JNI lookup failures carry a diagnostic NoSuchField/MethodError token.
 * The bridge cannot execute Java bytecode, but it must honor the JNI
 * exception-observation ABI used in GameActivity_register's failure path.
 */
static void *bs_jni_exception_occurred(void *env) {
    if (!bs_jni_require_env(env, "ExceptionOccurred")) return NULL;
    if (!g_missing_member_pending) return NULL;
    if (!g_live_exception) {
        BSJNIObject *saved = bs_jni_object_new(
            BS_JNI_OBJECT_THROWABLE, g_missing_member_exception.text);
        if (!saved) return NULL;
        /* Preserve the exact pending Java exception type, not merely its
         * diagnostic message or a generic Throwable class. */
        saved->kind = g_missing_member_exception.kind;
        memcpy(saved->throwable_class,
               g_missing_member_exception.throwable_class,
               sizeof saved->throwable_class);
        g_live_exception = saved;
    }
    return bs_jni_new_reference(env, g_live_exception, 1);
}

/* Original GameActivity registration uses synthetic method metadata but
 * exception state itself is real JNI-visible per-thread state. Only standard
 * Java runtime errors that require no custom bytecode can be raised here. */
static int bs_jni_builtin_throwable(const char *name) {
    static const char *const builtins[] = {
        "java/lang/RuntimeException",
        "java/lang/IllegalStateException",
        "java/lang/IllegalArgumentException",
        "java/lang/NullPointerException",
        "java/lang/UnsupportedOperationException",
        "java/lang/IndexOutOfBoundsException",
        "java/lang/NoSuchMethodError",
        "java/lang/NoSuchFieldError",
        "java/lang/NoClassDefFoundError",
        "java/lang/OutOfMemoryError",
        "java/lang/ClassNotFoundException",
        "java/lang/NoSuchMethodException",
        "java/lang/SecurityException"
    };
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; ++i)
        if (name && !strcmp(name, builtins[i])) return 1;
    return 0;
}

static int32_t bs_jni_throw(void *env, void *throwable) {
    if (!bs_jni_require_env(env, "Throw")) return BS_JNI_ERR;
    BSJNIObject *object = bs_jni_known_object(throwable);
    if (!object || !bs_jni_builtin_throwable(bs_jni_object_class(object))) {
        bs_jni_error("Throw requires a known Java exception handle");
        return BS_JNI_ERR;
    }
    if (g_missing_member_pending) return BS_JNI_ERR;
    g_missing_member_exception = *object;
    g_live_exception = object == &g_missing_member_exception ? NULL : object;
    g_missing_member_pending = 1;
    return BS_JNI_OK;
}

static int32_t bs_jni_throw_new(void *env, void *klass_ref,
                                 const char *message) {
    if (!bs_jni_require_env(env, "ThrowNew")) return BS_JNI_ERR;
    if (g_missing_member_pending) return BS_JNI_ERR;
    BSJNIClass *klass = bs_jni_known_class(klass_ref);
    if (!klass || !bs_jni_builtin_throwable(klass->name)) {
        bs_jni_error("ThrowNew requires a builtin Java exception class");
        return BS_JNI_ERR;
    }
    const char *source = message ? message : "";
    size_t length = strlen(source);
    if (length >= sizeof g_missing_member_exception.text) {
        bs_jni_error("ThrowNew exception message exceeds JNI capacity");
        return BS_JNI_ERR;
    }
    memset(&g_missing_member_exception, 0, sizeof g_missing_member_exception);
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_THROWABLE;
    memcpy(g_missing_member_exception.throwable_class, klass->name,
           strlen(klass->name) + 1);
    memcpy(g_missing_member_exception.text, source, length + 1);
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_log("[jni] ThrowNew %s: %s", klass->name, source);
    return BS_JNI_OK;
}
/* JNI slot 16, not slot 18. Per the JNI contract ExceptionDescribe clears
 * the pending exception after reporting it. A prior registration failure
 * (which is not a Java exception) is still available via last_error. */
static void bs_jni_exception_describe(void *env) {
    if (!bs_jni_require_env(env, "ExceptionDescribe")) return;
    bs_jni_log("[jni] ExceptionDescribe: %s",
               g_missing_member_pending ? g_missing_member_exception.text :
               g_last_error[0] ? g_last_error : "no pending Java exception");
    g_missing_member_pending = 0;
    g_live_exception = NULL;
}
static void bs_jni_exception_clear(void *env) {
    if (!bs_jni_require_env(env, "ExceptionClear")) return;
    g_missing_member_pending = 0;
    g_live_exception = NULL;
}
static uint8_t bs_jni_exception_check(void *env) {
    if (!bs_jni_require_env(env, "ExceptionCheck")) return 0;
    return g_missing_member_pending ? 1 : 0;
}

/* JNI slot 18 is FatalError(JNIEnv*, const char*), always a hard stop. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void bs_jni_fatal_error(void *env, const char *message) {
    (void)env;
    bs_jni_error("JNI FatalError: %s",
                 message ? message : "(no message)");
    abort();
}

/* Local frames scope the local references created after the push: explicit
 * reference records (NewLocalRef, ExceptionOccurred, PopLocalFrame) and
 * transient value objects (strings, arrays, files) the bridge created for
 * this thread. Permanent symbolic class/object handles are outside any
 * frame, as in ART where such handles would be long-lived global objects.
 * Frame depth is per thread, like the JNI local reference table. */
#define BS_JNI_MAX_LOCAL_FRAMES 64
static void *bs_jni_new_reference(void *env, void *obj, uint8_t kind);
static void *bs_jni_canonical_reference(void *obj);

static int32_t bs_jni_push_local_frame(void *env, int32_t capacity) {
    if (!bs_jni_require_env(env, "PushLocalFrame")) return BS_JNI_ERR;
    if (capacity < 0) {
        bs_jni_error("PushLocalFrame(%d) negative capacity", capacity);
        return BS_JNI_ERR;
    }
    if (g_local_frame_count == BS_JNI_MAX_LOCAL_FRAMES) {
        bs_jni_error("PushLocalFrame(%d): local frame stack exhausted",
                     capacity);
        return BS_JNI_ERR;
    }
    ++g_local_frame_count;
    return BS_JNI_OK;
}
static void *bs_jni_pop_local_frame(void *env, void *result) {
    if (!bs_jni_require_env(env, "PopLocalFrame")) return NULL;
    /* Resolve the survivor before its frame-local alias is released. */
    void *target = result ? bs_jni_canonical_reference(result) : NULL;
    if (!g_local_frame_count) {
        bs_jni_missing_host_value("PopLocalFrame", NULL, "no pushed frame");
        return NULL;
    }
    const uint32_t depth = g_local_frame_count--;
    const void *owner = (const void *)&g_thread_env_table;
    /* Create the survivor in the outer frame before releasing this frame's
     * references, so it never reuses (aliases) a slot of a popped ref. */
    void *survived = target ? bs_jni_new_reference(env, target, 1) : NULL;
    bs_jni_lock();
    BSJNIObject *survivor = (BSJNIObject *)(uintptr_t)bs_jni_pool_member(
        target, g_objects, sizeof g_objects[0], g_object_count);
    if (survivor) ++survivor->refs; /* hold across the release below */
    for (size_t i = 0; i < g_reference_count; ++i) {
        BSJNIReference *ref = &g_references[i];
        if (ref->active && ref->type == 1 && ref->frame >= depth &&
            ref->local_env_owner == owner)
            bs_jni_reference_release_locked(ref);
    }
    for (size_t i = 0; i < g_object_count; ++i) {
        BSJNIObject *object = &g_objects[i];
        if (object->magic == BS_JNI_MAGIC_OBJECT &&
            !object->raw_released && object->local_frame >= depth &&
            object->local_owner == owner &&
            bs_jni_object_transient(object)) {
            object->raw_released = 1;
            bs_jni_object_maybe_free_locked(object);
        }
    }
    bs_jni_unlock();
    if (survivor) {
        bs_jni_lock();
        if (survivor->refs) --survivor->refs;
        bs_jni_object_maybe_free_locked(survivor);
        bs_jni_unlock();
    }
    return survived;
}
static void *bs_jni_canonical_reference(void *obj) {
    if (!obj) return NULL;
    BSJNIReference *ref = bs_jni_reference_record(obj);
    if (ref) return bs_jni_reference_live(ref) ? (void *)ref->target : NULL;
    return (bs_jni_known_class(obj) || bs_jni_known_object(obj)) ? obj : NULL;
}
static void *bs_jni_new_reference(void *env, void *obj, uint8_t kind) {
    if (!bs_jni_require_env(env, "New JNI reference")) return NULL;
    if (!obj) return NULL;
    void *target = bs_jni_canonical_reference(obj);
    if (!target) {
        bs_jni_missing_host_value("New JNI reference", NULL, "invalid handle");
        return NULL;
    }
    bs_jni_lock();
    BSJNIReference *record;
    if (g_reference_free_count) {
        record = &g_references[g_reference_free[g_reference_free_head]];
        g_reference_free_head = (g_reference_free_head + 1) % BS_JNI_MAX_REFS;
        --g_reference_free_count;
    } else if (g_reference_count == BS_JNI_MAX_REFS) {
        bs_jni_unlock();
        bs_jni_missing_host_value("New JNI reference", NULL, "table exhausted");
        return NULL;
    } else {
        record = &g_references[g_reference_count];
        ++g_reference_count;
    }
    record->target = target;
    record->local_env_owner = kind == 1 ?
        (const void *)&g_thread_env_table : NULL;
    record->local_generation = kind == 1 ? g_attach_generation : 0;
    record->frame = kind == 1 ? g_local_frame_count : 0;
    record->type = kind;
    BSJNIObject *object = (BSJNIObject *)(uintptr_t)bs_jni_pool_member(
        target, g_objects, sizeof g_objects[0], g_object_count);
    if (object) ++object->refs;
    record->active = 1;
    bs_jni_unlock();
    return record;
}
static void *bs_jni_new_global_ref(void *env, void *obj) {
    return bs_jni_new_reference(env, obj, 2);
}
static void *bs_jni_new_local_ref(void *env, void *obj) {
    return bs_jni_new_reference(env, obj, 1);
}
static void *bs_jni_new_weak_global_ref(void *env, void *obj) {
    return bs_jni_new_reference(env, obj, 3);
}
static void bs_jni_delete_reference(void *env, void *obj, uint8_t kind) {
    if (!bs_jni_require_env(env, "Delete JNI reference")) return;
    if (!obj) return;
    BSJNIReference *ref = bs_jni_reference_record(obj);
    if (ref && ref->active && ref->type == kind &&
        (kind != 1 || ref->local_env_owner ==
            (const void *)&g_thread_env_table) &&
        (kind != 1 || ref->local_generation == g_attach_generation)) {
        bs_jni_lock();
        bs_jni_reference_release_locked(ref);
        bs_jni_unlock();
        return;
    }
    /* Primitive symbolic handles are persistent; destroying their storage
     * would invalidate GameActivity registration metadata. Transient value
     * objects (strings, arrays, files) are reclaimed once no reference
     * record targets them. */
    if (kind == 1) {
        BSJNIObject *object = ref ? NULL : bs_jni_known_object(obj);
        if (object) {
            bs_jni_lock();
            if (bs_jni_object_transient(object)) {
                object->raw_released = 1;
                bs_jni_object_maybe_free_locked(object);
            }
            bs_jni_unlock();
            return;
        }
        if (bs_jni_known_class(obj)) return;
    }
    if (kind == 2 && (obj == g_activity || obj == g_asset_manager)) return;
    bs_jni_missing_host_value("Delete JNI reference", NULL, "invalid handle");
}
static void bs_jni_delete_global_ref(void *env, void *obj) {
    bs_jni_delete_reference(env, obj, 2);
}
static void bs_jni_delete_local_ref(void *env, void *obj) {
    bs_jni_delete_reference(env, obj, 1);
}
static void bs_jni_delete_weak_global_ref(void *env, void *obj) {
    bs_jni_delete_reference(env, obj, 3);
}
static uint8_t bs_jni_is_same_object(void *env, void *one, void *two) {
    if (!bs_jni_require_env(env, "IsSameObject")) return 0;
    if (!one || !two) {
        /* A cleared weak global compares equal to NULL. */
        void *live = one ? bs_jni_canonical_reference(one)
                         : two ? bs_jni_canonical_reference(two) : NULL;
        return !live;
    }
    void *lhs = bs_jni_canonical_reference(one);
    void *rhs = bs_jni_canonical_reference(two);
    if (!lhs || !rhs) {
        bs_jni_missing_host_value("IsSameObject", NULL, "invalid handle");
        return 0;
    }
    return lhs == rhs;
}
static int32_t bs_jni_ensure_local_capacity(void *env, int32_t capacity) {
    if (!bs_jni_require_env(env, "EnsureLocalCapacity")) return BS_JNI_ERR;
    if (capacity < 0 || (size_t)capacity >
            BS_JNI_MAX_REFS - g_reference_count + g_reference_free_count) {
        bs_jni_error("EnsureLocalCapacity(%d) exceeds reference table",
                     capacity);
        return BS_JNI_ERR;
    }
    return BS_JNI_OK;
}

static void *bs_jni_get_object_class(void *env, void *obj) {
    if (!bs_jni_require_env(env, "GetObjectClass")) return NULL;
    const char *class_name = bs_jni_object_class(obj);
    if (!class_name) {
        bs_jni_log("[jni:trace] GetObjectClass(%p) unrecognized origin=%p "
                   "pending=%d", obj, __builtin_return_address(0),
                   g_missing_member_pending);
        bs_jni_missing_host_value("GetObjectClass", NULL,
                                  "unrecognized Java handle");
        return NULL;
    }
    return bs_jni_class_for_name(class_name);
}

static uint8_t bs_jni_is_instance_of(void *env, void *obj, void *cls) {
    if (!bs_jni_require_env(env, "IsInstanceOf")) return 0;
    BSJNIClass *requested = bs_jni_known_class(cls);
    if (!requested) {
        bs_jni_missing_host_value("IsInstanceOf", NULL, "invalid class");
        return 0;
    }
    if (!obj) return 1; /* JNI null is assignable to every reference class. */
    const char *actual = bs_jni_object_class(obj);
    if (!actual) {
        bs_jni_missing_host_value("IsInstanceOf", requested->name,
                                  "unrecognized object");
        return 0;
    }
    if (strcmp(requested->name, actual) == 0 ||
        strcmp(requested->name, "java/lang/Object") == 0)
        return 1;
    const void *receiver = bs_jni_canonical_reference(obj);
    if ((receiver == g_activity || receiver == g_gameactivity_receiver) &&
        (!strcmp(requested->name, "android/app/Activity") ||
         !strcmp(requested->name, "android/content/Context")))
        return 1;
    if (!strcmp(actual, BS_UNITY_PLAYER_GAME_ACTIVITY) &&
        !strcmp(requested->name, BS_UNITY_PLAYER))
        return 1;
    return bs_jni_class_extends(actual, requested->name) ? 1 : 0;
}

/* JNIEnv slot 10. Superclass of a modeled class: original APK class_defs or
 * the AOSP framework declaration. java/lang/Object yields NULL. Interfaces
 * are not modeled as classes here. Unmodeled classes fail closed. */
static void *bs_jni_get_superclass(void *env, void *class_ref) {
    if (!bs_jni_require_env(env, "GetSuperclass")) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    if (!klass) {
        bs_jni_error("GetSuperclass(%p) invalid class origin=%p", class_ref,
                     __builtin_return_address(0));
        return NULL;
    }
    char parent[BS_JNI_CLASS_BYTES];
    if (!bs_jni_superclass_of(klass->name, parent, sizeof parent)) {
        bs_jni_missing_host_value("GetSuperclass", klass->name,
                                  "(unmodeled class hierarchy)");
        return NULL;
    }
    return parent[0] ? bs_jni_class_for_name(parent) : NULL;
}

/* The small Activity model permits the real Unity bootstrap to request an
 * asset manager and package identity. Everything beyond these exact methods
 * fails explicitly; no fake Java application logic runs.
 */
static BSJNIMethodID *bs_jni_get_method(void *env, void *class_ref,
                                        const char *method,
                                        const char *signature) {
    if (!bs_jni_require_env(env, "GetMethodID")) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    if (!klass || !method || !signature || g_missing_member_pending) {
        if (!g_missing_member_pending) {
            bs_jni_log("[jni:trace] GetMethodID(class=%p) invalid origin=%p",
                       class_ref, __builtin_return_address(0));
            bs_jni_no_such_member("method", NULL, method, signature);
        }
        return NULL;
    }
    BSJNIHostMethod *host = bs_jni_find_host_method(klass->name, method,
                                                    signature, 0);
    if (host) return (BSJNIMethodID *)(void *)host;
    int is_context =
        !strcmp(klass->name, "com/unity3d/player/UnityPlayerGameActivity") ||
        (g_gameactivity_receiver && !strcmp(klass->name, BS_GAME_ACTIVITY)) ||
        !strcmp(klass->name, "android/app/Activity") ||
        !strcmp(klass->name, "android/content/Context");
    BSJNIMethodKind kind;
    if (is_context && !strcmp(method, "getAssets") &&
        !strcmp(signature, "()Landroid/content/res/AssetManager;"))
        kind = BS_JNI_METHOD_ASSETS;
    else if (is_context && !strcmp(method, "getApplicationContext") &&
             !strcmp(signature, "()Landroid/content/Context;"))
        kind = BS_JNI_METHOD_CONTEXT;
    else if (is_context && !strcmp(method, "getPackageName") &&
             !strcmp(signature, "()Ljava/lang/String;"))
        kind = BS_JNI_METHOD_PACKAGE;
    else {
        const BSJNIKnownMember *known = bs_jni_lookup_gameactivity_member(
            klass->name, method, signature, BS_JNI_INSTANCE_METHOD);
        if (known) {
            /* Stable opaque metadata only. bs_jni_call_object_noargs()
             * intentionally cannot invoke these methods. */
            return (BSJNIMethodID *)(uintptr_t)known;
        }
        BSJNIHostMethod *stub = bs_jni_lenient_method(klass->name, method,
                                                      signature, 0);
        if (stub) return (BSJNIMethodID *)(void *)stub;
        bs_jni_no_such_member("method", klass->name, method, signature);
        return NULL;
    }
    bs_jni_lock();
    for (size_t i = 0; i < g_method_count; ++i)
        if (g_methods[i].method == kind &&
            !strcmp(g_methods[i].class_name, klass->name)) {
            BSJNIMethodID *found = &g_methods[i];
            bs_jni_unlock();
            return found;
        }
    if (g_method_count == BS_JNI_MAX_METHOD_IDS) {
        bs_jni_unlock();
        bs_jni_no_such_member("method", klass->name, method, signature);
        return NULL;
    }
    BSJNIMethodID *id = &g_methods[g_method_count];
    id->magic = UINT32_C(0x4a4d4554);
    id->method = kind;
    memcpy(id->class_name, klass->name, strlen(klass->name) + 1);
    ++g_method_count;
    bs_jni_unlock();
    return id;
}

/* JNI slot 94. Only original GameActivity_register members receive jfieldIDs.
 * Numeric reads require independently observed Configuration/Insets objects;
 * symbolic IDs alone never supply a value. */
static void *bs_jni_get_field(void *env, void *class_ref,
                              const char *name, const char *signature) {
    if (!bs_jni_require_env(env, "GetFieldID")) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    if (g_missing_member_pending)
        return NULL;
    const BSJNIKnownMember *known = bs_jni_lookup_gameactivity_member(
        klass ? klass->name : NULL, name, signature, BS_JNI_INSTANCE_FIELD);
    if (known)
        return (void *)(uintptr_t)known;
    BSJNILenientField *stub = klass ?
        bs_jni_lenient_field_id(klass->name, name, signature, 0) : NULL;
    if (stub) return stub;
    bs_jni_no_such_member("field", klass ? klass->name : NULL, name, signature);
    return NULL;
}

/* JNI slot 113. The nine WindowInsetsCompat.Type API entrypoints in the
 * original libgame registration path are the entire supported set. Pure
 * constant integer calls are implemented at CallStaticIntMethod/V/A. */
static void *bs_jni_get_static_method(void *env, void *class_ref,
                                     const char *name, const char *signature) {
    if (!bs_jni_require_env(env, "GetStaticMethodID")) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    if (g_missing_member_pending)
        return NULL;
    if (klass && name && signature) {
        BSJNIHostMethod *host = bs_jni_find_host_method(klass->name, name,
                                                        signature, 1);
        if (host) return host;
    }
    const BSJNIKnownMember *known = bs_jni_lookup_gameactivity_member(
        klass ? klass->name : NULL, name, signature, BS_JNI_STATIC_METHOD);
    if (known)
        return (void *)(uintptr_t)known;
    BSJNIHostMethod *stub = klass ?
        bs_jni_lenient_method(klass->name, name, signature, 1) : NULL;
    if (stub) return stub;
    bs_jni_no_such_member("method", klass ? klass->name : NULL, name, signature);
    return NULL;
}

static int bs_jni_is_unityplayer_class(const char *name) {
    return name && (!strcmp(name, BS_UNITY_PLAYER) ||
                    !strcmp(name, BS_UNITY_PLAYER_ACTIVITY) ||
                    !strcmp(name, BS_UNITY_PLAYER_GAME_ACTIVITY));
}

/* JNI slot 144. The two original DEX subclasses
 * UnityPlayerForActivityOrService and UnityPlayerForGameActivity inherit
 * UnityPlayer's two static fields. A metadata ID does not execute either
 * constructor or establish any field's runtime contents. */
static void *bs_jni_get_static_field(void *env, void *class_ref,
                                    const char *name, const char *signature) {
    if (!bs_jni_require_env(env, "GetStaticFieldID")) return NULL;
    if (g_missing_member_pending) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    if (klass && bs_jni_is_unityplayer_class(klass->name) &&
        name && signature) {
        for (size_t i = 0; i < sizeof g_unityplayer_static_fields /
                                sizeof g_unityplayer_static_fields[0]; ++i) {
            const BSJNIKnownMember *field = &g_unityplayer_static_fields[i];
            if (!strcmp(field->name, name) &&
                !strcmp(field->signature, signature))
                return (void *)(uintptr_t)field;
        }
    }
    BSJNILenientField *stub = klass ?
        bs_jni_lenient_field_id(klass->name, name, signature, 1) : NULL;
    if (stub) return stub;
    bs_jni_no_such_member("field", klass ? klass->name : NULL,
                          name, signature);
    return NULL;
}

/* JNI slot 145. Return a true local JNI reference only after the original
 * UnityPlayer Java constructor's corresponding static assignment is observed.
 * Class lookup / native registration alone never initializes these fields. */
static void *bs_jni_get_static_object_field(void *env, void *class_ref,
                                            void *field_ref) {
    if (!bs_jni_require_env(env, "GetStaticObjectField")) return NULL;
    if (g_missing_member_pending) return NULL;
    BSJNILenientField *lenient = bs_jni_lenient_field(field_ref);
    if (lenient) return lenient->is_static ? lenient->value.l : NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    const BSJNIKnownMember *field = NULL;
    for (size_t i = 0; i < sizeof g_unityplayer_static_fields /
                            sizeof g_unityplayer_static_fields[0]; ++i)
        if (field_ref == &g_unityplayer_static_fields[i]) {
            field = &g_unityplayer_static_fields[i];
            break;
        }
    if (!klass || !bs_jni_is_unityplayer_class(klass->name) || !field) {
        bs_jni_no_such_member("field", klass ? klass->name : NULL,
                              field ? field->name : "(invalid field ID)",
                              field ? field->signature : NULL);
        return NULL;
    }
    if (field == &g_unityplayer_static_fields[0] &&
        bs_jni_unityplayer_has_current_activity())
        return bs_jni_new_reference(env, g_unityplayer_current_activity, 1);
    if (field == &g_unityplayer_static_fields[1] &&
        g_unityplayer_current_context &&
        g_unityplayer_current_context == g_gameactivity_receiver &&
        g_gameactivity_configuration &&
        bs_jni_is_backed_surface(env, g_gameactivity_surface))
        return bs_jni_new_reference(env, g_unityplayer_current_context, 1);

    bs_jni_missing_host_value("GetStaticObjectField", BS_UNITY_PLAYER,
                              field->name);
    return NULL;
}

/* Validate opaque IDs by address equality, never dereference untrusted guest
 * jfieldID/jmethodID pointers. The allowlist itself has permanent storage. */
static const BSJNIKnownMember *bs_jni_member_from_id(
    const void *reference, BSJNIMemberType type) {
    for (size_t i = 0;
         i < sizeof g_gameactivity_members / sizeof g_gameactivity_members[0];
         ++i) {
        const BSJNIKnownMember *candidate = &g_gameactivity_members[i];
        if (candidate->type == type && reference == candidate)
            return candidate;
    }
    return NULL;
}

static const BSJNIObservedCell *bs_jni_observed_field(
    void *object_ref, void *field_ref, const char *expected_signature) {
    if (g_missing_member_pending) return NULL;
    BSJNIObject *object = bs_jni_known_object(object_ref);
    const BSJNIKnownMember *field = bs_jni_member_from_id(
        field_ref, BS_JNI_INSTANCE_FIELD);
    const char *class_name = object ? bs_jni_object_class(object) : NULL;
    if (!object || !field || !class_name ||
        (object->kind != BS_JNI_OBJECT_CONFIGURATION &&
         object->kind != BS_JNI_OBJECT_INSETS) ||
        strcmp(field->class_name, class_name) ||
        strcmp(field->signature, expected_signature)) {
        bs_jni_missing_host_value("GetField", class_name,
                                  field ? field->name : "(unknown field ID)");
        return NULL;
    }
    for (size_t i = 0; i < object->observed_count; ++i) {
        if (object->observed[i].member == field)
            return &object->observed[i];
    }
    bs_jni_missing_host_value("GetField", class_name, field->name);
    return NULL;
}

/* Actual JNI 1.6 slots 100 and 102. Unknown values return the JNI default
 * only while a pending UnsupportedOperationException signals FAILURE.
 * No arbitrary Configuration/Insets zero values are synthesized. */
static int32_t bs_jni_get_int_field(void *env, void *object, void *field) {
    if (!bs_jni_require_env(env, "GetIntField")) return 0;
    BSJNILenientField *lenient = bs_jni_lenient_field(field);
    if (lenient) return lenient->seeded ? lenient->value.i : 0;
    const BSJNIObservedCell *cell = bs_jni_observed_field(object, field, "I");
    return cell ? cell->integer_value : 0;
}

static float bs_jni_get_float_field(void *env, void *object, void *field) {
    if (!bs_jni_require_env(env, "GetFloatField")) return 0.0f;
    if (bs_jni_lenient_field(field)) return 0.0f;
    const BSJNIObservedCell *cell = bs_jni_observed_field(object, field, "F");
    return cell ? cell->float_value : 0.0f;
}

/* AndroidX WindowInsetsCompat.Type's public bitmasks. These methods are
 * pure no-argument integer constants from AndroidX: no Java object, window,
 * insets measurement, or Activity lifecycle is required to evaluate them.
 * The guest's actual GetStaticMethodID must match the original signature. */
static int32_t bs_jni_window_insets_type_mask(const char *name) {
    if (!strcmp(name, "statusBars")) return 1 << 0;
    if (!strcmp(name, "navigationBars")) return 1 << 1;
    if (!strcmp(name, "captionBar")) return 1 << 2;
    if (!strcmp(name, "ime")) return 1 << 3;
    if (!strcmp(name, "systemGestures")) return 1 << 4;
    if (!strcmp(name, "mandatorySystemGestures")) return 1 << 5;
    if (!strcmp(name, "tappableElement")) return 1 << 6;
    if (!strcmp(name, "displayCutout")) return 1 << 7;
    if (!strcmp(name, "systemBars")) return (1 << 0) | (1 << 1) | (1 << 2);
    return 0; /* Not reachable after exact static-method ID validation. */
}

static int32_t bs_jni_call_static_int_noargs(void *class_ref,
                                              void *method_ref) {
    if (g_missing_member_pending) return 0;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    const BSJNIKnownMember *member = bs_jni_member_from_id(
        method_ref, BS_JNI_STATIC_METHOD);
    if (!klass || !member || strcmp(klass->name, BS_INSET_TYPE) ||
        strcmp(member->class_name, klass->name) ||
        strcmp(member->signature, "()I")) {
        bs_jni_missing_host_value("CallStaticIntMethod",
                                  klass ? klass->name : NULL,
                                  member ? member->name : NULL);
        return 0;
    }
    return bs_jni_window_insets_type_mask(member->name);
}

/* JNI slots 129/130/131. The only supported calls have ()I signatures,
 * so reading varargs or a jvalue array would itself be incorrect. */
static int32_t bs_jni_call_static_int(void *env, void *class_ref,
                                     void *method_ref, ...) {
    if (!bs_jni_require_env(env, "CallStaticIntMethod")) return 0;
    return bs_jni_call_static_int_noargs(class_ref, method_ref);
}

static int32_t bs_jni_call_static_int_v(void *env, void *class_ref,
                                       void *method_ref, va_list args) {
    if (!bs_jni_require_env(env, "CallStaticIntMethodV")) return 0;
    (void)args;
    return bs_jni_call_static_int_noargs(class_ref, method_ref);
}

static int32_t bs_jni_call_static_int_a(void *env, void *class_ref,
                                       void *method_ref, const void *args) {
    if (!bs_jni_require_env(env, "CallStaticIntMethodA")) return 0;
    (void)args;
    return bs_jni_call_static_int_noargs(class_ref, method_ref);
}

static int bs_jni_locale_get_index_method(void *method_ref) {
    const BSJNIKnownMember *member = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    return member && !strcmp(member->class_name, BS_LOCALE_LIST) &&
           !strcmp(member->name, "get") &&
           !strcmp(member->signature, "(I)Ljava/util/Locale;");
}

static int bs_jni_object_int_argument_method(void *method_ref) {
    if (bs_jni_locale_get_index_method(method_ref)) return 1;
    const BSJNIKnownMember *member = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    return member && !strcmp(member->class_name, BS_GAME_ACTIVITY) &&
           !strcmp(member->name, "getWindowInsets") &&
           !strcmp(member->signature,
                   "(I)Landroidx/core/graphics/Insets;");
}

static void *bs_jni_host_insets(BSJNIObject *receiver, int waterfall,
                                int32_t type_mask) {
    const BSJNIGameActivityHostServices *host =
        &receiver->gameactivity_host;
    int32_t edges[4] = {0};
    int rc;
    if (waterfall) {
        if (!host->get_waterfall_insets) {
            bs_jni_missing_host_value("CallObjectMethod", BS_GAME_ACTIVITY,
                                      "getWaterfallInsets");
            return NULL;
        }
        rc = host->get_waterfall_insets(host->user, edges);
    } else {
        if (!host->get_window_insets) {
            bs_jni_missing_host_value("CallObjectMethod", BS_GAME_ACTIVITY,
                                      "getWindowInsets");
            return NULL;
        }
        rc = host->get_window_insets(host->user, type_mask, edges);
    }
    if (rc != 0 || g_missing_member_pending ||
        edges[0] < 0 || edges[1] < 0 || edges[2] < 0 || edges[3] < 0) {
        bs_jni_missing_host_value("CallObjectMethod", BS_GAME_ACTIVITY,
                                  waterfall ? "getWaterfallInsets" :
                                              "getWindowInsets");
        return NULL;
    }
    if (!edges[0] && !edges[1] && !edges[2] && !edges[3])
        return NULL; /* GameActivity returns null for absent insets. */
    /* Original AndroidX Insets fields use the order left/right/top/bottom.
     * The host contract's edge order is left/top/right/bottom. */
    const BSJNIObservedField fields[] = {
        {"left", "I", edges[0], 0.f},
        {"right", "I", edges[2], 0.f},
        {"top", "I", edges[1], 0.f},
        {"bottom", "I", edges[3], 0.f}
    };
    return bs_jni_object_from_observed_fields(BS_INSETS, fields, 4);
}

static void *bs_jni_call_object_observed(void *obj, void *method_ref,
                                        int has_index, int32_t index) {
    if (g_missing_member_pending) return NULL;
    BSJNIMethodID *id = bs_jni_known_method(method_ref);
    if (id) {
        void *receiver = bs_jni_canonical_reference(obj);
        if (!receiver ||
            (receiver != g_activity && receiver != g_gameactivity_receiver)) {
            bs_jni_missing_host_value("CallObjectMethod",
                                      bs_jni_object_class(obj),
                                      "(invalid context receiver)");
            return NULL;
        }
        switch (id->method) {
            case BS_JNI_METHOD_ASSETS:
                return g_asset_manager;
            case BS_JNI_METHOD_CONTEXT:
                return receiver;
            case BS_JNI_METHOD_PACKAGE:
                return bs_jni_string_for_utf("com.beatgames.beatsaber");
        }
    }
    BSJNIObject *receiver = bs_jni_known_object(obj);
    const BSJNIKnownMember *member = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    const char *class_name = receiver ? bs_jni_object_class(receiver) : NULL;
    if (!receiver || !member || !class_name ||
        strcmp(member->class_name, class_name)) {
        bs_jni_missing_host_value("CallObjectMethod", class_name,
                                  member ? member->name : "(invalid method)");
        return NULL;
    }
    if (receiver->kind == BS_JNI_OBJECT_GAME_ACTIVITY_HOST) {
        if (!strcmp(member->name, "getWindowInsets") && has_index)
            return bs_jni_host_insets(receiver, 0, index);
        if (!strcmp(member->name, "getWaterfallInsets") && !has_index)
            return bs_jni_host_insets(receiver, 1, 0);
    } else if (receiver->kind == BS_JNI_OBJECT_CONFIGURATION &&
        !strcmp(member->name, "getLocales")) {
        if (receiver->locale_list)
            return receiver->locale_list;
    } else if (receiver->kind == BS_JNI_OBJECT_LOCALE_LIST &&
               bs_jni_locale_get_index_method(method_ref)) {
        if (!has_index) {
            bs_jni_missing_host_value("CallObjectMethod",
                                      class_name, "get(index required)");
            return NULL;
        }
        if (index < 0 || (size_t)index >= receiver->locale_count) {
            bs_jni_locale_bad_index(index, receiver->locale_count);
            return NULL;
        }
        return receiver->locale_items[index];
    } else if (receiver->kind == BS_JNI_OBJECT_LOCALE && !has_index) {
        const char *value = NULL;
        if (!strcmp(member->name, "getLanguage"))
            value = receiver->locale.language;
        else if (!strcmp(member->name, "getScript"))
            value = receiver->locale.script;
        else if (!strcmp(member->name, "getCountry"))
            value = receiver->locale.country;
        else if (!strcmp(member->name, "getVariant"))
            value = receiver->locale.variant;
        if (value) {
            void *result = bs_jni_string_for_utf(value);
            if (result) return result;
        }
    }
    bs_jni_missing_host_value("CallObjectMethod", class_name, member->name);
    return NULL;
}

/* JNI slots 34/35/36; consume argument registers/va_list/jvalue precisely
 * for the single supported indexed LocaleList.get(I). No non-GameActivity
 * Java methods are called unless backed by observed host state. */
static void *bs_jni_call_object(void *env, void *obj, void *method, ...) {
    if (!bs_jni_require_env(env, "CallObjectMethod")) return NULL;
    if (!bs_jni_object_int_argument_method(method))
        return bs_jni_call_object_observed(obj, method, 0, 0);
    va_list args;
    va_start(args, method);
    int32_t index = va_arg(args, int);
    va_end(args);
    return bs_jni_call_object_observed(obj, method, 1, index);
}
static void *bs_jni_call_object_v(void *env, void *obj, void *method,
                                 va_list arguments) {
    if (!bs_jni_require_env(env, "CallObjectMethodV")) return NULL;
    if (!bs_jni_object_int_argument_method(method))
        return bs_jni_call_object_observed(obj, method, 0, 0);
    const int32_t index = va_arg(arguments, int);
    return bs_jni_call_object_observed(obj, method, 1, index);
}
static void *bs_jni_call_object_a(void *env, void *obj, void *method,
                                 const void *arguments) {
    if (!bs_jni_require_env(env, "CallObjectMethodA")) return NULL;
    if (!bs_jni_object_int_argument_method(method))
        return bs_jni_call_object_observed(obj, method, 0, 0);
    if (!arguments)
        return bs_jni_call_object_observed(obj, method, 0, 0);
    int32_t index;
    memcpy(&index, arguments, sizeof index);
    return bs_jni_call_object_observed(obj, method, 1, index);
}

static void bs_jni_input_bad_index(const char *method, int32_t index,
                                   size_t count) {
    if (g_missing_member_pending) return;
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_INDEX_ERROR;
    snprintf(g_missing_member_exception.text,
             sizeof g_missing_member_exception.text,
             "IndexOutOfBoundsException: %s(%d) count=%lu",
             method, index, (unsigned long)count);
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_error("%s", g_missing_member_exception.text);
}

static int bs_jni_input_arity(void *method_ref, const char *result_signature) {
    const BSJNIKnownMember *member = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    if (!member || (strcmp(member->class_name, BS_MOTION) &&
                    strcmp(member->class_name, BS_KEY))) return 0;
    const char *signature = member->signature;
    const char *end = strchr(signature, ')');
    if (!end || strcmp(end + 1, result_signature)) return 0;
    if (end - signature == 1) return 0;
    if (!strcmp(signature, "(I)I") ||
        !strcmp(signature, "(I)J") ||
        !strcmp(signature, "(I)F")) return 1;
    if (!strcmp(signature, "(II)F")) return 2;
    if (!strcmp(signature, "(III)F")) return 3;
    return 0;
}

/* All integer getters are from independently supplied native event
 * observations. The index belongs to the original MotionEvent pointer list. */
static int32_t bs_jni_call_int_observed(void *obj, void *method_ref,
                                        const int32_t args[3]) {
    if (g_missing_member_pending) return 0;
    BSJNIObject *receiver = bs_jni_known_object(obj);
    const BSJNIKnownMember *member = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    if (receiver && receiver->kind == BS_JNI_OBJECT_LOCALE_LIST &&
        member && !strcmp(member->class_name, BS_LOCALE_LIST) &&
        !strcmp(member->name, "size") &&
        !strcmp(member->signature, "()I"))
        return (int32_t)receiver->locale_count;
    if (receiver && member && receiver->kind == BS_JNI_OBJECT_MOTION_EVENT &&
        !strcmp(member->class_name, BS_MOTION) && receiver->motion_event) {
        const BSJNIObservedMotionEvent *event = receiver->motion_event;
        const char *name = member->name;
        if (!strcmp(name, "getDeviceId")) return event->device_id;
        if (!strcmp(name, "getSource")) return event->source;
        if (!strcmp(name, "getAction")) return event->action;
        if (!strcmp(name, "getFlags")) return event->flags;
        if (!strcmp(name, "getMetaState")) return event->meta_state;
        if (!strcmp(name, "getActionButton")) return event->action_button;
        if (!strcmp(name, "getButtonState")) return event->button_state;
        if (!strcmp(name, "getClassification")) return event->classification;
        if (!strcmp(name, "getEdgeFlags")) return event->edge_flags;
        if (!strcmp(name, "getHistorySize")) return (int32_t)event->history_count;
        if (!strcmp(name, "getPointerCount")) return (int32_t)event->pointer_count;
        if (!strcmp(name, "getPointerId") || !strcmp(name, "getToolType")) {
            if (args[0] < 0 || (size_t)args[0] >= event->pointer_count) {
                bs_jni_input_bad_index(name, args[0], event->pointer_count);
                return 0;
            }
            return !strcmp(name, "getPointerId") ?
                event->pointers[args[0]].pointer_id :
                event->pointers[args[0]].tool_type;
        }
    }
    if (receiver && member && receiver->kind == BS_JNI_OBJECT_KEY_EVENT &&
        !strcmp(member->class_name, BS_KEY) && receiver->key_event) {
        const BSJNIObservedKeyEvent *event = receiver->key_event;
        const char *name = member->name;
        if (!strcmp(name, "getDeviceId")) return event->device_id;
        if (!strcmp(name, "getSource")) return event->source;
        if (!strcmp(name, "getAction")) return event->action;
        if (!strcmp(name, "getFlags")) return event->flags;
        if (!strcmp(name, "getMetaState")) return event->meta_state;
        if (!strcmp(name, "getModifiers")) return event->modifiers;
        if (!strcmp(name, "getRepeatCount")) return event->repeat_count;
        if (!strcmp(name, "getKeyCode")) return event->key_code;
        if (!strcmp(name, "getScanCode")) return event->scan_code;
        if (!strcmp(name, "getUnicodeChar")) return event->unicode_char;
    }
    bs_jni_missing_host_value("CallIntMethod",
                              receiver ? bs_jni_object_class(receiver) : NULL,
                              member ? member->name : "(invalid method)");
    return 0;
}

static int32_t bs_jni_call_int(void *env, void *obj, void *method, ...) {
    if (!bs_jni_require_env(env, "CallIntMethod")) return 0;
    int32_t args[3] = {0};
    const int argc = bs_jni_input_arity(method, "I");
    va_list values;
    va_start(values, method);
    for (int i = 0; i < argc; ++i) args[i] = va_arg(values, int);
    va_end(values);
    return bs_jni_call_int_observed(obj, method, args);
}
static int32_t bs_jni_call_int_v(void *env, void *obj, void *method,
                                 va_list args) {
    if (!bs_jni_require_env(env, "CallIntMethodV")) return 0;
    int32_t values[3] = {0};
    const int argc = bs_jni_input_arity(method, "I");
    for (int i = 0; i < argc; ++i) values[i] = va_arg(args, int);
    return bs_jni_call_int_observed(obj, method, values);
}
static int32_t bs_jni_call_int_a(void *env, void *obj, void *method,
                                 const void *args) {
    if (!bs_jni_require_env(env, "CallIntMethodA")) return 0;
    int32_t values[3] = {0};
    const int argc = bs_jni_input_arity(method, "I");
    if (argc && !args) {
        bs_jni_missing_host_value("CallIntMethodA", BS_MOTION,
                                  "missing jvalue array");
        return 0;
    }
    for (int i = 0; i < argc; ++i)
        memcpy(&values[i], (const uint8_t *)args + (size_t)i * 8,
               sizeof values[i]);
    return bs_jni_call_int_observed(obj, method, values);
}

/* GameActivityEventsInit obtains KeyEvent/MotionEvent float, long, boolean
 * and void methods, and GameActivity_register looks up Activity.finish,
 * setWindowFlags and setImeEditorInfoFields. Method IDs alone do not provide
 * Java receivers or a backed Android input stream. Typed JNI callbacks must
 * return the JNI default WITH a pending exception instead of entering an
 * untyped noreturn trampoline or silently reporting successful behavior. */
static void bs_jni_unbacked_invoke(void *object_ref, void *method_ref,
                                    const char *operation) {
    if (g_missing_member_pending) return;
    const BSJNIKnownMember *method = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    BSJNIObject *object = bs_jni_known_object(object_ref);
    const char *class_name = object ? bs_jni_object_class(object) : NULL;
    bs_jni_missing_host_value(operation, class_name,
                              method ? method->name : "invalid method/receiver");
}

#define BS_JNI_UNBACKED_CALLS(kind, result_type, failure_value) \
    static result_type bs_jni_call_##kind(void *env, void *obj, \
                                          void *method, ...) { \
        if (!bs_jni_require_env(env, "Call" #kind "Method")) \
            return failure_value; \
        bs_jni_unbacked_invoke(obj, method, "Call" #kind "Method"); \
        return failure_value; \
    } \
    static result_type bs_jni_call_##kind##_v(void *env, void *obj, \
                                               void *method, va_list args) { \
        (void)args; \
        if (!bs_jni_require_env(env, "Call" #kind "MethodV")) \
            return failure_value; \
        bs_jni_unbacked_invoke(obj, method, "Call" #kind "MethodV"); \
        return failure_value; \
    } \
    static result_type bs_jni_call_##kind##_a(void *env, void *obj, \
                                               void *method, const void *args) { \
        (void)args; \
        if (!bs_jni_require_env(env, "Call" #kind "MethodA")) \
            return failure_value; \
        bs_jni_unbacked_invoke(obj, method, "Call" #kind "MethodA"); \
        return failure_value; \
    }
BS_JNI_UNBACKED_CALLS(boolean, uint8_t, 0)
#undef BS_JNI_UNBACKED_CALLS

static int64_t bs_jni_call_long_observed(void *obj, void *method_ref,
                                         const int32_t args[3]) {
    if (g_missing_member_pending) return 0;
    BSJNIObject *receiver = bs_jni_known_object(obj);
    const BSJNIKnownMember *method = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    if (receiver && method &&
        receiver->kind == BS_JNI_OBJECT_MOTION_EVENT &&
        !strcmp(method->class_name, BS_MOTION) && receiver->motion_event) {
        const BSJNIObservedMotionEvent *event = receiver->motion_event;
        if (!strcmp(method->name, "getEventTime"))
            return event->event_time_ms;
        if (!strcmp(method->name, "getDownTime"))
            return event->down_time_ms;
        if (!strcmp(method->name, "getHistoricalEventTime")) {
            if (args[0] < 0 || (size_t)args[0] >= event->history_count) {
                bs_jni_input_bad_index(method->name, args[0],
                                       event->history_count);
                return 0;
            }
            return event->historical_event_times_ms[args[0]];
        }
    }
    if (receiver && method && receiver->kind == BS_JNI_OBJECT_KEY_EVENT &&
        !strcmp(method->class_name, BS_KEY) && receiver->key_event) {
        if (!strcmp(method->name, "getEventTime"))
            return receiver->key_event->event_time_ms;
        if (!strcmp(method->name, "getDownTime"))
            return receiver->key_event->down_time_ms;
    }
    bs_jni_unbacked_invoke(obj, method_ref, "CallLongMethod");
    return 0;
}

static float bs_jni_call_float_observed(void *obj, void *method_ref,
                                        const int32_t args[3]) {
    if (g_missing_member_pending) return 0.0f;
    BSJNIObject *receiver = bs_jni_known_object(obj);
    const BSJNIKnownMember *method = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    if (receiver && method &&
        receiver->kind == BS_JNI_OBJECT_MOTION_EVENT &&
        !strcmp(method->class_name, BS_MOTION) && receiver->motion_event) {
        const BSJNIObservedMotionEvent *event = receiver->motion_event;
        const char *name = method->name;
        if (!strcmp(name, "getXPrecision")) return event->x_precision;
        if (!strcmp(name, "getYPrecision")) return event->y_precision;
        if (!strcmp(name, "getRawX") || !strcmp(name, "getRawY") ||
            !strcmp(name, "getAxisValue") ||
            !strcmp(name, "getHistoricalAxisValue")) {
            const int32_t pointer =
                (!strcmp(name, "getRawX") || !strcmp(name, "getRawY")) ?
                args[0] : args[1];
            if (pointer < 0 || (size_t)pointer >= event->pointer_count) {
                bs_jni_input_bad_index(name, pointer, event->pointer_count);
                return 0.0f;
            }
            if (!strcmp(name, "getRawX"))
                return event->pointers[pointer].raw_x;
            if (!strcmp(name, "getRawY"))
                return event->pointers[pointer].raw_y;
            if (!strcmp(name, "getHistoricalAxisValue")) {
                const int32_t history = args[2];
                if (history < 0 || (size_t)history >= event->history_count) {
                    bs_jni_input_bad_index(name, history,
                                           event->history_count);
                    return 0.0f;
                }
                for (size_t i = 0; i < event->historical_axis_count; ++i) {
                    const BSJNIObservedHistoricalMotionAxis *entry =
                        &event->historical_axes[i];
                    if (entry->axis == args[0] &&
                        entry->pointer_index == pointer &&
                        entry->history_index == history)
                        return entry->value;
                }
                return 0.0f; /* Android's missing axis value. */
            }
            for (size_t i = 0; i < event->axis_count; ++i) {
                const BSJNIObservedMotionAxis *entry = &event->axes[i];
                if (entry->axis == args[0] && entry->pointer_index == pointer)
                    return entry->value;
            }
            return 0.0f; /* Android's missing axis value. */
        }
    }
    bs_jni_unbacked_invoke(obj, method_ref, "CallFloatMethod");
    return 0.0f;
}

/* The original methods consume only integer arguments. JNI's variadic
 * arguments are promoted to int, and each jvalue union occupies 8 bytes. */
#define BS_JNI_INPUT_CALLS(kind, result_type, signature, zero_value) \
    static result_type bs_jni_call_##kind(void *env, void *obj, \
                                          void *method, ...) { \
        if (!bs_jni_require_env(env, "Call" #kind "Method")) \
            return zero_value; \
        const int argc = bs_jni_input_arity(method, signature); \
        int32_t values[3] = {0}; \
        va_list args; \
        va_start(args, method); \
        for (int i = 0; i < argc; ++i) values[i] = va_arg(args, int); \
        va_end(args); \
        return bs_jni_call_##kind##_observed(obj, method, values); \
    } \
    static result_type bs_jni_call_##kind##_v(void *env, void *obj, \
                                               void *method, va_list args) { \
        if (!bs_jni_require_env(env, "Call" #kind "MethodV")) \
            return zero_value; \
        const int argc = bs_jni_input_arity(method, signature); \
        int32_t values[3] = {0}; \
        for (int i = 0; i < argc; ++i) values[i] = va_arg(args, int); \
        return bs_jni_call_##kind##_observed(obj, method, values); \
    } \
    static result_type bs_jni_call_##kind##_a(void *env, void *obj, \
                                               void *method, const void *args) { \
        if (!bs_jni_require_env(env, "Call" #kind "MethodA")) \
            return zero_value; \
        const int argc = bs_jni_input_arity(method, signature); \
        if (argc && !args) { \
            bs_jni_missing_host_value("Call" #kind "MethodA", BS_MOTION, \
                                      "missing jvalue array"); \
            return zero_value; \
        } \
        int32_t values[3] = {0}; \
        for (int i = 0; i < argc; ++i) \
            memcpy(&values[i], (const uint8_t *)args + (size_t)i * 8, \
                   sizeof values[i]); \
        return bs_jni_call_##kind##_observed(obj, method, values); \
    }
BS_JNI_INPUT_CALLS(long, int64_t, "J", 0)
BS_JNI_INPUT_CALLS(float, float, "F", 0.0f)
#undef BS_JNI_INPUT_CALLS

/* The original libgame.so asks for exactly three void-returning
 * GameActivity methods. Only a separately supplied native-host proxy is a
 * valid receiver; UnityPlayerGameActivity's bootstrap handle cannot execute
 * GameActivity methods. Other input and Android Java methods stay unbacked. */
static int bs_jni_host_void_arity(void *method_ref) {
    const BSJNIKnownMember *method = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    if (!method || strcmp(method->class_name, BS_GAME_ACTIVITY)) return -1;
    if (!strcmp(method->name, "finish") &&
        !strcmp(method->signature, "()V")) return 0;
    if (!strcmp(method->name, "setWindowFlags") &&
        !strcmp(method->signature, "(II)V")) return 2;
    if (!strcmp(method->name, "setImeEditorInfoFields") &&
        !strcmp(method->signature, "(III)V")) return 3;
    return -1;
}

static void bs_jni_call_void_host(void *obj, void *method_ref,
                                  const int32_t arguments[3]) {
    if (g_missing_member_pending) return;
    BSJNIObject *receiver = bs_jni_known_object(obj);
    const BSJNIKnownMember *method = bs_jni_member_from_id(
        method_ref, BS_JNI_INSTANCE_METHOD);
    if (!receiver || receiver->kind != BS_JNI_OBJECT_GAME_ACTIVITY_HOST ||
        !method || strcmp(method->class_name, BS_GAME_ACTIVITY)) {
        bs_jni_unbacked_invoke(obj, method_ref, "CallVoidMethod");
        return;
    }
    const BSJNIGameActivityHostServices *host = &receiver->gameactivity_host;
    int status = BS_JNI_ERR;
    if (!strcmp(method->name, "finish") && host->finish)
        status = host->finish(host->user);
    else if (!strcmp(method->name, "setWindowFlags") &&
             host->set_window_flags)
        status = host->set_window_flags(host->user,
                                        arguments[0], arguments[1]);
    else if (!strcmp(method->name, "setImeEditorInfoFields") &&
             host->set_ime_editor_info_fields)
        status = host->set_ime_editor_info_fields(
            host->user, arguments[0], arguments[1], arguments[2]);
    if (status != 0 || g_missing_member_pending)
        bs_jni_missing_host_value("CallVoidMethod", BS_GAME_ACTIVITY,
                                  method->name);
}

static void bs_jni_call_void(void *env, void *obj, void *method, ...) {
    if (!bs_jni_require_env(env, "CallVoidMethod")) return;
    int argc = bs_jni_host_void_arity(method);
    if (argc < 0) {
        bs_jni_unbacked_invoke(obj, method, "CallVoidMethod");
        return;
    }
    int32_t arguments[3] = {0};
    va_list args;
    va_start(args, method);
    for (int i = 0; i < argc; ++i)
        arguments[i] = va_arg(args, int);
    va_end(args);
    bs_jni_call_void_host(obj, method, arguments);
}
static void bs_jni_call_void_v(void *env, void *obj, void *method,
                               va_list args) {
    if (!bs_jni_require_env(env, "CallVoidMethodV")) return;
    int argc = bs_jni_host_void_arity(method);
    if (argc < 0) {
        bs_jni_unbacked_invoke(obj, method, "CallVoidMethodV");
        return;
    }
    int32_t arguments[3] = {0};
    for (int i = 0; i < argc; ++i)
        arguments[i] = va_arg(args, int);
    bs_jni_call_void_host(obj, method, arguments);
}
static void bs_jni_call_void_a(void *env, void *obj, void *method,
                               const void *args) {
    if (!bs_jni_require_env(env, "CallVoidMethodA")) return;
    int argc = bs_jni_host_void_arity(method);
    if (argc < 0) {
        bs_jni_unbacked_invoke(obj, method, "CallVoidMethodA");
        return;
    }
    if (argc && !args) {
        bs_jni_missing_host_value("CallVoidMethodA", BS_GAME_ACTIVITY,
                                  "missing jvalue argument array");
        return;
    }
    int32_t arguments[3] = {0};
    /* JNI's jvalue union is 8 bytes, even on a 32-bit host. */
    for (int i = 0; i < argc; ++i)
        memcpy(&arguments[i], (const uint8_t *)args + (size_t)i * 8,
               sizeof arguments[i]);
    bs_jni_call_void_host(obj, method, arguments);
}

static void *bs_jni_new_string_utf(void *env, const char *text) {
    if (!bs_jni_require_env(env, "NewStringUTF")) return NULL;
    if (g_missing_member_pending) return NULL;
    BSJNIObject *ref = bs_jni_string_for_utf(text);
    if (!ref) {
        bs_jni_missing_host_value("NewStringUTF", "java/lang/String",
                                  "invalid or exhausted string table");
        return NULL;
    }
    return ref;
}

static int32_t bs_jni_string_utf_length(void *env, void *ref) {
    if (!bs_jni_require_env(env, "GetStringUTFLength")) return 0;
    BSJNIObject *string = bs_jni_known_object(ref);
    if (!string || string->kind != BS_JNI_OBJECT_STRING) {
        bs_jni_missing_host_value("GetStringUTFLength", "java/lang/String",
                                  "unrecognized handle");
        return 0;
    }
    return (int32_t)strlen(string->text);
}

static const char *bs_jni_string_utf_chars(void *env, void *ref,
                                          uint8_t *copy) {
    if (!bs_jni_require_env(env, "GetStringUTFChars")) return NULL;
    BSJNIObject *string = bs_jni_known_object(ref);
    if (!string || string->kind != BS_JNI_OBJECT_STRING) {
        bs_jni_missing_host_value("GetStringUTFChars", "java/lang/String",
                                  "unrecognized handle");
        return NULL;
    }
    if (copy)
        *copy = 0;
    return string->text;
}
static void bs_jni_release_utf(void *env, void *ref, const char *chars) {
    if (!bs_jni_require_env(env, "ReleaseStringUTFChars")) return;
    BSJNIObject *string = bs_jni_known_object(ref);
    if (!string || string->kind != BS_JNI_OBJECT_STRING ||
        chars != string->text) {
        bs_jni_missing_host_value("ReleaseStringUTFChars",
                                  "java/lang/String", "invalid string/pointer");
        return;
    }
}

/* Real JNI native registration method array layout (NDK JNINativeMethod). */
typedef struct BSJNIUntrustedNativeMethod {
    const char *name;
    const char *signature;
    void *function;
} BSJNIUntrustedNativeMethod;
_Static_assert(sizeof(BSJNIUntrustedNativeMethod) == sizeof(void *) * 3,
               "JNI native method must contain three pointer-sized slots");

/* Slot 215: record the actual function pointers supplied by the original .so
 * to JNI_OnLoad. Do NOT call, patch or overwrite any guest native function.
 */
static int32_t bs_jni_register_natives(void *env, void *class_ref,
                                       const BSJNIUntrustedNativeMethod *methods,
                                       int32_t count) {
    if (!bs_jni_require_env(env, "RegisterNatives")) return BS_JNI_ERR;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    if (!g_initialized || !klass || !methods || count < 0 ||
        (size_t)count > BS_JNI_MAX_NATIVES || g_missing_member_pending) {
        bs_jni_error("RegisterNatives invalid class/array/count=%d", count);
        return BS_JNI_ERR;
    }

    const int is_gameactivity = !strcmp(klass->name, BS_GAME_ACTIVITY);
    if (is_gameactivity &&
        (size_t)count != BS_JNI_GAMEACTIVITY_NATIVE_COUNT) {
        bs_jni_error("GameActivity native registration requires exactly %lu "
                     "declarations, got %d",
                     (unsigned long)BS_JNI_GAMEACTIVITY_NATIVE_COUNT, count);
        return BS_JNI_ERR;
    }

    /* Serialize validation and commit across native threads. A half-filled
     * registry entry must never become observable to a second registration.
     * No logger or native guest callback runs under this lock. */
    bs_jni_lock();
    /* Two-phase commit. Validate ALL names, declarations, pointer identities
     * and capacities before modifying anything. Re-registering an identical
     * tuple with an identical original code pointer is idempotent (JNI class
     * method tables do not grow duplicate declarations on repeated JNI_OnLoad).
     * The function pointers are retained but NEVER dereferenced/invoked.
     */
    size_t new_count = 0;
    for (int32_t i = 0; i < count; ++i) {
        char name[BS_JNI_METHOD_BYTES];
        char signature[BS_JNI_SIGNATURE_BYTES];
        if (!methods[i].function ||
            bs_jni_copy(name, sizeof name, methods[i].name) != BS_JNI_OK ||
            bs_jni_copy(signature, sizeof signature,
                        methods[i].signature) != BS_JNI_OK ||
            !bs_jni_valid_signature(signature)) {
            bs_jni_unlock();
            bs_jni_error("RegisterNatives invalid method descriptor at %d", i);
            return BS_JNI_ERR;
        }
        if (is_gameactivity &&
            !bs_jni_known_gameactivity_native(name, signature)) {
            bs_jni_unlock();
            bs_jni_no_such_member("method", klass->name, name, signature);
            return BS_JNI_ERR;
        }
        int already_registered = 0;
        for (size_t k = 0; k < g_native_count; ++k) {
            const BSJNINativeRegistration *old = &g_native_methods[k];
            if (!strcmp(old->class_name, klass->name) &&
                !strcmp(old->method_name, name) &&
                !strcmp(old->signature, signature)) {
                if (old->function_address !=
                    (uintptr_t)methods[i].function) {
                    bs_jni_unlock();
                    bs_jni_error("RegisterNatives conflicting native %s.%s %s",
                                 klass->name, name, signature);
                    return BS_JNI_ERR;
                }
                already_registered = 1;
            }
        }
        for (int32_t k = 0; k < i; ++k) {
            if (!strcmp(methods[k].name, name) &&
                !strcmp(methods[k].signature, signature)) {
                bs_jni_unlock();
                bs_jni_error("RegisterNatives duplicate batch native %s.%s %s",
                             klass->name, name, signature);
                return BS_JNI_ERR;
            }
        }
        if (!already_registered) ++new_count;
    }
    if (new_count > BS_JNI_MAX_NATIVES - g_native_count) {
        bs_jni_unlock();
        bs_jni_error("RegisterNatives table exhausted: %lu new entries",
                     (unsigned long)new_count);
        return BS_JNI_ERR;
    }

    for (int32_t i = 0; i < count; ++i) {
        int already_registered = 0;
        for (size_t k = 0; k < g_native_count; ++k) {
            const BSJNINativeRegistration *old = &g_native_methods[k];
            if (!strcmp(old->class_name, klass->name) &&
                !strcmp(old->method_name, methods[i].name) &&
                !strcmp(old->signature, methods[i].signature)) {
                already_registered = 1;
                break;
            }
        }
        if (already_registered) continue;
        /* Make the fully written declaration visible as a single publish.
         * Readers consult atomic g_native_count without taking this lock. */
        BSJNINativeRegistration *entry = &g_native_methods[g_native_count];
        memcpy(entry->class_name, klass->name, strlen(klass->name) + 1);
        bs_jni_copy(entry->method_name, sizeof entry->method_name,
                    methods[i].name);
        bs_jni_copy(entry->signature, sizeof entry->signature,
                    methods[i].signature);
        entry->function_address = (uintptr_t)methods[i].function;
        ++g_native_count;
    }
    bs_jni_unlock();
    bs_jni_log("[jni] RegisterNatives captured %d declarations for %s "
               "(%lu new)", count, klass->name, (unsigned long)new_count);
    return BS_JNI_OK;
}

static int32_t bs_jni_get_java_vm(void *env, void **out_vm) {
    if (!bs_jni_require_env(env, "GetJavaVM")) return BS_JNI_ERR;
    if (!out_vm)
        return BS_JNI_ERR;
    *out_vm = (void *)&g_vm_table;
    return BS_JNI_OK;
}

static int32_t bs_jni_get_object_ref_type(void *env, void *reference) {
    if (!bs_jni_require_env(env, "GetObjectRefType")) return 0;
    if (!reference)
        return 0; /* JNIInvalidRefType */
    BSJNIReference *record = bs_jni_reference_record(reference);
    if (record) return record->active &&
                       (record->type != 1 ||
                        (record->local_env_owner ==
                             (const void *)&g_thread_env_table &&
                         record->local_generation == g_attach_generation))
                       ? record->type : 0;
    BSJNIObject *object = bs_jni_known_object(reference);
    if (object)
        return object == g_activity || object == g_asset_manager ? 2 : 1;
    if (bs_jni_known_class(reference))
        return 1; /* FindClass/GetObjectClass return local symbolic refs. */
    return 0;
}

/* JavaVM JNIInvokeInterface slots 3..7. */
static int32_t bs_jni_vm_destroy(void *vm) {
    if (vm != (void *)&g_vm_table) return BS_JNI_ERR;
    bs_jni_error("DestroyJavaVM unsupported: no embedded Java VM");
    return BS_JNI_ERR;
}
static int32_t bs_jni_vm_attach(void *vm, void **out_env,
                                const void *attach_args) {
    if (vm != (void *)&g_vm_table || !g_initialized || !out_env)
        return BS_JNI_ERR;
    *out_env = NULL;
    if (attach_args) {
        int32_t version;
        memcpy(&version, attach_args, sizeof version);
        if (!bs_jni_valid_version(version))
            return BS_JNI_EVERSION;
    }
    if (!g_attached) {
        ++g_attach_generation;
        if (!g_attach_generation) ++g_attach_generation;
        g_missing_member_pending = 0;
        g_live_exception = NULL;
        g_last_error[0] = '\0';
    }
    g_attached = 1;
    g_thread_env_table = g_env_functions;
    *out_env = (void *)&g_thread_env_table;
    return BS_JNI_OK;
}
static int32_t bs_jni_vm_detach(void *vm) {
    if (vm != (void *)&g_vm_table || !g_attached)
        return BS_JNI_ERR;
    /* JNI local references expire on DetachCurrentThread; global and weak
     * refs survive. The generation check also covers a later reattachment. */
    bs_jni_lock();
    for (size_t i = 0; i < g_reference_count; ++i) {
        BSJNIReference *ref = &g_references[i];
        if (ref->type == 1 && ref->local_env_owner ==
                (const void *)&g_thread_env_table &&
            ref->local_generation == g_attach_generation)
            bs_jni_reference_release_locked(ref);
    }
    bs_jni_unlock();
    g_missing_member_pending = 0;
    g_live_exception = NULL;
    g_last_error[0] = '\0';
    g_attached = 0;
    g_thread_env_table = NULL;
    return BS_JNI_OK;
}
static int32_t bs_jni_vm_get_env(void *vm, void **out_env, int32_t version) {
    if (vm != (void *)&g_vm_table || !out_env || !g_initialized)
        return BS_JNI_ERR;
    *out_env = NULL;
    if (!bs_jni_valid_version(version))
        return BS_JNI_EVERSION;
    if (!g_attached)
        return BS_JNI_EDETACHED;
    *out_env = (void *)&g_thread_env_table;
    return BS_JNI_OK;
}

/*
 * Use the known AAPCS64 representation to install typed function addresses
 * in pointer-size JNI table slots. All installed callbacks have C ABI.
 * Each unsupported slot has a hard-stop fallback (never a successful no-op).
 */
#define BS_JNI_SLOT(table, index, callback) \
    do { (table)[(index)] = (BSJniGenericFunction)(callback); } while (0)

/* Call<Type>Method / CallStatic<Type>Method: host-implemented Java methods
 * are dispatched first; every other method ID keeps its previous strict
 * behavior (observed-state handlers or a pending unbacked exception). */
static void bs_jni_unbacked_v(void *env, void *obj, void *method,
                              va_list args, const char *operation) {
    (void)args;
    if (bs_jni_require_env(env, operation))
        bs_jni_unbacked_invoke(obj, method, operation);
}
static void bs_jni_unbacked_a(void *env, void *obj, void *method,
                              const void *args, const char *operation) {
    (void)args;
    if (bs_jni_require_env(env, operation))
        bs_jni_unbacked_invoke(obj, method, operation);
}
static BSJNIHostMethod *bs_jni_host_dispatch_target(void *env, void *method,
                                                    int is_static, char result,
                                                    const char *operation) {
    BSJNIHostMethod *m = bs_jni_host_method(method);
    if (!m) return NULL;
    if (!bs_jni_require_env(env, operation)) return m;
    if (m->is_static != is_static || m->result != result) {
        bs_jni_host_wrong_kind(m, operation);
        return m;
    }
    return m;
}
#define BS_JNI_HOST_CALLS(Name, ctype, field, letter, is_static, \
                          fallback_v, fallback_a) \
    static ctype bs_jni_hcall_##Name##_v(void *env, void *obj, void *method, \
                                         va_list args) { \
        BSJNIHostMethod *m = bs_jni_host_dispatch_target( \
            env, method, is_static, letter, "Call" #Name "MethodV"); \
        if (m) { \
            if (g_missing_member_pending || !bs_jni_env_is_current(env)) \
                return (ctype)0; \
            va_list copy; \
            va_copy(copy, args); \
            BSJNIValue value = bs_jni_host_invoke(env, obj, m, &copy, NULL); \
            va_end(copy); \
            return value.field; \
        } \
        fallback_v; \
    } \
    static ctype bs_jni_hcall_##Name##_a(void *env, void *obj, void *method, \
                                         const void *args) { \
        BSJNIHostMethod *m = bs_jni_host_dispatch_target( \
            env, method, is_static, letter, "Call" #Name "MethodA"); \
        if (m) { \
            if (g_missing_member_pending || !bs_jni_env_is_current(env)) \
                return (ctype)0; \
            BSJNIValue value = bs_jni_host_invoke( \
                env, obj, m, NULL, (const BSJNIValue *)args); \
            return value.field; \
        } \
        fallback_a; \
    } \
    static ctype bs_jni_hcall_##Name(void *env, void *obj, void *method, ...) { \
        va_list args; \
        va_start(args, method); \
        ctype value = bs_jni_hcall_##Name##_v(env, obj, method, args); \
        va_end(args); \
        return value; \
    }
#define BS_JNI_MISSING_V(op) \
    bs_jni_unbacked_v(env, obj, method, args, op); return 0
#define BS_JNI_MISSING_A(op) \
    bs_jni_unbacked_a(env, obj, method, args, op); return 0
BS_JNI_HOST_CALLS(Object, void *, l, 'L', 0,
    return bs_jni_call_object_v(env, obj, method, args),
    return bs_jni_call_object_a(env, obj, method, args))
BS_JNI_HOST_CALLS(Boolean, uint8_t, z, 'Z', 0,
    return bs_jni_call_boolean_v(env, obj, method, args),
    return bs_jni_call_boolean_a(env, obj, method, args))
BS_JNI_HOST_CALLS(Byte, int8_t, b, 'B', 0,
    BS_JNI_MISSING_V("CallByteMethodV"), BS_JNI_MISSING_A("CallByteMethodA"))
BS_JNI_HOST_CALLS(Char, uint16_t, c, 'C', 0,
    BS_JNI_MISSING_V("CallCharMethodV"), BS_JNI_MISSING_A("CallCharMethodA"))
BS_JNI_HOST_CALLS(Short, int16_t, s, 'S', 0,
    BS_JNI_MISSING_V("CallShortMethodV"), BS_JNI_MISSING_A("CallShortMethodA"))
BS_JNI_HOST_CALLS(Int, int32_t, i, 'I', 0,
    return bs_jni_call_int_v(env, obj, method, args),
    return bs_jni_call_int_a(env, obj, method, args))
BS_JNI_HOST_CALLS(Long, int64_t, j, 'J', 0,
    return bs_jni_call_long_v(env, obj, method, args),
    return bs_jni_call_long_a(env, obj, method, args))
BS_JNI_HOST_CALLS(Float, float, f, 'F', 0,
    return bs_jni_call_float_v(env, obj, method, args),
    return bs_jni_call_float_a(env, obj, method, args))
BS_JNI_HOST_CALLS(Double, double, d, 'D', 0,
    BS_JNI_MISSING_V("CallDoubleMethodV"), BS_JNI_MISSING_A("CallDoubleMethodA"))
BS_JNI_HOST_CALLS(StaticObject, void *, l, 'L', 1,
    BS_JNI_MISSING_V("CallStaticObjectMethodV"),
    BS_JNI_MISSING_A("CallStaticObjectMethodA"))
BS_JNI_HOST_CALLS(StaticBoolean, uint8_t, z, 'Z', 1,
    BS_JNI_MISSING_V("CallStaticBooleanMethodV"),
    BS_JNI_MISSING_A("CallStaticBooleanMethodA"))
BS_JNI_HOST_CALLS(StaticByte, int8_t, b, 'B', 1,
    BS_JNI_MISSING_V("CallStaticByteMethodV"),
    BS_JNI_MISSING_A("CallStaticByteMethodA"))
BS_JNI_HOST_CALLS(StaticChar, uint16_t, c, 'C', 1,
    BS_JNI_MISSING_V("CallStaticCharMethodV"),
    BS_JNI_MISSING_A("CallStaticCharMethodA"))
BS_JNI_HOST_CALLS(StaticShort, int16_t, s, 'S', 1,
    BS_JNI_MISSING_V("CallStaticShortMethodV"),
    BS_JNI_MISSING_A("CallStaticShortMethodA"))
BS_JNI_HOST_CALLS(StaticInt, int32_t, i, 'I', 1,
    return bs_jni_call_static_int_v(env, obj, method, args),
    return bs_jni_call_static_int_a(env, obj, method, args))
BS_JNI_HOST_CALLS(StaticLong, int64_t, j, 'J', 1,
    BS_JNI_MISSING_V("CallStaticLongMethodV"),
    BS_JNI_MISSING_A("CallStaticLongMethodA"))
BS_JNI_HOST_CALLS(StaticFloat, float, f, 'F', 1,
    BS_JNI_MISSING_V("CallStaticFloatMethodV"),
    BS_JNI_MISSING_A("CallStaticFloatMethodA"))
BS_JNI_HOST_CALLS(StaticDouble, double, d, 'D', 1,
    BS_JNI_MISSING_V("CallStaticDoubleMethodV"),
    BS_JNI_MISSING_A("CallStaticDoubleMethodA"))
#undef BS_JNI_HOST_CALLS
#undef BS_JNI_MISSING_V
#undef BS_JNI_MISSING_A

#define BS_JNI_HOST_VOID_CALLS(Name, is_static, fallback_v, fallback_a) \
    static void bs_jni_hcall_##Name##_v(void *env, void *obj, void *method, \
                                        va_list args) { \
        BSJNIHostMethod *m = bs_jni_host_dispatch_target( \
            env, method, is_static, 'V', "Call" #Name "MethodV"); \
        if (m) { \
            if (g_missing_member_pending || !bs_jni_env_is_current(env)) \
                return; \
            va_list copy; \
            va_copy(copy, args); \
            (void)bs_jni_host_invoke(env, obj, m, &copy, NULL); \
            va_end(copy); \
            return; \
        } \
        fallback_v; \
    } \
    static void bs_jni_hcall_##Name##_a(void *env, void *obj, void *method, \
                                        const void *args) { \
        BSJNIHostMethod *m = bs_jni_host_dispatch_target( \
            env, method, is_static, 'V', "Call" #Name "MethodA"); \
        if (m) { \
            if (g_missing_member_pending || !bs_jni_env_is_current(env)) \
                return; \
            (void)bs_jni_host_invoke(env, obj, m, NULL, \
                                     (const BSJNIValue *)args); \
            return; \
        } \
        fallback_a; \
    } \
    static void bs_jni_hcall_##Name(void *env, void *obj, void *method, ...) { \
        va_list args; \
        va_start(args, method); \
        bs_jni_hcall_##Name##_v(env, obj, method, args); \
        va_end(args); \
    }
BS_JNI_HOST_VOID_CALLS(Void, 0,
    bs_jni_call_void_v(env, obj, method, args),
    bs_jni_call_void_a(env, obj, method, args))
BS_JNI_HOST_VOID_CALLS(StaticVoid, 1,
    bs_jni_unbacked_v(env, obj, method, args, "CallStaticVoidMethodV"),
    bs_jni_unbacked_a(env, obj, method, args, "CallStaticVoidMethodA"))
#undef BS_JNI_HOST_VOID_CALLS

static void bs_jni_install_host_call_slots(void) {
    BSJniGenericFunction *t = g_env_functions;
#define BS_JNI_HOST_SLOTS(base, Name) \
    BS_JNI_SLOT(t, (base), bs_jni_hcall_##Name); \
    BS_JNI_SLOT(t, (base) + 1, bs_jni_hcall_##Name##_v); \
    BS_JNI_SLOT(t, (base) + 2, bs_jni_hcall_##Name##_a)
    BS_JNI_HOST_SLOTS(34, Object);
    BS_JNI_HOST_SLOTS(37, Boolean);
    BS_JNI_HOST_SLOTS(40, Byte);
    BS_JNI_HOST_SLOTS(43, Char);
    BS_JNI_HOST_SLOTS(46, Short);
    BS_JNI_HOST_SLOTS(49, Int);
    BS_JNI_HOST_SLOTS(52, Long);
    BS_JNI_HOST_SLOTS(55, Float);
    BS_JNI_HOST_SLOTS(58, Double);
    BS_JNI_HOST_SLOTS(61, Void);
    BS_JNI_HOST_SLOTS(114, StaticObject);
    BS_JNI_HOST_SLOTS(117, StaticBoolean);
    BS_JNI_HOST_SLOTS(120, StaticByte);
    BS_JNI_HOST_SLOTS(123, StaticChar);
    BS_JNI_HOST_SLOTS(126, StaticShort);
    BS_JNI_HOST_SLOTS(129, StaticInt);
    BS_JNI_HOST_SLOTS(132, StaticLong);
    BS_JNI_HOST_SLOTS(135, StaticFloat);
    BS_JNI_HOST_SLOTS(138, StaticDouble);
    BS_JNI_HOST_SLOTS(141, StaticVoid);
#undef BS_JNI_HOST_SLOTS
}

/* ---- Java runtime classes provided by ART/libcore itself ---------------
 * java.lang.Class / ClassLoader / Object / Throwable members that native
 * code (Unity's jni helpers) needs for class lookup and diagnostics. App
 * class existence comes only from the original APK class_defs. */
int bs_jni_host_throw(const char *class_name, const char *message) {
    if (!bs_jni_builtin_throwable(class_name) || g_missing_member_pending)
        return BS_JNI_ERR;
    const char *source = message ? message : "";
    const size_t length = strlen(source);
    if (length >= sizeof g_missing_member_exception.text) return BS_JNI_ERR;
    memset(&g_missing_member_exception, 0, sizeof g_missing_member_exception);
    g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
    g_missing_member_exception.kind = BS_JNI_OBJECT_THROWABLE;
    memcpy(g_missing_member_exception.throwable_class, class_name,
           strlen(class_name) + 1);
    memcpy(g_missing_member_exception.text, source, length + 1);
    g_missing_member_pending = 1;
    g_live_exception = NULL;
    bs_jni_log("[jni] host throw %s: %s", class_name, source);
    return BS_JNI_OK;
}

void *bs_jni_new_host_instance(const char *class_name) {
    if (!g_initialized || !class_name ||
        (!g_lenient && !bs_jni_is_app_class(class_name, NULL, 0))) {
        bs_jni_error("host instance requires an original APK class: %s",
                     class_name ? class_name : "(null)");
        return NULL;
    }
    return bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE, class_name);
}

int bs_jni_is_host_instance(void *receiver, const char *class_name) {
    BSJNIObject *object = bs_jni_known_object(receiver);
    return object && class_name && object->kind == BS_JNI_OBJECT_HOST_INSTANCE &&
           !strcmp(object->text, class_name);
}

static BSJNIObject *g_app_class_loader;

static BSJNIObject *bs_jni_app_class_loader(void) {
    if (!g_app_class_loader)
        g_app_class_loader = bs_jni_object_new(
            BS_JNI_OBJECT_CLASS_LOADER, "dalvik/system/PathClassLoader");
    return g_app_class_loader;
}

static const char *bs_jni_string_text(void *ref) {
    BSJNIObject *object = bs_jni_known_object(ref);
    return object && object->kind == BS_JNI_OBJECT_STRING ? object->text : NULL;
}

static void bs_jni_dotted(char *dst, size_t cap, const char *internal) {
    size_t i = 0;
    for (; internal[i] && i + 1 < cap; ++i)
        dst[i] = internal[i] == '/' ? '.' : internal[i];
    dst[i] = '\0';
}

/* Class.forName/ClassLoader.loadClass resolution of a binary (dotted) name.
 * app_loader: the APK PathClassLoader (boot delegation + APK class_defs);
 * otherwise only the boot class path. Missing -> ClassNotFoundException. */
static int bs_jni_rt_resolve(void *name_ref, int app_loader, int initialize,
                             BSJNIValue *result) {
    const char *dotted = bs_jni_string_text(name_ref);
    if (!dotted) {
        if (!name_ref)
            return bs_jni_host_throw("java/lang/NullPointerException",
                                     "class name == null") ? -1 : 0;
        return -1;
    }
    char internal[BS_JNI_CLASS_BYTES];
    const size_t length = strlen(dotted);
    int valid = length > 0 && length < sizeof internal && dotted[0] != '[';
    for (size_t i = 0; valid && i < length; ++i) {
        if (dotted[i] == '/' || dotted[i] == ';') valid = 0;
        internal[i] = dotted[i] == '.' ? '/' : dotted[i];
    }
    if (valid) internal[length] = '\0';
    int found = 0;
    if (valid) {
        if (bs_jni_is_framework_class(internal))
            found = 1;
        else if (app_loader && bs_jni_is_app_class(internal, NULL, 0))
            found = 2;
    }
    if (!found) {
        bs_jni_log("[jni:class] %s not found by %s class loader", dotted,
                   app_loader ? "APK" : "boot");
        return bs_jni_host_throw("java/lang/ClassNotFoundException", dotted)
            ? -1 : 0;
    }
    if (found == 2 && initialize)
        bs_jni_log("[jni:class] %s resolved from APK class_defs "
                   "(<clinit> state is host-modeled, bytecode not run)",
                   internal);
    result->l = bs_jni_class_for_name(internal);
    return result->l ? 0 : -1;
}

/* Class.forName(String, boolean, ClassLoader) */
static int bs_jni_rt_class_for_name3(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    BSJNIObject *loader = args[2].l ? bs_jni_known_object(args[2].l) : NULL;
    if (args[2].l && (!loader || loader->kind != BS_JNI_OBJECT_CLASS_LOADER))
        return -1; /* unmodeled ClassLoader */
    return bs_jni_rt_resolve(args[0].l, loader != NULL, args[1].z, result);
}

/* Class.forName(String): caller's loader. JNI callers without a Java frame
 * use the system class loader, which for an app process is the APK loader. */
static int bs_jni_rt_class_for_name1(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    return bs_jni_rt_resolve(args[0].l, 1, 1, result);
}

/* ClassLoader.loadClass(String) on the APK PathClassLoader. */
static int bs_jni_rt_load_class(void *user, void *env, void *receiver,
                                const BSJNIValue *args, size_t argc,
                                BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    BSJNIObject *loader = bs_jni_known_object(receiver);
    if (!loader || loader->kind != BS_JNI_OBJECT_CLASS_LOADER) return -1;
    return bs_jni_rt_resolve(args[0].l, 1, 0, result);
}

/* Class.getClassLoader(): APK classes report the APK PathClassLoader.
 * Boot classes (BootClassLoader on Android) are not modeled yet. */
static int bs_jni_rt_get_class_loader(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    /* 1.40.8: the host Activity proxy keeps the 1.46 GameActivity class
     * name, which is absent from this DEX; every class the game asks about
     * is served by the single APK PathClassLoader. */
    BSJNIClass *klass = bs_jni_known_class(receiver);
    if (!klass) return -1;
    result->l = bs_jni_app_class_loader();
    return result->l ? 0 : -1;
}

/* Class.getName(): binary name with dots. */
static int bs_jni_rt_class_get_name(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIClass *klass = bs_jni_known_class(receiver);
    if (!klass) return -1;
    char dotted[BS_JNI_CLASS_BYTES];
    bs_jni_dotted(dotted, sizeof dotted, klass->name);
    result->l = bs_jni_string_for_utf(dotted);
    return result->l ? 0 : -1;
}

static int bs_jni_is_throwable_kind(BSJNIObjectKind kind) {
    return kind == BS_JNI_OBJECT_THROWABLE ||
           kind == BS_JNI_OBJECT_NO_SUCH_FIELD ||
           kind == BS_JNI_OBJECT_NO_SUCH_METHOD ||
           kind == BS_JNI_OBJECT_NO_CLASS_DEF ||
           kind == BS_JNI_OBJECT_UNSUPPORTED_ERROR ||
           kind == BS_JNI_OBJECT_INDEX_ERROR;
}

/* Throwable detail message: stored text minus a leading "SimpleName: ". */
static const char *bs_jni_throwable_message(BSJNIObject *object) {
    const char *class_name = bs_jni_object_class(object);
    const char *simple = class_name ? strrchr(class_name, '/') : NULL;
    simple = simple ? simple + 1 : class_name;
    const size_t n = simple ? strlen(simple) : 0;
    if (n && !strncmp(object->text, simple, n) &&
        object->text[n] == ':' && object->text[n + 1] == ' ')
        return object->text + n + 2;
    return object->text;
}

/* Throwable.getMessage() */
static int bs_jni_rt_throwable_get_message(void *user, void *env,
                                           void *receiver,
                                           const BSJNIValue *args,
                                           size_t argc, BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *object = bs_jni_known_object(receiver);
    if (!object || !bs_jni_is_throwable_kind(object->kind)) return -1;
    const char *message = bs_jni_throwable_message(object);
    result->l = message[0] ? bs_jni_string_for_utf(message) : NULL;
    return 0;
}

/* Object.toString() for the modeled value kinds: Throwable.toString(),
 * Class.toString() and String.toString(). Others fail closed. */
static int bs_jni_rt_object_to_string(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    char text[BS_JNI_STRING_BYTES];
    BSJNIObject *object = bs_jni_known_object(receiver);
    if (object && object->kind == BS_JNI_OBJECT_STRING) {
        /* A distinct local handle: the caller may delete it separately. */
        result->l = bs_jni_string_for_utf(object->text);
        return result->l ? 0 : -1;
    }
    if (object && bs_jni_is_throwable_kind(object->kind)) {
        char dotted[BS_JNI_CLASS_BYTES];
        const char *class_name = bs_jni_object_class(object);
        if (!class_name || !class_name[0]) return -1;
        bs_jni_dotted(dotted, sizeof dotted, class_name);
        const char *message = bs_jni_throwable_message(object);
        if (message[0])
            snprintf(text, sizeof text, "%s: %s", dotted, message);
        else
            snprintf(text, sizeof text, "%s", dotted);
    } else if (object) {
        /* Object.toString() default: <class>@<identity hash>. */
        char dotted[BS_JNI_CLASS_BYTES];
        const char *class_name = bs_jni_object_class(object);
        bs_jni_dotted(dotted, sizeof dotted,
                      class_name ? class_name : "java/lang/Object");
        snprintf(text, sizeof text, "%s@%x", dotted,
                 (unsigned)((uintptr_t)object >> 4));
    } else {
        BSJNIClass *klass = bs_jni_known_class(receiver);
        if (!klass) {
            if (!g_lenient) return -1;
            result->l = NULL;
            return 0;
        }
        char dotted[BS_JNI_CLASS_BYTES];
        bs_jni_dotted(dotted, sizeof dotted, klass->name);
        snprintf(text, sizeof text, "class %s", dotted);
    }
    result->l = bs_jni_string_for_utf(text);
    return result->l ? 0 : -1;
}

/* ---- Context package paths and java.io.File ---------------------------- */
static char g_package_code_path[BS_JNI_STRING_BYTES];
static char g_external_files_dir[BS_JNI_STRING_BYTES];

int bs_jni_set_package_paths(const char *code_path,
                             const char *external_files_dir) {
    if (!code_path || code_path[0] != '/' || !external_files_dir ||
        external_files_dir[0] != '/' ||
        bs_jni_copy(g_package_code_path, sizeof g_package_code_path,
                    code_path) ||
        bs_jni_copy(g_external_files_dir, sizeof g_external_files_dir,
                    external_files_dir)) {
        g_package_code_path[0] = g_external_files_dir[0] = '\0';
        return BS_JNI_ERR;
    }
    return BS_JNI_OK;
}

static int bs_jni_is_context_receiver(void *receiver) {
    const void *canonical = bs_jni_canonical_reference(receiver);
    return canonical && (canonical == g_activity ||
                         canonical == g_gameactivity_receiver);
}

/* Context.getPackageCodePath(): the installed APK path. */
static int bs_jni_rt_package_code_path(void *user, void *env, void *receiver,
                                       const BSJNIValue *args, size_t argc,
                                       BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_is_context_receiver(receiver) || !g_package_code_path[0])
        return -1;
    result->l = bs_jni_string_for_utf(g_package_code_path);
    return result->l ? 0 : -1;
}

/* Context.getExternalFilesDir(String type): <files> for null, otherwise
 * <files>/<type>. The host maps the Android path onto persistent storage
 * and guarantees the base directory exists. */
static int bs_jni_rt_external_files_dir(void *user, void *env, void *receiver,
                                        const BSJNIValue *args, size_t argc,
                                        BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    if (!bs_jni_is_context_receiver(receiver) || !g_external_files_dir[0])
        return -1;
    char path[BS_JNI_STRING_BYTES];
    if (args[0].l) {
        const char *type = bs_jni_string_text(args[0].l);
        if (!type || !type[0] || strchr(type, '/') || strstr(type, ".."))
            return -1;
        const size_t base = strlen(g_external_files_dir), extra = strlen(type);
        if (base + 1 + extra >= sizeof path) return -1;
        memcpy(path, g_external_files_dir, base);
        path[base] = '/';
        memcpy(path + base + 1, type, extra + 1);
    } else {
        snprintf(path, sizeof path, "%s", g_external_files_dir);
    }
    result->l = bs_jni_object_new(BS_JNI_OBJECT_FILE, path);
    return result->l ? 0 : -1;
}

/* File.getPath()/getAbsolutePath()/getCanonicalPath()/toString(): the
 * modeled paths are absolute and canonical, so all return the same text. */
static int bs_jni_rt_file_path(void *user, void *env, void *receiver,
                               const BSJNIValue *args, size_t argc,
                               BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_known_object(receiver);
    if (!file || file->kind != BS_JNI_OBJECT_FILE) return -1;
    result->l = bs_jni_string_for_utf(file->text);
    return result->l ? 0 : -1;
}

static BSJNIObject *bs_jni_array_new(char type, const char *class_name,
                                     int32_t length);
static size_t bs_jni_utf16_from_utf8(const char *text, uint16_t *out);

/* Context.getFilesDir() / getObbDir(): the app's private files directory.
 * The host exposes one writable directory, the mapped external files dir. */
static int bs_jni_rt_files_dir(void *user, void *env, void *receiver,
                               const BSJNIValue *args, size_t argc,
                               BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_is_context_receiver(receiver) || !g_external_files_dir[0])
        return -1;
    result->l = bs_jni_object_new(BS_JNI_OBJECT_FILE, g_external_files_dir);
    return result->l ? 0 : -1;
}

/* Context.getObbDir(): "<parent of external files dir>/obb". Unity looks for
 * main.<versionCode>.<package>.obb there and mounts it into its jar: VFS
 * (Beat Saber's Addressables content, assets/aa, lives only in the OBB). */
static int bs_jni_obb_dir_path(char *out, size_t size) {
    if (!g_external_files_dir[0]) return -1;
    const char *slash = strrchr(g_external_files_dir, '/');
    size_t base = slash ? (size_t)(slash - g_external_files_dir) : 0;
    if (base + 5 >= size) return -1;
    memcpy(out, g_external_files_dir, base);
    memcpy(out + base, "/obb", 5);
    return 0;
}

static int bs_jni_rt_obb_dir(void *user, void *env, void *receiver,
                             const BSJNIValue *args, size_t argc,
                             BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    char path[BS_JNI_STRING_BYTES];
    if (!bs_jni_is_context_receiver(receiver) ||
        bs_jni_obb_dir_path(path, sizeof path))
        return -1;
    result->l = bs_jni_object_new(BS_JNI_OBJECT_FILE, path);
    return result->l ? 0 : -1;
}

/* Context.getObbDirs(): the single modeled OBB directory. */
static int bs_jni_rt_obb_dirs(void *user, void *env, void *receiver,
                              const BSJNIValue *args, size_t argc,
                              BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    char path[BS_JNI_STRING_BYTES];
    if (!bs_jni_is_context_receiver(receiver) ||
        bs_jni_obb_dir_path(path, sizeof path))
        return -1;
    BSJNIObject *array = bs_jni_array_new('L', "[Ljava/io/File;", 1);
    BSJNIObject *file = bs_jni_object_new(BS_JNI_OBJECT_FILE, path);
    if (!array || !file) return -1;
    file->pinned = 1; /* the array keeps it reachable */
    ((void **)array->array_data)[0] = file;
    result->l = array;
    return 0;
}

/* Context.getPackageManager() / PackageManager.getPackageInfo(): fields
 * (versionCode, versionName) come from the lenient seed table. */
static int bs_jni_rt_package_manager(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    static BSJNIObject *pm;
    if (!pm) {
        pm = bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE,
                               "android/content/pm/PackageManager");
        if (pm) pm->pinned = 1;
    }
    result->l = pm;
    return pm ? 0 : -1;
}

static int bs_jni_rt_package_info(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)receiver;
    const char *package = argc > 0 && args[0].l ? bs_jni_string_text(args[0].l) : NULL;
    if (!package || strcmp(package, "com.beatgames.beatsaber")) {
        /* Only the game itself is installed (no Horizon / Platform SDK
         * runtime): Android throws PackageManager.NameNotFoundException. */
        if (!g_missing_member_pending) {
            static const char klass[] =
                "android/content/pm/PackageManager$NameNotFoundException";
            memset(&g_missing_member_exception, 0, sizeof g_missing_member_exception);
            g_missing_member_exception.magic = BS_JNI_MAGIC_OBJECT;
            g_missing_member_exception.kind = BS_JNI_OBJECT_THROWABLE;
            memcpy(g_missing_member_exception.throwable_class, klass, sizeof klass);
            snprintf(g_missing_member_exception.text,
                     sizeof g_missing_member_exception.text, "%s",
                     package ? package : "null");
            g_missing_member_pending = 1;
            g_live_exception = NULL;
        }
        bs_jni_log("[jni] getPackageInfo(%s) -> NameNotFoundException",
                   package ? package : "null");
        result->l = NULL;
        return 0;
    }
    static BSJNIObject *info;
    if (!info) {
        info = bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE,
                                 "android/content/pm/PackageInfo");
        if (info) info->pinned = 1;
    }
    result->l = info;
    return info ? 0 : -1;
}

/* ---- Text asset streams: AssetManager.open + java.util.Scanner --------
 * Unity reads small text assets (unity_app_guid, ...) with
 * new Scanner(assets.open(name), "UTF-8").useDelimiter("\\A").next().
 * Stream and scanner instances share their whole content via a side table. */
#define BS_JNI_MAX_TEXT_STREAMS 128
static struct { BSJNIObject *object; char *data; } g_text_streams[BS_JNI_MAX_TEXT_STREAMS];

FILE *bs_android_fopen(const char *path, const char *mode);

static char **bs_jni_text_stream_slot(BSJNIObject *object, int create) {
    if (!object) return NULL;
    int free_slot = -1;
    for (int i = 0; i < BS_JNI_MAX_TEXT_STREAMS; ++i) {
        if (g_text_streams[i].object == object) return &g_text_streams[i].data;
        if (free_slot < 0 && !g_text_streams[i].object) free_slot = i;
    }
    if (!create) return NULL;
    if (free_slot < 0) {
        /* Recycle the oldest entry: streams are short-lived. */
        static int next;
        free_slot = next++ % BS_JNI_MAX_TEXT_STREAMS;
        free(g_text_streams[free_slot].data);
        g_text_streams[free_slot].data = NULL;
    }
    g_text_streams[free_slot].object = object;
    return &g_text_streams[free_slot].data;
}

static int bs_jni_rt_asset_open(void *user, void *env, void *receiver,
                                const BSJNIValue *args, size_t argc,
                                BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    const char *name = bs_jni_string_text(args[0].l);
    if (!name)
        return bs_jni_host_throw("java/lang/NullPointerException",
                                 "asset name == null") ? -1 : 0;
    char path[BS_JNI_STRING_BYTES + 8];
    snprintf(path, sizeof path, "assets/%s", name);
    FILE *file = bs_android_fopen(path, "rb");
    if (!file) {
        bs_jni_log("[jni:assets] AssetManager.open(%s): not found", name);
        return bs_jni_host_throw("java/io/FileNotFoundException", name) ? -1 : 0;
    }
    size_t cap = 4096, len = 0;
    char *data = malloc(cap + 1);
    while (data) {
        size_t got = fread(data + len, 1, cap - len, file);
        len += got;
        if (got == 0) break;
        if (len == cap) {
            char *grown = cap < (1u << 20) ? realloc(data, cap * 2 + 1) : NULL;
            if (!grown) break;
            data = grown;
            cap *= 2;
        }
    }
    fclose(file);
    if (!data) return -1;
    data[len] = '\0';
    BSJNIObject *stream = bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE,
                                            "java/io/InputStream");
    char **slot = bs_jni_text_stream_slot(stream, 1);
    if (!slot) { free(data); return -1; }
    free(*slot);
    *slot = data;
    bs_jni_log("[jni:assets] AssetManager.open(%s) -> %zu bytes", name, len);
    result->l = stream;
    return 0;
}

static int bs_jni_rt_scanner_init(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)argc; (void)result;
    char **source = bs_jni_text_stream_slot(bs_jni_known_object(args[0].l), 0);
    char **slot = bs_jni_text_stream_slot(bs_jni_known_object(receiver), 1);
    if (!slot) return -1;
    free(*slot);
    const char *text = source && *source ? *source : "";
    size_t size = strlen(text) + 1;
    *slot = malloc(size);
    if (!*slot) return -1;
    memcpy(*slot, text, size);
    return 0;
}

static int bs_jni_rt_scanner_self(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    result->l = receiver;
    return 0;
}

static int bs_jni_rt_scanner_has_next(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    char **slot = bs_jni_text_stream_slot(bs_jni_known_object(receiver), 0);
    result->z = slot && *slot && (*slot)[0];
    return 0;
}

/* Scanner.next(): Unity always uses the "\\A" delimiter, i.e. the whole
 * remaining input is one token. */
static int bs_jni_rt_scanner_next(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    char **slot = bs_jni_text_stream_slot(bs_jni_known_object(receiver), 0);
    if (!slot || !*slot || !(*slot)[0])
        return bs_jni_host_throw("java/util/NoSuchElementException",
                                 "no input") ? -1 : 0;
    result->l = bs_jni_string_for_utf(*slot);
    (*slot)[0] = '\0';
    return result->l ? 0 : -1;
}

static int bs_jni_rt_noop(void *user, void *env, void *receiver,
                          const BSJNIValue *args, size_t argc,
                          BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc; (void)result;
    return 0;
}

/* Context.getApplicationInfo(): fields come from the lenient seed table. */
static int bs_jni_rt_application_info(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_is_context_receiver(receiver)) return -1;
    static BSJNIObject *info;
    if (!info) {
        info = bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE,
                                 "android/content/pm/ApplicationInfo");
        if (info) info->pinned = 1;
    }
    result->l = info;
    return info ? 0 : -1;
}

/* Environment.getExternalStorageState(): external storage is mounted. */
static int bs_jni_rt_storage_state(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    result->l = bs_jni_string_for_utf("mounted");
    return result->l ? 0 : -1;
}

/* String.equals(Object) */
static int bs_jni_rt_string_equals(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    const char *b = args[0].l ? bs_jni_string_text(args[0].l) : NULL;
    if (!a) return -1;
    result->z = b && !strcmp(a, b);
    return 0;
}

/* String.length() / isEmpty() */
static int bs_jni_rt_string_length(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    if (!a) return -1;
    result->i = (int32_t)bs_jni_utf16_from_utf8(a, NULL);
    return 0;
}
static int bs_jni_rt_string_is_empty(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    if (!a) return -1;
    result->z = !a[0];
    return 0;
}

#define BS_JNI_NATIVE_LIBRARY_DIR "/data/app/com.beatgames.beatsaber/lib/arm64"

/* ClassLoader.findLibrary(String): APK native libraries live in the
 * package's native library dir; the loader resolves them by basename. */
static int bs_jni_rt_find_library(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    const char *name = bs_jni_string_text(args[0].l);
    if (!name || !name[0] || strchr(name, '/')) return -1;
    char path[BS_JNI_STRING_BYTES];
    snprintf(path, sizeof path, BS_JNI_NATIVE_LIBRARY_DIR "/lib%.200s.so",
             name);
    result->l = bs_jni_string_for_utf(path);
    return result->l ? 0 : -1;
}

/* ---- java.io.File, StringBuilder and common String methods ------------
 * Unity's Java glue builds data paths with File/StringBuilder before handing
 * them to native code, which dereferences the resulting C strings without
 * null checks. These are real implementations over the mapped Android
 * filesystem, not default-value stubs. */
int bs_android_stat(const char *path, void *out);
int bs_android_mkdir(const char *path, unsigned mode);
int bs_android_unlinkat(int dirfd, const char *path, int flags);
#define BS_JNI_AT_FDCWD (-100)
#define BS_JNI_AT_REMOVEDIR 0x200

static BSJNIObject *bs_jni_file_receiver(void *receiver) {
    BSJNIObject *file = bs_jni_known_object(receiver);
    return file && file->kind == BS_JNI_OBJECT_FILE ? file : NULL;
}

/* Normalizes `path` in place: collapses duplicate slashes and drops a
 * trailing slash (except for the root). */
static void bs_jni_path_normalize(char *path) {
    char *w = path;
    for (const char *r = path; *r; ++r) {
        if (*r == '/' && w > path && w[-1] == '/') continue;
        *w++ = *r;
    }
    *w = '\0';
    size_t n = strlen(path);
    if (n > 1 && path[n - 1] == '/') path[n - 1] = '\0';
}

static int bs_jni_file_become(BSJNIObject *object, const char *parent,
                              const char *child) {
    if (!object || !child) return -1;
    char path[BS_JNI_STRING_BYTES];
    int n = parent && parent[0]
        ? snprintf(path, sizeof path, "%s/%s", parent, child)
        : snprintf(path, sizeof path, "%s", child);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    bs_jni_path_normalize(path);
    object->kind = BS_JNI_OBJECT_FILE;
    memcpy(object->text, path, strlen(path) + 1);
    return 0;
}

/* new String(byte[] bytes, String charset): Unity builds plugin names and
 * paths this way. Bytes are treated as UTF-8 (the only charset it passes). */
static int bs_jni_rt_string_init_bytes(void *user, void *env, void *receiver,
                                       const BSJNIValue *args, size_t argc,
                                       BSJNIValue *result) {
    (void)user; (void)env; (void)argc; (void)result;
    BSJNIObject *self = bs_jni_known_object(receiver);
    BSJNIObject *bytes = bs_jni_known_object(args[0].l);
    if (!self || !bytes || bytes->kind != BS_JNI_OBJECT_ARRAY ||
        bytes->array_type != 'B' || bytes->array_length < 0 ||
        bytes->array_length >= BS_JNI_STRING_BYTES)
        return bs_jni_host_throw("java/lang/NullPointerException",
                                 "String(byte[], charset)") ? -1 : 0;
    self->kind = BS_JNI_OBJECT_STRING;
    if (bytes->array_length)
        memcpy(self->text, bytes->array_data, (size_t)bytes->array_length);
    self->text[bytes->array_length] = '\0';
    return 0;
}

static int bs_jni_rt_file_init1(void *user, void *env, void *receiver,                                const BSJNIValue *args, size_t argc,
                                BSJNIValue *result) {
    (void)user; (void)env; (void)argc; (void)result;
    const char *path = bs_jni_string_text(args[0].l);
    if (!path)
        return bs_jni_host_throw("java/lang/NullPointerException",
                                 "File(null)") ? -1 : 0;
    return bs_jni_file_become(bs_jni_known_object(receiver), NULL, path);
}
static int bs_jni_rt_file_init_ss(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)argc; (void)result;
    const char *child = bs_jni_string_text(args[1].l);
    if (!child)
        return bs_jni_host_throw("java/lang/NullPointerException",
                                 "File(parent, null)") ? -1 : 0;
    return bs_jni_file_become(bs_jni_known_object(receiver),
                              args[0].l ? bs_jni_string_text(args[0].l) : NULL,
                              child);
}
static int bs_jni_rt_file_init_fs(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)argc; (void)result;
    const char *child = bs_jni_string_text(args[1].l);
    if (!child)
        return bs_jni_host_throw("java/lang/NullPointerException",
                                 "File(parent, null)") ? -1 : 0;
    BSJNIObject *parent = args[0].l ? bs_jni_file_receiver(args[0].l) : NULL;
    return bs_jni_file_become(bs_jni_known_object(receiver),
                              parent ? parent->text : NULL, child);
}

/* Length of the parent prefix, or -1 when the path has no parent. */
static int bs_jni_file_parent_len(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return -1;
    if (slash == path) return path[1] ? 1 : -1;
    return (int)(slash - path);
}
static int bs_jni_rt_file_get_parent(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    int n = bs_jni_file_parent_len(file->text);
    if (n < 0) { result->l = NULL; return 0; }
    char parent[BS_JNI_STRING_BYTES];
    memcpy(parent, file->text, (size_t)n);
    parent[n] = '\0';
    result->l = bs_jni_string_for_utf(parent);
    return result->l ? 0 : -1;
}
static int bs_jni_rt_file_get_parent_file(void *user, void *env, void *receiver,
                                          const BSJNIValue *args, size_t argc,
                                          BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    int n = bs_jni_file_parent_len(file->text);
    if (n < 0) { result->l = NULL; return 0; }
    char parent[BS_JNI_STRING_BYTES];
    memcpy(parent, file->text, (size_t)n);
    parent[n] = '\0';
    result->l = bs_jni_object_new(BS_JNI_OBJECT_FILE, parent);
    return result->l ? 0 : -1;
}
static int bs_jni_rt_file_get_name(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    const char *slash = strrchr(file->text, '/');
    result->l = bs_jni_string_for_utf(slash ? slash + 1 : file->text);
    return result->l ? 0 : -1;
}

/* Bionic AArch64 struct stat: st_mode at +16, st_size at +48. */
static int bs_jni_file_stat(BSJNIObject *file, uint32_t *mode, int64_t *size) {
    uint64_t st[16];
    memset(st, 0, sizeof st);
    if (!file || bs_android_stat(file->text, st) != 0) return -1;
    uint32_t m;
    int64_t s;
    memcpy(&m, (const char *)st + 16, sizeof m);
    memcpy(&s, (const char *)st + 48, sizeof s);
    if (mode) *mode = m;
    if (size) *size = s;
    return 0;
}
static int bs_jni_rt_file_exists(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    result->z = bs_jni_file_stat(file, NULL, NULL) == 0;
    return 0;
}
static int bs_jni_rt_file_is_directory(void *user, void *env, void *receiver,
                                       const BSJNIValue *args, size_t argc,
                                       BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    uint32_t mode = 0;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    result->z = bs_jni_file_stat(file, &mode, NULL) == 0 &&
                (mode & 0170000u) == 0040000u;
    return 0;
}
static int bs_jni_rt_file_is_file(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    uint32_t mode = 0;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    result->z = bs_jni_file_stat(file, &mode, NULL) == 0 &&
                (mode & 0170000u) == 0100000u;
    return 0;
}
static int bs_jni_rt_file_length(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    int64_t size = 0;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    result->j = bs_jni_file_stat(file, NULL, &size) == 0 ? size : 0;
    return 0;
}
static int bs_jni_rt_file_true_if_exists(void *user, void *env, void *receiver,
                                         const BSJNIValue *args, size_t argc,
                                         BSJNIValue *result) {
    /* canRead/canWrite/canExecute: the mapped tree is owner-accessible. */
    return bs_jni_rt_file_exists(user, env, receiver, args, argc, result);
}
static int bs_jni_rt_file_mkdir(void *user, void *env, void *receiver,
                                const BSJNIValue *args, size_t argc,
                                BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    result->z = bs_android_mkdir(file->text, 0770) == 0;
    return 0;
}
static int bs_jni_rt_file_mkdirs(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    uint32_t mode = 0;
    if (bs_jni_file_stat(file, &mode, NULL) == 0) { result->z = 0; return 0; }
    char path[BS_JNI_STRING_BYTES];
    memcpy(path, file->text, strlen(file->text) + 1);
    int created = 0;
    for (char *p = path + 1; ; ++p) {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = '\0';
        if (bs_android_mkdir(path, 0770) == 0) created = 1;
        *p = saved;
        if (!saved) break;
    }
    result->z = created && bs_jni_file_stat(file, NULL, NULL) == 0;
    return 0;
}
static int bs_jni_rt_file_delete(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *file = bs_jni_file_receiver(receiver);
    if (!file) return -1;
    uint32_t mode = 0;
    if (bs_jni_file_stat(file, &mode, NULL) != 0) { result->z = 0; return 0; }
    int dir = (mode & 0170000u) == 0040000u;
    result->z = bs_android_unlinkat(BS_JNI_AT_FDCWD, file->text,
                                    dir ? BS_JNI_AT_REMOVEDIR : 0) == 0;
    return 0;
}
static int bs_jni_rt_file_space(void *user, void *env, void *receiver,
                                const BSJNIValue *args, size_t argc,
                                BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_file_receiver(receiver)) return -1;
    /* Matches the free space Unity is told about the Quest 2 data volume. */
    result->j = (int64_t)8 << 30;
    return 0;
}

/* StringBuilder content lives in the text-stream side table. */
static char *bs_jni_builder_text(void *receiver) {
    char **slot = bs_jni_text_stream_slot(bs_jni_known_object(receiver), 1);
    if (!slot) return NULL;
    if (!*slot) *slot = calloc(1, 1);
    return *slot;
}
static int bs_jni_builder_append_text(void *receiver, const char *text) {
    char **slot = bs_jni_text_stream_slot(bs_jni_known_object(receiver), 1);
    if (!slot) return -1;
    size_t have = *slot ? strlen(*slot) : 0, add = strlen(text);
    char *grown = realloc(*slot, have + add + 1);
    if (!grown) return -1;
    memcpy(grown + have, text, add + 1);
    *slot = grown;
    return 0;
}
static int bs_jni_rt_builder_init(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc; (void)result;
    char **slot = bs_jni_text_stream_slot(bs_jni_known_object(receiver), 1);
    if (!slot) return -1;
    free(*slot);
    *slot = calloc(1, 1);
    return *slot ? 0 : -1;
}
static int bs_jni_rt_builder_init_s(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    if (bs_jni_rt_builder_init(user, env, receiver, args, argc, result))
        return -1;
    const char *text = args[0].l ? bs_jni_string_text(args[0].l) : NULL;
    return bs_jni_builder_append_text(receiver, text ? text : "null");
}
static int bs_jni_rt_builder_append_s(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    const char *text = args[0].l ? bs_jni_string_text(args[0].l) : NULL;
    if (args[0].l && !text) {
        BSJNIObject *object = bs_jni_known_object(args[0].l);
        if (object && object->kind == BS_JNI_OBJECT_FILE) text = object->text;
    }
    if (bs_jni_builder_append_text(receiver, text ? text : "null")) return -1;
    result->l = receiver;
    return 0;
}
static int bs_jni_rt_builder_append_i(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    char text[24];
    snprintf(text, sizeof text, "%d", (int)args[0].i);
    if (bs_jni_builder_append_text(receiver, text)) return -1;
    result->l = receiver;
    return 0;
}
static int bs_jni_rt_builder_append_j(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    char text[24];
    snprintf(text, sizeof text, "%lld", (long long)args[0].j);
    if (bs_jni_builder_append_text(receiver, text)) return -1;
    result->l = receiver;
    return 0;
}
static int bs_jni_rt_builder_append_c(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    char text[2] = {(char)(args[0].c < 0x80 ? args[0].c : '?'), 0};
    if (bs_jni_builder_append_text(receiver, text)) return -1;
    result->l = receiver;
    return 0;
}
static int bs_jni_rt_builder_append_z(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    if (bs_jni_builder_append_text(receiver, args[0].z ? "true" : "false"))
        return -1;
    result->l = receiver;
    return 0;
}
static int bs_jni_rt_builder_to_string(void *user, void *env, void *receiver,
                                       const BSJNIValue *args, size_t argc,
                                       BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    const char *text = bs_jni_builder_text(receiver);
    if (!text) return -1;
    result->l = bs_jni_string_for_utf(text);
    return result->l ? 0 : -1;
}
static int bs_jni_rt_builder_length(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    const char *text = bs_jni_builder_text(receiver);
    if (!text) return -1;
    result->i = (int32_t)strlen(text);
    return 0;
}

static int bs_jni_rt_string_concat(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    const char *b = bs_jni_string_text(args[0].l);
    if (!a) return -1;
    if (!b)
        return bs_jni_host_throw("java/lang/NullPointerException",
                                 "concat(null)") ? -1 : 0;
    char text[BS_JNI_STRING_BYTES * 2];
    snprintf(text, sizeof text, "%s%s", a, b);
    result->l = bs_jni_string_for_utf(text);
    return result->l ? 0 : -1;
}
static int bs_jni_rt_string_starts_with(void *user, void *env, void *receiver,
                                        const BSJNIValue *args, size_t argc,
                                        BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    const char *b = bs_jni_string_text(args[0].l);
    if (!a || !b) return -1;
    result->z = !strncmp(a, b, strlen(b));
    return 0;
}
static int bs_jni_rt_string_ends_with(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    const char *b = bs_jni_string_text(args[0].l);
    if (!a || !b) return -1;
    size_t la = strlen(a), lb = strlen(b);
    result->z = lb <= la && !strcmp(a + la - lb, b);
    return 0;
}
static int bs_jni_rt_string_contains(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    const char *b = bs_jni_string_text(args[0].l);
    if (!a || !b) return -1;
    result->z = strstr(a, b) != NULL;
    return 0;
}
static int bs_jni_rt_string_to_lower(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    const char *a = bs_jni_string_text(receiver);
    if (!a) return -1;
    char text[BS_JNI_STRING_BYTES];
    size_t i = 0;
    for (; a[i] && i + 1 < sizeof text; ++i)
        text[i] = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
    text[i] = '\0';
    result->l = bs_jni_string_for_utf(text);
    return result->l ? 0 : -1;
}
static int bs_jni_rt_string_self(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_string_text(receiver)) return -1;
    result->l = receiver;
    return 0;
}

/* JNIEnv slots 28/29/30 NewObject/V/A. Only host-reproduced constructors of
 * original APK classes: the method ID must be that exact class's "<init>"
 * host method. The new instance is published only if the constructor body
 * completes without a pending exception. */
static void *bs_jni_new_object_common(void *env, void *class_ref,
                                      void *method, va_list *list,
                                      const BSJNIValue *array,
                                      const char *operation) {
    if (!bs_jni_require_env(env, operation)) return NULL;
    if (g_missing_member_pending) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    BSJNIHostMethod *ctor = bs_jni_host_method(method);
    if (!klass || !ctor || ctor->is_static || ctor->result != 'V' ||
        strcmp(ctor->name, "<init>") || strcmp(ctor->class_name, klass->name)) {
        bs_jni_missing_host_value(operation, klass ? klass->name : NULL,
                                  "<init> (not a host constructor)");
        return NULL;
    }
    void *instance = bs_jni_new_host_instance(klass->name);
    if (!instance) {
        bs_jni_missing_host_value(operation, klass->name, "<init>");
        return NULL;
    }
    (void)bs_jni_host_invoke(env, instance, ctor, list, array);
    return g_missing_member_pending ? NULL : instance;
}

static void *bs_jni_new_object_v(void *env, void *class_ref, void *method,
                                 va_list args) {
    va_list copy;
    va_copy(copy, args);
    void *result = bs_jni_new_object_common(env, class_ref, method, &copy,
                                            NULL, "NewObjectV");
    va_end(copy);
    return result;
}

static void *bs_jni_new_object_a(void *env, void *class_ref, void *method,
                                 const void *args) {
    return bs_jni_new_object_common(env, class_ref, method, NULL,
                                    (const BSJNIValue *)args, "NewObjectA");
}

static void *bs_jni_new_object(void *env, void *class_ref, void *method,
                               ...) {
    va_list args;
    va_start(args, method);
    void *result = bs_jni_new_object_common(env, class_ref, method, &args,
                                            NULL, "NewObject");
    va_end(args);
    return result;
}

/* ---- Lenient field access (JNI slots 95-112 and 145-162) --------------
 * Instance and static accessors share one C shape: (env, object-or-class,
 * fieldID[, value]). Lenient static cells keep their last write; every
 * other unmodeled field reads as the JNI default. */
static int bs_jni_field_access_ok(void *env, void *field, const char *op) {
    if (!bs_jni_require_env(env, op)) return 0;
    if (!bs_jni_lenient_field(field))
        bs_jni_lenient_note(op, NULL, "(unmodeled field ID)", NULL);
    return 1;
}
#define BS_JNI_LENIENT_FIELD(Name, ctype, member) \
    static ctype bs_jni_lget_##Name(void *env, void *obj, void *field) { \
        (void)obj; \
        if (!bs_jni_field_access_ok(env, field, "Get" #Name "Field")) \
            return (ctype)0; \
        BSJNILenientField *f = bs_jni_lenient_field(field); \
        return f && (f->is_static || f->seeded) ? (ctype)f->value.member \
                                                : (ctype)0; \
    } \
    static void bs_jni_lset_##Name(void *env, void *obj, void *field, \
                                   ctype value) { \
        (void)obj; \
        if (!bs_jni_field_access_ok(env, field, "Set" #Name "Field")) \
            return; \
        BSJNILenientField *f = bs_jni_lenient_field(field); \
        if (f && f->is_static) f->value.member = value; \
    }
BS_JNI_LENIENT_FIELD(Boolean, uint8_t, z)
BS_JNI_LENIENT_FIELD(Byte, int8_t, b)
BS_JNI_LENIENT_FIELD(Char, uint16_t, c)
BS_JNI_LENIENT_FIELD(Short, int16_t, s)
BS_JNI_LENIENT_FIELD(Int, int32_t, i)
BS_JNI_LENIENT_FIELD(Long, int64_t, j)
BS_JNI_LENIENT_FIELD(Float, float, f)
BS_JNI_LENIENT_FIELD(Double, double, d)
#undef BS_JNI_LENIENT_FIELD

static void *bs_jni_lget_Object(void *env, void *obj, void *field) {
    (void)obj;
    if (!bs_jni_field_access_ok(env, field, "GetObjectField")) return NULL;
    BSJNILenientField *f = bs_jni_lenient_field(field);
    return f && (f->is_static || f->seeded) ? f->value.l : NULL;
}
static void bs_jni_lset_Object(void *env, void *obj, void *field,
                               void *value) {
    (void)obj;
    if (!bs_jni_field_access_ok(env, field, "SetObjectField")) return;
    BSJNILenientField *f = bs_jni_lenient_field(field);
    if (!f || !f->is_static) return;
    void *target = value ? bs_jni_canonical_reference(value) : NULL;
    BSJNIObject *object = target ? bs_jni_known_object(target) : NULL;
    if (object) object->pinned = 1; /* a static field keeps it reachable */
    f->value.l = target;
}

/* ---- Strings: UTF-16 views (slots 163-166, 220, 221, 224, 225) ------- */
/* Decode one UTF-8 (or modified UTF-8) sequence; returns bytes consumed. */
static size_t bs_jni_utf8_decode(const unsigned char *s, uint32_t *cp) {
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        return 2;
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x0f) << 12) |
              ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return 3;
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x07) << 18) |
              ((uint32_t)(s[1] & 0x3f) << 12) |
              ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return 4;
    }
    *cp = 0xfffd;
    return 1;
}

/* UTF-16 units of `text`; writes them to `out` when non-NULL. */
static size_t bs_jni_utf16_from_utf8(const char *text, uint16_t *out) {
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        uint32_t cp;
        s += bs_jni_utf8_decode(s, &cp);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (out) {
                out[n] = (uint16_t)(0xd800 + (cp >> 10));
                out[n + 1] = (uint16_t)(0xdc00 + (cp & 0x3ff));
            }
            n += 2;
        } else {
            if (out) out[n] = (uint16_t)cp;
            ++n;
        }
    }
    return n;
}

/* Encode UTF-16 units as UTF-8 into `out` (capacity `cap`, NUL included),
 * stopping at a whole character. Returns bytes written. */
static size_t bs_jni_utf8_from_utf16(const uint16_t *units, size_t count,
                                     char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < count &&
            units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (units[i + 1] - 0xdc00);
            ++i;
        }
        unsigned char buf[4];
        size_t len;
        if (cp == 0) { buf[0] = 0xc0; buf[1] = 0x80; len = 2; }
        else if (cp < 0x80) { buf[0] = (unsigned char)cp; len = 1; }
        else if (cp < 0x800) {
            buf[0] = (unsigned char)(0xc0 | (cp >> 6));
            buf[1] = (unsigned char)(0x80 | (cp & 0x3f));
            len = 2;
        } else if (cp < 0x10000) {
            buf[0] = (unsigned char)(0xe0 | (cp >> 12));
            buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
            buf[2] = (unsigned char)(0x80 | (cp & 0x3f));
            len = 3;
        } else {
            buf[0] = (unsigned char)(0xf0 | (cp >> 18));
            buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
            buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
            buf[3] = (unsigned char)(0x80 | (cp & 0x3f));
            len = 4;
        }
        if (n + len + 1 > cap) break;
        memcpy(out + n, buf, len);
        n += len;
    }
    if (cap) out[n] = '\0';
    return n;
}

static BSJNIObject *bs_jni_string_object(void *env, void *ref,
                                         const char *op) {
    if (!bs_jni_require_env(env, op)) return NULL;
    BSJNIObject *string = bs_jni_known_object(ref);
    if (!string || string->kind != BS_JNI_OBJECT_STRING) {
        bs_jni_missing_host_value(op, "java/lang/String",
                                  "unrecognized handle");
        return NULL;
    }
    return string;
}

static void *bs_jni_new_string16(void *env, const uint16_t *chars,
                                 int32_t length) {
    if (!bs_jni_require_env(env, "NewString")) return NULL;
    if (length < 0 || (length && !chars)) return NULL;
    char text[BS_JNI_STRING_BYTES];
    bs_jni_utf8_from_utf16(chars, (size_t)length, text, sizeof text);
    return bs_jni_string_for_utf(text);
}
static int32_t bs_jni_get_string_length(void *env, void *ref) {
    BSJNIObject *s = bs_jni_string_object(env, ref, "GetStringLength");
    return s ? (int32_t)bs_jni_utf16_from_utf8(s->text, NULL) : 0;
}
static const uint16_t *bs_jni_get_string_chars(void *env, void *ref,
                                               uint8_t *is_copy) {
    BSJNIObject *s = bs_jni_string_object(env, ref, "GetStringChars");
    if (!s) return NULL;
    const size_t n = bs_jni_utf16_from_utf8(s->text, NULL);
    uint16_t *units = malloc((n + 1) * sizeof *units);
    if (!units) return NULL;
    bs_jni_utf16_from_utf8(s->text, units);
    units[n] = 0;
    if (is_copy) *is_copy = 1;
    return units;
}
static void bs_jni_release_string_chars(void *env, void *ref,
                                        const uint16_t *chars) {
    (void)env; (void)ref;
    free((void *)(uintptr_t)chars);
}
static void bs_jni_get_string_region(void *env, void *ref, int32_t start,
                                     int32_t length, uint16_t *buf) {
    BSJNIObject *s = bs_jni_string_object(env, ref, "GetStringRegion");
    if (!s || !buf || start < 0 || length < 0) return;
    uint16_t units[BS_JNI_STRING_BYTES];
    const size_t n = bs_jni_utf16_from_utf8(s->text, units);
    if ((size_t)start + (size_t)length > n) {
        bs_jni_lenient_note("GetStringRegion", "java/lang/String",
                            "out of bounds", NULL);
        return;
    }
    memcpy(buf, units + start, (size_t)length * sizeof *buf);
}
static void bs_jni_get_string_utf_region(void *env, void *ref, int32_t start,
                                         int32_t length, char *buf) {
    BSJNIObject *s = bs_jni_string_object(env, ref, "GetStringUTFRegion");
    if (!s || !buf || start < 0 || length < 0) return;
    uint16_t units[BS_JNI_STRING_BYTES];
    const size_t n = bs_jni_utf16_from_utf8(s->text, units);
    if ((size_t)start + (size_t)length > n) {
        bs_jni_lenient_note("GetStringUTFRegion", "java/lang/String",
                            "out of bounds", NULL);
        return;
    }
    /* Caller sizes buf for the modified-UTF-8 encoding plus NUL. */
    bs_jni_utf8_from_utf16(units + start, (size_t)length, buf,
                           (size_t)length * 3 + 1);
}

/* ---- Arrays (slots 171-214, 222, 223) -------------------------------- */
static size_t bs_jni_element_size(char type) {
    switch (type) {
        case 'Z': case 'B': return 1;
        case 'C': case 'S': return 2;
        case 'I': case 'F': return 4;
        case 'J': case 'D': return 8;
        default: return sizeof(void *);
    }
}

static BSJNIObject *bs_jni_array_new(char type, const char *class_name,
                                     int32_t length) {
    if (length < 0) return NULL;
    void *data = calloc((size_t)length ? (size_t)length : 1,
                        bs_jni_element_size(type));
    if (!data) return NULL;
    BSJNIObject *array = bs_jni_object_new(BS_JNI_OBJECT_ARRAY, class_name);
    if (!array) {
        free(data);
        return NULL;
    }
    array->array_data = data;
    array->array_length = length;
    array->array_type = type;
    return array;
}

static BSJNIObject *bs_jni_array_object(void *ref, const char *op) {
    BSJNIObject *array = ref ? bs_jni_known_object(ref) : NULL;
    if (!array || array->kind != BS_JNI_OBJECT_ARRAY) {
        bs_jni_lenient_note(op, NULL, ref ? "not an array" : "null array",
                            NULL);
        return NULL;
    }
    return array;
}

static int32_t bs_jni_get_array_length(void *env, void *ref) {
    if (!bs_jni_require_env(env, "GetArrayLength")) return 0;
    BSJNIObject *array = bs_jni_array_object(ref, "GetArrayLength");
    return array ? array->array_length : 0;
}

static void *bs_jni_new_object_array(void *env, int32_t length,
                                     void *element_class, void *initial) {
    if (!bs_jni_require_env(env, "NewObjectArray")) return NULL;
    BSJNIClass *klass = bs_jni_known_class(element_class);
    char name[BS_JNI_CLASS_BYTES];
    const char *element = klass ? klass->name : "java/lang/Object";
    if (element[0] == '[')
        snprintf(name, sizeof name, "[%.*s",
                 (int)sizeof name - 2, element);
    else
        snprintf(name, sizeof name, "[L%.*s;",
                 (int)sizeof name - 4, element);
    BSJNIObject *array = bs_jni_array_new('L', name, length);
    if (!array) return NULL;
    void *target = initial ? bs_jni_canonical_reference(initial) : NULL;
    BSJNIObject *object = target ? bs_jni_known_object(target) : NULL;
    if (object) object->pinned = 1;
    for (int32_t i = 0; target && i < length; ++i)
        ((void **)array->array_data)[i] = target;
    return array;
}

static void *bs_jni_get_object_array_element(void *env, void *ref,
                                             int32_t index) {
    if (!bs_jni_require_env(env, "GetObjectArrayElement")) return NULL;
    BSJNIObject *array = bs_jni_array_object(ref, "GetObjectArrayElement");
    if (!array || array->array_type != 'L' || index < 0 ||
        index >= array->array_length)
        return NULL;
    return ((void **)array->array_data)[index];
}

static void bs_jni_set_object_array_element(void *env, void *ref,
                                            int32_t index, void *value) {
    if (!bs_jni_require_env(env, "SetObjectArrayElement")) return;
    BSJNIObject *array = bs_jni_array_object(ref, "SetObjectArrayElement");
    if (!array || array->array_type != 'L' || index < 0 ||
        index >= array->array_length)
        return;
    void *target = value ? bs_jni_canonical_reference(value) : NULL;
    BSJNIObject *object = target ? bs_jni_known_object(target) : NULL;
    if (object) object->pinned = 1; /* the array keeps it reachable */
    ((void **)array->array_data)[index] = target;
}

#define BS_JNI_NEW_PRIMITIVE_ARRAY(Name, letter, klass) \
    static void *bs_jni_new_##Name##_array(void *env, int32_t length) { \
        if (!bs_jni_require_env(env, "New" #Name "Array")) return NULL; \
        return bs_jni_array_new(letter, klass, length); \
    }
BS_JNI_NEW_PRIMITIVE_ARRAY(Boolean, 'Z', "[Z")
BS_JNI_NEW_PRIMITIVE_ARRAY(Byte, 'B', "[B")
BS_JNI_NEW_PRIMITIVE_ARRAY(Char, 'C', "[C")
BS_JNI_NEW_PRIMITIVE_ARRAY(Short, 'S', "[S")
BS_JNI_NEW_PRIMITIVE_ARRAY(Int, 'I', "[I")
BS_JNI_NEW_PRIMITIVE_ARRAY(Long, 'J', "[J")
BS_JNI_NEW_PRIMITIVE_ARRAY(Float, 'F', "[F")
BS_JNI_NEW_PRIMITIVE_ARRAY(Double, 'D', "[D")
#undef BS_JNI_NEW_PRIMITIVE_ARRAY

/* Get<Type>ArrayElements / GetPrimitiveArrayCritical: direct storage. */
static void *bs_jni_get_array_elements(void *env, void *ref,
                                       uint8_t *is_copy) {
    if (!bs_jni_require_env(env, "Get<Type>ArrayElements")) return NULL;
    BSJNIObject *array = bs_jni_array_object(ref, "Get<Type>ArrayElements");
    if (!array || array->array_type == 'L') return NULL;
    if (is_copy) *is_copy = 0;
    return array->array_data;
}
static void bs_jni_release_array_elements(void *env, void *ref, void *elems,
                                          int32_t mode) {
    (void)env; (void)ref; (void)elems; (void)mode;
}

static int bs_jni_array_range(BSJNIObject *array, int32_t start,
                              int32_t length, const char *op) {
    if (!array || array->array_type == 'L' || start < 0 || length < 0 ||
        (int64_t)start + length > array->array_length) {
        bs_jni_lenient_note(op, array ? array->text : NULL,
                            "invalid region", NULL);
        return 0;
    }
    return 1;
}
static void bs_jni_get_array_region(void *env, void *ref, int32_t start,
                                    int32_t length, void *buf) {
    if (!bs_jni_require_env(env, "Get<Type>ArrayRegion")) return;
    BSJNIObject *array = bs_jni_array_object(ref, "Get<Type>ArrayRegion");
    if (!buf || !bs_jni_array_range(array, start, length,
                                    "Get<Type>ArrayRegion"))
        return;
    const size_t size = bs_jni_element_size(array->array_type);
    memcpy(buf, (const uint8_t *)array->array_data + (size_t)start * size,
           (size_t)length * size);
}
static void bs_jni_set_array_region(void *env, void *ref, int32_t start,
                                    int32_t length, const void *buf) {
    if (!bs_jni_require_env(env, "Set<Type>ArrayRegion")) return;
    BSJNIObject *array = bs_jni_array_object(ref, "Set<Type>ArrayRegion");
    if (!buf || !bs_jni_array_range(array, start, length,
                                    "Set<Type>ArrayRegion"))
        return;
    const size_t size = bs_jni_element_size(array->array_type);
    memcpy((uint8_t *)array->array_data + (size_t)start * size, buf,
           (size_t)length * size);
}

/* ---- Misc: IsAssignableFrom, AllocObject, monitors -------------------- */
static uint8_t bs_jni_is_assignable_from(void *env, void *from, void *to) {
    if (!bs_jni_require_env(env, "IsAssignableFrom")) return 0;
    BSJNIClass *a = bs_jni_known_class(from), *b = bs_jni_known_class(to);
    if (!a || !b) return 0;
    return !strcmp(b->name, "java/lang/Object") ||
           bs_jni_class_extends(a->name, b->name);
}
static void *bs_jni_alloc_object(void *env, void *class_ref) {
    if (!bs_jni_require_env(env, "AllocObject")) return NULL;
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    return klass ? bs_jni_new_host_instance(klass->name) : NULL;
}
/* One JNI-visible thread drives Java-side state here; Java monitors guard
 * nothing the host models, so entering/exiting always succeeds. */
static int32_t bs_jni_monitor(void *env, void *obj) {
    (void)env; (void)obj;
    return BS_JNI_OK;
}

/* ReflectionHelper.getMethodID/getFieldID/getConstructorID: Unity's managed
 * AndroidJNIHelper resolves members through these and then FromReflected*.
 * The reflected object records class/name/signature; resolution to a JNI ID
 * happens in FromReflectedMethod/Field with the usual GetMethodID rules. */
static int bs_jni_reflected_new(void *class_ref, void *name_ref, void *sig_ref,
                                char kind, int is_static, BSJNIValue *result) {
    BSJNIClass *klass = bs_jni_known_class(class_ref);
    const char *name = kind == 'C' ? "<init>" : bs_jni_string_text(name_ref);
    const char *sig = bs_jni_string_text(sig_ref);
    if (!klass || !name || !sig || strlen(name) >= 128 || strlen(sig) >= 256)
        return -1;
    BSJNIObject *object = bs_jni_object_new(BS_JNI_OBJECT_REFLECTED, klass->name);
    if (!object) return -1;
    object->member_kind = kind;
    object->member_static = is_static ? 1 : 0;
    snprintf(object->member_name, sizeof object->member_name, "%s", name);
    snprintf(object->member_sig, sizeof object->member_sig, "%s", sig);
    if (kind != 'F') {
        BSJNIHostMethod *m = bs_jni_find_host_method_fuzzy(klass->name, name, sig,
                                                           is_static);
        if (m && strcmp(m->signature, sig))
            snprintf(object->member_sig, sizeof object->member_sig, "%s", m->signature);
    }
    result->l = object;
    return 0;
}

static BSJNIObject *bs_jni_reflected(const void *ref, char kind) {
    BSJNIObject *object = bs_jni_known_object(ref);
    if (!object || object->kind != BS_JNI_OBJECT_REFLECTED) return NULL;
    if (kind == 'F' ? object->member_kind != 'F' : object->member_kind == 'F')
        return NULL;
    return object;
}

static int bs_jni_rt_reflect_method(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    return bs_jni_reflected_new(args[0].l, args[1].l, args[2].l, 'M', args[3].z, result);
}

static int bs_jni_rt_reflect_field(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    return bs_jni_reflected_new(args[0].l, args[1].l, args[2].l, 'F', args[3].z, result);
}

static int bs_jni_rt_reflect_constructor(void *user, void *env, void *receiver,
                                         const BSJNIValue *args, size_t argc,
                                         BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    return bs_jni_reflected_new(args[0].l, NULL, args[1].l, 'C', 0, result);
}

static int bs_jni_rt_reflect_field_signature(void *user, void *env, void *receiver,
                                             const BSJNIValue *args, size_t argc,
                                             BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    BSJNIObject *field = bs_jni_reflected(args[0].l, 'F');
    if (!field) return -1;
    result->l = bs_jni_string_for_utf(field->member_sig);
    return result->l ? 0 : -1;
}

static int bs_jni_rt_member_declaring_class(void *user, void *env, void *receiver,
                                            const BSJNIValue *args, size_t argc,
                                            BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *member = bs_jni_known_object(receiver);
    if (!member || member->kind != BS_JNI_OBJECT_REFLECTED) return -1;
    result->l = bs_jni_class_for_name(member->text);
    return result->l ? 0 : -1;
}

static int bs_jni_rt_member_name(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *member = bs_jni_known_object(receiver);
    if (!member || member->kind != BS_JNI_OBJECT_REFLECTED) return -1;
    char dotted[BS_JNI_CLASS_BYTES];
    bs_jni_dotted(dotted, sizeof dotted, member->text);
    result->l = bs_jni_string_for_utf(member->member_kind == 'C' ? dotted
                                                                 : member->member_name);
    return result->l ? 0 : -1;
}

/* ReflectionHelper.newProxyInstance(UnityPlayer, long, Class): an
 * AndroidJavaProxy's Java side. No Java code calls into it on this host, so
 * a typed instance of the interface is enough for the managed wrapper. */
static int bs_jni_rt_new_proxy_instance(void *user, void *env, void *receiver,
                                        const BSJNIValue *args, size_t argc,
                                        BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    BSJNIClass *iface = bs_jni_known_class(args[2].l);
    bs_jni_log("[jni] AndroidJavaProxy %s handle=%#llx",
               iface ? iface->name : "?", (unsigned long long)args[1].j);
    result->l = bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE,
                                  iface ? iface->name : "java/lang/Object");
    return result->l ? 0 : -1;
}

/* UnityPermissions.hasUserAuthorizedPermission: every Android permission is
 * granted on this host (no runtime permission model). */
static int bs_jni_rt_true(void *user, void *env, void *receiver,
                          const BSJNIValue *args, size_t argc, BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    memset(result, 0, sizeof *result);
    result->z = 1;
    return 0;
}

/* UnityPlayer.loadLibrary(name): Java System.loadLibrary for a native plugin.
 * Every packaged lib<name>.so is already mapped by the so-loader; dlopen of
 * the bare name resolves it. Unity only proceeds to UnityPluginLoad when this
 * reports success. */
static int bs_jni_rt_load_library(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    const char *name = bs_jni_string_text(args[0].l);
    bs_jni_log("[jni] UnityPlayer.loadLibrary(%s) -> true", name ? name : "?");
    memset(result, 0, sizeof *result);
    result->z = name ? 1 : 0;
    return 0;
}

/* JNI slot 7. */
static void *bs_jni_from_reflected_method(void *env, void *method) {
    BSJNIObject *m = bs_jni_reflected(method, 'M');
    if (!m) {
        bs_jni_log("[jni] FromReflectedMethod(%p): not a reflected method", method);
        return NULL;
    }
    void *klass = bs_jni_class_for_name(m->text);
    return m->member_static
        ? bs_jni_get_static_method(env, klass, m->member_name, m->member_sig)
        : (void *)bs_jni_get_method(env, klass, m->member_name, m->member_sig);
}

/* JNI slot 8. */
static void *bs_jni_from_reflected_field(void *env, void *field) {
    BSJNIObject *f = bs_jni_reflected(field, 'F');
    if (!f) {
        bs_jni_log("[jni] FromReflectedField(%p): not a reflected field", field);
        return NULL;
    }
    void *klass = bs_jni_class_for_name(f->text);
    return f->member_static
        ? bs_jni_get_static_field(env, klass, f->member_name, f->member_sig)
        : bs_jni_get_field(env, klass, f->member_name, f->member_sig);
}

/* Object.getClass(): the class of any modeled handle. */
static int bs_jni_rt_object_get_class(void *user, void *env, void *receiver,
                                      const BSJNIValue *args, size_t argc,
                                      BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    const char *name = bs_jni_object_class(receiver);
    if (!name) return -1;
    result->l = bs_jni_class_for_name(name);
    return result->l ? 0 : -1;
}

static void bs_jni_install_lenient_slots(void) {
    BSJniGenericFunction *t = g_env_functions;
    BS_JNI_SLOT(t, 11, bs_jni_is_assignable_from);
    BS_JNI_SLOT(t, 27, bs_jni_alloc_object);
    /* Field accessors; 100, 102 and 145 keep their observed-state paths,
     * which accept lenient IDs. */
    BS_JNI_SLOT(t, 95, bs_jni_lget_Object);
    BS_JNI_SLOT(t, 96, bs_jni_lget_Boolean);
    BS_JNI_SLOT(t, 97, bs_jni_lget_Byte);
    BS_JNI_SLOT(t, 98, bs_jni_lget_Char);
    BS_JNI_SLOT(t, 99, bs_jni_lget_Short);
    BS_JNI_SLOT(t, 101, bs_jni_lget_Long);
    BS_JNI_SLOT(t, 103, bs_jni_lget_Double);
    BS_JNI_SLOT(t, 104, bs_jni_lset_Object);
    BS_JNI_SLOT(t, 105, bs_jni_lset_Boolean);
    BS_JNI_SLOT(t, 106, bs_jni_lset_Byte);
    BS_JNI_SLOT(t, 107, bs_jni_lset_Char);
    BS_JNI_SLOT(t, 108, bs_jni_lset_Short);
    BS_JNI_SLOT(t, 109, bs_jni_lset_Int);
    BS_JNI_SLOT(t, 110, bs_jni_lset_Long);
    BS_JNI_SLOT(t, 111, bs_jni_lset_Float);
    BS_JNI_SLOT(t, 112, bs_jni_lset_Double);
    BS_JNI_SLOT(t, 146, bs_jni_lget_Boolean);
    BS_JNI_SLOT(t, 147, bs_jni_lget_Byte);
    BS_JNI_SLOT(t, 148, bs_jni_lget_Char);
    BS_JNI_SLOT(t, 149, bs_jni_lget_Short);
    BS_JNI_SLOT(t, 150, bs_jni_lget_Int);
    BS_JNI_SLOT(t, 151, bs_jni_lget_Long);
    BS_JNI_SLOT(t, 152, bs_jni_lget_Float);
    BS_JNI_SLOT(t, 153, bs_jni_lget_Double);
    BS_JNI_SLOT(t, 154, bs_jni_lset_Object);
    BS_JNI_SLOT(t, 155, bs_jni_lset_Boolean);
    BS_JNI_SLOT(t, 156, bs_jni_lset_Byte);
    BS_JNI_SLOT(t, 157, bs_jni_lset_Char);
    BS_JNI_SLOT(t, 158, bs_jni_lset_Short);
    BS_JNI_SLOT(t, 159, bs_jni_lset_Int);
    BS_JNI_SLOT(t, 160, bs_jni_lset_Long);
    BS_JNI_SLOT(t, 161, bs_jni_lset_Float);
    BS_JNI_SLOT(t, 162, bs_jni_lset_Double);
    /* Strings */
    BS_JNI_SLOT(t, 163, bs_jni_new_string16);
    BS_JNI_SLOT(t, 164, bs_jni_get_string_length);
    BS_JNI_SLOT(t, 165, bs_jni_get_string_chars);
    BS_JNI_SLOT(t, 166, bs_jni_release_string_chars);
    BS_JNI_SLOT(t, 220, bs_jni_get_string_region);
    BS_JNI_SLOT(t, 221, bs_jni_get_string_utf_region);
    BS_JNI_SLOT(t, 224, bs_jni_get_string_chars);
    BS_JNI_SLOT(t, 225, bs_jni_release_string_chars);
    /* Arrays */
    BS_JNI_SLOT(t, 171, bs_jni_get_array_length);
    BS_JNI_SLOT(t, 172, bs_jni_new_object_array);
    BS_JNI_SLOT(t, 173, bs_jni_get_object_array_element);
    BS_JNI_SLOT(t, 174, bs_jni_set_object_array_element);
    BS_JNI_SLOT(t, 175, bs_jni_new_Boolean_array);
    BS_JNI_SLOT(t, 176, bs_jni_new_Byte_array);
    BS_JNI_SLOT(t, 177, bs_jni_new_Char_array);
    BS_JNI_SLOT(t, 178, bs_jni_new_Short_array);
    BS_JNI_SLOT(t, 179, bs_jni_new_Int_array);
    BS_JNI_SLOT(t, 180, bs_jni_new_Long_array);
    BS_JNI_SLOT(t, 181, bs_jni_new_Float_array);
    BS_JNI_SLOT(t, 182, bs_jni_new_Double_array);
    for (unsigned i = 183; i <= 190; ++i)
        BS_JNI_SLOT(t, i, bs_jni_get_array_elements);
    for (unsigned i = 191; i <= 198; ++i)
        BS_JNI_SLOT(t, i, bs_jni_release_array_elements);
    for (unsigned i = 199; i <= 206; ++i)
        BS_JNI_SLOT(t, i, bs_jni_get_array_region);
    for (unsigned i = 207; i <= 214; ++i)
        BS_JNI_SLOT(t, i, bs_jni_set_array_region);
    BS_JNI_SLOT(t, 222, bs_jni_get_array_elements);
    BS_JNI_SLOT(t, 223, bs_jni_release_array_elements);
    /* Monitors */
    BS_JNI_SLOT(t, 217, bs_jni_monitor);
    BS_JNI_SLOT(t, 218, bs_jni_monitor);
}

/* java.util.Locale default: en-US (Unity derives Application.systemLanguage). */
static int bs_jni_rt_locale_get_default(void *user, void *env, void *receiver,
                                        const BSJNIValue *args, size_t argc,
                                        BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    BSJNIObject *locale = bs_jni_object_new(BS_JNI_OBJECT_LOCALE, BS_LOCALE);
    if (!locale) return -1;
    strcpy(locale->locale.language, "en");
    strcpy(locale->locale.country, "US");
    result->l = locale;
    return 0;
}
static int bs_jni_rt_locale_to_tag(void *user, void *env, void *receiver,
                                   const BSJNIValue *args, size_t argc,
                                   BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    BSJNIObject *o = bs_jni_known_object(receiver);
    char tag[2 * BS_JNI_LOCALE_TAG_BYTES + 2] = "en-US";
    if (o && o->kind == BS_JNI_OBJECT_LOCALE && o->locale.language[0])
        snprintf(tag, sizeof tag, o->locale.country[0] ? "%s-%s" : "%s",
                 o->locale.language, o->locale.country);
    result->l = bs_jni_string_for_utf(tag);
    return result->l ? 0 : -1;
}

static int bs_jni_rt_audio_get_devices(void *user, void *env, void *receiver,
                                       const BSJNIValue *args, size_t argc,
                                       BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    result->l = bs_jni_array_new('L', "[Landroid/media/AudioDeviceInfo;", 0);
    return result->l ? 0 : -1;
}

static int bs_jni_rt_get_system_service(void *user, void *env, void *receiver,
                                        const BSJNIValue *args, size_t argc,
                                        BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    static BSJNIObject *audio;
    if (!audio) { audio = bs_jni_object_new(BS_JNI_OBJECT_HOST_INSTANCE, "android/media/AudioManager");
                  if (audio) audio->pinned = 1; }
    result->l = audio;
    return 0;
}

static void bs_jni_register_runtime_methods(void) {
    static const struct {
        const char *klass, *name, *signature;
        int is_static;
        BSJNIHostMethodFn fn;
    } methods[] = {
        {"java/lang/Class", "forName",
         "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", 1,
         bs_jni_rt_class_for_name3},
        {"java/lang/Class", "forName",
         "(Ljava/lang/String;)Ljava/lang/Class;", 1,
         bs_jni_rt_class_for_name1},
        {"java/lang/Class", "getClassLoader",
         "()Ljava/lang/ClassLoader;", 0, bs_jni_rt_get_class_loader},
        {"java/lang/Class", "getName", "()Ljava/lang/String;", 0,
         bs_jni_rt_class_get_name},
        {"java/lang/ClassLoader", "loadClass",
         "(Ljava/lang/String;)Ljava/lang/Class;", 0, bs_jni_rt_load_class},
        {"java/lang/Throwable", "getMessage", "()Ljava/lang/String;", 0,
         bs_jni_rt_throwable_get_message},
        {"java/lang/Object", "toString", "()Ljava/lang/String;", 0,
         bs_jni_rt_object_to_string},
        {"android/content/Context", "getSystemService",
         "(Ljava/lang/String;)Ljava/lang/Object;", 0, bs_jni_rt_get_system_service},
        {"android/app/Activity", "getSystemService",
         "(Ljava/lang/String;)Ljava/lang/Object;", 0, bs_jni_rt_get_system_service},
        {"android/media/AudioManager", "getDevices",
         "(I)[Landroid/media/AudioDeviceInfo;", 0, bs_jni_rt_audio_get_devices},
        {"java/util/Locale", "getDefault", "()Ljava/util/Locale;", 1,
         bs_jni_rt_locale_get_default},
        {"java/util/Locale", "toLanguageTag", "()Ljava/lang/String;", 0,
         bs_jni_rt_locale_to_tag},
        {"java/lang/Object", "getClass", "()Ljava/lang/Class;", 0,
         bs_jni_rt_object_get_class},
        {"com/unity3d/player/ReflectionHelper", "getMethodID",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;",
         1, bs_jni_rt_reflect_method},
        {"com/unity3d/player/ReflectionHelper", "getFieldID",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;",
         1, bs_jni_rt_reflect_field},
        {"com/unity3d/player/ReflectionHelper", "getConstructorID",
         "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;",
         1, bs_jni_rt_reflect_constructor},
        {"com/unity3d/player/ReflectionHelper", "getFieldSignature",
         "(Ljava/lang/reflect/Field;)Ljava/lang/String;", 1,
         bs_jni_rt_reflect_field_signature},
        {"java/lang/reflect/Field", "getDeclaringClass", "()Ljava/lang/Class;", 0,
         bs_jni_rt_member_declaring_class},
        {"java/lang/reflect/Method", "getDeclaringClass", "()Ljava/lang/Class;", 0,
         bs_jni_rt_member_declaring_class},
        {"java/lang/reflect/Constructor", "getDeclaringClass", "()Ljava/lang/Class;", 0,
         bs_jni_rt_member_declaring_class},
        {"com/unity3d/player/ReflectionHelper", "newProxyInstance",
         "(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;", 1,
         bs_jni_rt_new_proxy_instance},
        {"com/unity3d/player/UnityPermissions", "hasUserAuthorizedPermission",
         "(Lcom.google.androidgamesdk.GameActivity;Ljava/lang/String;)Z", 1, bs_jni_rt_true},
        {"com/unity3d/player/UnityPermissions", "hasUserAuthorizedPermission",
         "(Landroid/app/Activity;Ljava/lang/String;)Z", 1, bs_jni_rt_true},
        {"com/unity3d/player/UnityPlayerForGameActivity", "loadLibrary",
         "(Ljava/lang/String;)Z", 0, bs_jni_rt_load_library},
        {"com/unity3d/player/UnityPlayer", "loadLibrary",
         "(Ljava/lang/String;)Z", 0, bs_jni_rt_load_library},
        {"java/lang/reflect/Field", "getName", "()Ljava/lang/String;", 0,
         bs_jni_rt_member_name},
        {"java/lang/reflect/Method", "getName", "()Ljava/lang/String;", 0,
         bs_jni_rt_member_name},        {"android/content/Context", "getFilesDir", "()Ljava/io/File;", 0,
         bs_jni_rt_files_dir},
        {"android/content/Context", "getObbDir", "()Ljava/io/File;", 0,
         bs_jni_rt_obb_dir},
        {"android/content/Context", "getObbDirs", "()[Ljava/io/File;", 0,
         bs_jni_rt_obb_dirs},
        {"android/content/Context", "getPackageManager",
         "()Landroid/content/pm/PackageManager;", 0,
         bs_jni_rt_package_manager},
        {"android/content/pm/PackageManager", "getPackageInfo",
         "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", 0,
         bs_jni_rt_package_info},
        {"android/content/Context", "getApplicationInfo",
         "()Landroid/content/pm/ApplicationInfo;", 0,
         bs_jni_rt_application_info},
        {"android/os/Environment", "getExternalStorageState",
         "()Ljava/lang/String;", 1, bs_jni_rt_storage_state},
        {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", 0,
         bs_jni_rt_string_equals},
        {"java/lang/String", "length", "()I", 0, bs_jni_rt_string_length},
        {"java/lang/String", "isEmpty", "()Z", 0, bs_jni_rt_string_is_empty},
        {"java/lang/ClassLoader", "findLibrary",
         "(Ljava/lang/String;)Ljava/lang/String;", 0, bs_jni_rt_find_library},
        {"android/content/Context", "getPackageCodePath",
         "()Ljava/lang/String;", 0, bs_jni_rt_package_code_path},
        {"android/content/Context", "getExternalFilesDir",
         "(Ljava/lang/String;)Ljava/io/File;", 0,
         bs_jni_rt_external_files_dir},
        {"java/io/File", "getPath", "()Ljava/lang/String;", 0,
         bs_jni_rt_file_path},
        {"java/io/File", "getAbsolutePath", "()Ljava/lang/String;", 0,
         bs_jni_rt_file_path},
        {"java/io/File", "getCanonicalPath", "()Ljava/lang/String;", 0,
         bs_jni_rt_file_path},
        {"java/io/File", "toString", "()Ljava/lang/String;", 0,
         bs_jni_rt_file_path},
        {"android/content/res/AssetManager", "open",
         "(Ljava/lang/String;)Ljava/io/InputStream;", 0, bs_jni_rt_asset_open},
        {"android/content/res/AssetManager", "open",
         "(Ljava/lang/String;I)Ljava/io/InputStream;", 0, bs_jni_rt_asset_open},
        {"java/io/InputStream", "close", "()V", 0, bs_jni_rt_noop},
        {"java/util/Scanner", "<init>",
         "(Ljava/io/InputStream;Ljava/lang/String;)V", 0, bs_jni_rt_scanner_init},
        {"java/util/Scanner", "<init>", "(Ljava/io/InputStream;)V", 0,
         bs_jni_rt_scanner_init},
        {"java/util/Scanner", "useDelimiter",
         "(Ljava/lang/String;)Ljava/util/Scanner;", 0, bs_jni_rt_scanner_self},
        {"java/util/Scanner", "hasNext", "()Z", 0, bs_jni_rt_scanner_has_next},
        {"java/util/Scanner", "next", "()Ljava/lang/String;", 0,
         bs_jni_rt_scanner_next},
        {"java/util/Scanner", "close", "()V", 0, bs_jni_rt_noop},
        {"java/io/File", "<init>", "(Ljava/lang/String;)V", 0, bs_jni_rt_file_init1},
        {"java/lang/String", "<init>", "([BLjava/lang/String;)V", 0,
         bs_jni_rt_string_init_bytes},
        {"java/lang/String", "<init>", "([B)V", 0, bs_jni_rt_string_init_bytes},
        {"java/io/File", "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", 0,
         bs_jni_rt_file_init_ss},
        {"java/io/File", "<init>", "(Ljava/io/File;Ljava/lang/String;)V", 0,
         bs_jni_rt_file_init_fs},
        {"java/io/File", "getParent", "()Ljava/lang/String;", 0,
         bs_jni_rt_file_get_parent},
        {"java/io/File", "getParentFile", "()Ljava/io/File;", 0,
         bs_jni_rt_file_get_parent_file},
        {"java/io/File", "getAbsoluteFile", "()Ljava/io/File;", 0, bs_jni_rt_scanner_self},
        {"java/io/File", "getCanonicalFile", "()Ljava/io/File;", 0, bs_jni_rt_scanner_self},
        {"java/io/File", "getName", "()Ljava/lang/String;", 0, bs_jni_rt_file_get_name},
        {"java/io/File", "exists", "()Z", 0, bs_jni_rt_file_exists},
        {"java/io/File", "isDirectory", "()Z", 0, bs_jni_rt_file_is_directory},
        {"java/io/File", "isFile", "()Z", 0, bs_jni_rt_file_is_file},
        {"java/io/File", "length", "()J", 0, bs_jni_rt_file_length},
        {"java/io/File", "canRead", "()Z", 0, bs_jni_rt_file_true_if_exists},
        {"java/io/File", "canWrite", "()Z", 0, bs_jni_rt_file_true_if_exists},
        {"java/io/File", "mkdir", "()Z", 0, bs_jni_rt_file_mkdir},
        {"java/io/File", "mkdirs", "()Z", 0, bs_jni_rt_file_mkdirs},
        {"java/io/File", "delete", "()Z", 0, bs_jni_rt_file_delete},
        {"java/io/File", "getFreeSpace", "()J", 0, bs_jni_rt_file_space},
        {"java/io/File", "getUsableSpace", "()J", 0, bs_jni_rt_file_space},
        {"java/io/File", "getTotalSpace", "()J", 0, bs_jni_rt_file_space},
        {"java/lang/StringBuilder", "<init>", "()V", 0, bs_jni_rt_builder_init},
        {"java/lang/StringBuilder", "<init>", "(I)V", 0, bs_jni_rt_builder_init},
        {"java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V", 0,
         bs_jni_rt_builder_init_s},
        {"java/lang/StringBuilder", "append",
         "(Ljava/lang/String;)Ljava/lang/StringBuilder;", 0, bs_jni_rt_builder_append_s},
        {"java/lang/StringBuilder", "append",
         "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", 0, bs_jni_rt_builder_append_s},
        {"java/lang/StringBuilder", "append",
         "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;", 0,
         bs_jni_rt_builder_append_s},
        {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", 0,
         bs_jni_rt_builder_append_i},
        {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", 0,
         bs_jni_rt_builder_append_j},
        {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", 0,
         bs_jni_rt_builder_append_c},
        {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", 0,
         bs_jni_rt_builder_append_z},
        {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;", 0,
         bs_jni_rt_builder_to_string},
        {"java/lang/StringBuilder", "length", "()I", 0, bs_jni_rt_builder_length},
        {"java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;", 0,
         bs_jni_rt_string_concat},
        {"java/lang/String", "startsWith", "(Ljava/lang/String;)Z", 0,
         bs_jni_rt_string_starts_with},
        {"java/lang/String", "endsWith", "(Ljava/lang/String;)Z", 0,
         bs_jni_rt_string_ends_with},
        {"java/lang/String", "contains", "(Ljava/lang/CharSequence;)Z", 0,
         bs_jni_rt_string_contains},
        {"java/lang/String", "toLowerCase", "()Ljava/lang/String;", 0,
         bs_jni_rt_string_to_lower},
        {"java/lang/String", "toString", "()Ljava/lang/String;", 0,
         bs_jni_rt_string_self},
        {"java/lang/String", "trim", "()Ljava/lang/String;", 0, bs_jni_rt_string_self},
        {"java/lang/String", "intern", "()Ljava/lang/String;", 0, bs_jni_rt_string_self},
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; ++i)
        if (bs_jni_register_host_method(methods[i].klass, methods[i].name,
                                        methods[i].signature,
                                        methods[i].is_static, methods[i].fn,
                                        NULL) != BS_JNI_OK)
            bs_jni_error("runtime method %s.%s%s not registered",
                         methods[i].klass, methods[i].name,
                         methods[i].signature);
}

int bs_jni_init(BSJNILogFn log, void *user) {
    if (g_initialized)
        return BS_JNI_OK;
    g_logger = log;
    g_logger_user = user;
    g_native_count = 0;
    g_class_count = 0;
    g_object_count = 0;
    g_unityplayer_host = NULL;
    g_app_class_loader = NULL;
    g_method_count = 0;
    g_reference_count = 0;
    g_reference_free_count = 0;
    g_reference_free_head = 0;
    g_object_free_count = 0;
    g_local_frame_count = 0;
    g_unityplayer_current_activity = NULL;
    g_unityplayer_current_context = NULL;
    g_last_error[0] = '\0';
    g_missing_member_pending = 0;
    g_live_exception = NULL;
    g_attach_generation = 1;
    g_attached = 1; /* NativeActivity/bootstrap main thread starts attached. */
    g_thread_env_table = g_env_functions;
    for (size_t i = 0; i < BS_JNI_ENV_SLOTS; ++i)
        g_env_functions[i] = i < 4 ? NULL :
            (BSJniGenericFunction)g_jni_traps[i];
    for (size_t i = 0; i < BS_JNI_VM_SLOTS; ++i)
        g_vm_functions[i] = i < 3 ? NULL :
            (BSJniGenericFunction)bs_jni_vm_unsupported;

    BS_JNI_SLOT(g_env_functions, 4, bs_jni_get_version);
    BS_JNI_SLOT(g_env_functions, 6, bs_jni_find_class);
    BS_JNI_SLOT(g_env_functions, 7, bs_jni_from_reflected_method);
    BS_JNI_SLOT(g_env_functions, 8, bs_jni_from_reflected_field);
    BS_JNI_SLOT(g_env_functions, 10, bs_jni_get_superclass);
    BS_JNI_SLOT(g_env_functions, 13, bs_jni_throw);
    BS_JNI_SLOT(g_env_functions, 14, bs_jni_throw_new);
    BS_JNI_SLOT(g_env_functions, 15, bs_jni_exception_occurred);
    BS_JNI_SLOT(g_env_functions, 16, bs_jni_exception_describe);
    BS_JNI_SLOT(g_env_functions, 17, bs_jni_exception_clear);
    BS_JNI_SLOT(g_env_functions, 18, bs_jni_fatal_error);
    BS_JNI_SLOT(g_env_functions, 19, bs_jni_push_local_frame);
    BS_JNI_SLOT(g_env_functions, 20, bs_jni_pop_local_frame);
    BS_JNI_SLOT(g_env_functions, 21, bs_jni_new_global_ref);
    BS_JNI_SLOT(g_env_functions, 22, bs_jni_delete_global_ref);
    BS_JNI_SLOT(g_env_functions, 23, bs_jni_delete_local_ref);
    BS_JNI_SLOT(g_env_functions, 24, bs_jni_is_same_object);
    BS_JNI_SLOT(g_env_functions, 25, bs_jni_new_local_ref);
    BS_JNI_SLOT(g_env_functions, 26, bs_jni_ensure_local_capacity);
    BS_JNI_SLOT(g_env_functions, 28, bs_jni_new_object);
    BS_JNI_SLOT(g_env_functions, 29, bs_jni_new_object_v);
    BS_JNI_SLOT(g_env_functions, 30, bs_jni_new_object_a);
    BS_JNI_SLOT(g_env_functions, 31, bs_jni_get_object_class);
    BS_JNI_SLOT(g_env_functions, 32, bs_jni_is_instance_of);
    BS_JNI_SLOT(g_env_functions, 33, bs_jni_get_method);
    BS_JNI_SLOT(g_env_functions, 34, bs_jni_call_object);
    BS_JNI_SLOT(g_env_functions, 35, bs_jni_call_object_v);
    BS_JNI_SLOT(g_env_functions, 36, bs_jni_call_object_a);
    BS_JNI_SLOT(g_env_functions, 37, bs_jni_call_boolean);
    BS_JNI_SLOT(g_env_functions, 38, bs_jni_call_boolean_v);
    BS_JNI_SLOT(g_env_functions, 39, bs_jni_call_boolean_a);
    BS_JNI_SLOT(g_env_functions, 49, bs_jni_call_int);
    BS_JNI_SLOT(g_env_functions, 50, bs_jni_call_int_v);
    BS_JNI_SLOT(g_env_functions, 51, bs_jni_call_int_a);
    BS_JNI_SLOT(g_env_functions, 52, bs_jni_call_long);
    BS_JNI_SLOT(g_env_functions, 53, bs_jni_call_long_v);
    BS_JNI_SLOT(g_env_functions, 54, bs_jni_call_long_a);
    BS_JNI_SLOT(g_env_functions, 55, bs_jni_call_float);
    BS_JNI_SLOT(g_env_functions, 56, bs_jni_call_float_v);
    BS_JNI_SLOT(g_env_functions, 57, bs_jni_call_float_a);
    BS_JNI_SLOT(g_env_functions, 61, bs_jni_call_void);
    BS_JNI_SLOT(g_env_functions, 62, bs_jni_call_void_v);
    BS_JNI_SLOT(g_env_functions, 63, bs_jni_call_void_a);
    BS_JNI_SLOT(g_env_functions, 94, bs_jni_get_field);
    BS_JNI_SLOT(g_env_functions, 100, bs_jni_get_int_field);
    BS_JNI_SLOT(g_env_functions, 102, bs_jni_get_float_field);
    BS_JNI_SLOT(g_env_functions, 113, bs_jni_get_static_method);
    BS_JNI_SLOT(g_env_functions, 129, bs_jni_call_static_int);
    BS_JNI_SLOT(g_env_functions, 130, bs_jni_call_static_int_v);
    BS_JNI_SLOT(g_env_functions, 131, bs_jni_call_static_int_a);
    BS_JNI_SLOT(g_env_functions, 144, bs_jni_get_static_field);
    BS_JNI_SLOT(g_env_functions, 145, bs_jni_get_static_object_field);
    BS_JNI_SLOT(g_env_functions, 167, bs_jni_new_string_utf);
    BS_JNI_SLOT(g_env_functions, 168, bs_jni_string_utf_length);
    BS_JNI_SLOT(g_env_functions, 169, bs_jni_string_utf_chars);
    BS_JNI_SLOT(g_env_functions, 170, bs_jni_release_utf);
    BS_JNI_SLOT(g_env_functions, 215, bs_jni_register_natives);
    BS_JNI_SLOT(g_env_functions, 219, bs_jni_get_java_vm);
    BS_JNI_SLOT(g_env_functions, 226, bs_jni_new_weak_global_ref);
    BS_JNI_SLOT(g_env_functions, 227, bs_jni_delete_weak_global_ref);
    BS_JNI_SLOT(g_env_functions, 228, bs_jni_exception_check);
    BS_JNI_SLOT(g_env_functions, 232, bs_jni_get_object_ref_type);
    bs_jni_install_host_call_slots();
    bs_jni_install_lenient_slots();
    bs_jni_register_runtime_methods();

    BS_JNI_SLOT(g_vm_functions, 3, bs_jni_vm_destroy);
    BS_JNI_SLOT(g_vm_functions, 4, bs_jni_vm_attach);
    BS_JNI_SLOT(g_vm_functions, 5, bs_jni_vm_detach);
    BS_JNI_SLOT(g_vm_functions, 6, bs_jni_vm_get_env);
    BS_JNI_SLOT(g_vm_functions, 7, bs_jni_vm_attach);

    g_activity = bs_jni_object_new(
        BS_JNI_OBJECT_ACTIVITY, "com.beatgames.beatsaber");
    g_asset_manager = bs_jni_object_new(
        BS_JNI_OBJECT_ASSET_MANAGER, "android/content/res/AssetManager");
    if (!g_activity || !g_asset_manager)
        return BS_JNI_ERR;
    g_initialized = 1;
    bs_jni_log("[jni] JNI 1.6 bridge ready; GameActivity and AssetManager "
               "handles are synthetic, no JavaVM bytecode runtime exists");
    return BS_JNI_OK;
}

void *bs_jni_vm(void) {
    return g_initialized ? (void *)&g_vm_table : NULL;
}
void *bs_jni_env(void) {
    return g_initialized && g_attached ?
           (void *)&g_thread_env_table : NULL;
}
void *bs_jni_activity(void) {
    return g_initialized ? g_activity : NULL;
}
void *bs_jni_asset_manager(void) {
    return g_initialized ? g_asset_manager : NULL;
}
size_t bs_jni_registration_count(void) {
    return g_native_count;
}
const BSJNINativeRegistration *bs_jni_registration_at(size_t index) {
    return index < g_native_count ? &g_native_methods[index] : NULL;
}
const char *bs_jni_last_error(void) {
    return g_last_error;
}

int bs_jni_gameactivity_natives_captured(void) {
    size_t found = 0;
    for (size_t i = 0; i < BS_JNI_GAMEACTIVITY_NATIVE_COUNT; ++i) {
        const BSJNIKnownNative *decl = &g_gameactivity_natives[i];
        size_t matches = 0;
        for (size_t j = 0; j < g_native_count; ++j) {
            const BSJNINativeRegistration *entry = &g_native_methods[j];
            if (!strcmp(entry->class_name, BS_GAME_ACTIVITY) &&
                !strcmp(entry->method_name, decl->name) &&
                !strcmp(entry->signature, decl->signature) &&
                entry->function_address != 0)
                ++matches;
        }
        if (matches != 1) return 0;
        ++found;
    }
    return found == BS_JNI_GAMEACTIVITY_NATIVE_COUNT;
}

int bs_jni_gameactivity_registration_abi_ready(void) {
    /* libgame JNI_OnLoad@0x1d4cc calls VM->GetEnv(JNI_1_6), then
     * GameActivity_register@0x1bd2c and GameActivityEventsInit@0x1fc88.
     * All JNI operations in those registration paths are metadata lookups,
     * DeleteLocalRef, or RegisterNatives. Exact 77/24 counts pinned above.
     * Host imports/stack canary/TLS are deliberately caller-side checks. */
    if (!g_initialized || !g_attached || g_missing_member_pending ||
        g_thread_env_table != g_env_functions ||
        g_vm_table != g_vm_functions ||
        g_class_count == BS_JNI_MAX_CLASSES ||
        g_native_count > BS_JNI_MAX_NATIVES - BS_JNI_GAMEACTIVITY_NATIVE_COUNT)
        return 0;

    if (g_vm_functions[6] != (BSJniGenericFunction)bs_jni_vm_get_env ||
        g_env_functions[6] != (BSJniGenericFunction)bs_jni_find_class ||
        g_env_functions[23] != (BSJniGenericFunction)bs_jni_delete_local_ref ||
        g_env_functions[33] != (BSJniGenericFunction)bs_jni_get_method ||
        g_env_functions[94] != (BSJniGenericFunction)bs_jni_get_field ||
        g_env_functions[113] != (BSJniGenericFunction)bs_jni_get_static_method ||
        g_env_functions[215] != (BSJniGenericFunction)bs_jni_register_natives)
        return 0;

    for (size_t i = 0; i < 77; ++i) {
        const BSJNIKnownMember *member = &g_gameactivity_members[i];
        if (!member->class_name || !*member->class_name || !member->name ||
            !*member->name || !member->signature || !*member->signature ||
            (member->type != BS_JNI_INSTANCE_FIELD &&
             !bs_jni_valid_signature(member->signature))) return 0;
        if (member->type == BS_JNI_INSTANCE_FIELD &&
            strcmp(member->signature, "I") &&
            strcmp(member->signature, "F")) return 0;
        for (size_t j = 0; j < i; ++j) {
            const BSJNIKnownMember *old = &g_gameactivity_members[j];
            if (old->type == member->type &&
                !strcmp(old->class_name, member->class_name) &&
                !strcmp(old->name, member->name) &&
                !strcmp(old->signature, member->signature)) return 0;
        }
    }
    for (size_t i = 0; i < BS_JNI_GAMEACTIVITY_NATIVE_COUNT; ++i) {
        const BSJNIKnownNative *method = &g_gameactivity_natives[i];
        if (!method->name || !*method->name ||
            !bs_jni_valid_signature(method->signature)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(g_gameactivity_natives[j].name, method->name) &&
                !strcmp(g_gameactivity_natives[j].signature,
                        method->signature)) return 0;
    }
    return 1;
}

/* The GameActivity Surface JNI identity is distinct from an NWindow*. Only
 * libnx's live NWindow validation establishes a real native attachment.
 * Callers must keep their NWindow alive until bs_jni_revoke_surface(). */
void *bs_jni_surface_from_nwindow(void *native_window) {
#if defined(__SWITCH__)
    if (!g_initialized || !native_window ||
        !nwindowIsValid((NWindow *)native_window)) {
        bs_jni_error("Java Surface requires a live, verified libnx NWindow");
        return NULL;
    }
    BSJNIObject *object = bs_jni_object_new(
        BS_JNI_OBJECT_SURFACE, "android/view/Surface");
    if (object) object->native_window = native_window;
    return object;
#else
    (void)native_window;
    bs_jni_error("Java Surface requires libnx NWindow verification");
    return NULL;
#endif
}

void *bs_jni_backed_surface_nwindow(void *env, void *surface) {
    if (!bs_jni_env_is_current(env)) return NULL;
    BSJNIObject *object = bs_jni_known_object(surface);
    if (!object || object->kind != BS_JNI_OBJECT_SURFACE ||
        !object->native_window) return NULL;
#if defined(__SWITCH__)
    return nwindowIsValid((NWindow *)object->native_window) ?
           object->native_window : NULL;
#else
    return NULL;
#endif
}

int bs_jni_is_backed_surface(void *env, void *surface) {
    return bs_jni_backed_surface_nwindow(env, surface) != NULL;
}

void bs_jni_revoke_surface(void *surface) {
    BSJNIObject *object = bs_jni_known_object(surface);
    if (object && object->kind == BS_JNI_OBJECT_SURFACE)
        object->native_window = NULL;
}

static uint32_t bs_jni_gameactivity_host_service_mask(
    const BSJNIGameActivityHostServices *host) {
    uint32_t mask = 0;
    if (host->finish) mask |= BS_JNI_HOST_FINISH;
    if (host->set_window_flags) mask |= BS_JNI_HOST_WINDOW_FLAGS;
    if (host->set_ime_editor_info_fields) mask |= BS_JNI_HOST_IME_EDITOR_INFO;
    if (host->get_window_insets) mask |= BS_JNI_HOST_WINDOW_INSETS;
    if (host->get_waterfall_insets) mask |= BS_JNI_HOST_WATERFALL_INSETS;
    return mask;
}

int bs_jni_gameactivity_host_supports_insets(void *activity) {
    BSJNIObject *receiver = bs_jni_known_object(activity);
    return receiver && receiver->kind == BS_JNI_OBJECT_GAME_ACTIVITY_HOST &&
           receiver->gameactivity_host.get_window_insets &&
           receiver->gameactivity_host.get_waterfall_insets;
}

int bs_jni_gameactivity_host_supports_ime(void *activity) {
    BSJNIObject *receiver = bs_jni_known_object(activity);
    return receiver && receiver->kind == BS_JNI_OBJECT_GAME_ACTIVITY_HOST &&
           receiver->gameactivity_host.set_ime_editor_info_fields;
}

static int bs_jni_gameactivity_configuration_complete(
    const BSJNIObject *configuration) {
    return configuration &&
        configuration->kind == BS_JNI_OBJECT_CONFIGURATION &&
        configuration->observed_count == 18 &&
        configuration->locale_list &&
        configuration->locale_list->locale_count > 0;
}

int bs_jni_gameactivity_bind_host(void *activity, void *asset_manager,
                                 void *configuration, void *surface) {
    /* The pinned initializer reads Configuration and starts NativeCode and
     * GameTextInput with no input connection. Its IME and inset methods are
     * available only when their respective host services can answer a call. */
    const uint32_t required_services = BS_JNI_HOST_FINISH |
                                       BS_JNI_HOST_WINDOW_FLAGS;
    BSJNIObject *receiver = bs_jni_known_object(activity);
    BSJNIObject *config = bs_jni_known_object(configuration);
    if (!receiver || receiver->kind != BS_JNI_OBJECT_GAME_ACTIVITY_HOST ||
        asset_manager != g_asset_manager ||
        (bs_jni_gameactivity_host_service_mask(&receiver->gameactivity_host) &
         required_services) != required_services ||
        !bs_jni_gameactivity_configuration_complete(config) ||
        !bs_jni_is_backed_surface(bs_jni_env(), surface))
        return BS_JNI_ERR;

    if (g_gameactivity_receiver &&
        (g_gameactivity_receiver != receiver ||
         g_gameactivity_configuration != config ||
         g_gameactivity_surface != surface))
        return BS_JNI_ERR;
    g_gameactivity_receiver = receiver;
    g_gameactivity_configuration = config;
    g_gameactivity_surface = surface;
    return BS_JNI_OK;
}

int bs_jni_gameactivity_launch_matches(void *activity, void *asset_manager,
                                      void *configuration) {
    return g_gameactivity_receiver && activity == g_gameactivity_receiver &&
        asset_manager == g_asset_manager &&
        configuration == g_gameactivity_configuration &&
        bs_jni_is_backed_surface(bs_jni_env(), g_gameactivity_surface);
}

int bs_jni_unityplayer_has_current_activity(void) {
    void *env = bs_jni_env();
    return env && !g_missing_member_pending &&
           g_unityplayer_current_activity &&
           g_unityplayer_current_activity == g_gameactivity_receiver &&
           g_unityplayer_current_context == g_unityplayer_current_activity &&
           g_gameactivity_configuration &&
           bs_jni_is_backed_surface(env, g_gameactivity_surface);
}

int bs_jni_unityplayer_observe_current_activity(void *activity) {
    if (!g_initialized || !bs_jni_env() || g_missing_member_pending) {
        bs_jni_error("UnityPlayer.currentActivity observation requires an "
                     "attached JNI host without pending exceptions");
        return BS_JNI_ERR;
    }
    if (!activity) {
        g_unityplayer_current_activity = NULL;
        return BS_JNI_OK;
    }
    BSJNIObject *receiver = bs_jni_known_object(activity);
    if (!receiver || receiver->kind != BS_JNI_OBJECT_GAME_ACTIVITY_HOST ||
        !bs_jni_gameactivity_launch_matches(receiver, g_asset_manager,
                                            g_gameactivity_configuration) ||
        g_unityplayer_current_context != receiver) {
        /* Original UnityPlayer.<init> writes currentContext at DEX code
         * offset 0x90, then currentActivity at 0x9a only if Context is an
         * Activity. An Activity-only observation would be out of order. */
        bs_jni_error("UnityPlayer.currentActivity requires the same bound "
                     "GameActivity Context assignment first");
        return BS_JNI_ERR;
    }
    g_unityplayer_current_activity = receiver;
    return BS_JNI_OK;
}

int bs_jni_unityplayer_observe_current_context(void *context) {
    /* The Quest UnityPlayer.<init> stores the Context parameter unconditionally
     * before its instanceof-Activity branch stores currentActivity. The only
     * context currently backed by this Switch host is the exact bound
     * GameActivity receiver; no Android Service/Application Context is
     * invented or accepted. The caller must have executed/translated the
     * original DEX assignment before notifying this observation. */
    if (!g_initialized || !bs_jni_env() || g_missing_member_pending) {
        bs_jni_error("UnityPlayer.currentContext observation requires an "
                     "attached JNI host without pending exceptions");
        return BS_JNI_ERR;
    }
    if (!context) {
        g_unityplayer_current_context = NULL;
        return BS_JNI_OK;
    }
    BSJNIObject *receiver = bs_jni_known_object(context);
    if (!receiver || receiver->kind != BS_JNI_OBJECT_GAME_ACTIVITY_HOST ||
        !bs_jni_gameactivity_launch_matches(receiver, g_asset_manager,
                                            g_gameactivity_configuration)) {
        bs_jni_error("UnityPlayer.currentContext requires the bound live "
                     "GameActivity Context receiver");
        return BS_JNI_ERR;
    }
    g_unityplayer_current_context = receiver;
    return BS_JNI_OK;
}

/* Precise evidence for original GameActivity_register; symbolic IDs and
 * Java-ref stubs are never counted as a backing Activity or callbacks. */
int bs_jni_gameactivity_readiness(BSJNIGameActivityEvidence *out) {
    if (!out) return BS_JNI_ERR;
    BSJNIGameActivityEvidence evidence = {0};
    evidence.expected_native_methods = BS_JNI_GAMEACTIVITY_NATIVE_COUNT;
    evidence.current_thread_attached = g_initialized && g_attached;
    for (size_t i = 0; i < BS_JNI_GAMEACTIVITY_NATIVE_COUNT; ++i) {
        const BSJNIKnownNative *known = &g_gameactivity_natives[i];
        for (size_t j = 0; j < g_native_count; ++j) {
            const BSJNINativeRegistration *registered = &g_native_methods[j];
            if (!strcmp(registered->class_name, BS_GAME_ACTIVITY) &&
                !strcmp(registered->method_name, known->name) &&
                !strcmp(registered->signature, known->signature) &&
                registered->function_address != 0) {
                ++evidence.captured_native_methods;
                break;
            }
        }
    }
    if (!g_initialized)
        evidence.blocker_flags |= BS_JNI_GAMEACTIVITY_NOT_INITIALIZED;
    if (!bs_jni_gameactivity_natives_captured())
        evidence.blocker_flags |= BS_JNI_GAMEACTIVITY_NATIVES_NOT_CAPTURED;

    const int backed_activity = g_gameactivity_receiver != NULL;
    evidence.actual_java_activity_ready = backed_activity ? 1 : 0;
    if (!backed_activity)
        evidence.blocker_flags |= BS_JNI_GAMEACTIVITY_ACTIVITY_UNBACKED;

    int complete_configuration = 0;
    for (size_t i = 0; i < g_object_count; ++i) {
        const BSJNIObject *object = &g_objects[i];
        if (object->kind == BS_JNI_OBJECT_CONFIGURATION &&
            object->observed_count >= evidence.observed_configuration_fields +
                                      evidence.defaulted_configuration_fields) {
            evidence.defaulted_configuration_fields =
                object->configuration_default_count;
            evidence.observed_configuration_fields = object->observed_count -
                object->configuration_default_count;
            evidence.observed_locale_count = object->locale_list ?
                object->locale_list->locale_count : 0;
        }
        if (bs_jni_gameactivity_configuration_complete(object))
            complete_configuration = 1;
        if (object->kind == BS_JNI_OBJECT_SURFACE && object->native_window) {
#if defined(__SWITCH__)
            if (nwindowIsValid((NWindow *)object->native_window))
                ++evidence.verified_native_surface_count;
#endif
        }
        if (object->kind == BS_JNI_OBJECT_GAME_ACTIVITY_HOST)
            ++evidence.native_activity_proxy_count;
        if (object->kind == BS_JNI_OBJECT_MOTION_EVENT &&
            object->motion_event)
            ++evidence.observed_motion_event_count;
        if (object->kind == BS_JNI_OBJECT_KEY_EVENT && object->key_event)
            ++evidence.observed_key_event_count;
        if (object->kind == BS_JNI_OBJECT_GAME_ACTIVITY_HOST) {
            const BSJNIGameActivityHostServices *host =
                &object->gameactivity_host;
            uint32_t mask = bs_jni_gameactivity_host_service_mask(host);
            /* Report coverage of one concrete proxy. Do not aggregate
             * disconnected partial proxies as a single complete Activity. */
            unsigned bits = 0, previous_bits = 0;
            for (uint32_t p = mask; p; p >>= 1) bits += p & 1u;
            for (uint32_t p = evidence.most_complete_host_service_mask;
                 p; p >>= 1) previous_bits += p & 1u;
            if (bits > previous_bits)
                evidence.most_complete_host_service_mask = mask;
        }
    }
    /* All 18 configuration scalar fields and an observed locale list are
     * needed for the full original GameActivity_register field inventory. */
    if (!complete_configuration)
        evidence.blocker_flags |= BS_JNI_GAMEACTIVITY_CONFIGURATION_MISSING;

    if (!backed_activity)
        evidence.blocker_flags |= BS_JNI_GAMEACTIVITY_CALLBACKS_UNBACKED;
    if (!evidence.verified_native_surface_count ||
        (backed_activity &&
         !bs_jni_is_backed_surface(bs_jni_env(), g_gameactivity_surface)))
        evidence.blocker_flags |= BS_JNI_GAMEACTIVITY_SURFACE_UNBACKED;

    if (evidence.blocker_flags & BS_JNI_GAMEACTIVITY_NOT_INITIALIZED)
        evidence.first_blocker = "JNI VM function table not initialized";
    else if (evidence.blocker_flags & BS_JNI_GAMEACTIVITY_NATIVES_NOT_CAPTURED)
        evidence.first_blocker = "original 24 GameActivity native registrations incomplete";
    else if (evidence.blocker_flags & BS_JNI_GAMEACTIVITY_ACTIVITY_UNBACKED)
        evidence.first_blocker = "Java GameActivity is symbolic, with no backed Activity instance";
    else if (evidence.blocker_flags & BS_JNI_GAMEACTIVITY_CONFIGURATION_MISSING)
        evidence.first_blocker = "Android Configuration values or host locale unavailable";
    else if (evidence.blocker_flags & BS_JNI_GAMEACTIVITY_CALLBACKS_UNBACKED)
        evidence.first_blocker = "Java Activity methods and lifecycle callbacks not executable";
    else if (evidence.blocker_flags & BS_JNI_GAMEACTIVITY_SURFACE_UNBACKED)
        evidence.first_blocker = "no verified native NWindow-backed Java Surface";
    else
        evidence.first_blocker = "all Java GameActivity prerequisites verified";
    *out = evidence;
    return BS_JNI_OK;
}

int bs_jni_gameactivity_java_ready(void) {
    BSJNIGameActivityEvidence evidence;
    return bs_jni_gameactivity_readiness(&evidence) == BS_JNI_OK &&
           !g_missing_member_pending &&
           evidence.blocker_flags == 0;
}

int32_t bs_jni_call_onload_if_ready(const char *library,
                                   BSJNIOnLoadFn entry,
                                   const BSJNIReadiness *ready) {
    /* GameActivity_register performs real Java method lookups/registration
     * and invokes nontrivial Activity/Configuration/WindowInsets callbacks.
     * Symbolic handles and observed value snapshots do NOT constitute a real
     * installed Android GameActivity. No caller-supplied readiness boolean can
     * waive that missing Java VM lifecycle ABI. The separately verified
     * libmain/libil2cpp onload paths remain available to their owners.
     */
    if (library) {
        const char *last_slash = strrchr(library, '/');
        const char *last_backslash = strrchr(library, '\\');
        const char *base = last_slash && last_backslash ?
            (last_slash > last_backslash ? last_slash : last_backslash) :
            (last_slash ? last_slash : last_backslash);
        base = base ? base + 1 : library;
        if (!strcmp(base, "libgame.so")) {
            bs_jni_error("refusing real libgame.so JNI_OnLoad: "
                         "GameActivity Java lifecycle is not implemented");
            return BS_JNI_ERR;
        }
    }
    if (!g_initialized || !entry || !ready || !library || !*library ||
        !ready->platform_services_ready || ready->unresolved_imports ||
        ready->missing_needed_libraries) {
        bs_jni_error("refusing JNI_OnLoad for %s: engine ABI/runtime "
                     "readiness is unverified or imports unresolved",
                     library ? library : "(null)");
        return BS_JNI_ERR;
    }
    bs_jni_log("[jni] invoking verified JNI_OnLoad from %s", library);
    int32_t version = entry(bs_jni_vm(), NULL);
    /* Returning JNI_VERSION_1_6 is not enough when the original guest left
     * an exception pending. In particular, UnityOpenXR currently requests
     * UnityPlayer.currentActivity before the real Java constructor has
     * published that field. Do not quietly accept this failed initialization
     * and defer the error to an unrelated GameActivity native callback. */
    if (g_missing_member_pending) {
        bs_jni_log("[jni] %s JNI_OnLoad left a pending Java exception; "
                   "refusing successful initialization", library);
        return BS_JNI_ERR;
    }
    if (!bs_jni_valid_version(version)) {
        bs_jni_error("%s JNI_OnLoad returned unexpected version 0x%08x",
                     library, (unsigned)version);
        return BS_JNI_ERR;
    }
    bs_jni_log("[jni] %s JNI_OnLoad returned 0x%08x; captured %lu methods",
               library, (unsigned)version, (unsigned long)g_native_count);
    return version;
}
