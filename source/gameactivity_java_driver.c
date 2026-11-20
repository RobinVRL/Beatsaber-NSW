#include "gameactivity_java_driver.h"
#include "jni_bridge.h"
#if defined(__SWITCH__)
#include "guest_tls.h"
#include "util.h"
/* A blocking GameActivity callback (the original glue waits for its
 * android_main worker to acknowledge each command) must be visible. */
#define BS_GA_TRACE(...) debugPrintf(__VA_ARGS__)
#else
#define BS_GA_TRACE(...) ((void)0)
#endif

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Lock-free progress marker read by the host scheduler watchdog. */
_Atomic int bs_gameactivity_java_driver_stage;
#define BS_GA_STAGE(n) (bs_gameactivity_java_driver_stage = (n))

/* Only these exact registered methods belong to the supplied Quest 1.46
 * GameActivity.NativeCode class. Java callback dispatch never uses guessed
 * offsets into a libgame image or an arbitrary substitute android_app. */
#define BS_GA_CLASS "com/google/androidgamesdk/GameActivity"
#define BS_GA_INITIALIZE_SIG "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J"

static uintptr_t native_method(const char *name, const char *signature) {
    uintptr_t pointer = 0;
    for (size_t i = 0; i < bs_jni_registration_count(); ++i) {
        const BSJNINativeRegistration *method = bs_jni_registration_at(i);
        if (!method || strcmp(method->class_name, BS_GA_CLASS) ||
            strcmp(method->method_name, name) ||
            strcmp(method->signature, signature)) continue;
        if (pointer) return 0; /* duplicate declarations are not executable */
        pointer = method->function_address;
    }
    return pointer;
}

static int driver_env(BsGameActivityJavaDriver *driver, void **env) {
    if (!driver || !driver->native_code || !driver->activity ||
        driver->phase == BS_GA_JAVA_DESTROYED || !bs_jni_env())
        return -EINVAL;
#if defined(__SWITCH__)
    if (!bs_guest_tls_is_thread_attached()) return -ENOTSUP;
#endif
    *env = bs_jni_env();
    return 0;
}

static int pending_exception(void *env) {
    if (!env) return -EINVAL;
    uintptr_t table = 0;
    memcpy(&table, env, sizeof table);
    if (!table) return -EINVAL;
    uintptr_t slot = 0;
    memcpy(&slot, (const void *)(table + 228 * sizeof(void *)), sizeof slot);
    if (!slot) return -ENOSYS;
    return ((uint8_t (*)(void *))slot)(env) ? -EIO : 0;
}

static int driver_result(BsGameActivityJavaDriver *driver, void *env) {
    int rc = pending_exception(env);
    if (rc) {
        driver->last_error = rc;
        driver->phase = BS_GA_JAVA_FAILED;
    }
    return rc;
}

static int driver_void(BsGameActivityJavaDriver *driver,
                       const char *name) {
    void *env = NULL;
    BS_GA_STAGE(100);
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    BS_GA_STAGE(1);
    uintptr_t method = native_method(name, "(J)V");
    if (!method) return -ENOENT;
    BS_GA_STAGE(2);
    BS_GA_TRACE("[gameactivity:java] -> original %s\n", name);
    BS_GA_STAGE(3);
    ((void (*)(void *, void *, int64_t))method)(
        env, driver->activity, (int64_t)driver->native_code);
    BS_GA_STAGE(4);
    rc = driver_result(driver, env);
    BS_GA_STAGE(5);
    BS_GA_TRACE("[gameactivity:java] <- original %s rc=%d\n", name, rc);
    return rc;
}

