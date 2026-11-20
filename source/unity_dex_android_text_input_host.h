/*
 * The Android text-input subset imported by the ORIGINAL Quest GameActivity
 * and gamesdk gametextinput DEX classes.
 *
 * EditorInfo is a real guest-Java data object: default construction and the
 * explicitly imported primitive fields are implemented in the guest VM.
 * BaseInputConnection(View, boolean) requires the same live Context and
 * InputMethodManager as Android, so it is unavailable until an independently
 * witnessed native service backs the attached Activity's view hierarchy.
 *
 * The host does not create a keyboard, pretend an Android IME exists, or
 * substitute a native applet/Surface for a Java View or Activity. It never
 * calls Unity/graphics and cannot set UnityPlayer.currentActivity.
 */
#ifndef BS_UNITY_DEX_ANDROID_TEXT_INPUT_HOST_H
#define BS_UNITY_DEX_ANDROID_TEXT_INPUT_HOST_H

#include "unity_dex_android_context_host.h"
#include "unity_dex_android_surface_host.h"
#include <stddef.h>
#include <stdint.h>

#define BS_DEX_TEXT_MAX_EDITORS 8u
#define BS_DEX_TEXT_MAX_CONNECTIONS 8u

typedef struct BsDexTextEditorInfo {
    BsDexValue reference;
    int32_t input_type, action_id, ime_options;
    int allocated, initialized;
} BsDexTextEditorInfo;

typedef struct BsDexTextConnection {
    BsDexValue receiver;
    BsDexValue target_view, context, input_method_manager;
    void *native_input_method;
    uint64_t native_generation;
    int dummy_mode, initialized;
} BsDexTextConnection;

typedef struct BsDexTextInputMethodManager {
    void *activity;
    void *native_service;
    uint64_t generation;
    int initialized;
} BsDexTextInputMethodManager;

typedef struct BsDexAndroidTextNative {
    void *user;
    /* Recheck the owner thread every time an Android UI service is used. */
    int (*on_ui_thread)(void *user);
    /* Return a real, live per-Context native input service identity and its
     * lifetime generation. An NWindow, touchscreen sample or fabricated
     * token is not an InputMethodManager by itself. */
    int (*observe_input_method)(void *user, void *activity,
                                void **native_service, uint64_t *generation);
    int (*verify_input_method)(void *user, void *activity,
                               void *native_service, uint64_t generation);
    /* Real Android Log.d side effect. The returned integer is the actual
     * native sink's positive byte count, or a negative error code. */
    int (*write_debug_log)(void *user, const char *tag, const char *message,
                           int32_t *result);
} BsDexAndroidTextNative;

typedef struct BsDexAndroidTextInputHost {
    BsDexAndroidContextHost *context; /* borrowed, attached while in use */
    BsDexAndroidSurfaceHost *surface; /* borrowed, constructed Java View */
    BsDexAndroidTextNative native;
    BsDexVM *vm;                      /* borrowed, destroy before reset */
    BsDexValue input_method_manager;  /* guest identity for native owner */
    BsDexTextInputMethodManager java_input_method;
    void *native_input_method;
    uint64_t native_generation;
    BsDexTextEditorInfo editors[BS_DEX_TEXT_MAX_EDITORS];
    BsDexTextConnection connections[BS_DEX_TEXT_MAX_CONNECTIONS];
    size_t editor_count, connection_count;
    size_t observed_log_writes, observed_service_reads;
} BsDexAndroidTextInputHost;

void bs_dex_android_text_input_init(BsDexAndroidTextInputHost *host,
                                     BsDexAndroidContextHost *context,
                                     BsDexAndroidSurfaceHost *surface,
                                     const BsDexAndroidTextNative *native);
/* Reset only after destroying the borrowed guest VM. No outstanding native
 * OS service is owned by this module; callers release native services. */
int bs_dex_android_text_input_reset(BsDexAndroidTextInputHost *host);

int bs_dex_android_text_input_new_platform(
    BsDexAndroidTextInputHost *host, BsDexVM *vm, const char *descriptor,
    BsDexValue *object, const char **exception);
int bs_dex_android_text_input_invoke(
    BsDexAndroidTextInputHost *host, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception);
int bs_dex_android_text_input_field(
    BsDexAndroidTextInputHost *host, BsDexVM *vm,
    const char *declaring_class, const char *field_name,
    const char *field_type, BsDexValue receiver, int is_write,
    BsDexValue *value, const char **exception);
/* Read-only diagnostics of completed, original calls; used by focused host
 * tests and to report service dependencies. */
int bs_dex_android_text_editor_snapshot(
    const BsDexAndroidTextInputHost *host, BsDexValue editor,
    BsDexTextEditorInfo *out);
int bs_dex_android_text_connection_snapshot(
    const BsDexAndroidTextInputHost *host, BsDexValue connection,
    BsDexTextConnection *out);

#endif
