/*
 * Beat Saber Quest 1.46 / Unity 6000.3.19f1 JNI bootstrap ABI.
 *
 * Independent from Android headers and from the older PvZ JNI shim.
 * C JNI handles are POINTERS TO function-table pointers, not the tables
 * themselves. bs_jni_vm() and bs_jni_env() return that native ABI layout.
 *
 * Copyright (c) 2026 Beat Saber Quest Switch port contributors.
 * MIT license; see ../LICENSE.
 */
#ifndef BEAT_SABER_JNI_BRIDGE_H
#define BEAT_SABER_JNI_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BS_JNI_OK           0
#define BS_JNI_ERR         -1
#define BS_JNI_EDETACHED   -2
#define BS_JNI_EVERSION    -3
#define BS_JNI_VERSION_1_6 0x00010006

#define BS_JNI_MAX_CLASSES 256
#define BS_JNI_MAX_NATIVES 1024
#define BS_JNI_CLASS_BYTES 192
#define BS_JNI_METHOD_BYTES 128
#define BS_JNI_SIGNATURE_BYTES 256
#define BS_JNI_MAX_EVENT_POINTERS 16
#define BS_JNI_MAX_EVENT_HISTORY 16
#define BS_JNI_MAX_EVENT_AXES 64
#define BS_JNI_MAX_EVENT_HISTORICAL_AXES 128

/* Log strings are transient; the bridge owns all registrations below. */
typedef void (*BSJNILogFn)(void *user, const char *message);

typedef struct BSJNINativeRegistration {
    char class_name[BS_JNI_CLASS_BYTES];
    char method_name[BS_JNI_METHOD_BYTES];
    char signature[BS_JNI_SIGNATURE_BYTES];
    uintptr_t function_address; /* Original mapped Quest code, NEVER invoked. */
} BSJNINativeRegistration;

/*
 * Require all libraries' relocations/imports AND the platform facilities to
 * be ready before the caller can explicitly request a JNI_OnLoad invocation.
 * Zero-initialized readiness structures ALWAYS refuse guest execution.
 *
 * platform_services_ready means verified JNI/GameActivity, asset mapping,
 * Bionic, EGL/Vulkan, OpenXR and thread/TLS runtime. It must not be guessed.
 */
typedef struct BSJNIReadiness {
    size_t unresolved_imports;
    size_t missing_needed_libraries;
    int platform_services_ready;
} BSJNIReadiness;

/* JNI_OnLoad(JavaVM *vm, void *reserved), using the AAPCS64 ABI. */
typedef int32_t (*BSJNIOnLoadFn)(void *java_vm, void *reserved);

/* Call once on the loader's initial thread, BEFORE any guest JNI operation. */
int bs_jni_init(BSJNILogFn log, void *user);

/* Returns JavaVM* / JNIEnv* ABI values, suitable for JNI_OnLoad/NativeActivity. */
void *bs_jni_vm(void);
void *bs_jni_env(void); /* NULL on a detached thread. */

/*
 * JNIEnv lookup slots 33 (GetMethodID), 94 (GetFieldID), and 113
 * (GetStaticMethodID) accept only exact class/name/signature combinations
 * established from the original Quest libgame.so GameActivity_register and
 * GameActivityEventsInit. They return stable metadata handles, NOT executable
 * Java methods or backed Android fields. Unrecognized lookups return NULL
 * with a pending diagnostic NoSuchMethodError / NoSuchFieldError; JNI slots
 * 15/16/17/228 observe, describe, clear and check that state respectively.
 * Field access is available ONLY on observed host snapshots created below;
 * selected pure AndroidX WindowInsetsCompat.Type static mask methods are
 * callable. JNI slots 144/145 also expose the original DEX-declared
 * UnityPlayer.currentActivity and currentContext static field identities,
 * including inherited lookup through UnityPlayerForActivityOrService.
 * Each UnityPlayer static object field can be read only after its own exact
 * Java assignment is explicitly observed below. Other static calls, Java Activity lifecycle,
 * and Java input methods remain unsupported: none proves platform readiness.
 */

