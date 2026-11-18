/* Native-only GameActivity glue transport for original Quest Unity 6.
 * The original Java GameActivity and its 632-byte NativeCode are absent.
 * No original android_main or JNI GameActivity entrypoint is called here.
 * SPDX-License-Identifier: MIT
 */
#include "gameactivity_native_glue.h"
#if defined(BS_GA_HOST_TEST) && defined(_WIN32) && !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
#define _SSIZE_T_DEFINED
#endif
#include "android_window_bridge.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef BS_GA_HOST_TEST
#include <switch.h>
#include "bionic_threads.h"
#include "asset_fs_bridge.h"
#include "guest_tls.h"
void bs_host_console_release_for_graphics(void); /* main.c */
#else
/* The host test supplies deterministic NDK pipe, guest pthread and HID
 * shims. No false hardware availability is introduced in the Switch build. */
extern int bs_mutex_init(void **slot, const int *attr);
extern int bs_mutex_destroy(void **slot);
extern int bs_mutex_lock(void **slot);
extern int bs_mutex_unlock(void **slot);
extern int bs_cond_init(void **slot, const int *attr);
extern int bs_cond_destroy(void **slot);
extern int bs_cond_broadcast(void **slot);
extern void bs_android_configuration_from_assetmanager(void *config, void *manager);
extern void *bs_ga_test_native_window(uint32_t *width, uint32_t *height);
extern int bs_ga_test_native_focus(void);
extern int bs_ga_test_native_input(BsGaNativeInputSnapshot *out);
#endif

#define GA_F_SETFL 4
#define GA_O_NONBLOCK 0x800
#define GA_SAVED_STATE_LIMIT (4u * 1024u * 1024u)
#define GA_GATE_CLOSING (1u << 31)
#define GA_GATE_COUNT_MASK (GA_GATE_CLOSING - 1u)

/* Protect the lifetime of the Bionic mutex, command pipe and save-state
 * storage against Java/NDK command producers on foreign threads. Admission
 * and shutdown are one atomic transition: no producer can enter after the
 * owner closes the gate, while the owner waits for admitted producers to
 * finish before touching the native handles. */
static int ga_enter_producer(BsGameActivityGlue *glue) {
    unsigned value = atomic_load_explicit(&glue->producerGate,
                                           memory_order_acquire);
    for (;;) {
        if (value & GA_GATE_CLOSING) return -EPIPE;
        if ((value & GA_GATE_COUNT_MASK) == GA_GATE_COUNT_MASK)
            return -EOVERFLOW;
        if (atomic_compare_exchange_weak_explicit(
                &glue->producerGate, &value, value + 1u,
                memory_order_acq_rel, memory_order_acquire))
            return 0;
    }
}

static void ga_leave_producer(BsGameActivityGlue *glue) {
    (void)atomic_fetch_sub_explicit(&glue->producerGate, 1u,
                                    memory_order_release);
}

static int ga_lock(BsGameActivityGlue *glue) {
    int rc = bs_mutex_lock((void **)glue->app.bionicMutex);
    return rc ? -rc : 0;
}

static void ga_unlock(BsGameActivityGlue *glue) {
    (void)bs_mutex_unlock((void **)glue->app.bionicMutex);
}

BsGameActivityApp *bs_gameactivity_glue_app(BsGameActivityGlue *glue) {
    return glue && glue->nativePrepared ? &glue->app : NULL;
}

bool bs_gameactivity_glue_java_activity_ready(const BsGameActivityGlue *glue) {
    /* Having a native pipe, config and a real NWindow never proves a Java
     * GameActivity, a Java object, or original NativeCode initialization. */
    (void)glue;
    return false;
}

int bs_gameactivity_glue_last_error(const BsGameActivityGlue *glue) {
    return glue ? glue->commandDispatchError : -EINVAL;
}

