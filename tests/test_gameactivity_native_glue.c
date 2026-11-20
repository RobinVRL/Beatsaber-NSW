/* Original Quest 1.46 native_app_glue ABI and command protocol host tests.
 * Builds with -DBS_GA_HOST_TEST. No Java Activity or original game code runs.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/gameactivity_native_glue.h"
#if defined(_WIN32) && !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

static int g_looper_is_created;
static int g_looper_releases;
static int g_add_fd_calls;
static int g_remove_fd_calls;
static int g_remove_fd_fail;
static int g_close_fd_calls;
static int g_cond_broadcasts;
static void *g_registered_data;
static int g_registered_ident;
static int g_registered_events;
static int g_config_updates;
static void *g_config;
static void *g_asset_manager;
static uint8_t g_pipe_bytes[512];
static size_t g_pipe_begin, g_pipe_count;
static int g_window_object;
static int g_other_window_object;
static void *g_window;
static uint32_t g_window_width, g_window_height;
static int g_focus = -1;
static int g_callback_commands[512];
static size_t g_callback_count;
static BsGameActivityGlue *g_callback_glue;
static int g_resume_callback_saw_saved_state;

void *bs_looper_for_thread(void) {
    return g_looper_is_created ? (void *)&g_looper_is_created : NULL;
}
void *bs_looper_prepare(int options) {
    assert(options == 1);
    g_looper_is_created = 1;
    return &g_looper_is_created;
}
void bs_looper_release(void *ptr) {
    assert(ptr == &g_looper_is_created);
    ++g_looper_releases;
    g_looper_is_created = 0;
}
int bs_looper_add_fd(void *looper, int fd, int ident, int events,
                     void *callback, void *data) {
    assert(looper == &g_looper_is_created);
    assert(fd == 0x4000 && !callback);
    ++g_add_fd_calls;
    g_registered_data = data;
    g_registered_ident = ident;
    g_registered_events = events;
    return 1;
}
int bs_looper_remove_fd(void *looper, int fd) {
    assert(looper == &g_looper_is_created && fd == 0x4000);
    ++g_remove_fd_calls;
    g_registered_data = NULL;
    return g_remove_fd_fail ? 0 : 1;
}
int bs_pipe(int fds[2]) {
    assert(!g_pipe_count);
    fds[0] = 0x4000;
    fds[1] = 0x4001;
    g_pipe_begin = 0;
    return 0;
}
int bs_pipe_fcntl(int fd, int command, ...) {
    assert((fd == 0x4000 || fd == 0x4001) && command == 4);
    return 0;
}
ssize_t bs_pipe_read(int fd, void *buffer, size_t count) {
    assert(fd == 0x4000 && count == 1);
    if (!g_pipe_count) { errno = EAGAIN; return -1; }
    *(uint8_t *)buffer = g_pipe_bytes[g_pipe_begin++];
    g_pipe_begin %= sizeof(g_pipe_bytes);
    --g_pipe_count;
    return 1;
}
ssize_t bs_pipe_write(int fd, const void *buffer, size_t count) {
    assert(fd == 0x4001 && count == 1);
    if (g_pipe_count == sizeof(g_pipe_bytes)) return -1;
    g_pipe_bytes[(g_pipe_begin + g_pipe_count) % sizeof(g_pipe_bytes)] =
        *(const uint8_t *)buffer;
    ++g_pipe_count;
    return 1;
}
int bs_pipe_close(int fd) {
    assert(fd == 0x4000 || fd == 0x4001);
    ++g_close_fd_calls;
    g_pipe_count = 0;
    return 0;
}

static int verify_guest_slot(void **slot) {
    assert(slot != NULL);
    return 0;
}
int bs_mutex_init(void **slot, const int *attr) {
    (void)attr;
    verify_guest_slot(slot);
    *(uint32_t *)(void *)slot = UINT32_C(0xa1001001);
    return 0;
}
int bs_mutex_destroy(void **slot) {
    verify_guest_slot(slot);
    assert(*(uint32_t *)(void *)slot == UINT32_C(0xa1001001));
    *(uint32_t *)(void *)slot = 0;
    return 0;
}
int bs_mutex_lock(void **slot) {
    verify_guest_slot(slot);
    assert(*(uint32_t *)(void *)slot == UINT32_C(0xa1001001));
    return 0;
}
int bs_mutex_unlock(void **slot) { return bs_mutex_lock(slot); }
int bs_cond_init(void **slot, const int *attr) {
    (void)attr;
    verify_guest_slot(slot);
    *(uint32_t *)(void *)slot = UINT32_C(0xa2001001);
    return 0;
}
int bs_cond_destroy(void **slot) {
    verify_guest_slot(slot);
    assert(*(uint32_t *)(void *)slot == UINT32_C(0xa2001001));
    *(uint32_t *)(void *)slot = 0;
    return 0;
}
int bs_cond_broadcast(void **slot) {
    verify_guest_slot(slot);
    assert(*(uint32_t *)(void *)slot == UINT32_C(0xa2001001));
    ++g_cond_broadcasts;
    return 0;
}
void bs_android_configuration_from_assetmanager(void *config, void *manager) {
    assert(config == g_config && manager == g_asset_manager);
    ++g_config_updates;
}
void *bs_ga_test_native_window(uint32_t *width, uint32_t *height) {
    if (g_window) {
        *width = g_window_width;
        *height = g_window_height;
    }
    return g_window;
}
int bs_ga_test_native_focus(void) { return g_focus; }
int bs_ga_test_native_input(BsGaNativeInputSnapshot *out) {
    out->touchSampleSequence = 100;
    out->touchCount = 1;
    out->touches[0] = (BsGaNativeTouch){4, 480, 300, 9, 11};
    out->controllerAvailable = 1;
    out->controllerSampleSequence = 200;
    out->controllerButtons = UINT64_C(0x400);
    return 0;
}

static void callback(BsGameActivityApp *app, int32_t command) {
    assert(app == &g_callback_glue->app);
    assert(g_callback_count < sizeof(g_callback_commands) / sizeof(int));
    g_callback_commands[g_callback_count++] = command;
    switch (command) {
        case BS_GA_CMD_INIT_WINDOW:
            assert(app->window == &g_window_object);
            break;
        case BS_GA_CMD_TERM_WINDOW:
            /* Post-exec clears window only AFTER the onAppCmd callback. */
            assert(app->window == &g_window_object);
            break;
        case BS_GA_CMD_CONFIG_CHANGED:
            assert(g_config_updates == 1 && app->config == g_config);
            break;
        case BS_GA_CMD_START:
        case BS_GA_CMD_RESUME:
        case BS_GA_CMD_PAUSE:
        case BS_GA_CMD_STOP:
            assert(app->activityState == command);
            if (command == BS_GA_CMD_RESUME && app->savedStateSize == 10 &&
                memcmp(app->savedState, "real-state", 10) == 0)
                ++g_resume_callback_saw_saved_state;
            break;
        case BS_GA_CMD_DESTROY:
            assert(app->destroyRequested == 1);
            break;
        case BS_GA_CMD_SAVE_STATE:
            assert(app->savedState == NULL && app->savedStateSize == 0);
            assert(bs_gameactivity_glue_copy_saved_state(
                g_callback_glue, "real-state", 10) == 0);
            break;
        default:
            break;
    }
}