int bs_gameactivity_java_driver_create(BsGameActivityJavaDriver *driver,
                                        const BsGameActivityJavaLaunch *launch) {
    if (!driver || !launch || !launch->activity || !launch->asset_manager ||
        !launch->configuration || !launch->first_path || !launch->second_path ||
        !launch->third_path || !*launch->first_path ||
        !*launch->second_path || !*launch->third_path)
        return -EINVAL;
    if (driver->native_code || driver->phase != BS_GA_JAVA_NEW)
        return -EALREADY;
    if (!bs_jni_gameactivity_natives_captured() ||
        !bs_jni_gameactivity_java_ready() || !bs_jni_env() ||
        !bs_jni_gameactivity_launch_matches(launch->activity,
                                            launch->asset_manager,
                                            launch->configuration))
        return -ENOTSUP;
#if defined(__SWITCH__)
    if (!bs_guest_tls_is_thread_attached()) return -ENOTSUP;
#endif
    uintptr_t initialize = native_method("initializeNativeCode",
                                          BS_GA_INITIALIZE_SIG);
    if (!initialize) return -ENOENT;

    void *env = bs_jni_env();
    uintptr_t table = 0, new_string = 0, delete_ref = 0;
    memcpy(&table, env, sizeof table);
    if (!table) return -EINVAL;
    memcpy(&new_string, (const void *)(table + 167 * sizeof(void *)),
           sizeof new_string);
    memcpy(&delete_ref, (const void *)(table + 23 * sizeof(void *)),
           sizeof delete_ref);
    if (!new_string || !delete_ref) return -ENOSYS;
    const char *paths[3] = {
        launch->first_path, launch->second_path, launch->third_path
    };
    void *strings[3] = {NULL, NULL, NULL};
    for (size_t i = 0; i < 3; ++i) {
        strings[i] = ((void *(*)(void *, const char *))new_string)(
            env, paths[i]);
        if (!strings[i] || pending_exception(env)) {
            for (size_t j = 0; j <= i; ++j)
                if (strings[j])
                    ((void (*)(void *, void *))delete_ref)(env, strings[j]);
            driver->last_error = -EIO;
            driver->phase = BS_GA_JAVA_FAILED;
            return -EIO;
        }
    }
    int64_t code = ((int64_t (*)(void *, void *, void *, void *, void *,
                               void *, void *, void *))initialize)(
        env, launch->activity, strings[0], strings[1], strings[2],
        launch->asset_manager, NULL, launch->configuration);
    for (size_t i = 0; i < 3; ++i)
        ((void (*)(void *, void *))delete_ref)(env, strings[i]);
    if (pending_exception(env) || !code) {
        driver->last_error = -EIO;
        driver->phase = BS_GA_JAVA_FAILED;
        return -EIO;
    }
    driver->native_code = (uint64_t)code;
    driver->activity = launch->activity;
    driver->configuration = launch->configuration;
    driver->phase = BS_GA_JAVA_CREATED;
    return 0;
}

int bs_gameactivity_java_driver_start(BsGameActivityJavaDriver *driver) {
    if (!driver || (driver->phase != BS_GA_JAVA_CREATED &&
                    driver->phase != BS_GA_JAVA_STOPPED)) return -EINVAL;
    int rc = driver_void(driver, "onStartNative");
    if (!rc) driver->phase = BS_GA_JAVA_STARTED;
    return rc;
}

int bs_gameactivity_java_driver_resume(BsGameActivityJavaDriver *driver) {
    if (!driver || (driver->phase != BS_GA_JAVA_STARTED &&
                    driver->phase != BS_GA_JAVA_PAUSED)) return -EINVAL;
    int rc = driver_void(driver, "onResumeNative");
    if (!rc) driver->phase = BS_GA_JAVA_RESUMED;
    return rc;
}

int bs_gameactivity_java_driver_pause(BsGameActivityJavaDriver *driver) {
    if (!driver || driver->phase != BS_GA_JAVA_RESUMED) return -EINVAL;
    int rc = driver_void(driver, "onPauseNative");
    if (!rc) driver->phase = BS_GA_JAVA_PAUSED;
    return rc;
}

int bs_gameactivity_java_driver_stop(BsGameActivityJavaDriver *driver) {
    if (!driver || (driver->phase != BS_GA_JAVA_STARTED &&
                    driver->phase != BS_GA_JAVA_PAUSED)) return -EINVAL;
    int rc = driver_void(driver, "onStopNative");
    if (!rc) driver->phase = BS_GA_JAVA_STOPPED;
    return rc;
}

