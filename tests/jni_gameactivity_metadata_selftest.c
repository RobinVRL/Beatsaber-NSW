/*
 * Original Quest 1.46 libgame.so JNI metadata lookup contract.
 *
 * Names and signatures come from static disassembly of:
 *   GameActivity_register  @ ELF VA 0x1bd2c (4296 bytes)
 *   GameActivityEventsInit @ ELF VA 0x1fc88 (1624 bytes)
 *
 * Compile with the official Android NDK jni.h and the host C runtime;
 * no Android/Quest shared library is linked or invoked.
 */
#include <jni.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/jni_bridge.h"

_Static_assert(offsetof(struct JNINativeInterface, ExceptionOccurred) ==
               15 * sizeof(void *), "ExceptionOccurred slot mismatch");
_Static_assert(offsetof(struct JNINativeInterface, ExceptionDescribe) ==
               16 * sizeof(void *), "ExceptionDescribe is JNI slot 16");
_Static_assert(offsetof(struct JNINativeInterface, FatalError) ==
               18 * sizeof(void *), "FatalError is JNI slot 18");
_Static_assert(offsetof(struct JNINativeInterface, GetMethodID) ==
               33 * sizeof(void *), "GetMethodID slot mismatch");
_Static_assert(offsetof(struct JNINativeInterface, GetFieldID) ==
               94 * sizeof(void *), "GetFieldID slot mismatch");
_Static_assert(offsetof(struct JNINativeInterface, GetStaticMethodID) ==
               113 * sizeof(void *), "GetStaticMethodID slot mismatch");

#define GAME "com/google/androidgamesdk/GameActivity"
#define INSETS "androidx/core/graphics/Insets"
#define CONFIG "android/content/res/Configuration"
#define TYPE "androidx/core/view/WindowInsetsCompat$Type"
#define LIST "android/os/LocaleList"
#define LOCALE "java/util/Locale"
#define MOTION "android/view/MotionEvent"
#define KEY "android/view/KeyEvent"

enum { LOOKUP_METHOD, LOOKUP_FIELD, LOOKUP_STATIC_METHOD };
typedef struct Member {
    const char *class_name;
    const char *member_name;
    const char *signature;
    int lookup;
} Member;
#define M(C, N, S) { C, N, S, LOOKUP_METHOD }
#define F(C, N, S) { C, N, S, LOOKUP_FIELD }
#define S(C, N, G) { C, N, G, LOOKUP_STATIC_METHOD }

static const Member original_members[] = {
    M(GAME, "finish", "()V"),
    M(GAME, "setWindowFlags", "(II)V"),
    M(GAME, "getWindowInsets", "(I)Landroidx/core/graphics/Insets;"),
    M(GAME, "getWaterfallInsets", "()Landroidx/core/graphics/Insets;"),
    M(GAME, "setImeEditorInfoFields", "(III)V"),

    F(INSETS, "left", "I"),
    F(INSETS, "right", "I"),
    F(INSETS, "top", "I"),
    F(INSETS, "bottom", "I"),

    F(CONFIG, "colorMode", "I"),
    F(CONFIG, "densityDpi", "I"),
    F(CONFIG, "fontScale", "F"),
    F(CONFIG, "fontWeightAdjustment", "I"),
    F(CONFIG, "hardKeyboardHidden", "I"),
    F(CONFIG, "keyboard", "I"),
    F(CONFIG, "keyboardHidden", "I"),
    F(CONFIG, "mcc", "I"),
    F(CONFIG, "mnc", "I"),
    F(CONFIG, "navigation", "I"),
    F(CONFIG, "navigationHidden", "I"),
    F(CONFIG, "orientation", "I"),
    F(CONFIG, "screenHeightDp", "I"),
    F(CONFIG, "screenLayout", "I"),
    F(CONFIG, "screenWidthDp", "I"),
    F(CONFIG, "smallestScreenWidthDp", "I"),
    F(CONFIG, "touchscreen", "I"),
    F(CONFIG, "uiMode", "I"),
    M(CONFIG, "getLocales", "()Landroid/os/LocaleList;"),

    M(LIST, "size", "()I"),
    M(LIST, "get", "(I)Ljava/util/Locale;"),
    M(LOCALE, "getLanguage", "()Ljava/lang/String;"),
    M(LOCALE, "getScript", "()Ljava/lang/String;"),
    M(LOCALE, "getCountry", "()Ljava/lang/String;"),
    M(LOCALE, "getVariant", "()Ljava/lang/String;"),

    S(TYPE, "captionBar", "()I"),
    S(TYPE, "displayCutout", "()I"),
    S(TYPE, "ime", "()I"),
    S(TYPE, "mandatorySystemGestures", "()I"),
    S(TYPE, "navigationBars", "()I"),
    S(TYPE, "statusBars", "()I"),
    S(TYPE, "systemBars", "()I"),
    S(TYPE, "systemGestures", "()I"),
    S(TYPE, "tappableElement", "()I"),

    M(MOTION, "getDeviceId", "()I"),
    M(MOTION, "getSource", "()I"),
    M(MOTION, "getAction", "()I"),
    M(MOTION, "getEventTime", "()J"),
    M(MOTION, "getDownTime", "()J"),
    M(MOTION, "getFlags", "()I"),
    M(MOTION, "getMetaState", "()I"),
    M(MOTION, "getActionButton", "()I"),
    M(MOTION, "getButtonState", "()I"),
    M(MOTION, "getClassification", "()I"),
    M(MOTION, "getEdgeFlags", "()I"),
    M(MOTION, "getHistorySize", "()I"),
    M(MOTION, "getHistoricalEventTime", "(I)J"),
    M(MOTION, "getPointerCount", "()I"),
    M(MOTION, "getPointerId", "(I)I"),
    M(MOTION, "getToolType", "(I)I"),
    M(MOTION, "getRawX", "(I)F"),
    M(MOTION, "getRawY", "(I)F"),
    M(MOTION, "getXPrecision", "()F"),
    M(MOTION, "getYPrecision", "()F"),
    M(MOTION, "getAxisValue", "(II)F"),
    M(MOTION, "getHistoricalAxisValue", "(III)F"),

    M(KEY, "getDeviceId", "()I"),
    M(KEY, "getSource", "()I"),
    M(KEY, "getAction", "()I"),
    M(KEY, "getEventTime", "()J"),
    M(KEY, "getDownTime", "()J"),
    M(KEY, "getFlags", "()I"),
    M(KEY, "getMetaState", "()I"),
    M(KEY, "getModifiers", "()I"),
    M(KEY, "getRepeatCount", "()I"),
    M(KEY, "getKeyCode", "()I"),
    M(KEY, "getScanCode", "()I"),
    M(KEY, "getUnicodeChar", "()I")
};

