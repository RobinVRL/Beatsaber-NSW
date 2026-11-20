/*
 * Isolated ABI contract test. Compile against Android NDK's official jni.h
 * to check table offsets and call through real JNIEnv and JavaVM pointer types.
 *
 * No Android or Unity shared object is linked or executed by this test.
 */
#include <jni.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/jni_bridge.h"

_Static_assert(sizeof(void *) == sizeof(void (*)(void)),
               "JNI pointer slots must hold C function pointers");
_Static_assert(offsetof(struct JNINativeInterface, GetVersion) ==
               4 * sizeof(void *), "JNI GetVersion index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, FindClass) ==
               6 * sizeof(void *), "JNI FindClass index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, GetObjectClass) ==
               31 * sizeof(void *), "JNI GetObjectClass index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, GetMethodID) ==
               33 * sizeof(void *), "JNI GetMethodID index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, CallObjectMethodA) ==
               36 * sizeof(void *), "JNI CallObjectMethodA index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, NewStringUTF) ==
               167 * sizeof(void *), "JNI NewStringUTF index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, RegisterNatives) ==
               215 * sizeof(void *), "JNI RegisterNatives index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, GetJavaVM) ==
               219 * sizeof(void *), "JNI GetJavaVM index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, ExceptionCheck) ==
               228 * sizeof(void *), "JNI ExceptionCheck index mismatch");
_Static_assert(offsetof(struct JNINativeInterface, GetObjectRefType) ==
               232 * sizeof(void *), "JNI GetObjectRefType index mismatch");
_Static_assert(sizeof(struct JNINativeInterface) == 233 * sizeof(void *),
               "JNI 1.6 interface must have 233 pointer slots");
_Static_assert(offsetof(struct JNIInvokeInterface, GetEnv) ==
               6 * sizeof(void *), "JavaVM GetEnv index mismatch");
_Static_assert(sizeof(struct JNIInvokeInterface) == 8 * sizeof(void *),
               "JavaVM interface must have 8 pointer slots");

static int g_logs;
static int g_fake_guest_invocations;

static void report_log(void *user, const char *line) {
    int *count = user;
    assert(line && *line);
    ++*count;
    ++g_logs;
}

static void example_registered_fn(JNIEnv *env, jobject object) {
    (void)env;
    (void)object;
    ++g_fake_guest_invocations;
}

static jint example_guest_onload(void *vm, void *reserved) {
    assert(vm && !reserved);
    ++g_fake_guest_invocations;
    return BS_JNI_VERSION_1_6;
}