int bs_gameactivity_java_driver_focus(BsGameActivityJavaDriver *driver,
                                       bool focused) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    if (driver->focused == focused) return 0;
    uintptr_t method = native_method("onWindowFocusChangedNative", "(JZ)V");
    if (!method) return -ENOENT;
    BS_GA_TRACE("[gameactivity:java] -> original onWindowFocusChangedNative(%d)\n",
                (int)focused);
    ((void (*)(void *, void *, int64_t, uint8_t))method)(
        env, driver->activity, (int64_t)driver->native_code,
        (uint8_t)focused);
    rc = driver_result(driver, env);
    BS_GA_TRACE("[gameactivity:java] <- original onWindowFocusChangedNative rc=%d\n", rc);
    if (!rc) driver->focused = focused;
    return rc;
}

int bs_gameactivity_java_driver_surface_created(
    BsGameActivityJavaDriver *driver, void *verified_surface,
    int32_t format, int32_t width, int32_t height) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    if (!verified_surface || !bs_jni_is_backed_surface(env, verified_surface) ||
        driver->surface_attached || width <= 0 || height <= 0 || format <= 0)
        return -EINVAL;
    uintptr_t method = native_method("onSurfaceCreatedNative",
                                      "(JLandroid/view/Surface;)V");
    if (!method) return -ENOENT;
    BS_GA_TRACE("[gameactivity:java] -> original onSurfaceCreatedNative %dx%d fmt=%d\n",
                (int)width, (int)height, (int)format);
    ((void (*)(void *, void *, int64_t, void *))method)(
        env, driver->activity, (int64_t)driver->native_code, verified_surface);
    rc = driver_result(driver, env);
    BS_GA_TRACE("[gameactivity:java] <- original onSurfaceCreatedNative rc=%d\n", rc);
    if (rc) return rc;
    driver->surface = verified_surface;
    driver->surface_attached = true;
    rc = bs_gameactivity_java_driver_surface_changed(
        driver, format, width, height);
    if (rc) {
        /* Native onSurfaceCreated has already accepted this Surface. If the
         * next method is absent, no Java exception is pending and we can
         * balance that callback before the caller releases the reference.
         * With a pending JNI exception, leave ownership visible so the host
         * can resolve the exception before attempting native teardown. */
        if (rc == -ENOENT || rc == -ENOSYS) {
            if (driver_void(driver, "onSurfaceDestroyedNative") == 0) {
                driver->surface_attached = false;
                driver->surface = NULL;
            }
        }
        return rc;
    }
    return 0;
}

int bs_gameactivity_java_driver_surface_changed(
    BsGameActivityJavaDriver *driver, int32_t format,
    int32_t width, int32_t height) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    if (!driver->surface_attached || !driver->surface ||
        !bs_jni_is_backed_surface(env, driver->surface) ||
        format <= 0 || width <= 0 || height <= 0) return -EINVAL;
    uintptr_t method = native_method("onSurfaceChangedNative",
                                      "(JLandroid/view/Surface;III)V");
    if (!method) return -ENOENT;
    BS_GA_TRACE("[gameactivity:java] -> original onSurfaceChangedNative\n");
    ((void (*)(void *, void *, int64_t, void *, int32_t,
              int32_t, int32_t))method)(
        env, driver->activity, (int64_t)driver->native_code,
        driver->surface, format, width, height);
    rc = driver_result(driver, env);
    BS_GA_TRACE("[gameactivity:java] <- original onSurfaceChangedNative rc=%d\n", rc);
    return rc;
}

int bs_gameactivity_java_driver_surface_redraw_needed(
    BsGameActivityJavaDriver *driver) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    if (!driver->surface_attached || !driver->surface ||
        !bs_jni_is_backed_surface(env, driver->surface)) return -EINVAL;
    uintptr_t method = native_method("onSurfaceRedrawNeededNative",
                                      "(JLandroid/view/Surface;)V");
    if (!method) return -ENOENT;
    ((void (*)(void *, void *, int64_t, void *))method)(
        env, driver->activity, (int64_t)driver->native_code, driver->surface);
    return driver_result(driver, env);
}

int bs_gameactivity_java_driver_content_rect(
    BsGameActivityJavaDriver *driver, int32_t left, int32_t top,
    int32_t right, int32_t bottom) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    if (right < left || bottom < top) return -EINVAL;
    uintptr_t method = native_method("onContentRectChangedNative", "(JIIII)V");
    if (!method) return -ENOENT;
    ((void (*)(void *, void *, int64_t, int32_t, int32_t,
              int32_t, int32_t))method)(
        env, driver->activity, (int64_t)driver->native_code,
        left, top, right, bottom);
    return driver_result(driver, env);
}