/*
 * Immutable, independently observed Java Configuration / Insets fields.
 *
 * The original Quest APK requests android/content/res/Configuration's
 * numeric fields (mostly "I", plus fontScale "F") and
 * androidx/core/graphics/Insets's four integer fields. A native caller can
 * create a handle *only* from field values measured by a real host service.
 * There are deliberately NO default dpi, fontScale, keyboard, locale,
 * screen-size or inset values. A missing field raises an explicit JNI
 * UnsupportedOperationException if the guest tries to read it.
 *
 * Each name/signature is verified against the exact original libgame.so
 * member table. 'I' uses integer_value and 'F' uses float_value. Calls
 * perform a two-phase validation/copy; unknown or duplicate fields fail
 * without allocating a partial Java object.
 *
 * Example (Horizon display geometry measured by the integrator):
 *   BSJNIObservedField f[] = {
 *       {"screenWidthDp", "I", measured_width_dp, 0.f},
 *       {"screenHeightDp", "I", measured_height_dp, 0.f},
 *   };
 *   void *configuration = bs_jni_object_from_observed_fields(
 *       "android/content/res/Configuration", f, 2);
 *
 * IMPORTANT: raw Horizon NWindow pixel dimensions are not Android dp;
 * convert only when a genuinely established densityDpi is available.
 * AConfiguration's current synthetic sdk/locale fallback is not a trusted
 * source for Java Configuration fields. This is an object-value interface,
 * not Android Activity creation or an IL2CPP startup approval.
 */
typedef struct BSJNIObservedField {
    const char *name;
    const char *signature;
    int32_t integer_value;
    float float_value;
} BSJNIObservedField;

void *bs_jni_object_from_observed_fields(const char *class_name,
                                          const BSJNIObservedField *fields,
                                          size_t count);

/*
 * Explicitly observed host locale metadata, never inferred from the
 * currently synthetic AConfiguration language/country fallback. The caller
 * must supply actual Java-style language, script, country and variant tags,
 * with empty optional tags represented as "" (not NULL). The language tag
 * must be nonempty. At most 8 locale entries are supported for now.
 *
 * Creates an immutable Configuration with one attached LocaleList and its
 * Locale objects; GetLocales/LocaleList.size/get and Locale.getLanguage/
 * getScript/getCountry/getVariant then return values from this observation.
 * All returned JNI objects have process lifetime. No JNI lifecycle method
 * or Android package services are synthesized by this API.
 */
typedef struct BSJNIObservedLocale {
    const char *language;
    const char *script;
    const char *country;
    const char *variant;
} BSJNIObservedLocale;

void *bs_jni_configuration_from_observed_locales(
    const BSJNIObservedField *fields, size_t field_count,
    const BSJNIObservedLocale *locales, size_t locale_count);

/* Configuration.setToDefaults() with a measured host orientation and locale. */
void *bs_jni_configuration_from_host_display(
    uint32_t width, uint32_t height, const BSJNIObservedLocale *locale);

/*
 * Native-only GameActivity Java method adapter. The host must implement each
 * callback using an actual Switch service, and return 0 ONLY when that service
 * completed the requested operation. Missing callbacks and failed calls raise
 * pending UnsupportedOperationException on the calling JNI thread. No callbacks
 * run while the JNI registry lock is held. Insets are measured edges in pixels
 * (left, top, right, bottom), NOT invented Android window defaults.
 *
 * This creates a JNI receiver with the original GameActivity class identity
 * for the five exact methods looked up by libgame.so. It does not execute Java
 * bytecode, create a Java lifecycle, grant GameActivity readiness, or replace
 * the UnityPlayerGameActivity bootstrap handle returned by bs_jni_activity().
 * The caller owns `user` and must keep it alive while this proxy is used.
 */
typedef struct BSJNIGameActivityHostServices {
    void *user;
    int (*finish)(void *user);
    int (*set_window_flags)(void *user, int32_t flags, int32_t mask);
    int (*set_ime_editor_info_fields)(void *user, int32_t input_type,
                                      int32_t action_id, int32_t ime_options);
    int (*get_window_insets)(void *user, int32_t type_mask,
                             int32_t edges_ltrb[4]);
    int (*get_waterfall_insets)(void *user, int32_t edges_ltrb[4]);
} BSJNIGameActivityHostServices;

void *bs_jni_gameactivity_native_proxy(
    const BSJNIGameActivityHostServices *services);
/* Query the callbacks on this exact GameActivity proxy. These guard event
 * notifications that would otherwise make unbacked Java service calls. */