int main(void) {
    assert(bs_jni_vm() == NULL);
    assert(bs_jni_env() == NULL);
    int callback_count = 0;
    assert(bs_jni_init(report_log, &callback_count) == BS_JNI_OK);
    assert(callback_count > 0);

    JavaVM *vm = (JavaVM *)bs_jni_vm();
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(vm && env && (*vm) && (*env));
    assert((*env)->GetVersion(env) == JNI_VERSION_1_6);

    /* Every valid ABI slot has a defined callback; unsupported calls abort. */
    uintptr_t env_words[233] = {0};
    memcpy(env_words, *env, sizeof env_words);
    for (int i = 0; i < 4; ++i)
        assert(env_words[i] == 0);
    for (int i = 4; i < 233; ++i)
        assert(env_words[i] != 0);

    jclass game = (*env)->FindClass(env, "com/unity3d/player/UnityPlayerGameActivity");
    jclass context = (*env)->FindClass(env, "android/content/Context");
    jclass native_class = (*env)->FindClass(env, "com/beatgames/NativeBootstrap");
    assert(game && context && native_class);
    jobject activity = bs_jni_activity();
    jobject assets = bs_jni_asset_manager();
    assert(activity && assets);
    assert((*env)->GetObjectClass(env, activity) == game);
    assert((*env)->IsInstanceOf(env, activity, context) == JNI_TRUE);

    jmethodID get_assets = (*env)->GetMethodID(
        env, game, "getAssets", "()Landroid/content/res/AssetManager;");
    jmethodID get_context = (*env)->GetMethodID(
        env, game, "getApplicationContext", "()Landroid/content/Context;");
    jmethodID get_package = (*env)->GetMethodID(
        env, game, "getPackageName", "()Ljava/lang/String;");
    assert(get_assets && get_context && get_package);
    assert((*env)->CallObjectMethodA(env, activity, get_assets, NULL) == assets);
    assert((*env)->CallObjectMethod(env, activity, get_context) == activity);
    jstring package = (*env)->CallObjectMethod(env, activity, get_package);
    const char *package_chars = (*env)->GetStringUTFChars(env, package, NULL);
    assert(strcmp(package_chars, "com.beatgames.beatsaber") == 0);
    (*env)->ReleaseStringUTFChars(env, package, package_chars);

    jstring empty = (*env)->NewStringUTF(env, "");
    assert((*env)->GetStringUTFLength(env, empty) == 0);
    assert(strcmp((*env)->GetStringUTFChars(env, empty, NULL), "") == 0);
    assert((*env)->GetObjectRefType(env, empty) == JNILocalRefType);

    JNINativeMethod natives[] = {
        {"nativeInit", "(J)V", (void *)&example_registered_fn},
        {"nativeStop", "()V", (void *)&example_registered_fn},
    };
    assert((*env)->RegisterNatives(env, native_class, natives, 2) == JNI_OK);
    assert(bs_jni_registration_count() == 2);
    const BSJNINativeRegistration *saved = bs_jni_registration_at(0);
    assert(saved);
    assert(!strcmp(saved->class_name, "com/beatgames/NativeBootstrap"));
    assert(!strcmp(saved->method_name, "nativeInit"));
    assert(!strcmp(saved->signature, "(J)V"));
    assert(saved->function_address == (uintptr_t)natives[0].fnPtr);
    assert(bs_jni_registration_at(2) == NULL);
    assert(g_fake_guest_invocations == 0); /* Registration NEVER calls it. */

    JNINativeMethod invalid = {"oops", "not-a-signature", natives[0].fnPtr};
    assert((*env)->RegisterNatives(env, native_class, &invalid, 1) == JNI_ERR);
    assert(bs_jni_registration_count() == 2);
    assert(*bs_jni_last_error());
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);

    JavaVM *vm_from_env = NULL;
    assert((*env)->GetJavaVM(env, &vm_from_env) == JNI_OK);
    assert(vm_from_env == vm);
    JNIEnv *queried = NULL;
    assert((*vm)->GetEnv(vm, (void **)&queried, JNI_VERSION_1_6) == JNI_OK);
    assert(queried == env);
    assert((*vm)->GetEnv(vm, (void **)&queried, 0x00010008) == JNI_EVERSION);
    assert((*vm)->DetachCurrentThread(vm) == JNI_OK);
    assert(bs_jni_env() == NULL);
    assert((*vm)->GetEnv(vm, (void **)&queried, JNI_VERSION_1_6) == JNI_EDETACHED);
    assert((*vm)->AttachCurrentThread(vm, &queried, NULL) == JNI_OK);
    assert(queried == env);

    BSJNIReadiness readiness = {0};
    assert(bs_jni_call_onload_if_ready(
        "libunity.so", example_guest_onload, &readiness) == BS_JNI_ERR);
    assert(g_fake_guest_invocations == 0);
    readiness.platform_services_ready = 1;
    readiness.unresolved_imports = 1;
    assert(bs_jni_call_onload_if_ready(
        "libunity.so", example_guest_onload, &readiness) == BS_JNI_ERR);
    assert(g_fake_guest_invocations == 0);
    readiness.unresolved_imports = 0;
    readiness.missing_needed_libraries = 1;
    assert(bs_jni_call_onload_if_ready(
        "libunity.so", example_guest_onload, &readiness) == BS_JNI_ERR);
    assert(g_fake_guest_invocations == 0);
    readiness.missing_needed_libraries = 0;
    assert(bs_jni_call_onload_if_ready(
        "isolated-test-stub", example_guest_onload, &readiness) ==
        JNI_VERSION_1_6);
    assert(g_fake_guest_invocations == 1);

    printf("JNI ABI selftest OK: %lu captured; %d log callbacks; "
           "no original Unity code executed\n",
           (unsigned long)bs_jni_registration_count(), g_logs);
    return 0;
}