/* Actual 24-entry JNINativeMethod array at original libgame.so ELF VA
 * 0x2b840, decoded from its R_AARCH64_RELATIVE relocations. The numeric
 * function values are unrelocated original ELF VAs, never callable host
 * pointers. RegisterNatives must record them exactly without invocation. */
typedef struct NativeFromElf {
    const char *name;
    const char *signature;
    uintptr_t elf_vaddr;
} NativeFromElf;

static const NativeFromElf original_natives[] = {
    { "initializeNativeCode",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J",
      0x1ce68 },
    { "getDlError", "()Ljava/lang/String;", 0x1d5e0 },
    { "terminateNativeCode", "(J)V", 0x1d720 },
    { "onStartNative", "(J)V", 0x1d750 },
    { "onResumeNative", "(J)V", 0x1d768 },
    { "onSaveInstanceStateNative", "(J)[B", 0x1d780 },
    { "onPauseNative", "(J)V", 0x1d7f4 },
    { "onStopNative", "(J)V", 0x1d80c },
    { "onConfigurationChangedNative",
      "(JLandroid/content/res/Configuration;)V", 0x1d824 },
    { "onTrimMemoryNative", "(JI)V", 0x1d868 },
    { "onWindowFocusChangedNative", "(JZ)V", 0x1d884 },
    { "onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", 0x1d8a4 },
    { "onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", 0x1d90c },
    { "onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V", 0x1da3c },
    { "onSurfaceDestroyedNative", "(J)V", 0x1da5c },
    { "onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z",
      0x1daa8 },
    { "onKeyDownNative", "(JLandroid/view/KeyEvent;)Z", 0x1db48 },
    { "onKeyUpNative", "(JLandroid/view/KeyEvent;)Z", 0x1dba0 },
    { "onTextInputEventNative",
      "(JLcom/google/androidgamesdk/gametextinput/State;)V", 0x1dbf8 },
    { "onWindowInsetsChangedNative", "(J)V", 0x1dc0c },
    { "setInputConnectionNative",
      "(JLcom/google/androidgamesdk/gametextinput/InputConnection;)V",
      0x1dd9c },
    { "onContentRectChangedNative", "(JIIII)V", 0x1dda8 },
    { "onSoftwareKeyboardVisibilityChangedNative", "(JZ)V", 0x1de10 },
    { "onEditorActionNative", "(JI)V", 0x1de30 }
};

static int log_count;
static void log_callback(void *user, const char *message) {
    (void)user;
    assert(message != NULL && message[0] != '\0');
    ++log_count;
}

static void *lookup(JNIEnv *env, const Member *entry, jclass cls) {
    switch (entry->lookup) {
        case LOOKUP_METHOD:
            return (*env)->GetMethodID(env, cls, entry->member_name,
                                       entry->signature);
        case LOOKUP_FIELD:
            return (*env)->GetFieldID(env, cls, entry->member_name,
                                      entry->signature);
        case LOOKUP_STATIC_METHOD:
            return (*env)->GetStaticMethodID(env, cls, entry->member_name,
                                             entry->signature);
    }
    return NULL;
}