static int ga_enqueue(BsGameActivityGlue *glue,
                      const BsGameActivityQueuedCommand *entry) {
    if (!glue || !entry) return -EINVAL;
    int rc = ga_enter_producer(glue);
    if (rc) return rc;
    if (!glue->nativePrepared) {
        ga_leave_producer(glue);
        return -EINVAL;
    }
    rc = ga_lock(glue);
    if (rc) {
        ga_leave_producer(glue);
        return rc;
    }
    if (glue->app.destroyRequested || glue->destroyQueued) {
        ga_unlock(glue);
        ga_leave_producer(glue);
        return -EPIPE;
    }
    if (glue->pendingCount == BS_GA_COMMAND_QUEUE_CAPACITY) {
        ga_unlock(glue);
        ga_leave_producer(glue);
        return -ENOSPC;
    }
    /* ALOOPER_EVENT_INPUT triggers only when a real one-byte command reached
     * the same virtual FD watched by the owner's Android looper. Keep queue
     * metadata locked until the byte and its payload are both committed. */
    const uint8_t byte = entry->command;
    if (bs_pipe_write(glue->app.msgwrite, &byte, 1) != 1) {
        ga_unlock(glue);
        ga_leave_producer(glue);
        return -EIO;
    }
    unsigned tail = (glue->pendingHead + glue->pendingCount) %
                    BS_GA_COMMAND_QUEUE_CAPACITY;
    glue->commands[tail] = *entry;
    ++glue->pendingCount;
    if (entry->command == BS_GA_CMD_DESTROY) glue->destroyQueued = 1;
    ga_unlock(glue);
    ga_leave_producer(glue);
    return 0;
}

int bs_gameactivity_glue_prepare_native(BsGameActivityGlue *glue) {
    if (!glue) return -EINVAL;
    memset(glue, 0, sizeof(*glue));
    atomic_init(&glue->producerGate, GA_GATE_CLOSING);
    glue->app.msgread = -1;
    glue->app.msgwrite = -1;
    glue->lastFocus = -1;
    int rc = bs_mutex_init((void **)glue->app.bionicMutex, NULL);
    if (rc) return -rc;
    rc = bs_cond_init((void **)glue->app.bionicCond, NULL);
    if (rc) {
        bs_mutex_destroy((void **)glue->app.bionicMutex);
        return -rc;
    }
    void *existing = bs_looper_for_thread();
    void *looper = bs_looper_prepare(1); /* ALLOW_NON_CALLBACKS, original uses 1 */
    if (!looper) {
        rc = -ENODEV;
        goto fail_cond;
    }
    glue->ownsLooper = !existing;
    glue->app.looper = looper;
    int pipe_fds[2];
    if (bs_pipe(pipe_fds) < 0) {
        rc = -EMFILE;
        goto fail_looper;
    }
    glue->app.msgread = pipe_fds[0];
    glue->app.msgwrite = pipe_fds[1];
    if (bs_pipe_fcntl(pipe_fds[0], GA_F_SETFL, GA_O_NONBLOCK) < 0 ||
        bs_pipe_fcntl(pipe_fds[1], GA_F_SETFL, GA_O_NONBLOCK) < 0) {
        rc = -ENOTSUP;
        goto fail_pipe;
    }
    /* Original GameActivity_onCreate@0x218c8 stores id=1 and a 24-byte poll
     * source at android_app+304, then registers cmd fd with callback=NULL. */
    glue->app.cmdPollSource.id = BS_GA_LOOPER_ID_MAIN;
    glue->app.cmdPollSource.app = &glue->app;
    glue->app.cmdPollSource.process = bs_gameactivity_glue_process_cmd;
    if (bs_looper_add_fd(looper, pipe_fds[0], BS_GA_LOOPER_ID_MAIN,
                         BS_GA_EVENT_INPUT, NULL, &glue->app.cmdPollSource) != 1) {
        rc = -EINVAL;
        goto fail_pipe;
    }
    glue->app.running = 1;  /* Native command worker, not Java lifecycle. */
    glue->nativePrepared = 1;
    atomic_store_explicit(&glue->producerGate, 0u, memory_order_release);
    return 0;

fail_pipe:
    bs_pipe_close(glue->app.msgread);
    bs_pipe_close(glue->app.msgwrite);
    glue->app.msgread = -1;
    glue->app.msgwrite = -1;
fail_looper:
    if (glue->ownsLooper) bs_looper_release(glue->app.looper);
    glue->app.looper = NULL;
fail_cond:
    bs_cond_destroy((void **)glue->app.bionicCond);
    bs_mutex_destroy((void **)glue->app.bionicMutex);
    return rc;
}

