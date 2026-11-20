/*
 * JNI AArch64 table and observed Android field semantics.
 *
 * The numeric inputs below are HOST TEST FIXTURES, not claims about the
 * Switch's actual density, locale, screen geometry, keyboard or window.
 * Production snapshot values must be obtained from a real native service.
 * No original Quest .so, Android Java bytecode or Activity is executed.
 */
#include <jni.h>

#include <assert.h>
#include <float.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/jni_bridge.h"

_Static_assert(offsetof(struct JNINativeInterface, GetIntField) ==
               100 * sizeof(void *), "JNI GetIntField slot");
_Static_assert(offsetof(struct JNINativeInterface, GetFloatField) ==
               102 * sizeof(void *), "JNI GetFloatField slot");
_Static_assert(offsetof(struct JNINativeInterface, CallIntMethod) ==
               49 * sizeof(void *), "JNI CallIntMethod slot");
_Static_assert(offsetof(struct JNINativeInterface, CallIntMethodV) ==
               50 * sizeof(void *), "JNI CallIntMethodV slot");
_Static_assert(offsetof(struct JNINativeInterface, CallIntMethodA) ==
               51 * sizeof(void *), "JNI CallIntMethodA slot");
_Static_assert(offsetof(struct JNINativeInterface, GetStaticMethodID) ==
               113 * sizeof(void *), "JNI GetStaticMethodID slot");
_Static_assert(offsetof(struct JNINativeInterface, CallStaticIntMethod) ==
               129 * sizeof(void *), "JNI CallStaticIntMethod slot");
_Static_assert(offsetof(struct JNINativeInterface, CallStaticIntMethodV) ==
               130 * sizeof(void *), "JNI CallStaticIntMethodV slot");
_Static_assert(offsetof(struct JNINativeInterface, CallStaticIntMethodA) ==
               131 * sizeof(void *), "JNI CallStaticIntMethodA slot");

#define CONFIG "android/content/res/Configuration"
#define INSETS "androidx/core/graphics/Insets"
#define TYPE "androidx/core/view/WindowInsetsCompat$Type"
#define LIST "android/os/LocaleList"
#define LOCALE "java/util/Locale"

static int logged;
static void log_callback(void *user, const char *message) {
    (void)user;
    assert(message && *message);
    ++logged;
}

