/* Native-only GameActivity method bridge regression using the real JNI ABI.
 * Host callbacks represent measured/implemented Switch services. The proxy
 * must never become a Java Activity readiness proof. */
#include <jni.h>
#include "../source/jni_bridge.h"
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GAME "com/google/androidgamesdk/GameActivity"
#define INSETS "androidx/core/graphics/Insets"

typedef struct HostState {
    int finish_calls;
    int flags_calls;
    int ime_calls;
    int insets_calls;
    int waterfall_calls;
    int fail;
    int32_t flags;
    int32_t mask;
    int32_t input_type;
    int32_t action_id;
    int32_t ime_options;
    int32_t last_inset_type;
} HostState;

static int host_finish(void *userdata) {
    HostState *state = (HostState *)userdata;
    ++state->finish_calls;
    return state->fail ? -1 : 0;
}
static int host_flags(void *userdata, int32_t flags, int32_t mask) {
    HostState *state = (HostState *)userdata;
    ++state->flags_calls;
    state->flags = flags;
    state->mask = mask;
    return state->fail ? -1 : 0;
}
static int host_ime(void *userdata, int32_t type, int32_t action,
                    int32_t options) {
    HostState *state = (HostState *)userdata;
    ++state->ime_calls;
    state->input_type = type;
    state->action_id = action;
    state->ime_options = options;
    return state->fail ? -1 : 0;
}
static int host_insets(void *userdata, int32_t type_mask, int32_t result[4]) {
    HostState *state = (HostState *)userdata;
    ++state->insets_calls;
    state->last_inset_type = type_mask;
    result[0] = 11; result[1] = 22; result[2] = 33; result[3] = 44;
    return state->fail ? -1 : 0;
}
static int host_waterfall(void *userdata, int32_t result[4]) {
    HostState *state = (HostState *)userdata;
    ++state->waterfall_calls;
    result[0] = 5; result[1] = 6; result[2] = 7; result[3] = 8;
    return state->fail ? -1 : 0;
}

static void call_void_v(JNIEnv *env, jobject obj, jmethodID method, ...) {
    va_list args;
    va_start(args, method);
    (*env)->CallVoidMethodV(env, obj, method, args);
    va_end(args);
}
static jobject call_object_v(JNIEnv *env, jobject obj, jmethodID method, ...) {
    va_list args;
    va_start(args, method);
    jobject result = (*env)->CallObjectMethodV(env, obj, method, args);
    va_end(args);
    return result;
}