int bs_gameactivity_glue_close_native(BsGameActivityGlue *glue) {
    if (!glue || !glue->nativePrepared ||
        bs_looper_for_thread() != glue->app.looper)
        return -EINVAL;
    /* ALOOPER callbacks are owner-thread only; owner stops dispatch before
     * calling close. Atomically seal admission to all other thread producers
     * and let already-admitted operations complete before destroying handles.
     * Never hold the guest mutex while waiting for an admitted producer. */
    (void)atomic_fetch_or_explicit(&glue->producerGate, GA_GATE_CLOSING,
                                   memory_order_acq_rel);
    while ((atomic_load_explicit(&glue->producerGate, memory_order_acquire) &
            GA_GATE_COUNT_MASK) != 0u) {
#ifndef BS_GA_HOST_TEST
        svcSleepThread(1000000); /* 1 ms yield for native callback threads */
#endif
    }
    /* External caller must stop ALooper polling before destroying this
     * owner-thread glue. ALooper data pointers must not outlive it. */
    /* A failed deregistration is significant when the owner shares its
     * looper: a stale watch may outlive this pipe and its callback data.
     * Complete local teardown but report the failed native operation. */
    int cleanup_error = 0;
    if (bs_looper_remove_fd(glue->app.looper, glue->app.msgread) != 1)
        cleanup_error = -EIO;
    if (bs_pipe_close(glue->app.msgread) < 0) cleanup_error = -EIO;
    if (bs_pipe_close(glue->app.msgwrite) < 0) cleanup_error = -EIO;
    if (glue->app.savedState) {
        free(glue->app.savedState);
        glue->app.savedState = NULL;
        glue->app.savedStateSize = 0;
    }
    glue->app.running = 0;
    glue->app.destroyed = 1; /* Original worker exit @0x21958. */
    (void)bs_cond_broadcast((void **)glue->app.bionicCond);
    glue->nativePrepared = 0;
    bs_cond_destroy((void **)glue->app.bionicCond);
    bs_mutex_destroy((void **)glue->app.bionicMutex);
    if (glue->ownsLooper) bs_looper_release(glue->app.looper);
    glue->app.looper = NULL;
    glue->app.msgread = -1;
    glue->app.msgwrite = -1;
    return cleanup_error;
}

int bs_gameactivity_glue_copy_saved_state(BsGameActivityGlue *glue,
                                          const void *bytes, size_t size) {
    if (!glue || size > GA_SAVED_STATE_LIMIT ||
        (size && !bytes)) return -EINVAL;
    int rc = ga_enter_producer(glue);
    if (rc) return rc;
    if (!glue->nativePrepared) {
        ga_leave_producer(glue);
        return -EINVAL;
    }
    /* Original android_app savedState is written by the native app thread
     * while it handles SAVE_STATE. Java producers queue the command and wait;
     * allowing a foreign thread to replace the buffer would invalidate the
     * pointer held by an in-progress native onAppCmd callback. */
    if (bs_looper_for_thread() != glue->app.looper) {
        ga_leave_producer(glue);
        return -EPERM;
    }
    void *copy = NULL;
    if (size) {
        copy = malloc(size);
        if (!copy) {
            ga_leave_producer(glue);
            return -ENOMEM;
        }
        memcpy(copy, bytes, size);
    }
    rc = ga_lock(glue);
    if (rc) {
        free(copy);
        ga_leave_producer(glue);
        return rc;
    }
    if (glue->app.destroyRequested || glue->destroyQueued) {
        ga_unlock(glue);
        free(copy);
        ga_leave_producer(glue);
        return -EPIPE;
    }
    void *old = glue->app.savedState;
    glue->app.savedState = copy;
    glue->app.savedStateSize = size;
    ga_unlock(glue);
    free(old);
    ga_leave_producer(glue);
    return 0;
}

