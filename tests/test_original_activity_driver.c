/* Host regression for the real GameActivity JNI dispatcher.
 * Uses a fake JNI function table and mock native registrations; no original
 * ARM64 code or Android Java bytecode runs here.
 *
 * clang -std=c11 -Wall -Wextra -Werror -O2 -Isource \
 *   tests/test_original_activity_driver.c source/gameactivity_java_driver.c \
 *   -o tests/test_original_activity_driver_host.exe
 */
#include "gameactivity_java_driver.h"
#include "jni_bridge.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GAME_CLASS "com/google/androidgamesdk/GameActivity"
#define INIT_SIG "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J"
#define NATIVE_CODE INT64_C(0x1234567812345678)

typedef struct MockString {
    char value[64];
    int live;
} MockString;

static uintptr_t s_slots[233];
static uintptr_t *s_slots_ptr = s_slots;
static BSJNINativeRegistration s_native[20];
static size_t s_native_count;
static int s_captured, s_java_ready, s_env_ready, s_surface_backed;
static int s_activity, s_assets, s_config, s_config_next, s_surface;
static int s_new_count, s_delete_count, s_fail_new_at, s_fail_initialize;
static int s_pending_exception;
static const char *s_fail_event;
static MockString s_strings[24];
static const char *s_events[64];
static size_t s_event_count;

size_t bs_jni_registration_count(void) { return s_native_count; }
const BSJNINativeRegistration *bs_jni_registration_at(size_t i) {
    return i < s_native_count ? &s_native[i] : NULL;
}
int bs_jni_gameactivity_natives_captured(void) { return s_captured; }
int bs_jni_gameactivity_java_ready(void) { return s_java_ready; }
int bs_jni_gameactivity_launch_matches(void *receiver, void *manager,
                                      void *configuration) {
    return receiver == &s_activity && manager == &s_assets &&
           configuration == &s_config;
}
void *bs_jni_env(void) { return s_env_ready ? &s_slots_ptr : NULL; }
int bs_jni_is_backed_surface(void *env, void *surface) {
    assert(env == &s_slots_ptr);
    return s_surface_backed && surface == &s_surface;
}
int bs_jni_gameactivity_host_supports_insets(void *activity) {
    return activity == &s_activity;
}
int bs_jni_gameactivity_host_supports_ime(void *activity) {
    return activity == &s_activity;
}

static void record(const char *event) {
    assert(s_event_count < sizeof s_events / sizeof s_events[0]);
    s_events[s_event_count++] = event;
    if (s_fail_event && !strcmp(s_fail_event, event))
        s_pending_exception = 1;
}

static void require_receiver(void *env, void *receiver, int64_t code) {
    assert(env == &s_slots_ptr);
    assert(receiver == &s_activity);
    assert(code == NATIVE_CODE);
}

static uint8_t fake_exception_check(void *env) {
    assert(env == &s_slots_ptr);
    return (uint8_t)s_pending_exception;
}
static void *fake_new_string(void *env, const char *utf8) {
    assert(env == &s_slots_ptr && utf8);
    ++s_new_count;
    if (s_fail_new_at == s_new_count) return NULL;
    assert(s_new_count <= (int)(sizeof s_strings / sizeof s_strings[0]));
    MockString *item = &s_strings[s_new_count - 1];
    assert(!item->live);
    assert(strlen(utf8) < sizeof item->value);
    memcpy(item->value, utf8, strlen(utf8) + 1);
    item->live = 1;
    return item;
}
static void fake_delete_ref(void *env, void *ref) {
    assert(env == &s_slots_ptr && ref);
    for (size_t i = 0; i < sizeof s_strings / sizeof s_strings[0]; ++i) {
        if (ref == &s_strings[i]) {
            assert(s_strings[i].live);
            s_strings[i].live = 0;
            ++s_delete_count;
            return;
        }
    }
    assert(!"DeleteLocalRef received unknown reference");
}
static int64_t fake_initialize(void *env, void *receiver, void *first,
                               void *second, void *third, void *assets,
                               void *saved_state, void *config) {
    assert(env == &s_slots_ptr && receiver == &s_activity);
    assert(assets == &s_assets && saved_state == NULL && config == &s_config);
    assert(!strcmp(((MockString *)first)->value, "files"));
    assert(!strcmp(((MockString *)second)->value, "cache"));
    assert(!strcmp(((MockString *)third)->value, "libs"));
    record("initialize");
    return s_fail_initialize ? 0 : NATIVE_CODE;
}

#define VOID_CALLBACK(function, event_name) \
    static void function(void *env, void *receiver, int64_t code) { \
        require_receiver(env, receiver, code); \
        record(event_name); \
    }
VOID_CALLBACK(fake_start, "start")
VOID_CALLBACK(fake_resume, "resume")
VOID_CALLBACK(fake_pause, "pause")
VOID_CALLBACK(fake_stop, "stop")
VOID_CALLBACK(fake_terminate, "terminate")
VOID_CALLBACK(fake_surface_destroyed, "surface_destroyed")

