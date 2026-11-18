/* Regression for JNI exceptions observed by the original GameActivity path.
 * Real JDK/NDK function-table signatures exercise local and global ownership.
 * This test does not create a Java Activity or invoke guest code. */
#include <jni.h>
#include "../source/jni_bridge.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static JavaVM *g_vm;
static jobject g_exception_global;

static void *check_attached_worker(void *unused) {
    (void)unused;
    JNIEnv *worker = NULL;
    assert((*g_vm)->GetEnv(g_vm, (void **)&worker,
                           JNI_VERSION_1_6) == JNI_EDETACHED);
    assert((*g_vm)->AttachCurrentThread(g_vm, &worker, NULL) == JNI_OK);
    assert(worker);
    assert((*worker)->GetObjectRefType(worker, g_exception_global) ==
           JNIGlobalRefType);
    jclass exception_type = (*worker)->GetObjectClass(worker,
                                                      g_exception_global);
    assert(exception_type == (*worker)->FindClass(
        worker, "java/lang/NoClassDefFoundError"));
    assert((*worker)->Throw(worker, (jthrowable)g_exception_global) == JNI_OK);
    jthrowable pending = (*worker)->ExceptionOccurred(worker);
    assert(pending);
    assert((*worker)->IsSameObject(worker, pending, g_exception_global));
    (*worker)->ExceptionClear(worker);
    assert((*worker)->GetObjectRefType(worker, pending) == JNILocalRefType);
    (*worker)->DeleteLocalRef(worker, pending);
    assert((*g_vm)->DetachCurrentThread(g_vm) == JNI_OK);
    return NULL;
}

int main(void) {
    assert(bs_jni_init(NULL, NULL) == JNI_OK);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    g_vm = (JavaVM *)bs_jni_vm();
    assert(env && g_vm);

    jclass clazz = (*env)->FindClass(env, "invalid.class.name");
    assert(!clazz && (*env)->ExceptionCheck(env));
    jthrowable first = (*env)->ExceptionOccurred(env);
    jthrowable second = (*env)->ExceptionOccurred(env);
    assert(first && second && first != second);
    assert((*env)->GetObjectRefType(env, first) == JNILocalRefType);
    assert((*env)->IsSameObject(env, first, second));
    g_exception_global = (*env)->NewGlobalRef(env, first);
    assert(g_exception_global);
    (*env)->ExceptionClear(env);
    assert(!(*env)->ExceptionCheck(env));
    assert((*env)->IsSameObject(env, g_exception_global, first));

    /* A second pending exception cannot mutate the first Java object. */
    jclass game = (*env)->FindClass(
        env, "com/google/androidgamesdk/GameActivity");
    assert(game);
    assert((*env)->GetMethodID(env, game, "missing", "()V") == NULL);
    assert((*env)->ExceptionCheck(env));
    jthrowable later = (*env)->ExceptionOccurred(env);
    assert(later && !(*env)->IsSameObject(env, later, first));
    (*env)->ExceptionClear(env);
    assert((*env)->GetObjectClass(env, first) ==
           (*env)->FindClass(env, "java/lang/NoClassDefFoundError"));
    assert((*env)->GetObjectClass(env, later) ==
           (*env)->FindClass(env, "java/lang/NoSuchMethodError"));

    /* A global Throwable outlives the originating pending state and thread. */
    pthread_t thread;
    assert(pthread_create(&thread, NULL, check_attached_worker, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(!(*env)->ExceptionCheck(env));
    assert((*env)->Throw(env, (jthrowable)g_exception_global) == JNI_OK);
    jthrowable rethrown = (*env)->ExceptionOccurred(env);
    assert(rethrown && (*env)->IsSameObject(env, rethrown,
                                            g_exception_global));
    (*env)->ExceptionClear(env);

    /* An empty ThrowNew message still yields a valid exception object. */
    jclass state = (*env)->FindClass(env, "java/lang/IllegalStateException");
    assert(state && (*env)->ThrowNew(env, state, "") == JNI_OK);
    jthrowable empty_message = (*env)->ExceptionOccurred(env);
    assert(empty_message);
    (*env)->ExceptionClear(env);
    assert((*env)->GetObjectClass(env, empty_message) == state);

    (*env)->DeleteLocalRef(env, first);
    (*env)->DeleteLocalRef(env, second);
    (*env)->DeleteLocalRef(env, later);
    (*env)->DeleteLocalRef(env, rethrown);
    (*env)->DeleteLocalRef(env, empty_message);
    (*env)->DeleteGlobalRef(env, g_exception_global);
    assert(!(*env)->ExceptionCheck(env));
    puts("PASS: JNI GameActivity Throwable identity, local/global references, cross-thread rethrow, empty messages");
    return 0;
}