int bs_gameactivity_glue_clear_saved_state(BsGameActivityGlue *glue) {
    return bs_gameactivity_glue_copy_saved_state(glue, NULL, 0);
}

static void ga_pre(BsGameActivityGlue *glue,
                   const BsGameActivityQueuedCommand *entry) {
    BsGameActivityApp *app = &glue->app;
    switch (entry->command) {
        case BS_GA_CMD_INIT_WINDOW:
            app->pendingWindow = entry->window;
            app->window = app->pendingWindow; /* Original @0x20434 */
            break;
        /* Focus and dimensions are projected from the latest actual Switch
         * observation at enqueue time. An older queued command must not
         * rewind that projection when the looper eventually dispatches it. */
        case BS_GA_CMD_START:
        case BS_GA_CMD_RESUME:
        case BS_GA_CMD_PAUSE:
        case BS_GA_CMD_STOP:
            app->activityState = (int32_t)entry->command; /* @0x203f0 */
            glue->stage = entry->command;
            (void)bs_cond_broadcast((void **)app->bionicCond);
            break;
        case BS_GA_CMD_DESTROY:
            app->destroyRequested = 1; /* Original @0x20478 */
            break;
        case BS_GA_CMD_SAVE_STATE:
            free(app->savedState);
            app->savedState = NULL;
            app->savedStateSize = 0; /* Original read_cmd@0x20334 */
            app->stateSaved = 0;
            break;
        case BS_GA_CMD_CONFIG_CHANGED:
            /* Apply the exact configuration that accompanied this byte.
             * Multiple outstanding updates cannot overtake one another. */
            if (entry->configuration && entry->assetManager) {
                app->config = entry->configuration;
                glue->realAssetManager = entry->assetManager;
            }
            break;
        default:
            break;
    }
}

static void ga_post(BsGameActivityGlue *glue, int command) {
    BsGameActivityApp *app = &glue->app;
    switch (command) {
        case BS_GA_CMD_TERM_WINDOW:
            app->window = NULL; /* Original @0x204c0 */
            app->pendingWindow = NULL;
            (void)bs_cond_broadcast((void **)app->bionicCond);
            break;
        case BS_GA_CMD_SAVE_STATE:
            app->stateSaved = 1; /* Original @0x204d8 */
            (void)bs_cond_broadcast((void **)app->bionicCond);
            break;
        case BS_GA_CMD_RESUME:
            /* Original android_app_post_exec_cmd@0x204e8..0x20500 frees the
             * saved buffer AFTER the application callback sees RESUME. */
            free(app->savedState);
            app->savedState = NULL;
            app->savedStateSize = 0;
            break;
        default:
            break;
    }
}

