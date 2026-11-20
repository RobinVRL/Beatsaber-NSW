/* Host-only verification of the exact original JNI dispatch ABI.
 * These callbacks are test doubles; no guest code or Java Activity runs. */
#include "../source/gameactivity_java_driver.h"
#include "../source/jni_bridge.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME "com/google/androidgamesdk/GameActivity"
#define INITIALIZE "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J"

static uintptr_t jni_slots[233];
static uintptr_t *jni_table = jni_slots;
static int ready, pending, fail_string_at, string_calls, string_deletes;
static int init_calls, start_calls, resume_calls, pause_calls, stop_calls;
static int created_calls, changed_calls, destroyed_calls, terminate_calls;
static int focus_calls, trim_calls, config_calls;
static int redraw_calls, content_calls, insets_calls, keyboard_calls, editor_calls;
static int supports_insets = 1, supports_ime = 1;
static int asset, activity, config, surface;
static int64_t native_code = INT64_C(0x12345678);
static int skip_changed;

void *bs_jni_env(void) { return &jni_table; }
int bs_jni_gameactivity_natives_captured(void) { return 1; }
int bs_jni_gameactivity_java_ready(void) { return ready; }
int bs_jni_gameactivity_launch_matches(void *receiver, void *manager,
                                      void *configuration) {
    return receiver == &activity && manager == &asset &&
           configuration == &config;
}
int bs_jni_is_backed_surface(void *env, void *object) {
    return env == bs_jni_env() && object == &surface;
}
int bs_jni_gameactivity_host_supports_insets(void *receiver) {
    return receiver == &activity && supports_insets;
}
int bs_jni_gameactivity_host_supports_ime(void *receiver) {
    return receiver == &activity && supports_ime;
}
static uint8_t exception_check(void *env) {
    assert(env == bs_jni_env());
    return (uint8_t)pending;
}
static void *new_utf(void *env, const char *value) {
    assert(env == bs_jni_env() && value);
    ++string_calls;
    if (string_calls == fail_string_at) return NULL;
    size_t len = strlen(value) + 1;
    char *copy = malloc(len);
    assert(copy);
    memcpy(copy, value, len);
    return copy;
}
static void delete_local(void *env, void *object) {
    assert(env == bs_jni_env() && object);
    ++string_deletes;
    free(object);
}
static void verify_native_args(void *env, void *receiver, int64_t code) {
    assert(env == bs_jni_env());
    assert(receiver == &activity && code == native_code);
}
static int64_t initialize(void *env, void *receiver, void *first, void *second,
                          void *third, void *manager, void *saved,
                          void *configuration) {
    assert(env == bs_jni_env() && receiver == &activity);
    assert(strcmp(first, "/first") == 0);
    assert(strcmp(second, "/second") == 0);
    assert(strcmp(third, "/third") == 0);
    assert(manager == &asset && saved == NULL && configuration == &config);
    ++init_calls;
    return native_code;
}
static void on_start(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++start_calls;
}
static void on_resume(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++resume_calls;
}
static void on_pause(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++pause_calls;
}
static void on_stop(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++stop_calls;
}
static void on_terminate(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++terminate_calls;
}
static void on_created(void *e, void *r, int64_t n, void *s) {
    verify_native_args(e, r, n); assert(s == &surface); ++created_calls;
}
static void on_changed(void *e, void *r, int64_t n, void *s,
                       int32_t f, int32_t w, int32_t h) {
    verify_native_args(e, r, n);
    assert(s == &surface && f == 4 && w == 1280 && h == 720);
    ++changed_calls;
}
static void on_destroyed(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++destroyed_calls;
}
static void on_redraw(void *e, void *r, int64_t n, void *s) {
    verify_native_args(e, r, n); assert(s == &surface); ++redraw_calls;
}
static void on_content(void *e, void *r, int64_t n,
                       int32_t l, int32_t t, int32_t rr, int32_t b) {
    verify_native_args(e, r, n);
    assert(l == 11 && t == 22 && rr == 1100 && b == 670);
    ++content_calls;
}
static void on_insets(void *e, void *r, int64_t n) {
    verify_native_args(e, r, n); ++insets_calls;
}
static void on_keyboard(void *e, void *r, int64_t n, uint8_t visible) {
    verify_native_args(e, r, n);
    assert(visible == 0 || visible == 1);
    ++keyboard_calls;
}
static void on_editor(void *e, void *r, int64_t n, int32_t action) {
    verify_native_args(e, r, n); assert(action == 6); ++editor_calls;
}
static void on_focus(void *e, void *r, int64_t n, uint8_t focused) {
    verify_native_args(e, r, n); assert(focused == 1); ++focus_calls;
}
static void on_trim(void *e, void *r, int64_t n, int32_t level) {
    verify_native_args(e, r, n); assert(level == 15); ++trim_calls;
}
static void on_config(void *e, void *r, int64_t n, void *configuration) {
    verify_native_args(e, r, n); assert(configuration == &config);
    ++config_calls;
}
static BSJNINativeRegistration methods[] = {
    {GAME, "initializeNativeCode", INITIALIZE, 0},
    {GAME, "onStartNative", "(J)V", 0},
    {GAME, "onResumeNative", "(J)V", 0},
    {GAME, "onPauseNative", "(J)V", 0},
    {GAME, "onStopNative", "(J)V", 0},
    {GAME, "terminateNativeCode", "(J)V", 0},
    {GAME, "onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", 0},
    {GAME, "onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", 0},
    {GAME, "onSurfaceDestroyedNative", "(J)V", 0},
    {GAME, "onWindowFocusChangedNative", "(JZ)V", 0},
    {GAME, "onTrimMemoryNative", "(JI)V", 0},
    {GAME, "onConfigurationChangedNative", "(JLandroid/content/res/Configuration;)V", 0},
    {GAME, "onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V", 0},
    {GAME, "onContentRectChangedNative", "(JIIII)V", 0},
    {GAME, "onWindowInsetsChangedNative", "(J)V", 0},
    {GAME, "onSoftwareKeyboardVisibilityChangedNative", "(JZ)V", 0},
    {GAME, "onEditorActionNative", "(JI)V", 0}
};
size_t bs_jni_registration_count(void) {
    return sizeof(methods) / sizeof(methods[0]);
}
const BSJNINativeRegistration *bs_jni_registration_at(size_t index) {
    if (index >= bs_jni_registration_count()) return NULL;
    if (skip_changed && index == 7) return NULL;
    return &methods[index];
}
int main(void) {
    jni_slots[167] = (uintptr_t)new_utf;
    jni_slots[23] = (uintptr_t)delete_local;
    jni_slots[228] = (uintptr_t)exception_check;
    uintptr_t fns[] = {
        (uintptr_t)initialize, (uintptr_t)on_start, (uintptr_t)on_resume,
        (uintptr_t)on_pause, (uintptr_t)on_stop, (uintptr_t)on_terminate,
        (uintptr_t)on_created, (uintptr_t)on_changed, (uintptr_t)on_destroyed,
        (uintptr_t)on_focus, (uintptr_t)on_trim, (uintptr_t)on_config,
        (uintptr_t)on_redraw, (uintptr_t)on_content, (uintptr_t)on_insets,
        (uintptr_t)on_keyboard, (uintptr_t)on_editor
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); ++i)
        methods[i].function_address = fns[i];
    BsGameActivityJavaLaunch launch = {
        &activity, &asset, &config, "/first", "/second", "/third"
    };
    BsGameActivityJavaDriver driver = {0};
    assert(bs_gameactivity_java_driver_create(&driver, &launch) == -ENOTSUP);
    assert(init_calls == 0 && driver.phase == BS_GA_JAVA_NEW);
    ready = 1;
    BsGameActivityJavaLaunch mismatched = launch;
    mismatched.configuration = &asset;
    assert(bs_gameactivity_java_driver_create(&driver, &mismatched) == -ENOTSUP);
    assert(init_calls == 0 && driver.phase == BS_GA_JAVA_NEW);
    assert(bs_gameactivity_java_driver_create(&driver, &launch) == 0);
    assert(driver.phase == BS_GA_JAVA_CREATED && driver.native_code == (uint64_t)native_code);
    assert(string_calls == 3 && string_deletes == 3 && init_calls == 1);
    assert(bs_gameactivity_java_driver_resume(&driver) == -EINVAL);
    assert(bs_gameactivity_java_driver_start(&driver) == 0);
    assert(bs_gameactivity_java_driver_resume(&driver) == 0);
    assert(bs_gameactivity_java_driver_focus(&driver, true) == 0);
    assert(bs_gameactivity_java_driver_surface_redraw_needed(&driver) == -EINVAL);
    assert(bs_gameactivity_java_driver_surface_created(
        &driver, &surface, 4, 1280, 720) == 0);
    assert(created_calls == 1 && changed_calls == 1 && driver.surface_attached);
    assert(bs_gameactivity_java_driver_surface_redraw_needed(&driver) == 0);
    assert(bs_gameactivity_java_driver_content_rect(&driver, 11, 22, 10, 670)
           == -EINVAL);
    assert(bs_gameactivity_java_driver_content_rect(&driver, 11, 22, 1100, 670)
           == 0);
    supports_insets = 0;
    assert(bs_gameactivity_java_driver_window_insets_changed(&driver) == -ENOTSUP);
    supports_insets = 1;
    assert(bs_gameactivity_java_driver_window_insets_changed(&driver) == 0);
    assert(bs_gameactivity_java_driver_keyboard_visibility(&driver, true) == 0);
    assert(bs_gameactivity_java_driver_keyboard_visibility(&driver, false) == 0);
    supports_ime = 0;
    assert(bs_gameactivity_java_driver_editor_action(&driver, 6) == -ENOTSUP);
    supports_ime = 1;
    assert(bs_gameactivity_java_driver_editor_action(&driver, 6) == 0);
    assert(bs_gameactivity_java_driver_trim_memory(&driver, 15) == 0);
    assert(bs_gameactivity_java_driver_configuration(&driver, &config) == 0);
    assert(bs_gameactivity_java_driver_destroy(&driver) == 0);
    assert(driver.phase == BS_GA_JAVA_DESTROYED && !driver.native_code);
    assert(start_calls == 1 && resume_calls == 1 && pause_calls == 1);
    assert(stop_calls == 1 && terminate_calls == 1 && destroyed_calls == 1);
    assert(focus_calls == 1 && trim_calls == 1 && config_calls == 1);
    assert(redraw_calls == 1 && content_calls == 1 && insets_calls == 1);
    assert(keyboard_calls == 2 && editor_calls == 1);
    assert(bs_gameactivity_java_driver_surface_redraw_needed(&driver) == -EINVAL);

    /* A missing surfaceChanged native must not strand a successfully
     * created surface when surfaceDestroyed remains callable. */
    skip_changed = 1;
    BsGameActivityJavaDriver partial = {0};
    assert(bs_gameactivity_java_driver_create(&partial, &launch) == 0);
    assert(bs_gameactivity_java_driver_surface_created(
        &partial, &surface, 4, 1280, 720) == -ENOENT);
    assert(!partial.surface_attached && partial.surface == NULL);
    assert(created_calls == 2 && changed_calls == 1 && destroyed_calls == 2);
    assert(bs_gameactivity_java_driver_destroy(&partial) == 0);

    /* Failed Java UTF string construction frees earlier local references
     * and does not claim that original initializeNativeCode was invoked. */
    skip_changed = 0;
    fail_string_at = string_calls + 2;
    BsGameActivityJavaDriver failed = {0};
    int before = init_calls, deleted = string_deletes;
    assert(bs_gameactivity_java_driver_create(&failed, &launch) == -EIO);
    assert(failed.phase == BS_GA_JAVA_FAILED && failed.native_code == 0);
    assert(init_calls == before && string_deletes == deleted + 1);
    assert(!pending);
    puts("PASS: exact GameActivity JNI dispatch, lifecycle, surface rollback, and failed UTF refs");
    return 0;
}
