/* Host test of Quest UnityPlayer.<init>'s distinct currentContext DEX write.
 * This is a JNI ABI/ownership test, NOT execution of original Java bytecode. */
#include <jni.h>
#include <assert.h>
#include <stdio.h>
#include <switch/display/native_window.h>

#include "../source/jni_bridge.h"

static int finish(void *unused) { (void)unused; return 0; }
static int flags(void *unused, int32_t value, int32_t mask) {
    (void)unused; (void)value; (void)mask;
    return 0;
}

static void clear_exception(JNIEnv *env) {
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
}

int main(void) {
    assert(bs_jni_init(NULL, NULL) == BS_JNI_OK);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env);
    jclass unity = (*env)->FindClass(env, "com/unity3d/player/UnityPlayer");
    jclass derived = (*env)->FindClass(
        env, "com/unity3d/player/UnityPlayerForGameActivity");
    assert(unity && derived);
    jfieldID context_id = (*env)->GetStaticFieldID(
        env, unity, "currentContext", "Landroid/content/Context;");
    jfieldID activity_id = (*env)->GetStaticFieldID(
        env, unity, "currentActivity", "Landroid/app/Activity;");
    assert(context_id && activity_id && context_id != activity_id);
    /* The Java subclass inherits the same static field, not a new value. */
    assert((*env)->GetStaticFieldID(env, derived, "currentContext",
               "Landroid/content/Context;") == context_id);
    assert((*env)->GetStaticObjectField(env, unity, context_id) == NULL);
    clear_exception(env);

    const BSJNIGameActivityHostServices services = {
        .finish = finish, .set_window_flags = flags
    };
    jobject receiver = bs_jni_gameactivity_native_proxy(&services);
    jobject different = bs_jni_gameactivity_native_proxy(&services);
    assert(receiver && different);
    assert(bs_jni_unityplayer_observe_current_context(receiver) == BS_JNI_ERR);
    const BSJNIObservedLocale locale = {"en", "", "US", ""};
    jobject config = bs_jni_configuration_from_host_display(1280, 720, &locale);
    NWindow window = {UINT32_C(0x53464e57)};
    jobject surface = bs_jni_surface_from_nwindow(&window);
    assert(config && surface);
    assert(bs_jni_gameactivity_bind_host(receiver, bs_jni_asset_manager(),
                                         config, surface) == BS_JNI_OK);

    assert(bs_jni_unityplayer_observe_current_context(different) == BS_JNI_ERR);
    assert(bs_jni_unityplayer_observe_current_context(bs_jni_activity()) ==
           BS_JNI_ERR);
    /* The caller reports a real DEX constructor's Context assignment, never
     * class registration alone. Observing Context does not publish Activity. */
    assert(bs_jni_unityplayer_observe_current_context(receiver) == BS_JNI_OK);
    jobject observed = (*env)->GetStaticObjectField(env, derived, context_id);
    assert(observed && (*env)->ExceptionCheck(env) == JNI_FALSE);
    assert((*env)->IsSameObject(env, observed, receiver) == JNI_TRUE);
    assert((*env)->GetObjectRefType(env, observed) == JNILocalRefType);
    jclass context = (*env)->FindClass(env, "android/content/Context");
    assert((*env)->IsInstanceOf(env, observed, context) == JNI_TRUE);
    (*env)->DeleteLocalRef(env, observed);
    assert((*env)->GetStaticObjectField(env, unity, activity_id) == NULL);
    clear_exception(env);

    /* Revoking the actual host surface revokes permission to expose the
     * field, even though the underlying JNI target still has lifetime. */
    window.live_token = 0;
    assert((*env)->GetStaticObjectField(env, unity, context_id) == NULL);
    clear_exception(env);
    assert(bs_jni_unityplayer_observe_current_context(receiver) == BS_JNI_ERR);
    assert(bs_jni_unityplayer_observe_current_context(NULL) == BS_JNI_OK);
    assert((*env)->GetStaticObjectField(env, unity, context_id) == NULL);
    clear_exception(env);
    puts("PASS: UnityPlayer.currentContext independent observed JNI field");
    return 0;
}
