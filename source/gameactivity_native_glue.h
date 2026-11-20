/* Original Quest GameActivity native_app_glue layout and isolated native
 * transport. This module does not construct a Java GameActivity or NativeCode.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_GAMEACTIVITY_NATIVE_GLUE_H
#define BS_GAMEACTIVITY_NATIVE_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct BsGameActivityApp BsGameActivityApp;
typedef struct BsGameActivityPollSource BsGameActivityPollSource;
typedef struct BsGameActivityGlue BsGameActivityGlue;

/* Original Quest libgame.so emits exactly these command bytes. */
enum BsGameActivityCommand {
    BS_GA_CMD_INPUT_CHANGED = 0,
    BS_GA_CMD_INIT_WINDOW = 1,
    BS_GA_CMD_TERM_WINDOW = 2,
    BS_GA_CMD_WINDOW_RESIZED = 3,
    BS_GA_CMD_WINDOW_REDRAW_NEEDED = 4,
    BS_GA_CMD_CONTENT_RECT_CHANGED = 5,
    BS_GA_CMD_SOFTWARE_KB_VIS_CHANGED = 6,
    BS_GA_CMD_GAINED_FOCUS = 7,
    BS_GA_CMD_LOST_FOCUS = 8,
    BS_GA_CMD_CONFIG_CHANGED = 9,
    BS_GA_CMD_LOW_MEMORY = 10,
    BS_GA_CMD_START = 11,
    BS_GA_CMD_RESUME = 12,
    BS_GA_CMD_SAVE_STATE = 13,
    BS_GA_CMD_PAUSE = 14,
    BS_GA_CMD_STOP = 15,
    BS_GA_CMD_DESTROY = 16,
    BS_GA_CMD_WINDOW_INSETS_CHANGED = 17,
    BS_GA_CMD_TEXT_INPUT_STATE_CHANGED = 18,
    BS_GA_CMD_KEY_EVENTS = 19,
    BS_GA_CMD_MOTION_EVENTS = 20
};

enum { BS_GA_LOOPER_ID_MAIN = 1, BS_GA_EVENT_INPUT = 1 };

typedef struct BsGameActivityRect {
    int32_t left, top, right, bottom;
} BsGameActivityRect;

/* Android's GameActivityInputBuffer holds two pointer/count/capacity triples.
 * Actual Quest GameActivityMotionEvent is 1760 bytes; never construct one
 * from untyped Switch input or fabricate Java event pointers. */
typedef struct BsGameActivityInputBuffer {
    void *motionEvents;
    size_t motionEventsCount;
    size_t motionEventsCapacity;
    void *keyEvents;
    size_t keyEventsCount;
    size_t keyEventsCapacity;
} BsGameActivityInputBuffer;

struct BsGameActivityPollSource {
    int32_t id;
    int32_t reserved;
    BsGameActivityApp *app;
    void (*process)(BsGameActivityApp *app, BsGameActivityPollSource *source);
};

/* Precisely sized to the 384-byte android_app allocated by original
 * libgame.so GameActivity_onCreate@0x20708. Offsets are checked below.
 * Unknown fields are intentionally opaque, including the original Bionic
 * mutex (40 B), condvar (48 B), and two words at 80..87. This struct is
 * NOT a live Java GameActivity; its native-only readiness remains false.
 */