int bs_jni_gameactivity_host_supports_insets(void *activity);
int bs_jni_gameactivity_host_supports_ime(void *activity);

/* Host receiver standing in for the Java UnityPlayerForGameActivity instance
 * (class identity only; its Java constructor is NOT executed). Stable until
 * the next bs_jni_init. */
void *bs_jni_unityplayer_host_object(void);
/* Nonzero when JNIEnv function-table slot `slot` holds a real bridge
 * implementation (not NULL and not its fail-closed diagnostic trap). */
int bs_jni_env_slot_implemented(unsigned slot);

/* Host-implemented Java methods. Each entry stands in for one exact original
 * DEX method (class, name, signature) whose behavior the host reproduces.
 * GetMethodID/GetStaticMethodID resolve them (UnityPlayer subclasses inherit
 * UnityPlayer entries) and Call*Method dispatches with decoded arguments.
 * A handler returning nonzero raises a pending RuntimeException. */
typedef union BSJNIValue {
    uint8_t z;
    int8_t b;
    uint16_t c;
    int16_t s;
    int32_t i;
    int64_t j;
    float f;
    double d;
    void *l;
} BSJNIValue;

#define BS_JNI_HOST_MAX_ARGS 12
typedef int (*BSJNIHostMethodFn)(void *user, void *env, void *receiver,
                                 const BSJNIValue *args, size_t argc,
                                 BSJNIValue *result);
int bs_jni_register_host_method(const char *class_name, const char *name,
                                const char *signature, int is_static,
                                BSJNIHostMethodFn fn, void *user);
/* Whether a receiver handle is this bridge's UnityPlayer host object. */
int bs_jni_is_unityplayer_host(void *receiver);

/* App class resolver backed by the original APK classes.dex class_defs.
 * Returns 1 when `internal_name` is defined by the APK and writes its
 * declared superclass internal name ("" when none). This is what ART's
 * PathClassLoader consults; framework classes are NOT reported here. */
typedef int (*BSJNIAppClassResolver)(void *user, const char *internal_name,
                                     char *super_out, size_t super_cap);
void bs_jni_set_app_class_resolver(BSJNIAppClassResolver fn, void *user);

/* Raise a pending Java exception of builtin class `class_name` (internal
 * name) from a host method. The handler then returns 0. */
int bs_jni_host_throw(const char *class_name, const char *message);

/* Host-created instance of an original app class whose DEX constructor body
 * the caller reproduced. Class identity only; stable until bs_jni_init. */
void *bs_jni_new_host_instance(const char *class_name);
/* Lenient framework model (default on): unmodeled Java members resolve to
 * logged stubs returning JNI defaults, and unbacked JNI operations log once
 * and return defaults instead of raising Java exceptions or aborting.
 * Disable for fail-closed (strict) behavior. */
void bs_jni_set_lenient(int enabled);
/* Display metrics reported by the lenient android.view.Display model. */
void bs_jni_set_display_metrics(int32_t width, int32_t height, float hz);
int bs_jni_lenient(void);
/* Android-visible Context.getPackageCodePath() and getExternalFilesDir(null)
 * values. Both must be absolute guest paths the filesystem bridge maps. */
int bs_jni_set_package_paths(const char *code_path,
                             const char *external_files_dir);
/* Nonzero when `receiver` is a host instance of exactly `class_name`. */
int bs_jni_is_host_instance(void *receiver, const char *class_name);

/* Bind the receiver and exact resources used by the original NativeCode
 * initializer. Call on the Activity thread before guest startup. Optional
 * IME and inset methods still require their own backing when called. */
int bs_jni_gameactivity_bind_host(void *activity, void *asset_manager,
                                 void *configuration, void *surface);
int bs_jni_gameactivity_launch_matches(void *activity, void *asset_manager,
                                      void *configuration);

/* Record an observed UnityPlayer.currentActivity assignment from the Java
 * host. A non-NULL value must be the SAME bound, live GameActivity receiver
 * with a verified native Surface AND a prior matching currentContext
 * observation, respecting original UnityPlayer.<init> DEX write order;
 * an arbitrary Java object or the synthetic bs_jni_activity() handle is
 * rejected. NULL records a real field clearing
 * (e.g. during teardown). Do not call merely because UnityPlayer was found
 * or its native methods were registered: that does not execute its Java
 * constructor. GetStaticObjectField returns a local reference only while
 * this observation and its live host backing remain valid. */
