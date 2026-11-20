/* Isolated JVM/JNI semantics tested against official JDK JNI declarations.
 * Original Quest code is inspected separately, never executed in host tests.
 */
#include <jni.h>
#include "../source/jni_bridge.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static JavaVM *g_vm;
static jobject g_shared;
static JNIEnv *g_main_env;

static void *attached_thread(void *ignored) {
    (void)ignored;
    JNIEnv *env = NULL;
    assert((int32_t)(*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) ==
           JNI_EDETACHED);
    assert(env == NULL);
    assert((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) == JNI_OK);
    assert(env && env != g_main_env);
    assert((*env)->GetVersion(env) == JNI_VERSION_1_6);
    assert((int32_t)(*env)->GetVersion(g_main_env) == JNI_ERR);
    jclass known = (*env)->FindClass(env, "com/google/androidgamesdk/GameActivity");
    assert(known && !(*env)->ExceptionCheck(env));
    assert((*env)->GetObjectRefType(env, g_shared) == JNIGlobalRefType);
    jobject local = (*env)->NewLocalRef(env, g_shared);
    assert(local);
    assert((*env)->GetObjectRefType(env, local) == JNILocalRefType);
    assert((*env)->IsSameObject(env, local, g_shared) == JNI_TRUE);
    (*env)->DeleteLocalRef(env, local);
    assert((*env)->GetObjectRefType(env, local) == JNIInvalidRefType);

    jclass no_class = (*env)->FindClass(env, "invalid.class.name");
    assert(no_class == NULL);
    assert((*env)->ExceptionCheck(env));
    assert((*env)->ExceptionOccurred(env));
    (*env)->ExceptionClear(env);
    assert(!(*env)->ExceptionCheck(env));

    jclass exception = (*env)->FindClass(env, "java/lang/IllegalStateException");
    assert(exception && (*env)->ThrowNew(env, exception, "observed failure") ==
           JNI_OK);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);

    assert((*g_vm)->DetachCurrentThread(g_vm) == JNI_OK);
    assert((int32_t)(*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) ==
           JNI_EDETACHED);
    assert((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) == JNI_OK);
    assert((*env)->GetObjectRefType(env, local) == JNIInvalidRefType);
    assert((*env)->GetObjectRefType(env, g_shared) == JNIGlobalRefType);
    assert((*g_vm)->DetachCurrentThread(g_vm) == JNI_OK);
    return NULL;
}

int main(void) {
    BSJNIGameActivityEvidence evidence;
    assert(bs_jni_gameactivity_readiness(&evidence) == JNI_OK);
    assert(!bs_jni_gameactivity_java_ready());
    assert(evidence.blocker_flags & BS_JNI_GAMEACTIVITY_NOT_INITIALIZED);
    assert(bs_jni_init(NULL, NULL) == JNI_OK);
    assert(bs_jni_gameactivity_registration_abi_ready());
    assert(!bs_jni_gameactivity_java_ready());
    assert(bs_jni_gameactivity_readiness(&evidence) == JNI_OK);
    assert(evidence.expected_native_methods == 24);
    assert(evidence.captured_native_methods == 0);
    assert(evidence.blocker_flags & BS_JNI_GAMEACTIVITY_ACTIVITY_UNBACKED);

    g_vm = (JavaVM *)bs_jni_vm();
    g_main_env = (JNIEnv *)bs_jni_env();
    assert(g_vm && g_main_env);
    assert((*g_main_env)->GetVersion(g_main_env) == JNI_VERSION_1_6);
    jclass clazz = (*g_main_env)->FindClass(
        g_main_env, "com/google/androidgamesdk/GameActivity");
    assert(clazz);
    jmethodID finish = (*g_main_env)->GetMethodID(
        g_main_env, clazz, "finish", "()V");
    assert(finish);
    (*g_main_env)->CallVoidMethod(g_main_env, bs_jni_activity(), finish);
    assert((*g_main_env)->ExceptionCheck(g_main_env));
    assert(!bs_jni_gameactivity_registration_abi_ready());
    (*g_main_env)->ExceptionDescribe(g_main_env);
    assert(!(*g_main_env)->ExceptionCheck(g_main_env));
    assert(bs_jni_gameactivity_registration_abi_ready());

    assert(bs_jni_surface_from_nwindow((void *)0x1234) == NULL);
    assert(!bs_jni_is_backed_surface(g_main_env, bs_jni_activity()));
    assert(!bs_jni_backed_surface_nwindow(g_main_env, bs_jni_activity()));

    g_shared = (*g_main_env)->NewGlobalRef(g_main_env, bs_jni_asset_manager());
    assert(g_shared && (*g_main_env)->GetObjectRefType(g_main_env, g_shared) ==
           JNIGlobalRefType);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, attached_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert((*g_main_env)->ExceptionCheck(g_main_env) == JNI_FALSE);
    (*g_main_env)->DeleteGlobalRef(g_main_env, g_shared);
    puts("PASS: JNI GameActivity original registration gate, strict JNIEnv thread ownership, refs, exceptions and unbacked activity/surface");
    return 0;
}