struct BsGameActivityApp {
    void *userData;                                    /* +0 */
    void (*onAppCmd)(BsGameActivityApp *, int32_t);     /* +8 */
    void *activity;                                    /* +16 */
    void *config;                                      /* +24 */
    void *savedState;                                  /* +32 */
    size_t savedStateSize;                             /* +40 */
    void *looper;                                      /* +48 */
    void *window;                                      /* +56 */
    BsGameActivityRect contentRect;                    /* +64 */
    uint8_t softwareKeyboardVisible;                   /* +80 */
    uint8_t opaque_81[3];                              /* +81 */
    int32_t textInputState;                            /* +84 */
    int32_t activityState;                             /* +88 */
    int32_t destroyRequested;                          /* +92 */
    BsGameActivityInputBuffer inputBuffers[2];         /* +96 */
    int32_t currentInputBuffer;                        /* +192 */
    uint8_t opaque_196[4];                             /* +196 */
    uint8_t bionicMutex[40];                           /* +200 */
    uint8_t bionicCond[48];                            /* +240 */
    int32_t msgread;                                   /* +288 */
    int32_t msgwrite;                                  /* +292 */
    uintptr_t thread;                                  /* +296 */
    BsGameActivityPollSource cmdPollSource;            /* +304 */
    int32_t running;                                   /* +328 */
    int32_t stateSaved;                                /* +332 */
    int32_t destroyed;                                 /* +336 */
    int32_t redrawNeeded;                              /* +340 */
    void *pendingWindow;                               /* +344 */
    BsGameActivityRect pendingContentRect;             /* +352 */
    void *keyEventFilter;                              /* +368 */
    void *motionEventFilter;                           /* +376 */
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && UINTPTR_MAX == UINT64_MAX
#define BS_GA_ASSERT_FIELD(field, pos) \
    _Static_assert(offsetof(BsGameActivityApp, field) == (pos), \
                   "Original Quest android_app ABI: " #field)
_Static_assert(sizeof(BsGameActivityApp) == 384, "Original Quest android_app: 384 bytes");
_Static_assert(sizeof(BsGameActivityPollSource) == 24, "Original Quest poll source: 24 bytes");
_Static_assert(sizeof(BsGameActivityInputBuffer) == 48, "Original Quest input buffer: 48 bytes");
BS_GA_ASSERT_FIELD(onAppCmd, 8);
BS_GA_ASSERT_FIELD(activity, 16);
BS_GA_ASSERT_FIELD(config, 24);
BS_GA_ASSERT_FIELD(savedState, 32);
BS_GA_ASSERT_FIELD(savedStateSize, 40);
BS_GA_ASSERT_FIELD(looper, 48);
BS_GA_ASSERT_FIELD(window, 56);
BS_GA_ASSERT_FIELD(contentRect, 64);
BS_GA_ASSERT_FIELD(softwareKeyboardVisible, 80);
BS_GA_ASSERT_FIELD(textInputState, 84);
BS_GA_ASSERT_FIELD(activityState, 88);
BS_GA_ASSERT_FIELD(destroyRequested, 92);
BS_GA_ASSERT_FIELD(inputBuffers, 96);
BS_GA_ASSERT_FIELD(currentInputBuffer, 192);
BS_GA_ASSERT_FIELD(bionicMutex, 200);
BS_GA_ASSERT_FIELD(bionicCond, 240);
BS_GA_ASSERT_FIELD(msgread, 288);
BS_GA_ASSERT_FIELD(msgwrite, 292);
BS_GA_ASSERT_FIELD(cmdPollSource, 304);
BS_GA_ASSERT_FIELD(running, 328);
BS_GA_ASSERT_FIELD(stateSaved, 332);
BS_GA_ASSERT_FIELD(destroyed, 336);
BS_GA_ASSERT_FIELD(pendingWindow, 344);
BS_GA_ASSERT_FIELD(pendingContentRect, 352);
BS_GA_ASSERT_FIELD(keyEventFilter, 368);
BS_GA_ASSERT_FIELD(motionEventFilter, 376);
#undef BS_GA_ASSERT_FIELD
#endif

enum { BS_GA_COMMAND_QUEUE_CAPACITY = 32 };
typedef struct BsGameActivityQueuedCommand {
    uint8_t command;
    void *window; /* only live libnx NWindow when INIT_WINDOW */
    uint32_t width, height; /* observed display dimensions */
    /* Original AConfiguration / AAssetManager objects for this specific
     * CONFIG_CHANGED command. The producer owns both through dispatch. */
    void *configuration, *assetManager;
} BsGameActivityQueuedCommand;

struct BsGameActivityGlue {
    BsGameActivityApp app; /* MUST remain first for original app-pointer ABI */
    BsGameActivityQueuedCommand commands[BS_GA_COMMAND_QUEUE_CAPACITY];
    unsigned pendingHead, pendingCount;
    void *observedWindow;
    uint32_t observedWidth, observedHeight;
    int lastFocus;                 /* -1 unknown, 0 out of focus, 1 focused */
    int nativePrepared;
    int ownsLooper;
    int commandDispatchError;
    int destroyQueued;             /* no producer posts after CMD_DESTROY */
    int stage;                     /* passive lifecycle telemetry only */
    int observedLifecyclePhase;    /* queued OS transitions, separate from guest app */
    void *realAssetManager;        /* NULL until explicit genuine asset binding */
    /* Native-only shutdown gate. High bit seals future producer admissions;
     * lower bits count producers still using Bionic mutex/pipe/state. This
     * lives OUTSIDE the original 384-byte android_app ABI. */
    atomic_uint producerGate;
};

/* Prepares real virtual NDK ALooper command pipe and 24-byte poll source on
 * the owning thread. Success means native transport only, NEVER GameActivity
 * or Unity startup. Destroy it on that same thread after poll dispatch stops.
 * close_native seals cross-thread producers, then drains in-flight operations
 * BEFORE it closes the pipe and destroys Bionic locks. Callers must ensure
 * the glue's storage remains allocated until close_native returns; do not
 * reinitialize or concurrently call prepare_native on that same storage.
 */
int bs_gameactivity_glue_prepare_native(BsGameActivityGlue *glue);
int bs_gameactivity_glue_close_native(BsGameActivityGlue *glue);
BsGameActivityApp *bs_gameactivity_glue_app(BsGameActivityGlue *glue);
bool bs_gameactivity_glue_java_activity_ready(const BsGameActivityGlue *glue);

/* Observations read actual Horizon state. observe_switch_window uses a
 * valid libnx NWindow and measured dimensions; observe_switch_focus reads
 * the applet focus. Both emit NDK APP_CMD_ bytes through the real pipe.
 * Caller must have processed appletMainLoop() before observing focus.
 */
int bs_gameactivity_glue_observe_switch_window(BsGameActivityGlue *glue);
int bs_gameactivity_glue_observe_switch_focus(BsGameActivityGlue *glue);
/* Call on the owner thread with the actual bool returned by appletMainLoop.
 * This observes Horizon focus and queues native START/RESUME/PAUSE/STOP and
 * irreversible DESTROY only on appletMainLoop(false). Returns number of
 * commands queued or a negative error. This is native homebrew telemetry;
 * it DOES NOT assert any Android Java Activity lifecycle or readiness.
 * Only the owner polls ALooper and dispatches the queued event source.
 */
int bs_gameactivity_glue_observe_switch_lifecycle(
    BsGameActivityGlue *glue, bool applet_running);

/* Configuration is accepted only from existing AConfiguration and
 * AAssetManager objects supplied by their real owners, never synthetic
 * Java objects. The producer retains both until the owner dispatches the
 * queued command; unsuccessful enqueue leaves app.config unchanged.
 * No Java Activity readiness is inferred.
 */
int bs_gameactivity_glue_bind_configuration(
    BsGameActivityGlue *glue, void *actual_config, void *actual_asset_manager);

/* A native command source returned from ALooper_pollOnce calls this with
 * (app, source). The native command callback executes pre/callback/post.
 * Calling onAppCmd from guest code additionally requires attached guest TLS.
 */
void bs_gameactivity_glue_process_cmd(BsGameActivityApp *app,
                                      BsGameActivityPollSource *source);
int bs_gameactivity_glue_last_error(const BsGameActivityGlue *glue);

/* The native looper owner may copy concrete callback-provided saved-state
 * bytes before DESTROY executes. Foreign Java/NDK threads must enqueue
 * SAVE_STATE and await native completion; replacing the savedState buffer
 * while the native callback holds it would be a use-after-free. The copy is
 * not delivered to Java until a real GameActivity host exists.
 */
int bs_gameactivity_glue_copy_saved_state(BsGameActivityGlue *glue,
                                          const void *bytes, size_t size);
int bs_gameactivity_glue_clear_saved_state(BsGameActivityGlue *glue);

/* Measured native HID input for later GameActivity event conversion. The
 * original GameActivityMotionEvent / Java input buffers are NOT populated:
 * those require the original 1760-byte event ABI and Java callback host.
 */
typedef struct BsGaNativeTouch {
    uint32_t fingerId, x, y, diameterX, diameterY;
} BsGaNativeTouch;
typedef struct BsGaNativeInputSnapshot {
    uint64_t touchSampleSequence;
    int touchCount;
    BsGaNativeTouch touches[16];
    uint64_t controllerSampleSequence;
    uint64_t controllerButtons;
    int controllerAvailable;
} BsGaNativeInputSnapshot;
int bs_gameactivity_glue_sample_real_input(BsGaNativeInputSnapshot *out);

#ifdef BS_GA_HOST_TEST
/* Isolated tests exercise the ORIGINAL command protocol without creating a
 * Java activity. The test-only command injection is never built into NRO. */
int bs_gameactivity_glue_test_enqueue(BsGameActivityGlue *glue, int cmd,
                                      void *window, uint32_t width, uint32_t height);
#endif
#endif