static void expect_unsupported(JNIEnv *env) {
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    assert((*env)->ExceptionOccurred(env) != NULL);
    assert(strstr(bs_jni_last_error(), "UnsupportedOperationException"));
    (*env)->ExceptionDescribe(env);
    assert((*env)->ExceptionOccurred(env) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
}

static jint call_static_v(JNIEnv *env, jclass type, jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    jint result = (*env)->CallStaticIntMethodV(env, type, method, ap);
    va_end(ap);
    return result;
}

static jobject call_object_v(JNIEnv *env, jobject receiver,
                             jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    jobject result = (*env)->CallObjectMethodV(env, receiver, method, ap);
    va_end(ap);
    return result;
}

static jint call_int_v(JNIEnv *env, jobject receiver,
                       jmethodID method, ...) {
    va_list ap;
    va_start(ap, method);
    jint result = (*env)->CallIntMethodV(env, receiver, method, ap);
    va_end(ap);
    return result;
}

static void assert_java_string(JNIEnv *env, jstring value, const char *expect) {
    assert(value && (*env)->GetObjectRefType(env, value) == JNILocalRefType);
    const char *utf = (*env)->GetStringUTFChars(env, value, NULL);
    assert(utf && !strcmp(utf, expect));
    (*env)->ReleaseStringUTFChars(env, value, utf);
}

int main(void) {
    /* A snapshot cannot be created before the JNI bridge is initialized. */
    BSJNIObservedField preinit[] = {{"densityDpi", "I", 320, 0.f}};
    assert(bs_jni_object_from_observed_fields(CONFIG, preinit, 1) == NULL);
    assert(bs_jni_init(log_callback, NULL) == BS_JNI_OK);
    JNIEnv *env = bs_jni_env();
    assert(env && *env);

    /* Explicitly supplied, *fictional* host test measurements only. */
    BSJNIObservedField config_values[] = {
        {"densityDpi", "I", 320, 0.f},
        {"fontScale", "F", 0, 1.25f},
        {"screenWidthDp", "I", 640, 0.f},
        {"orientation", "I", 2, 0.f}
    };
    jobject config = bs_jni_object_from_observed_fields(
        CONFIG, config_values, sizeof config_values / sizeof config_values[0]);
    assert(config);
    jclass config_class = (*env)->FindClass(env, CONFIG);
    assert(config_class);
    assert((*env)->GetObjectClass(env, config) == config_class);
    assert((*env)->IsInstanceOf(env, config, config_class) == JNI_TRUE);

    jfieldID dpi = (*env)->GetFieldID(env, config_class, "densityDpi", "I");
    jfieldID scale = (*env)->GetFieldID(env, config_class, "fontScale", "F");
    jfieldID width = (*env)->GetFieldID(env, config_class, "screenWidthDp", "I");
    jfieldID orientation = (*env)->GetFieldID(env, config_class, "orientation", "I");
    jfieldID height = (*env)->GetFieldID(env, config_class, "screenHeightDp", "I");
    assert(dpi && scale && width && orientation && height);
    assert((*env)->GetIntField(env, config, dpi) == 320);
    assert((*env)->GetFloatField(env, config, scale) == 1.25f);
    assert((*env)->GetIntField(env, config, width) == 640);
    assert((*env)->GetIntField(env, config, orientation) == 2);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);

    /* The unobserved field MUST NOT silently report a real height of zero. */
    assert((*env)->GetIntField(env, config, height) == 0);
    expect_unsupported(env);
    assert((*env)->GetIntField(env, config, dpi) == 320);
    assert((*env)->GetIntField(env, config, (jfieldID)scale) == 0);
    expect_unsupported(env);
    assert((*env)->GetFloatField(env, config, (jfieldID)dpi) == 0.f);
    expect_unsupported(env);

    BSJNIObservedField inset_values[] = {
        {"left", "I", 13, 0.f},
        {"top", "I", 25, 0.f},
        {"right", "I", 17, 0.f},
        {"bottom", "I", 39, 0.f}
    };
    jobject insets = bs_jni_object_from_observed_fields(
        INSETS, inset_values, sizeof inset_values / sizeof inset_values[0]);
    jclass insets_class = (*env)->FindClass(env, INSETS);
    assert(insets && insets_class);
    assert((*env)->GetObjectClass(env, insets) == insets_class);
    const char *names[] = {"left", "top", "right", "bottom"};
    const jint want[] = {13, 25, 17, 39};
    for (size_t i = 0; i < 4; ++i) {
        jfieldID field = (*env)->GetFieldID(env, insets_class, names[i], "I");
        assert(field && (*env)->GetIntField(env, insets, field) == want[i]);
    }
    assert((*env)->GetIntField(env, config,
                               (*env)->GetFieldID(env, insets_class, "left", "I"))
           == 0);
    expect_unsupported(env);
    assert((*env)->GetIntField(env, insets, (jfieldID)(uintptr_t)0x123) == 0);
    expect_unsupported(env);

    /* Strict two-phase creation; invalid/duplicate/unrelated snapshots do
     * not allocate partially populated Java objects. */
    assert(bs_jni_object_from_observed_fields(
        "android/view/KeyEvent", inset_values, 4) == NULL);
    BSJNIObservedField invalid[] = {{"fontScale", "I", 0, 0.f}};
    assert(bs_jni_object_from_observed_fields(CONFIG, invalid, 1) == NULL);
    BSJNIObservedField duplicates[] = {
        {"densityDpi", "I", 320, 0.f},
        {"densityDpi", "I", 480, 0.f}
    };
    assert(bs_jni_object_from_observed_fields(CONFIG, duplicates, 2) == NULL);
    BSJNIObservedField not_finite[] = {
        {"fontScale", "F", 0, FLT_MAX * 2.f}
    };
    assert(bs_jni_object_from_observed_fields(CONFIG, not_finite, 1) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);

    /* AndroidX Type names/signatures were verified in the original
     * GameActivity_register. Their masks are pure API constants, not
     * measurements of the presence of a bar, IME, or display cutout. */
    const struct {const char *name; jint mask;} expected[] = {
        {"statusBars", 1}, {"navigationBars", 2},
        {"captionBar", 4}, {"ime", 8},
        {"systemGestures", 16}, {"mandatorySystemGestures", 32},
        {"tappableElement", 64}, {"displayCutout", 128},
        {"systemBars", 7}
    };
    jclass type_class = (*env)->FindClass(env, TYPE);
    assert(type_class);
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
        jmethodID method = (*env)->GetStaticMethodID(
            env, type_class, expected[i].name, "()I");
        assert(method);
        assert((*env)->CallStaticIntMethod(env, type_class, method)
               == expected[i].mask);
        assert(call_static_v(env, type_class, method) == expected[i].mask);
        assert((*env)->CallStaticIntMethodA(env, type_class, method, NULL)
               == expected[i].mask);
        assert((*env)->ExceptionCheck(env) == JNI_FALSE);
    }

    jmethodID ime = (*env)->GetStaticMethodID(env, type_class, "ime", "()I");
    assert((*env)->CallStaticIntMethod(env, config_class, ime) == 0);
    expect_unsupported(env);
    assert((*env)->CallStaticIntMethodA(
        env, type_class, (jmethodID)(uintptr_t)0x1234, NULL) == 0);
    expect_unsupported(env);
    assert((*env)->GetStaticMethodID(env, type_class, "ime", "()V") == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);

    /* A genuinely observed LocaleList can be represented without loading
     * a Java class or guessing the current Horizon language. Both locales
     * are test fixtures deliberately provided by this host-only program. */
    const BSJNIObservedLocale observed_locales[] = {
        {"en", "", "US", ""},
        {"zh", "Hant", "TW", ""}
    };
    jobject localized = bs_jni_configuration_from_observed_locales(
        config_values, sizeof config_values / sizeof config_values[0],
        observed_locales, 2);
    assert(localized);
    assert((*env)->GetIntField(env, localized, dpi) == 320);
    jmethodID get_locales = (*env)->GetMethodID(
        env, config_class, "getLocales", "()Landroid/os/LocaleList;");
    assert(get_locales);
    jobject list = (*env)->CallObjectMethod(env, localized, get_locales);
    jclass list_class = (*env)->FindClass(env, LIST);
    assert(list && (*env)->GetObjectClass(env, list) == list_class);
    jmethodID size = (*env)->GetMethodID(env, list_class, "size", "()I");
    jmethodID get = (*env)->GetMethodID(
        env, list_class, "get", "(I)Ljava/util/Locale;");
    assert(size && get);
    assert((*env)->CallIntMethod(env, list, size) == 2);
    assert(call_int_v(env, list, size) == 2);
    assert((*env)->CallIntMethodA(env, list, size, NULL) == 2);
    jvalue which = {.i = 1};
    jobject locale_one = (*env)->CallObjectMethodA(env, list, get, &which);
    jobject locale_zero = (*env)->CallObjectMethod(env, list, get, 0);
    assert(locale_one && locale_zero);
    assert(call_object_v(env, list, get, 1) == locale_one);
    jclass locale_class = (*env)->FindClass(env, LOCALE);
    assert((*env)->GetObjectClass(env, locale_one) == locale_class);
    jmethodID language = (*env)->GetMethodID(
        env, locale_class, "getLanguage", "()Ljava/lang/String;");
    jmethodID script = (*env)->GetMethodID(
        env, locale_class, "getScript", "()Ljava/lang/String;");
    jmethodID country = (*env)->GetMethodID(
        env, locale_class, "getCountry", "()Ljava/lang/String;");
    jmethodID variant = (*env)->GetMethodID(
        env, locale_class, "getVariant", "()Ljava/lang/String;");
    assert(language && script && country && variant);
    assert_java_string(env,
        (*env)->CallObjectMethod(env, locale_one, language), "zh");
    assert_java_string(env,
        (*env)->CallObjectMethodA(env, locale_one, script, NULL), "Hant");
    assert_java_string(env,
        call_object_v(env, locale_one, country), "TW");
    assert_java_string(env,
        (*env)->CallObjectMethod(env, locale_one, variant), "");
    assert_java_string(env,
        (*env)->CallObjectMethod(env, locale_zero, language), "en");

    assert((*env)->CallObjectMethod(env, list, get, -1) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    assert(strstr(bs_jni_last_error(), "IndexOutOfBoundsException"));
    (*env)->ExceptionDescribe(env);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
    assert((*env)->CallObjectMethod(env, list, get, 2) == NULL);
    assert(strstr(bs_jni_last_error(), "IndexOutOfBoundsException"));
    (*env)->ExceptionClear(env);
    assert((*env)->CallObjectMethodA(env, list, get, NULL) == NULL);
    expect_unsupported(env);

    assert((*env)->CallObjectMethod(env, config, get_locales) == NULL);
    expect_unsupported(env);
    BSJNIObservedLocale invalid_locale = {NULL, "", "US", ""};
    assert(bs_jni_configuration_from_observed_locales(
        NULL, 0, &invalid_locale, 1) == NULL);
    assert(bs_jni_configuration_from_observed_locales(
        NULL, 0, observed_locales, 0) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);

    /* No Activity configuration/window methods or native game methods
     * were called, and no pending Android state was silently invented. */
    assert(bs_jni_registration_count() == 0);
    printf("JNI observed Configuration/Insets/LocaleList and AndroidX Type "
           "selftest OK (9 real masks, 2 provided locales, explicit "
           "absent-value errors; %d logs)\n", logged);
    return 0;
}