int main(void) {
    assert(bs_jni_init(NULL, NULL) == JNI_OK);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env);
    jclass game = (*env)->FindClass(env, GAME);
    jclass insets_class = (*env)->FindClass(env, INSETS);
    assert(game && insets_class);

    HostState state = {0};
    BSJNIGameActivityHostServices host = {
        .user = &state,
        .finish = host_finish,
        .set_window_flags = host_flags,
        .set_ime_editor_info_fields = host_ime,
        .get_window_insets = host_insets,
        .get_waterfall_insets = host_waterfall
    };
    assert(bs_jni_gameactivity_native_proxy(NULL) == NULL);
    jobject proxy = bs_jni_gameactivity_native_proxy(&host);
    assert(proxy && proxy != bs_jni_activity());
    assert(bs_jni_gameactivity_host_supports_insets(proxy));
    assert(bs_jni_gameactivity_host_supports_ime(proxy));
    assert(!bs_jni_gameactivity_host_supports_insets(NULL));
    assert(!bs_jni_gameactivity_host_supports_ime(bs_jni_activity()));
    assert((*env)->GetObjectClass(env, proxy) == game);
    assert((*env)->IsInstanceOf(env, proxy, game) == JNI_TRUE);

    jmethodID finish = (*env)->GetMethodID(env, game, "finish", "()V");
    jmethodID flags = (*env)->GetMethodID(env, game, "setWindowFlags", "(II)V");
    jmethodID ime = (*env)->GetMethodID(env, game,
                                      "setImeEditorInfoFields", "(III)V");
    jmethodID window = (*env)->GetMethodID(env, game, "getWindowInsets",
                                "(I)Landroidx/core/graphics/Insets;");
    jmethodID waterfall = (*env)->GetMethodID(env, game,
                        "getWaterfallInsets", "()Landroidx/core/graphics/Insets;");
    assert(finish && flags && ime && window && waterfall);

    (*env)->CallVoidMethod(env, proxy, finish);
    assert(!(*env)->ExceptionCheck(env) && state.finish_calls == 1);
    call_void_v(env, proxy, flags, 0x1357, 0x2468);
    assert(!(*env)->ExceptionCheck(env) && state.flags_calls == 1);
    assert(state.flags == 0x1357 && state.mask == 0x2468);
    jvalue args[3] = {{.i = 14}, {.i = 15}, {.i = 16}};
    (*env)->CallVoidMethodA(env, proxy, ime, args);
    assert(!(*env)->ExceptionCheck(env) && state.ime_calls == 1);
    assert(state.input_type == 14 && state.action_id == 15 &&
           state.ime_options == 16);

    jobject measured = (*env)->CallObjectMethod(env, proxy, window, 0x81);
    assert(measured && state.last_inset_type == 0x81 && state.insets_calls == 1);
    assert((*env)->IsInstanceOf(env, measured, insets_class));
    const char *names[] = {"left", "top", "right", "bottom"};
    const int32_t values[] = {11, 22, 33, 44};
    for (size_t i = 0; i < 4; ++i) {
        jfieldID field = (*env)->GetFieldID(env, insets_class, names[i], "I");
        assert(field);
        assert((*env)->GetIntField(env, measured, field) == values[i]);
    }
    measured = call_object_v(env, proxy, window, 0x37);
    assert(measured && state.last_inset_type == 0x37);
    jvalue mask = {.i = 0x7f};
    measured = (*env)->CallObjectMethodA(env, proxy, window, &mask);
    assert(measured && state.last_inset_type == 0x7f);
    measured = (*env)->CallObjectMethodA(env, proxy, waterfall, NULL);
    assert(measured && state.waterfall_calls == 1);
    assert((*env)->GetIntField(env, measured,
        (*env)->GetFieldID(env, insets_class, "bottom", "I")) == 8);

    /* Synthetic Unity Activity has a different Java class and no backed
     * GameActivity methods, even after a native proxy has been installed. */
    (*env)->CallVoidMethod(env, bs_jni_activity(), finish);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    assert(state.finish_calls == 1);

    state.fail = 1;
    (*env)->CallVoidMethod(env, proxy, finish);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    assert((*env)->CallObjectMethod(env, proxy, window, 1) == NULL);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    state.fail = 0;

    BSJNIGameActivityHostServices partial = {.user = &state,
                                               .set_window_flags = host_flags};
    jobject incomplete = bs_jni_gameactivity_native_proxy(&partial);
    assert(incomplete);
    assert(!bs_jni_gameactivity_host_supports_insets(incomplete));
    assert(!bs_jni_gameactivity_host_supports_ime(incomplete));
    assert(bs_jni_gameactivity_bind_host(proxy, bs_jni_asset_manager(),
                                         NULL, NULL) == BS_JNI_ERR);
    assert(!bs_jni_gameactivity_launch_matches(proxy, bs_jni_asset_manager(),
                                               NULL));
    (*env)->CallVoidMethod(env, incomplete, finish);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    assert((*env)->CallObjectMethod(env, incomplete, waterfall) == NULL);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);

    BSJNIGameActivityEvidence evidence;
    assert(bs_jni_gameactivity_readiness(&evidence) == JNI_OK);
    assert(evidence.native_activity_proxy_count == 2);
    assert(evidence.blocker_flags & BS_JNI_GAMEACTIVITY_ACTIVITY_UNBACKED);
    assert(evidence.blocker_flags & BS_JNI_GAMEACTIVITY_CALLBACKS_UNBACKED);
    assert(!evidence.actual_java_activity_ready);
    assert(!bs_jni_gameactivity_java_ready());
    puts("PASS: exact original GameActivity JNI proxy callbacks, measured insets, errors, strict Java readiness");
    return 0;
}