void bs_gameactivity_glue_process_cmd(BsGameActivityApp *app,
                                      BsGameActivityPollSource *source) {
    if (!app || !source) return;
    BsGameActivityGlue *glue = (BsGameActivityGlue *)(void *)app;
    if (!glue->nativePrepared || source != &app->cmdPollSource ||
        source->app != app || source->id != BS_GA_LOOPER_ID_MAIN ||
        bs_looper_for_thread() != app->looper) return;
    uint8_t received = 0;
    /* One command byte, as in original android_app_read_cmd@0x202e0. */
    if (bs_pipe_read(app->msgread, &received, 1) != 1) {
        glue->commandDispatchError = -EIO;
        return;
    }
    if (ga_lock(glue)) {
        glue->commandDispatchError = -EDEADLK;
        return;
    }
    if (!glue->pendingCount) {
        glue->commandDispatchError = -EPROTO;
        ga_unlock(glue);
        return;
    }
    BsGameActivityQueuedCommand entry = glue->commands[glue->pendingHead];
    if (received != entry.command) {
        glue->commandDispatchError = -EPROTO;
        ga_unlock(glue);
        return;
    }
    glue->pendingHead = (glue->pendingHead + 1) % BS_GA_COMMAND_QUEUE_CAPACITY;
    --glue->pendingCount;
    ga_pre(glue, &entry);
    void *config = app->config;
    void *asset_manager = glue->realAssetManager;
    ga_unlock(glue);

    if (entry.command == BS_GA_CMD_CONFIG_CHANGED && config && asset_manager)
        bs_android_configuration_from_assetmanager(config, asset_manager);

    /* The callback may belong to original Unity, so never enter it on a
     * thread missing original Bionic guest TLS. Call outside all locks. */
    if (app->onAppCmd) {
#ifndef BS_GA_HOST_TEST
        if (!bs_guest_tls_is_thread_attached())
            glue->commandDispatchError = -EPERM;
        else
#endif
            app->onAppCmd(app, (int32_t)entry.command);
    }

    if (!ga_lock(glue)) {
        ga_post(glue, entry.command);
        ga_unlock(glue);
    } else {
        glue->commandDispatchError = -EDEADLK;
    }
}

static int ga_switch_window(void **out_window, uint32_t *width, uint32_t *height) {
#ifndef BS_GA_HOST_TEST
    NWindow *window = nwindowGetDefault();
    if (!window || !nwindowIsValid(window)) {
        *out_window = NULL;
        return 0;
    }
    u32 w = 0, h = 0;
    if (R_FAILED(nwindowGetDimensions(window, &w, &h)) || !w || !h)
        return -EIO; /* Do not report a missing window on a transient error. */
    /* Unity will build its EGL surface on this NWindow: release the host
     * console framebuffer that still owns its buffers. */
    bs_host_console_release_for_graphics();
    *width = w;
    *height = h;
    *out_window = window;
    return 0;
#else
    *out_window = bs_ga_test_native_window(width, height);
    return 0;
#endif
}

int bs_gameactivity_glue_observe_switch_window(BsGameActivityGlue *glue) {
    if (!glue || !glue->nativePrepared ||
        bs_looper_for_thread() != glue->app.looper) return -EINVAL;
    uint32_t w = 0, h = 0;
    void *window = NULL;
    int sample_rc = ga_switch_window(&window, &w, &h);
    if (sample_rc) return sample_rc;
    int command = -1;
    int emitted = 0;
    if (window != glue->observedWindow) {
        if (glue->observedWindow && window) {
            /* The caller must deliver TERM_WINDOW before INIT_WINDOW if
             * the physical NWindow actually changes. Commit the observed
             * intermediate NULL state now, so a failed second enqueue can
             * retry INIT without duplicating the pending TERM. */
            BsGameActivityQueuedCommand old = { .command = BS_GA_CMD_TERM_WINDOW };
            int rc = ga_enqueue(glue, &old);
            if (rc) return rc;
            glue->observedWindow = NULL;
            glue->observedWidth = 0;
            glue->observedHeight = 0;
            ++emitted;
        }
        command = window ? BS_GA_CMD_INIT_WINDOW : BS_GA_CMD_TERM_WINDOW;
    } else if (window && (glue->observedWidth != w || glue->observedHeight != h)) {
        command = BS_GA_CMD_WINDOW_RESIZED;
    }
    if (command == BS_GA_CMD_TERM_WINDOW && !glue->observedWindow)
        command = -1;
    if (command != -1) {
        BsGameActivityQueuedCommand msg = {
            .command = (uint8_t)command, .window = window, .width = w, .height = h
        };
        int rc = ga_enqueue(glue, &msg);
        if (rc) return rc;
        ++emitted;
    }
    glue->observedWindow = window;
    glue->observedWidth = w;
    glue->observedHeight = h;
    return emitted;
}

static int ga_switch_focus_state(void) {
#ifndef BS_GA_HOST_TEST
    return (int)appletGetFocusState();
#else
    int focus = bs_ga_test_native_focus();
    /* Test fixture's 0 means OutOfFocus, retaining previous behavior. */
    return focus == 0 ? 2 : focus;
#endif
}