int bs_jni_unityplayer_observe_current_activity(void *activity);
/* Check whether GetStaticObjectField can read the observed Activity without
 * requesting a JNI reference or setting an exception. */
int bs_jni_unityplayer_has_current_activity(void);

/* Record the SEPARATE original UnityPlayer.<init> currentContext assignment.
 * Its DEX write precedes currentActivity. At present only the exact bound
 * GameActivity-as-Context receiver is supported, with live Surface and host
 * Configuration; arbitrary symbolic and synthetic Context objects fail.
 * NULL clears the observation. The Java constructor must actually have run
 * (or have been faithfully translated) before calling this function. */
int bs_jni_unityplayer_observe_current_context(void *context);

/*
 * Native input events observed by a platform input service. libgame.so's
 * GameActivityEventsInit resolves the real MotionEvent/KeyEvent getters
 * listed below. These values must be populated from an actual event; the
 * bridge does not infer a Java source, keyboard map or touchscreen device.
 *
 * Axis entries are sparse: Android MotionEvent.getAxisValue returns 0 for an
 * absent axis. Every pointer and historical sample that exists must be
 * observed. Coordinates, history and event timestamps use Android's raw
 * pixels and monotonic milliseconds, as provided by the platform adapter.
 * No conversion from libnx ticks or hid coordinates occurs in this bridge.
 */
typedef struct BSJNIObservedMotionPointer {
    int32_t pointer_id;
    int32_t tool_type;
    float raw_x;
    float raw_y;
} BSJNIObservedMotionPointer;

typedef struct BSJNIObservedMotionAxis {
    int32_t axis;
    int32_t pointer_index;
    float value;
} BSJNIObservedMotionAxis;

typedef struct BSJNIObservedHistoricalMotionAxis {
    int32_t axis;
    int32_t pointer_index;
    int32_t history_index;
    float value;
} BSJNIObservedHistoricalMotionAxis;

typedef struct BSJNIObservedMotionEvent {
    int32_t device_id;
    int32_t source;
    int32_t action;
    int32_t flags;
    int32_t meta_state;
    int32_t action_button;
    int32_t button_state;
    int32_t classification;
    int32_t edge_flags;
    int64_t event_time_ms;
    int64_t down_time_ms;
    float x_precision;
    float y_precision;
    size_t pointer_count;
    BSJNIObservedMotionPointer pointers[BS_JNI_MAX_EVENT_POINTERS];
    size_t history_count;
    int64_t historical_event_times_ms[BS_JNI_MAX_EVENT_HISTORY];
    size_t axis_count;
    BSJNIObservedMotionAxis axes[BS_JNI_MAX_EVENT_AXES];
    size_t historical_axis_count;
    BSJNIObservedHistoricalMotionAxis historical_axes[
        BS_JNI_MAX_EVENT_HISTORICAL_AXES];
} BSJNIObservedMotionEvent;

typedef struct BSJNIObservedKeyEvent {
    int32_t device_id;
    int32_t source;
    int32_t action;
    int32_t flags;
    int32_t meta_state;
    int32_t modifiers;
    int32_t repeat_count;
    int32_t key_code;
    int32_t scan_code;
    int32_t unicode_char;
    int64_t event_time_ms;
    int64_t down_time_ms;
} BSJNIObservedKeyEvent;

/*
 * Creates a stable Java event handle, or refreshes a previously created
 * handle for a subsequent event. Refresh only AFTER the native GameActivity
 * input callback has returned and no JNI thread is reading the old event;
 * an Android event is likewise mutable/recycled only outside its dispatch.
 * Reuse prevents unbounded consumption of the process-lifetime JNI pool.
 * Invalid observations are rejected atomically without changing the handle.
 * No event snapshot is proof of a running Java Activity or input queue.
 */
void *bs_jni_motion_event_from_observation(
    const BSJNIObservedMotionEvent *event);
void *bs_jni_key_event_from_observation(const BSJNIObservedKeyEvent *event);
int bs_jni_motion_event_update(void *event_handle,
                               const BSJNIObservedMotionEvent *event);
int bs_jni_key_event_update(void *event_handle,
                            const BSJNIObservedKeyEvent *event);