static void protocol_callback(BsGameActivityApp *app, int32_t command) {
    assert(app == &g_callback_glue->app);
    assert(g_callback_count < sizeof(g_callback_commands) / sizeof(int));
    g_callback_commands[g_callback_count++] = command;
}

static void process_expected(BsGameActivityGlue *glue, int cmd) {
    assert(g_pipe_count > 0);
    assert(g_registered_data == &glue->app.cmdPollSource);
    BsGameActivityPollSource *source = g_registered_data;
    assert(source->id == 1 && source->app == &glue->app);
    assert(source->process == bs_gameactivity_glue_process_cmd);
    size_t before = g_callback_count;
    source->process(&glue->app, source);
    assert(bs_gameactivity_glue_last_error(glue) == 0);
    assert(g_callback_count == before + 1);
    assert(g_callback_commands[before] == cmd);
}

static void test_native_protocol_and_real_observers(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    assert(bs_gameactivity_glue_app(&glue) == &glue.app);
    assert(!bs_gameactivity_glue_java_activity_ready(&glue));
    assert(glue.app.activity == NULL); /* No invented Java object */
    assert(glue.app.msgread == 0x4000 && glue.app.msgwrite == 0x4001);
    assert(glue.app.cmdPollSource.id == BS_GA_LOOPER_ID_MAIN);
    assert(g_registered_ident == 1 && g_registered_events == 1);
    assert(g_add_fd_calls == 1);
    g_callback_glue = &glue;
    glue.app.onAppCmd = callback;
    g_window = &g_window_object;
    g_window_width = 1280;
    g_window_height = 720;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 0);
    process_expected(&glue, BS_GA_CMD_INIT_WINDOW);
    assert(glue.app.pendingWindow == &g_window_object);
    g_window_width = 1920;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_WINDOW_RESIZED);
    assert(glue.observedWidth == 1920 && glue.observedHeight == 720);
    /* Native resized window cannot fabricate the Java content rectangle. */
    assert(glue.app.contentRect.right == 0);
    g_focus = 1;
    assert(bs_gameactivity_glue_observe_switch_focus(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_GAINED_FOCUS);
    assert(bs_gameactivity_glue_observe_switch_focus(&glue) == 0);
    g_focus = 0;
    assert(bs_gameactivity_glue_observe_switch_focus(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_LOST_FOCUS);
    g_window = NULL;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_TERM_WINDOW);
    assert(glue.app.window == NULL && glue.app.pendingWindow == NULL);
    int config = 4;
    int asset = 5;
    g_config = &config;
    g_asset_manager = &asset;
    assert(bs_gameactivity_glue_bind_configuration(&glue, NULL, &asset) == -EINVAL);
    assert(bs_gameactivity_glue_bind_configuration(&glue, &config, &asset) == 0);
    process_expected(&glue, BS_GA_CMD_CONFIG_CHANGED);
    assert(g_config_updates == 1);
    assert(glue.app.activity == NULL);
    BsGaNativeInputSnapshot snapshot;
    assert(bs_gameactivity_glue_sample_real_input(&snapshot) == 0);
    assert(snapshot.touchCount == 1 && snapshot.touches[0].x == 480);
    assert(snapshot.controllerAvailable && snapshot.controllerButtons == 0x400);
    assert(glue.app.inputBuffers[0].motionEvents == NULL);
    assert(glue.app.inputBuffers[1].keyEvents == NULL);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
    assert(glue.app.running == 0 && glue.app.destroyed == 1);
    /* Closing must seal new producers before the Bionic mutex is destroyed.
     * The host mutex verifier would assert if a late producer touched it. */
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_LOW_MEMORY,
                                              NULL, 0, 0) == -EPIPE);
    assert(bs_gameactivity_glue_copy_saved_state(&glue, "late", 4) == -EPIPE);
    assert(bs_gameactivity_glue_bind_configuration(
        &glue, &config, &asset) == -EPIPE);
    assert((atomic_load(&glue.producerGate) & (1u << 31)) != 0u);
    assert(g_remove_fd_calls == 1 && g_close_fd_calls == 2);
    assert(g_looper_releases == 1);
}