static void expect_pending_error(JNIEnv *env, const char *fragment,
                                 int describe) {
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    jthrowable exception = (*env)->ExceptionOccurred(env);
    assert(exception != NULL);
    assert((*env)->GetObjectRefType(env, exception) == JNILocalRefType);
    assert(strstr(bs_jni_last_error(), fragment) != NULL);
    if (describe)
        (*env)->ExceptionDescribe(env);
    else
        (*env)->ExceptionClear(env);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
    assert((*env)->ExceptionOccurred(env) == NULL);
}

int main(void) {
    /* The loader defaults to lenient framework stubs; this test pins the
     * strict fail-closed JNI contract. */
    bs_jni_set_lenient(0);
    assert(bs_jni_init(log_callback, NULL) == BS_JNI_OK);
    JNIEnv *env = bs_jni_env();
    assert(env && *env);

    for (size_t i = 0;
         i < sizeof original_members / sizeof original_members[0]; ++i) {
        const Member *entry = &original_members[i];
        jclass cls = (*env)->FindClass(env, entry->class_name);
        assert(cls);
        void *id = lookup(env, entry, cls);
        assert(id != NULL);
        assert(id == lookup(env, entry, cls)); /* Stable IDs, no guest code. */
        assert((*env)->ExceptionCheck(env) == JNI_FALSE);
    }

    jclass config = (*env)->FindClass(env, CONFIG);
    assert((*env)->GetFieldID(env, config, "fontScale", "I") == NULL);
    expect_pending_error(env, "NoSuchFieldError", 1);
    assert((*env)->GetFieldID(env, config, "fontScale", "F") != NULL);

    jclass game = (*env)->FindClass(env, GAME);
    assert((*env)->GetMethodID(env, game, "setWindowFlags", "()V") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 0);
    assert((*env)->GetStaticMethodID(env, game, "finish", "()V") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 0);

    jclass type = (*env)->FindClass(env, TYPE);
    assert((*env)->GetMethodID(env, type, "ime", "()I") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 1);
    assert((*env)->GetStaticMethodID(env, type, "ime", "()V") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 0);
    assert((*env)->GetFieldID(env, type, "ime", "I") == NULL);
    expect_pending_error(env, "NoSuchFieldError", 0);

    jclass made_up = (*env)->FindClass(env, "nonexistent/NoJavaRuntime");
    assert(made_up); /* Symbolic registration handle, no Java class loaded. */
    assert((*env)->GetMethodID(env, made_up, "anything", "()V") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 1);
    assert((*env)->GetFieldID(env, made_up, "anything", "I") == NULL);
    expect_pending_error(env, "NoSuchFieldError", 0);

    assert((*env)->GetMethodID(env, NULL, "getAssets",
                               "()Landroid/content/res/AssetManager;") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 0);
    assert((*env)->GetStaticMethodID(env, NULL, "ime", "()I") == NULL);
    expect_pending_error(env, "NoSuchMethodError", 0);

    jclass unity = (*env)->FindClass(
        env, "com/unity3d/player/UnityPlayerGameActivity");
    jmethodID assets_id = (*env)->GetMethodID(
        env, unity, "getAssets", "()Landroid/content/res/AssetManager;");
    assert(assets_id);
    assert((*env)->CallObjectMethodA(env, bs_jni_activity(), assets_id, NULL) ==
           bs_jni_asset_manager());
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
    assert(bs_jni_registration_count() == 0);

    /* The original registration contains 24 native function pointers.
     * Capturing metadata from those exact ELF records does not execute any
     * original implementation or imply that Java lifecycle calls work. */
    JNINativeMethod natives[
        sizeof original_natives / sizeof original_natives[0]];
    for (size_t i = 0;
         i < sizeof original_natives / sizeof original_natives[0]; ++i) {
        natives[i].name = (char *)original_natives[i].name;
        natives[i].signature = (char *)original_natives[i].signature;
        natives[i].fnPtr = (void *)original_natives[i].elf_vaddr;
    }
    assert((*env)->RegisterNatives(
        env, game, natives,
        (jint)(sizeof original_natives / sizeof original_natives[0])) == JNI_OK);
    assert(bs_jni_registration_count() ==
           sizeof original_natives / sizeof original_natives[0]);
    for (size_t i = 0; i < bs_jni_registration_count(); ++i) {
        const BSJNINativeRegistration *captured = bs_jni_registration_at(i);
        assert(captured && !strcmp(captured->class_name, GAME));
        assert(!strcmp(captured->method_name, original_natives[i].name));
        assert(!strcmp(captured->signature, original_natives[i].signature));
        assert(captured->function_address == original_natives[i].elf_vaddr);
    }

    printf("JNI GameActivity metadata test passed: %zu exact members, "
           "%zu captured original native entries, %d diagnostic logs; "
           "no guest or Java lifecycle invoked\n",
           sizeof original_members / sizeof original_members[0],
           sizeof original_natives / sizeof original_natives[0], log_count);
    return 0;
}