static void fake_focus(void *env, void *receiver, int64_t code, uint8_t focus) {
    require_receiver(env, receiver, code);
    assert(focus == 1);
    record("focus");
}
static void fake_surface_created(void *env, void *receiver, int64_t code,
                                 void *surface) {
    require_receiver(env, receiver, code);
    assert(surface == &s_surface);
    record("surface_created");
}
static void fake_surface_changed(void *env, void *receiver, int64_t code,
                                 void *surface, int32_t format,
                                 int32_t width, int32_t height) {
    require_receiver(env, receiver, code);
    assert(surface == &s_surface);
    assert(format == 4 && width > 0 && height > 0);
    record("surface_changed");
}
static void fake_config(void *env, void *receiver, int64_t code, void *config) {
    require_receiver(env, receiver, code);
    assert(config == &s_config_next);
    record("configuration");
}
static void fake_trim(void *env, void *receiver, int64_t code, int32_t level) {
    require_receiver(env, receiver, code);
    assert(level == 15);
    record("trim");
}

static void register_native(const char *name, const char *sig, uintptr_t pointer) {
    assert(s_native_count < sizeof s_native / sizeof s_native[0]);
    BSJNINativeRegistration *entry = &s_native[s_native_count++];
    memset(entry, 0, sizeof *entry);
    assert(strlen(GAME_CLASS) < sizeof entry->class_name);
    assert(strlen(name) < sizeof entry->method_name);
    assert(strlen(sig) < sizeof entry->signature);
    memcpy(entry->class_name, GAME_CLASS, sizeof GAME_CLASS);
    memcpy(entry->method_name, name, strlen(name) + 1);
    memcpy(entry->signature, sig, strlen(sig) + 1);
    entry->function_address = pointer;
}

static void prepare(void) {
    memset(s_slots, 0, sizeof s_slots);
    memset(s_strings, 0, sizeof s_strings);
    memset(s_native, 0, sizeof s_native);
    memset(s_events, 0, sizeof s_events);
    s_native_count = 0;
    s_event_count = 0;
    s_captured = s_java_ready = s_env_ready = s_surface_backed = 1;
    s_new_count = s_delete_count = s_fail_new_at = s_fail_initialize = 0;
    s_pending_exception = 0;
    s_fail_event = NULL;
    s_slots[23] = (uintptr_t)fake_delete_ref;
    s_slots[167] = (uintptr_t)fake_new_string;
    s_slots[228] = (uintptr_t)fake_exception_check;
    register_native("initializeNativeCode", INIT_SIG, (uintptr_t)fake_initialize);
    register_native("onStartNative", "(J)V", (uintptr_t)fake_start);
    register_native("onResumeNative", "(J)V", (uintptr_t)fake_resume);
    register_native("onPauseNative", "(J)V", (uintptr_t)fake_pause);
    register_native("onStopNative", "(J)V", (uintptr_t)fake_stop);
    register_native("terminateNativeCode", "(J)V", (uintptr_t)fake_terminate);
    register_native("onWindowFocusChangedNative", "(JZ)V", (uintptr_t)fake_focus);
    register_native("onSurfaceCreatedNative",
                    "(JLandroid/view/Surface;)V", (uintptr_t)fake_surface_created);
    register_native("onSurfaceChangedNative",
                    "(JLandroid/view/Surface;III)V", (uintptr_t)fake_surface_changed);
    register_native("onSurfaceDestroyedNative", "(J)V",
                    (uintptr_t)fake_surface_destroyed);
    register_native("onConfigurationChangedNative",
                    "(JLandroid/content/res/Configuration;)V", (uintptr_t)fake_config);
    register_native("onTrimMemoryNative", "(JI)V", (uintptr_t)fake_trim);
}

static BsGameActivityJavaLaunch launch(void) {
    const BsGameActivityJavaLaunch result = {
        .activity = &s_activity,
        .asset_manager = &s_assets,
        .configuration = &s_config,
        .first_path = "files",
        .second_path = "cache",
        .third_path = "libs"
    };
    return result;
}

static void expect_events(const char *const *expected, size_t count) {
    assert(s_event_count == count);
    for (size_t i = 0; i < count; ++i)
        assert(!strcmp(s_events[i], expected[i]));
}