/* Stable Java handles: Activity and android.content.res.AssetManager. */
void *bs_jni_activity(void);
void *bs_jni_asset_manager(void);

/* Query the complete captured RegisterNatives name/signature/function list. */
size_t bs_jni_registration_count(void);
const BSJNINativeRegistration *bs_jni_registration_at(size_t index);
const char *bs_jni_last_error(void);

/* The exact original 24 GameActivity native method names/signatures have
 * been captured with non-NULL original guest code pointers. This is a
 * metadata-only diagnostic and NEVER means Java classes, input events,
 * Android Activity lifecycle or native method execution are ready.
 * Returns 1 only after complete, uniquely matched registration; otherwise 0.
 */
int bs_jni_gameactivity_natives_captured(void);

/* Read-only evidence for the ORIGINAL libgame.so GameActivity_register
 * (0x1bd2c) and its 24 native declarations at 0x2b840. Metadata capture
 * alone cannot satisfy Java Activity initialization. blocker_flags is 0
 * only when all actual prerequisites have a verified implementation. */
enum {
    BS_JNI_GAMEACTIVITY_NOT_INITIALIZED       = 1u << 0,
    BS_JNI_GAMEACTIVITY_NATIVES_NOT_CAPTURED  = 1u << 1,
    BS_JNI_GAMEACTIVITY_ACTIVITY_UNBACKED     = 1u << 2,
    BS_JNI_GAMEACTIVITY_CONFIGURATION_MISSING = 1u << 3,
    BS_JNI_GAMEACTIVITY_CALLBACKS_UNBACKED    = 1u << 4,
    BS_JNI_GAMEACTIVITY_SURFACE_UNBACKED      = 1u << 5
};
typedef struct BSJNIGameActivityEvidence {
    uint32_t blocker_flags;
    size_t expected_native_methods;
    size_t captured_native_methods;
    size_t observed_configuration_fields;
    size_t defaulted_configuration_fields;
    size_t observed_locale_count;
    size_t native_activity_proxy_count;
    size_t observed_motion_event_count;
    size_t observed_key_event_count;
    uint32_t most_complete_host_service_mask;
    int current_thread_attached;
    int actual_java_activity_ready;
    int verified_native_surface_count;
    const char *first_blocker; /* static diagnostic, never NULL */
} BSJNIGameActivityEvidence;

enum {
    BS_JNI_HOST_FINISH                 = 1u << 0,
    BS_JNI_HOST_WINDOW_FLAGS          = 1u << 1,
    BS_JNI_HOST_IME_EDITOR_INFO       = 1u << 2,
    BS_JNI_HOST_WINDOW_INSETS         = 1u << 3,
    BS_JNI_HOST_WATERFALL_INSETS      = 1u << 4
};

int bs_jni_gameactivity_readiness(BSJNIGameActivityEvidence *evidence);
int bs_jni_gameactivity_java_ready(void);

/* Limited ABI preflight for original libgame.so JNI_OnLoad registration ONLY.
 * GameActivity_register does 77 JNI member lookups plus RegisterNatives(24)
 * without constructing an Activity. This gate checks the metadata and slots;
 * the caller separately proves original ELF dependencies/TLS/entrypoint.
 * It does NOT imply Java lifecycle readiness. */
int bs_jni_gameactivity_registration_abi_ready(void);

/* A Java Surface token can only be constructed from a live, verified libnx
 * NWindow on Switch. Neither an arbitrary non-NULL Java object nor the
 * default NWindow is implicitly interchangeable with Android Surface. */
void *bs_jni_surface_from_nwindow(void *native_window);
int bs_jni_is_backed_surface(void *env, void *surface);
void *bs_jni_backed_surface_nwindow(void *env, void *surface);
void bs_jni_revoke_surface(void *surface);

/*
 * Optional controlled entrypoint call, never invoked during bs_jni_init().
 * Returns JNI_ERR unless initialized, the entire dependency graph has zero
 * missing imports/dependencies, and explicit platform readiness is asserted.
 * Other JNI entrypoints still fail hard if the Java operation is unsupported.
 */
int32_t bs_jni_call_onload_if_ready(const char *library,
                                   BSJNIOnLoadFn entry,
                                   const BSJNIReadiness *readiness);

#ifdef __cplusplus
}
#endif
#endif
