/* Verify JNI_OnLoad cannot report success while the original guest has left
 * an Android Java exception pending. This host test does not run Quest ELF. */
#include <jni.h>
#include <assert.h>
#include <stdio.h>

#include "../source/jni_bridge.h"

static int32_t clean_onload(void *vm, void *reserved) {
    assert(vm == bs_jni_vm() && !reserved);
    return JNI_VERSION_1_6;
}

static int32_t unbacked_activity_onload(void *vm, void *reserved) {
    assert(vm == bs_jni_vm() && !reserved);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env != NULL);
    jclass unity = (*env)->FindClass(env, "com/unity3d/player/UnityPlayer");
    assert(unity != NULL);
    jfieldID field = (*env)->GetStaticFieldID(
        env, unity, "currentActivity", "Landroid/app/Activity;");
    assert(field != NULL);
    assert((*env)->GetStaticObjectField(env, unity, field) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    /* Simulate the real libUnityOpenXR guest return: Java exception still
     * pending even though the native function returns a valid JNI version. */
    return JNI_VERSION_1_6;
}

int main(void) {
    assert(bs_jni_init(NULL, NULL) == BS_JNI_OK);
    const BSJNIReadiness ready = {
        .unresolved_imports = 0,
        .missing_needed_libraries = 0,
        .platform_services_ready = 1
    };
    assert(bs_jni_call_onload_if_ready(
        "libUnityOpenXR.so", clean_onload, &ready) == JNI_VERSION_1_6);
    assert(bs_jni_call_onload_if_ready(
        "libUnityOpenXR.so", unbacked_activity_onload, &ready) == BS_JNI_ERR);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env && (*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
    puts("PASS: original JNI_OnLoad fails closed on pending Java exception");
    return 0;
}