static void test_original_cmd_state_and_state_ownership(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = callback;
    assert(!bs_gameactivity_glue_java_activity_ready(&glue));
    /* Only the isolated HOST TEST exposes a command injector. Production
     * lifecycle signals require a real Java GameActivity event producer. */
    for (int cmd = BS_GA_CMD_START; cmd <= BS_GA_CMD_STOP; ++cmd) {
        assert(bs_gameactivity_glue_test_enqueue(&glue, cmd, NULL, 0, 0) == 0);
        process_expected(&glue, cmd);
        if (cmd == BS_GA_CMD_SAVE_STATE) {
            assert(glue.app.stateSaved == 1);
            assert(glue.app.savedStateSize == 10);
            assert(memcmp(glue.app.savedState, "real-state", 10) == 0);
        }
    }
    assert(glue.app.activityState == BS_GA_CMD_STOP);
    assert(glue.app.savedState != NULL);
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_START,
                                              NULL, 0, 0) == 0);
    process_expected(&glue, BS_GA_CMD_START);
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_RESUME,
                                              NULL, 0, 0) == 0);
    process_expected(&glue, BS_GA_CMD_RESUME);
    assert(g_resume_callback_saw_saved_state == 1);
    /* Original post_exec_cmd frees savedState AFTER the RESUME callback. */
    assert(glue.app.savedState == NULL && glue.app.savedStateSize == 0);
    assert(bs_gameactivity_glue_clear_saved_state(&glue) == 0);
    assert(glue.app.savedState == NULL && !glue.app.savedStateSize);
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_DESTROY,
                                              NULL, 0, 0) == 0);
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_LOW_MEMORY,
                                              NULL, 0, 0) == -EPIPE);
    process_expected(&glue, BS_GA_CMD_DESTROY);
    assert(glue.app.destroyRequested == 1);
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_START,
                                              NULL, 0, 0) == -EPIPE);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
    assert(g_looper_releases == 2);
}