static void test_full_lifecycle(void) {
    prepare();
    BsGameActivityJavaDriver driver = {0};
    BsGameActivityJavaLaunch config = launch();
    assert(bs_gameactivity_java_driver_resume(&driver) == -EINVAL);
    assert(bs_gameactivity_java_driver_create(&driver, &config) == 0);
    assert(driver.native_code == (uint64_t)NATIVE_CODE);
    assert(driver.phase == BS_GA_JAVA_CREATED);
    assert(driver.activity == &s_activity && driver.configuration == &s_config);
    assert(s_new_count == 3 && s_delete_count == 3);
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -EALREADY);
    assert(bs_gameactivity_java_driver_resume(&driver) == -EINVAL);
    assert(bs_gameactivity_java_driver_start(&driver) == 0);
    assert(bs_gameactivity_java_driver_resume(&driver) == 0);
    assert(bs_gameactivity_java_driver_focus(&driver, true) == 0);
    assert(bs_gameactivity_java_driver_focus(&driver, true) == 0);
    assert(bs_gameactivity_java_driver_surface_created(
        &driver, &s_activity, 4, 1280, 720) == -EINVAL);
    assert(bs_gameactivity_java_driver_surface_created(
        &driver, &s_surface, 0, 1280, 720) == -EINVAL);
    assert(bs_gameactivity_java_driver_surface_created(
        &driver, &s_surface, 4, 1280, 720) == 0);
    assert(bs_gameactivity_java_driver_surface_created(
        &driver, &s_surface, 4, 1280, 720) == -EINVAL);
    assert(bs_gameactivity_java_driver_surface_changed(
        &driver, 4, 1920, 1080) == 0);
    assert(bs_gameactivity_java_driver_configuration(
        &driver, &s_config_next) == 0);
    assert(driver.configuration == &s_config_next);
    assert(bs_gameactivity_java_driver_trim_memory(&driver, 15) == 0);
    assert(bs_gameactivity_java_driver_pause(&driver) == 0);
    assert(bs_gameactivity_java_driver_stop(&driver) == 0);
    assert(bs_gameactivity_java_driver_start(&driver) == 0);
    assert(bs_gameactivity_java_driver_resume(&driver) == 0);
    /* Destroy must pause/stop, disconnect the Surface, and terminate NativeCode. */
    assert(bs_gameactivity_java_driver_destroy(&driver) == 0);
    assert(driver.phase == BS_GA_JAVA_DESTROYED && !driver.native_code);
    assert(!driver.focused && !driver.surface_attached);
    assert(bs_gameactivity_java_driver_destroy(&driver) == -EINVAL);
    assert(bs_gameactivity_java_driver_start(&driver) == -EINVAL);
    const char *const events[] = {
        "initialize", "start", "resume", "focus", "surface_created",
        "surface_changed", "surface_changed", "configuration", "trim",
        "pause", "stop", "start", "resume", "pause", "stop",
        "surface_destroyed", "terminate"
    };
    expect_events(events, sizeof events / sizeof events[0]);
}

static void test_fail_closed_paths(void) {
    BsGameActivityJavaDriver driver;
    BsGameActivityJavaLaunch config;
    prepare();
    memset(&driver, 0, sizeof driver);
    config = launch();
    assert(bs_gameactivity_java_driver_create(NULL, &config) == -EINVAL);
    assert(bs_gameactivity_java_driver_create(&driver, NULL) == -EINVAL);
    config.activity = NULL;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -EINVAL);
    config = launch();
    config.first_path = "";
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -EINVAL);
    config = launch();
    s_java_ready = 0;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -ENOTSUP);
    s_java_ready = 1;
    s_captured = 0;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -ENOTSUP);
    s_captured = 1;
    s_env_ready = 0;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -ENOTSUP);
    s_env_ready = 1;

    /* Registration absence/duplication must never invoke arbitrary pointers. */
    const size_t saved_count = s_native_count;
    s_native_count = 0;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -ENOENT);
    s_native_count = saved_count;
    s_native[s_native_count++] = s_native[0];
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -ENOENT);
    s_native_count = saved_count;

    s_slots[167] = 0;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -ENOSYS);
    s_slots[167] = (uintptr_t)fake_new_string;
    s_fail_new_at = 2;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -EIO);
    assert(s_new_count == 2 && s_delete_count == 1);
    assert(driver.native_code == 0 && driver.phase == BS_GA_JAVA_FAILED);

    prepare();
    memset(&driver, 0, sizeof driver);
    s_fail_initialize = 1;
    assert(bs_gameactivity_java_driver_create(&driver, &config) == -EIO);
    assert(s_new_count == 3 && s_delete_count == 3);
    assert(driver.phase == BS_GA_JAVA_FAILED && !driver.native_code);
    assert(bs_gameactivity_java_driver_start(&driver) == -EINVAL);

    prepare();
    memset(&driver, 0, sizeof driver);
    assert(bs_gameactivity_java_driver_create(&driver, &config) == 0);
    s_fail_event = "start";
    assert(bs_gameactivity_java_driver_start(&driver) == -EIO);
    assert(driver.phase == BS_GA_JAVA_FAILED && driver.last_error == -EIO);
    assert(bs_gameactivity_java_driver_resume(&driver) == -EINVAL);
    /* Java may clear a pending exception before asking for native teardown. */
    s_pending_exception = 0;
    s_fail_event = NULL;
    assert(bs_gameactivity_java_driver_destroy(&driver) == 0);
    assert(driver.phase == BS_GA_JAVA_DESTROYED);
    const char *const events[] = {"initialize", "start", "terminate"};
    expect_events(events, sizeof events / sizeof events[0]);
}

int main(void) {
    test_full_lifecycle();
    test_fail_closed_paths();
    puts("PASS: original GameActivity driver native call order, JNI ownership, "
         "surface and lifecycle gates, failures and termination");
    return 0;
}
