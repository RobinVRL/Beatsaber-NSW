/* Original Quest GameActivity.NativeCode JNI lifecycle dispatcher.
 * The Java-side host supplies an actual Activity, asset manager and observed
 * Configuration. This dispatcher enters only the original registered native
 * methods; libgame itself owns its NativeCode, android_app and worker thread.
 */
#ifndef BS_GAMEACTIVITY_JAVA_DRIVER_H
#define BS_GAMEACTIVITY_JAVA_DRIVER_H
#include <stdbool.h>
#include <stdint.h>

typedef enum BsGameActivityJavaPhase {
    BS_GA_JAVA_NEW = 0,
    BS_GA_JAVA_CREATED,
    BS_GA_JAVA_STARTED,
    BS_GA_JAVA_RESUMED,
    BS_GA_JAVA_PAUSED,
    BS_GA_JAVA_STOPPED,
    BS_GA_JAVA_DESTROYED,
    BS_GA_JAVA_FAILED
} BsGameActivityJavaPhase;

typedef struct BsGameActivityJavaLaunch {
    void *activity;           /* live GameActivity jobject, not symbolic */
    void *asset_manager;      /* Java AssetManager */
    void *configuration;      /* observed Java Configuration */
    const char *first_path;   /* Java initializeNativeCode String argument 1 */
    const char *second_path;  /* Java initializeNativeCode String argument 2 */
    const char *third_path;   /* Java initializeNativeCode String argument 3 */
} BsGameActivityJavaLaunch;

typedef struct BsGameActivityJavaDriver {
    uint64_t native_code;     /* original NativeCode* returned by libgame */
    void *activity;           /* Java receiver kept alive by the host */
    void *configuration;
    void *surface;            /* live Java Surface, held by the caller */
    BsGameActivityJavaPhase phase;
    bool focused;
    bool surface_attached;
    int last_error;
} BsGameActivityJavaDriver;

/* Returns zero only after real libgame initializeNativeCode has returned a
 * nonzero NativeCode pointer. The caller must have executed original
 * constructors and the pinned registration-only JNI_OnLoad first. */
int bs_gameactivity_java_driver_create(BsGameActivityJavaDriver *driver,
                                        const BsGameActivityJavaLaunch *launch);
int bs_gameactivity_java_driver_start(BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_resume(BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_pause(BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_stop(BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_focus(BsGameActivityJavaDriver *driver,
                                       bool focused);
int bs_gameactivity_java_driver_surface_created(
    BsGameActivityJavaDriver *driver, void *verified_surface,
    int32_t format, int32_t width, int32_t height);
int bs_gameactivity_java_driver_surface_changed(
    BsGameActivityJavaDriver *driver, int32_t format,
    int32_t width, int32_t height);
/* These mirror exact original GameActivity.NativeCode callback signatures.
 * The caller supplies observations from a real host Surface/Window and never
 * invokes them to manufacture window/input readiness. */
int bs_gameactivity_java_driver_surface_redraw_needed(
    BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_surface_destroyed(
    BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_content_rect(
    BsGameActivityJavaDriver *driver, int32_t left, int32_t top,
    int32_t right, int32_t bottom);
int bs_gameactivity_java_driver_window_insets_changed(
    BsGameActivityJavaDriver *driver);
int bs_gameactivity_java_driver_keyboard_visibility(
    BsGameActivityJavaDriver *driver, bool visible);
int bs_gameactivity_java_driver_editor_action(
    BsGameActivityJavaDriver *driver, int32_t action);
int bs_gameactivity_java_driver_configuration(
    BsGameActivityJavaDriver *driver, void *observed_configuration);
int bs_gameactivity_java_driver_trim_memory(
    BsGameActivityJavaDriver *driver, int32_t level);
int bs_gameactivity_java_driver_destroy(BsGameActivityJavaDriver *driver);
/* Last progress point inside a lifecycle callback (diagnostic only). */
extern _Atomic int bs_gameactivity_java_driver_stage;
#endif