static void test_bounded_command_pipe(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = callback;
    for (unsigned i = 0; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i)
        assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_LOW_MEMORY,
                                                  NULL, 0, 0) == 0);
    assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_LOW_MEMORY,
                                              NULL, 0, 0) == -ENOSPC);
    for (unsigned i = 0; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i)
        process_expected(&glue, BS_GA_CMD_LOW_MEMORY);
    assert(glue.pendingCount == 0 && g_pipe_count == 0);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
}

static void test_every_original_command_byte_without_java_input(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = protocol_callback;
    /* DESTROY (16) must be last; afterward the real glue refuses posts.
     * 19/20 exercise the exact COMMAND transport only: the two Android
     * GameActivityInputBuffers remain empty without actual Java events. */
    static const int commands[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 17, 18, 19, 20, 16
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        int cmd = commands[i];
        assert(bs_gameactivity_glue_test_enqueue(&glue, cmd, NULL, 0, 0) == 0);
        process_expected(&glue, cmd);
    }
    assert(bs_gameactivity_glue_test_enqueue(&glue, 21, NULL, 0, 0) == -EINVAL);
    assert(glue.app.inputBuffers[0].motionEvents == NULL);
    assert(glue.app.inputBuffers[0].keyEvents == NULL);
    assert(glue.app.inputBuffers[1].motionEvents == NULL);
    assert(glue.app.inputBuffers[1].keyEvents == NULL);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
}

static void test_observed_horizon_lifecycle_transitions(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = callback;
    g_focus = 1;
    /* In-focus native applet: START then RESUME, even if called repeatedly
     * before ALooper drains the two command bytes. */
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 2);
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 0);
    assert(g_pipe_count == 2);
    assert(!bs_gameactivity_glue_java_activity_ready(&glue));
    process_expected(&glue, BS_GA_CMD_START);
    process_expected(&glue, BS_GA_CMD_RESUME);
    assert(glue.app.activityState == BS_GA_CMD_RESUME);
    /* The native library applet loses focus, but remains running. */
    g_focus = 0;
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 1);
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 0);
    process_expected(&glue, BS_GA_CMD_PAUSE);
    /* Home-menu background means a full STOP, once. */
    g_focus = 3;
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 1);
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 0);
    process_expected(&glue, BS_GA_CMD_STOP);
    assert(glue.app.activityState == BS_GA_CMD_STOP);
    /* Foreground restoration sends START + RESUME, without duplicate focus
     * callbacks: those use observe_switch_focus separately. */
    g_focus = 1;
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 2);
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == 0);
    process_expected(&glue, BS_GA_CMD_START);
    process_expected(&glue, BS_GA_CMD_RESUME);
    /* appletMainLoop(false), obtained by owner: PAUSE, STOP, DESTROY. */
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, false) == 3);
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, false) == 0);
    assert(bs_gameactivity_glue_observe_switch_lifecycle(&glue, true) == -EPIPE);
    process_expected(&glue, BS_GA_CMD_PAUSE);
    process_expected(&glue, BS_GA_CMD_STOP);
    process_expected(&glue, BS_GA_CMD_DESTROY);
    assert(glue.app.destroyRequested == 1);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
}

static void test_window_replacement_and_partial_enqueue_recovery(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = protocol_callback;
    g_window = &g_window_object;
    g_window_width = 1280;
    g_window_height = 720;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_INIT_WINDOW);
    g_window = &g_other_window_object;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 2);
    process_expected(&glue, BS_GA_CMD_TERM_WINDOW);
    assert(glue.app.window == NULL);
    process_expected(&glue, BS_GA_CMD_INIT_WINDOW);
    assert(glue.app.window == &g_other_window_object);

    /* Leave exactly one queue slot. Replacing the window commits TERM and
     * fails INIT; the next sample must enqueue only the missing INIT. */
    for (unsigned i = 1; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i)
        assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_LOW_MEMORY,
                                                  NULL, 0, 0) == 0);
    g_window = &g_window_object;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == -ENOSPC);
    assert(glue.observedWindow == NULL);
    for (unsigned i = 1; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i)
        process_expected(&glue, BS_GA_CMD_LOW_MEMORY);
    process_expected(&glue, BS_GA_CMD_TERM_WINDOW);
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_INIT_WINDOW);
    assert(glue.app.window == &g_window_object);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
    g_window = NULL;
}