int bs_gameactivity_glue_observe_switch_focus(BsGameActivityGlue *glue) {
    if (!glue || !glue->nativePrepared ||
        bs_looper_for_thread() != glue->app.looper) return -EINVAL;
    int focus_state = ga_switch_focus_state();
    if (focus_state < 1 || focus_state > 3) return -ENODATA;
    int focus = focus_state == 1;
    if (glue->lastFocus == focus) return 0;
    if (glue->lastFocus < 0 && !focus) {
        glue->lastFocus = 0; /* No fabricated first LOST_FOCUS event. */
        return 0;
    }
    BsGameActivityQueuedCommand msg = {
        .command = focus ? BS_GA_CMD_GAINED_FOCUS : BS_GA_CMD_LOST_FOCUS
    };
    int rc = ga_enqueue(glue, &msg);
    if (rc) return rc;
    /* Store observation separately; process_cmd will update callback state. */
    glue->lastFocus = focus;
    return 1;
}

/* Observed Horizon applet states, not the Android Java Activity's lifecycle.
 * The projected state tracks commands already written to the pipe, so
 * repeated appletMainLoop observations cannot enqueue duplicates before
 * the owner ALooper processes those bytes. */
enum {
    GA_PHASE_UNOBSERVED = 0,
    GA_PHASE_STARTED = 1,
    GA_PHASE_RESUMED = 2,
    GA_PHASE_PAUSED = 3,
    GA_PHASE_STOPPED = 4,
    GA_PHASE_DESTROYED = 5
};

static int ga_lifecycle_enqueue(BsGameActivityGlue *glue, int command,
                                 int new_phase) {
    BsGameActivityQueuedCommand event = {
        .command = (uint8_t)command
    };
    int rc = ga_enqueue(glue, &event);
    if (rc) return rc;
    glue->observedLifecyclePhase = new_phase;
    return 0;
}

int bs_gameactivity_glue_observe_switch_lifecycle(
    BsGameActivityGlue *glue, bool applet_running) {
    if (!glue || !glue->nativePrepared ||
        bs_looper_for_thread() != glue->app.looper) return -EINVAL;

    int phase = glue->observedLifecyclePhase;
    if (phase == GA_PHASE_DESTROYED) return applet_running ? -EPIPE : 0;
    int focus = 0;
    if (applet_running) {
        focus = ga_switch_focus_state();
        if (focus < 1 || focus > 3) return -ENODATA;
    }
    int emitted = 0;
    int rc;

#define GA_QUEUE(what, next_phase) do { \
    rc = ga_lifecycle_enqueue(glue, what, next_phase); \
    if (rc) return rc; \
    ++emitted; \
    phase = next_phase; \
} while (0)

    if (!applet_running) {
        if (phase == GA_PHASE_RESUMED)
            GA_QUEUE(BS_GA_CMD_PAUSE, GA_PHASE_PAUSED);
        if (phase == GA_PHASE_STARTED || phase == GA_PHASE_PAUSED)
            GA_QUEUE(BS_GA_CMD_STOP, GA_PHASE_STOPPED);
        /* Horizon has observed the appletMainLoop termination condition;
         * no Java GameActivity lifecycle event is invented from this. */
        GA_QUEUE(BS_GA_CMD_DESTROY, GA_PHASE_DESTROYED);
        return emitted;
    }

    if (phase == GA_PHASE_UNOBSERVED ||
        (phase == GA_PHASE_STOPPED && focus != 3))
        GA_QUEUE(BS_GA_CMD_START, GA_PHASE_STARTED);

    if (focus == 1) {
        if (phase == GA_PHASE_STARTED || phase == GA_PHASE_PAUSED)
            GA_QUEUE(BS_GA_CMD_RESUME, GA_PHASE_RESUMED);
    } else {
        if (phase == GA_PHASE_RESUMED)
            GA_QUEUE(BS_GA_CMD_PAUSE, GA_PHASE_PAUSED);
        /* Horizon Background means the applet is outside the foreground;
         * OutOfFocus from a library applet suspends but need not STOP. */
        if (focus == 3 &&
            (phase == GA_PHASE_STARTED || phase == GA_PHASE_PAUSED))
            GA_QUEUE(BS_GA_CMD_STOP, GA_PHASE_STOPPED);
    }