int bs_gameactivity_java_driver_window_insets_changed(
    BsGameActivityJavaDriver *driver) {
    if (!driver || !bs_jni_gameactivity_host_supports_insets(driver->activity))
        return -ENOTSUP;
    return driver_void(driver, "onWindowInsetsChangedNative");
}

int bs_gameactivity_java_driver_keyboard_visibility(
    BsGameActivityJavaDriver *driver, bool visible) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    uintptr_t method = native_method("onSoftwareKeyboardVisibilityChangedNative",
                                      "(JZ)V");
    if (!method) return -ENOENT;
    ((void (*)(void *, void *, int64_t, uint8_t))method)(
        env, driver->activity, (int64_t)driver->native_code, (uint8_t)visible);
    return driver_result(driver, env);
}

int bs_gameactivity_java_driver_editor_action(
    BsGameActivityJavaDriver *driver, int32_t action) {
    if (!driver || !bs_jni_gameactivity_host_supports_ime(driver->activity))
        return -ENOTSUP;
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    uintptr_t method = native_method("onEditorActionNative", "(JI)V");
    if (!method) return -ENOENT;
    ((void (*)(void *, void *, int64_t, int32_t))method)(
        env, driver->activity, (int64_t)driver->native_code, action);
    return driver_result(driver, env);
}

int bs_gameactivity_java_driver_surface_destroyed(
    BsGameActivityJavaDriver *driver) {
    if (!driver || !driver->surface_attached) return -EINVAL;
    int rc = driver_void(driver, "onSurfaceDestroyedNative");
    if (!rc) {
        driver->surface_attached = false;
        driver->surface = NULL;
    }
    return rc;
}

int bs_gameactivity_java_driver_configuration(
    BsGameActivityJavaDriver *driver, void *observed_configuration) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    if (!observed_configuration) return -EINVAL;
    uintptr_t method = native_method("onConfigurationChangedNative",
                                      "(JLandroid/content/res/Configuration;)V");
    if (!method) return -ENOENT;
    ((void (*)(void *, void *, int64_t, void *))method)(
        env, driver->activity, (int64_t)driver->native_code,
        observed_configuration);
    rc = driver_result(driver, env);
    if (!rc) driver->configuration = observed_configuration;
    return rc;
}

int bs_gameactivity_java_driver_trim_memory(
    BsGameActivityJavaDriver *driver, int32_t level) {
    void *env = NULL;
    int rc = driver_env(driver, &env);
    if (rc) return rc;
    uintptr_t method = native_method("onTrimMemoryNative", "(JI)V");
    if (!method) return -ENOENT;
    ((void (*)(void *, void *, int64_t, int32_t))method)(
        env, driver->activity, (int64_t)driver->native_code, level);
    return driver_result(driver, env);
}

int bs_gameactivity_java_driver_destroy(BsGameActivityJavaDriver *driver) {
    if (!driver || !driver->native_code ||
        driver->phase == BS_GA_JAVA_DESTROYED) return -EINVAL;
    /* Java Activity.onDestroy invokes terminateNativeCode after onStop.
     * Preserve the guest's shutdown ownership even if a previous event
     * failed, so NativeCode's native thread is not intentionally leaked. */
    if (driver->phase == BS_GA_JAVA_RESUMED) {
        int rc = bs_gameactivity_java_driver_pause(driver);
        if (rc) return rc;
    }
    if (driver->phase == BS_GA_JAVA_PAUSED ||
        driver->phase == BS_GA_JAVA_STARTED) {
        int rc = bs_gameactivity_java_driver_stop(driver);
        if (rc) return rc;
    }
    if (driver->surface_attached) {
        int rc = bs_gameactivity_java_driver_surface_destroyed(driver);
        if (rc) return rc;
    }
    int rc = driver_void(driver, "terminateNativeCode");
    if (!rc) {
        driver->phase = BS_GA_JAVA_DESTROYED;
        driver->native_code = 0;
        driver->focused = false;
    }
    return rc;
}
