/* Original Quest classes.dex UnityPlayer static field JNI contract.
 * Build host-only with fake_switch NWindow validation to exercise the
 * observation boundaries. This does NOT run Unity's Java constructors. */
#include <jni.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <switch/display/native_window.h>
#include "../source/jni_bridge.h"

#define PLAYER "com/unity3d/player/UnityPlayer"
#define DERIVED "com/unity3d/player/UnityPlayerForActivityOrService"
#define GAME "com/google/androidgamesdk/GameActivity"

_Static_assert(offsetof(struct JNINativeInterface, GetStaticFieldID) ==
               144 * sizeof(void *), "JNI GetStaticFieldID slot changed");
_Static_assert(offsetof(struct JNINativeInterface, GetStaticObjectField) ==
               145 * sizeof(void *), "JNI GetStaticObjectField slot changed");

static int host_finish(void *user) { (void)user; return 0; }
static int host_flags(void *user, int32_t flags, int32_t mask) {
    (void)user; (void)flags; (void)mask;
    return 0;
}

static void clear_failure(JNIEnv *env, const char *type) {
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    jthrowable exception = (*env)->ExceptionOccurred(env);
    assert(exception);
    jclass expected = (*env)->GetObjectClass(env, exception);
    (*env)->ExceptionClear(env);
    assert(expected == (*env)->FindClass(env, type));
    (*env)->DeleteLocalRef(env, exception);
}

int main(void) {
    assert(bs_jni_init(NULL, NULL) == JNI_OK);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env);
    assert(!bs_jni_unityplayer_has_current_activity());
    assert(!(*env)->ExceptionCheck(env));
    jclass player = (*env)->FindClass(env, PLAYER);
    jclass derived = (*env)->FindClass(env, DERIVED);
    jclass game = (*env)->FindClass(env, GAME);
    jclass unrelated = (*env)->FindClass(env, "java/lang/String");
    assert(player && derived && game && unrelated);

    jfieldID activity_id = (*env)->GetStaticFieldID(
        env, player, "currentActivity", "Landroid/app/Activity;");
    assert(activity_id);
    assert(activity_id == (*env)->GetStaticFieldID(
        env, derived, "currentActivity", "Landroid/app/Activity;"));
    jfieldID context_id = (*env)->GetStaticFieldID(
        env, player, "currentContext", "Landroid/content/Context;");
    assert(context_id && context_id != activity_id);

    assert((*env)->GetStaticFieldID(env, player, "currentActivity",
                                    "Ljava/lang/Object;") == NULL);
    clear_failure(env, "java/lang/NoSuchFieldError");
    assert((*env)->GetStaticFieldID(env, unrelated, "currentActivity",
                                    "Landroid/app/Activity;") == NULL);
    clear_failure(env, "java/lang/NoSuchFieldError");
    assert((*env)->GetStaticObjectField(env, unrelated, activity_id) == NULL);
    clear_failure(env, "java/lang/NoSuchFieldError");

    /* A DEX-declared static ID is NOT a value from a running Java host. */
    assert((*env)->GetStaticObjectField(env, derived, activity_id) == NULL);
    clear_failure(env, "java/lang/UnsupportedOperationException");
    assert(bs_jni_unityplayer_observe_current_activity(bs_jni_activity()) ==
           BS_JNI_ERR);

    BSJNIGameActivityHostServices services = {
        .finish = host_finish,
        .set_window_flags = host_flags
    };
    jobject proxy = bs_jni_gameactivity_native_proxy(&services);
    jobject other_proxy = bs_jni_gameactivity_native_proxy(&services);
    assert(proxy && other_proxy && proxy != other_proxy);
    assert(bs_jni_unityplayer_observe_current_activity(proxy) == BS_JNI_ERR);

    const BSJNIObservedLocale locale = {"en", "", "US", ""};
    jobject config = bs_jni_configuration_from_host_display(1280, 720, &locale);
    assert(config);
    NWindow window = {UINT32_C(0x53464e57)};
    jobject surface = bs_jni_surface_from_nwindow(&window);
    assert(surface && bs_jni_is_backed_surface(env, surface));
    assert(bs_jni_gameactivity_bind_host(proxy, bs_jni_asset_manager(),
                                         config, surface) == JNI_OK);
    assert(bs_jni_unityplayer_observe_current_activity(other_proxy) ==
           BS_JNI_ERR);

    /* The DEX constructor writes currentContext first, then checks
     * instanceof Activity before publishing currentActivity. Enforce that
     * ordering rather than permitting an Activity-only shortcut. */
    assert(bs_jni_unityplayer_observe_current_activity(proxy) == BS_JNI_ERR);
    assert(bs_jni_unityplayer_observe_current_context(proxy) == JNI_OK);
    assert(!bs_jni_unityplayer_has_current_activity());
    /* The host explicitly reports the Java field assignment; the getter
     * returns a fresh local reference to that exact Activity instance. */
    assert(bs_jni_unityplayer_observe_current_activity(proxy) == JNI_OK);
    assert(bs_jni_unityplayer_has_current_activity());
    jobject live = (*env)->GetStaticObjectField(env, derived, activity_id);
    assert(live && !(*env)->ExceptionCheck(env));
    assert((*env)->IsSameObject(env, live, proxy) == JNI_TRUE);
    assert((*env)->GetObjectRefType(env, live) == JNILocalRefType);
    assert((*env)->GetObjectClass(env, live) == game);
    assert((*env)->IsInstanceOf(env, live,
        (*env)->FindClass(env, "android/app/Activity")) == JNI_TRUE);
    jobject live_again = (*env)->GetStaticObjectField(env, player, activity_id);
    assert(live_again && live_again != live);
    assert((*env)->IsSameObject(env, live_again, live));
    (*env)->DeleteLocalRef(env, live);
    (*env)->DeleteLocalRef(env, live_again);

    /* The separately observed currentContext field is a true local ref. */
    jobject live_context = (*env)->GetStaticObjectField(env, player, context_id);
    assert(live_context &&
           (*env)->IsSameObject(env, live_context, proxy) == JNI_TRUE);
    (*env)->DeleteLocalRef(env, live_context);

    /* The published Activity must cease being readable if its live native
     * Surface is invalidated, even if the Java handle remains allocated. */
    window.live_token = 0;
    assert(!bs_jni_unityplayer_has_current_activity());
    assert(!(*env)->ExceptionCheck(env));
    assert((*env)->GetStaticObjectField(env, player, activity_id) == NULL);
    clear_failure(env, "java/lang/UnsupportedOperationException");
    assert(bs_jni_unityplayer_observe_current_activity(proxy) == BS_JNI_ERR);
    assert(bs_jni_unityplayer_observe_current_activity(NULL) == JNI_OK);
    assert((*env)->GetStaticObjectField(env, player, activity_id) == NULL);
    clear_failure(env, "java/lang/UnsupportedOperationException");

    puts("PASS: DEX-derived UnityPlayer inherited static field IDs, explicit "
         "Activity assignment, JNI local refs, host invalidation and errors");
    return 0;
}