#undef GA_QUEUE
    return emitted;
}

int bs_gameactivity_glue_bind_configuration(
    BsGameActivityGlue *glue, void *actual_config, void *actual_asset_manager) {
    if (!glue || !actual_config || !actual_asset_manager) return -EINVAL;
    /* Native AConfiguration / AAssetManager pointers come from the genuine
     * NDK adapter. The event carries the exact pointers through its queue;
     * only the owner applies them, so unsuccessful enqueue is side-effect
     * free. A GameActivity Java object is still absent. */
    BsGameActivityQueuedCommand msg = {
        .command = BS_GA_CMD_CONFIG_CHANGED,
        .configuration = actual_config,
        .assetManager = actual_asset_manager
    };
    return ga_enqueue(glue, &msg);
}

int bs_gameactivity_glue_sample_real_input(BsGaNativeInputSnapshot *out) {
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
#ifndef BS_GA_HOST_TEST
    HidTouchScreenState touch;
    bool have_touch = hidGetTouchScreenStates(&touch, 1) == 1;
    if (have_touch) {
        out->touchSampleSequence = touch.sampling_number;
        int count = touch.count;
        if (count < 0) count = 0;
        if (count > 16) count = 16;
        out->touchCount = count;
        for (int i = 0; i < count; ++i) {
            out->touches[i].fingerId = touch.touches[i].finger_id;
            out->touches[i].x = touch.touches[i].x;
            out->touches[i].y = touch.touches[i].y;
            out->touches[i].diameterX = touch.touches[i].diameter_x;
            out->touches[i].diameterY = touch.touches[i].diameter_y;
        }
    }
    HidNpadIdType id = HidNpadIdType_Handheld;
    u32 styles = hidGetNpadStyleSet(id);
    if (!(styles & HidNpadStyleTag_NpadHandheld)) {
        id = HidNpadIdType_No1;
        styles = hidGetNpadStyleSet(id);
    }
    HidNpadCommonState pad;
    size_t n = 0;
    if (styles & HidNpadStyleTag_NpadHandheld)
        n = hidGetNpadStatesHandheld(id, &pad, 1);
    else if (styles & HidNpadStyleTag_NpadFullKey)
        n = hidGetNpadStatesFullKey(id, &pad, 1);
    else if (styles & HidNpadStyleTag_NpadJoyDual)
        n = hidGetNpadStatesJoyDual(id, &pad, 1);
    else if (styles & HidNpadStyleTag_NpadJoyLeft)
        n = hidGetNpadStatesJoyLeft(id, &pad, 1);
    else if (styles & HidNpadStyleTag_NpadJoyRight)
        n = hidGetNpadStatesJoyRight(id, &pad, 1);
    if (n == 1 && (pad.attributes & HidNpadAttribute_IsConnected)) {
        out->controllerSampleSequence = pad.sampling_number;
        out->controllerButtons = pad.buttons;
        out->controllerAvailable = 1;
    }
    return have_touch || out->controllerAvailable ? 0 : -ENODEV;
#else
    return bs_ga_test_native_input(out);
#endif
}

#ifdef BS_GA_HOST_TEST
int bs_gameactivity_glue_test_enqueue(BsGameActivityGlue *glue, int cmd,
                                      void *window, uint32_t width, uint32_t height) {
    if (cmd < BS_GA_CMD_INPUT_CHANGED || cmd > BS_GA_CMD_MOTION_EVENTS)
        return -EINVAL;
    BsGameActivityQueuedCommand msg = {
        .command = (uint8_t)cmd, .window = window,
        .width = width, .height = height
    };
    return ga_enqueue(glue, &msg);
}
#endif