static void test_configuration_snapshot_and_queue_failure(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = protocol_callback;
    int config_a = 11, asset_a = 12;
    int config_b = 21, asset_b = 22;
    int updates_before = g_config_updates;
    assert(bs_gameactivity_glue_bind_configuration(
        &glue, &config_a, &asset_a) == 0);
    assert(bs_gameactivity_glue_bind_configuration(
        &glue, &config_b, &asset_b) == 0);
    assert(glue.app.config == NULL);
    g_config = &config_a;
    g_asset_manager = &asset_a;
    process_expected(&glue, BS_GA_CMD_CONFIG_CHANGED);
    assert(glue.app.config == &config_a);
    assert(g_config_updates == updates_before + 1);
    g_config = &config_b;
    g_asset_manager = &asset_b;
    process_expected(&glue, BS_GA_CMD_CONFIG_CHANGED);
    assert(glue.app.config == &config_b);
    assert(g_config_updates == updates_before + 2);
    for (unsigned i = 0; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i)
        assert(bs_gameactivity_glue_test_enqueue(&glue, BS_GA_CMD_LOW_MEMORY,
                                                  NULL, 0, 0) == 0);
    assert(bs_gameactivity_glue_bind_configuration(
        &glue, &config_a, &asset_a) == -ENOSPC);
    assert(glue.app.config == &config_b);
    for (unsigned i = 0; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i)
        process_expected(&glue, BS_GA_CMD_LOW_MEMORY);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
}

static void test_observations_do_not_regress_on_delayed_dispatch(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_callback_glue = &glue;
    glue.app.onAppCmd = protocol_callback;
    g_focus = 1;
    assert(bs_gameactivity_glue_observe_switch_focus(&glue) == 1);
    g_focus = 0;
    assert(bs_gameactivity_glue_observe_switch_focus(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_GAINED_FOCUS);
    assert(glue.lastFocus == 0);
    assert(bs_gameactivity_glue_observe_switch_focus(&glue) == 0);
    process_expected(&glue, BS_GA_CMD_LOST_FOCUS);

    g_window = &g_window_object;
    g_window_width = 1280;
    g_window_height = 720;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    g_window_width = 1920;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    g_window_width = 1280;
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 1);
    process_expected(&glue, BS_GA_CMD_INIT_WINDOW);
    process_expected(&glue, BS_GA_CMD_WINDOW_RESIZED);
    assert(glue.observedWidth == 1280);
    assert(bs_gameactivity_glue_observe_switch_window(&glue) == 0);
    process_expected(&glue, BS_GA_CMD_WINDOW_RESIZED);
    assert(glue.observedWidth == 1280);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
    g_window = NULL;
}

static void test_failed_looper_deregistration_is_reported(void) {
    BsGameActivityGlue glue;
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    g_remove_fd_fail = 1;
    assert(bs_gameactivity_glue_close_native(&glue) == -EIO);
    g_remove_fd_fail = 0;
    assert(!glue.nativePrepared && glue.app.msgread == -1);
}

int main(void) {
    /* Quest 1.46 libgame.so @0x21570 emits CMD6 with byte +80; @0x21414
     * emits CMD17, which has no software-keyboard visibility byte. */
    assert(BS_GA_CMD_SOFTWARE_KB_VIS_CHANGED == 6);
    assert(BS_GA_CMD_WINDOW_INSETS_CHANGED == 17);
    assert(BS_GA_CMD_CONFIG_CHANGED == 9);
    assert(BS_GA_CMD_START == 11 && BS_GA_CMD_DESTROY == 16);
    test_native_protocol_and_real_observers();
    test_original_cmd_state_and_state_ownership();
    test_bounded_command_pipe();
    test_every_original_command_byte_without_java_input();
    test_observed_horizon_lifecycle_transitions();
    test_window_replacement_and_partial_enqueue_recovery();
    test_configuration_snapshot_and_queue_failure();
    test_observations_do_not_regress_on_delayed_dispatch();
    test_failed_looper_deregistration_is_reported();
    puts("PASS: original Quest GameActivity 384-byte ABI, command source, "
         "real-observer transport, saved state, bounded queue, and Java gate");
    return 0;
}
