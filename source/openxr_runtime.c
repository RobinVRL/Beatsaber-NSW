/* Minimal in-process OpenXR runtime for Switch: GLES swapchains composited to
 * the TV/handheld screen, a seated head and two hands driven by Switch
 * controllers (buttons/sticks from any pad, orientation from Joy-Con motion).
 * SPDX-License-Identifier: MIT
 *
 * The original Khronos Android loader (libopenxr_loader.so) cannot find a
 * runtime on Horizon. Every loader export is redirected here with a branch
 * hook, so libUnityOpenXR, OVRPlugin and dlsym callers all reach this
 * runtime. Unity then renders its normal XR stereo path and we present the
 * left eye, giving the game real XR head/hand poses and controller actions.
 */
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_GRAPHICS_API_OPENGL_ES
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "egl_bridge.h"
#include "joycon_bridge.h"
#include "joycon_tracker.h"
#include "openxr_runtime.h"
#include "il2cpp_probe.h"
#include "so_util.h"
#include "util.h"

/* IL2CPP object probes are debug-only (they fault on hardware). */
static int il2cpp_probe_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        FILE *f = fopen("sdmc:/switch/beat-saber-1408/il2cpp_probe", "r");
        enabled = f != NULL;
        if (f) fclose(f);
    }
    return enabled;
}

#define XRLOG(...) debugPrintf("[xr:rt] " __VA_ARGS__)

/* Not under XR_USE_PLATFORM_ANDROID here (avoids jni.h); same layout. */
typedef struct {
    XrStructureType type;
    const void *next;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
} BsXrGraphicsBindingGLES;

/* ---- configuration --------------------------------------------------- */
/* Unity scales the recommended size by eyeTextureResolutionScale (1.2), so
 * report target/1.2: 1067x600 -> 1280x720, 1600x900 -> 1920x1080. */
int bs_render_1080p(void);
#define EYE_W (bs_render_1080p() ? 1600 : 1067)
#define EYE_H (bs_render_1080p() ? 900 : 600)
#define TAN_HALF_V 0.70020754f            /* 35 degrees */
#define TAN_HALF_H (TAN_HALF_V * 16.0f / 9.0f)
#define HEAD_HEIGHT 1.70f
#define FRAME_NS 16666667LL

/* ---- small math ------------------------------------------------------ */
typedef struct { float x, y, z, w; } Quat;
typedef struct { float x, y, z; } Vec3;
typedef struct { Quat q; Vec3 p; } Pose;

static Quat qmul(Quat a, Quat b) {
    return (Quat){a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                  a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                  a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                  a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
static Quat qconj(Quat a) { return (Quat){-a.x, -a.y, -a.z, a.w}; }
static Quat qnorm(Quat a) {
    float n = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z + a.w*a.w);
    if (n < 1e-6f) return (Quat){0, 0, 0, 1};
    return (Quat){a.x/n, a.y/n, a.z/n, a.w/n};
}
static Vec3 qrot(Quat q, Vec3 v) {
    Quat r = qmul(qmul(q, (Quat){v.x, v.y, v.z, 0}), qconj(q));
    return (Vec3){r.x, r.y, r.z};
}
static Vec3 vadd(Vec3 a, Vec3 b) { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static Pose pmul(Pose a, Pose b) {      /* a * b: b expressed in a's frame */
    return (Pose){qmul(a.q, b.q), vadd(a.p, qrot(a.q, b.p))};
}
static Pose pinv(Pose a) {
    Quat c = qconj(a.q);
    Vec3 p = qrot(c, a.p);
    return (Pose){c, (Vec3){-p.x, -p.y, -p.z}};
}
static Pose from_xr(XrPosef p) {
    return (Pose){qnorm((Quat){p.orientation.x, p.orientation.y, p.orientation.z,
                               p.orientation.w}),
                  (Vec3){p.position.x, p.position.y, p.position.z}};
}
static XrPosef to_xr(Pose p) {
    XrPosef r;
    r.orientation = (XrQuaternionf){p.q.x, p.q.y, p.q.z, p.q.w};
    r.position = (XrVector3f){p.p.x, p.p.y, p.p.z};
    return r;
}
static Quat yaw_pitch(float yaw, float pitch) {   /* radians, yaw about +Y */
    Quat qy = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    Quat qp = {sinf(pitch * 0.5f), 0, 0, cosf(pitch * 0.5f)};
    return qmul(qy, qp);
}

static XrTime now_ns(void) { return (XrTime)armTicksToNs(armGetSystemTick()); }

/* ---- object model ---------------------------------------------------- */
#define MAX_PATHS 2048
#define MAX_ACTIONS 256
#define MAX_BINDINGS 1024
#define MAX_SPACES 128
#define MAX_SWAPCHAINS 32
#define MAX_SUBACTIONS 4
#define MAX_EVENTS 16

enum { SIDE_NONE = 0, SIDE_LEFT = 1, SIDE_RIGHT = 2 };

typedef struct XrInstance_T { int magic; } BsInstance;
typedef struct XrSession_T { int magic; } BsSession;
typedef struct XrActionSet_T { char name[XR_MAX_ACTION_SET_NAME_SIZE]; } BsActionSet;
typedef struct XrAction_T {
    BsActionSet *set;
    XrActionType type;
    char name[XR_MAX_ACTION_NAME_SIZE];
    XrPath sub[MAX_SUBACTIONS];
    uint32_t sub_count;
    float prev[3][2];                /* per side: last value(s) */
    XrTime changed[3];
    int used;
} BsAction;
typedef struct XrSpace_T {
    int kind;                         /* 1 = reference, 2 = action */
    XrReferenceSpaceType ref;
    BsAction *action;
    XrPath sub;
    Pose offset;
    int used;
} BsSpace;
typedef struct XrSwapchain_T {
    GLuint tex[3];
    uint32_t count, w, h, array, next, last;
    int64_t format;
    int is_array, used;
} BsSwapchain;

static BsInstance g_instance = {0x58524931};
static BsSession g_session = {0x58525331};
static int g_instance_live, g_session_live, g_session_running;
static BsXrGraphicsBindingGLES g_binding;

static char *g_paths[MAX_PATHS];
static uint32_t g_path_count;
static BsAction g_actions[MAX_ACTIONS];
static BsActionSet g_sets[64];
static uint32_t g_set_count;
static struct { BsAction *action; XrPath path, profile; } g_bindings[MAX_BINDINGS];
static uint32_t g_binding_count;
static XrPath g_profile;              /* chosen interaction profile */
static BsSpace g_spaces[MAX_SPACES];
static BsSwapchain g_swapchains[MAX_SWAPCHAINS];
static Mutex g_lock;

static XrEventDataBuffer g_events[MAX_EVENTS];
static uint32_t g_ev_head, g_ev_tail;

static Mutex g_frame_lock;
static CondVar g_frame_cv;
static uint64_t g_frames_waited, g_frames_ended;
static XrTime g_last_display_time;

/* Head: seated, angles from the loader's L+D-pad control. */
static volatile int g_head_yaw_deg, g_head_pitch_deg;

/* Hand aim state. */
typedef struct {
    float yaw, pitch;                 /* stick-aim fallback, radians */
    int calibrated;
    Quat ref;                         /* Joy-Con orientation at recenter */
    Quat motion;                      /* relative motion in XR frame */
    Quat base;                        /* aim the recentered pose maps to */
    Quat heading;                     /* removes the recenter-time heading */
    Quat prev_raw;                    /* last raw HID orientation */
    int has_prev;
    int has_motion;
    BsJcTracker tracker;              /* estimated 6DoF (arm + inertial) */
    uint64_t last_motion_seq;
    XrTime last_motion_t;
} BsHand;
/* Horizon's fused direction matrix convention (body->world or its
 * transpose) is detected at runtime: frame-to-frame orientation deltas must
 * rotate about the gyro's measured body-frame angular velocity. 0 = still
 * undecided (body->world assumed), 1 = body->world, -1 = world->body. */
static int g_motion_conv;
static float g_motion_conv_score;
static BsHand g_hands[2];
static PadState g_pad;
static int g_pad_ready;
static u64 g_buttons;
static HidAnalogStickState g_sticks[2];
static XrTime g_last_sync;
static unsigned long g_n_locate_hand, g_n_bool, g_n_float, g_n_vec2, g_n_pose, g_n_sync;

/* ---- paths ----------------------------------------------------------- */
static XrPath path_intern(const char *s) {
    for (uint32_t i = 0; i < g_path_count; ++i)
        if (!strcmp(g_paths[i], s)) return i + 1;
    if (g_path_count >= MAX_PATHS) return XR_NULL_PATH;
    g_paths[g_path_count] = strdup(s);
    return ++g_path_count;
}
static const char *path_str(XrPath p) {
    return p && p <= g_path_count ? g_paths[p - 1] : "";
}
static int path_side(XrPath p) {
    const char *s = path_str(p);
    if (!strncmp(s, "/user/hand/left", 15)) return SIDE_LEFT;
    if (!strncmp(s, "/user/hand/right", 16)) return SIDE_RIGHT;
    return SIDE_NONE;
}

static void push_event(const void *ev, size_t size) {
    if (g_ev_tail - g_ev_head >= MAX_EVENTS) return;
    XrEventDataBuffer *b = &g_events[g_ev_tail++ % MAX_EVENTS];
    memset(b, 0, sizeof(*b));
    memcpy(b, ev, size);
}
static void push_state(XrSessionState s) {
    XrEventDataSessionStateChanged e = {.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    e.session = (XrSession)&g_session;
    e.state = s;
    e.time = now_ns();
    push_event(&e, sizeof(e));
    XRLOG("session state -> %d\n", (int)s);
}

static XrResult two_call(uint32_t cap, uint32_t *out, uint32_t n) {
    if (out) *out = n;
    if (cap && cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    return XR_SUCCESS;
}

/* ---- input ----------------------------------------------------------- */
void bs_openxr_set_head_angles(int yaw_degrees, int pitch_degrees) {
    g_head_yaw_deg = yaw_degrees;
    g_head_pitch_deg = pitch_degrees;
}

/* Real trackers never report two identical poses. The Input System only
 * starts a Value action (activeControl) on a control state *change*; a
 * perfectly static pose leaves every pose action at its default (0,0,0) /
 * (0,0,0,0) forever. Alternate an invisible 10 micron / 10 microradian
 * offset every synced frame. */
static float tracking_jitter(void) {
    return (g_n_sync & 1) ? 1e-5f : -1e-5f;
}

static Pose head_pose_world(void) {
    const float d2r = 0.01745329252f;
    float j = tracking_jitter();
    return (Pose){yaw_pitch(-g_head_yaw_deg * d2r + j, g_head_pitch_deg * d2r),
                  (Vec3){j, HEAD_HEIGHT, 0}};
}

/* Nintendo sensor frame -> OpenXR frame: x->x, y->-z, z->y. */
static Quat sensor_to_xr(const float q[4]) {
    return qnorm((Quat){q[0], q[2], -q[1], q[3]});
}

/* Scripted input for unattended tests: sdmc:/switch/beat-saber-1408/
 * autoinput.txt, one step per line: "<start_s> <end_s> TOKEN...", times
 * from the first input poll. Tokens: button names (ZR ZL R L A B X Y PLUS
 * MINUS UP DOWN LEFT RIGHT) and "laim=yaw,pitch" / "raim=yaw,pitch" (rad). */
#define MAX_AUTO 64
typedef struct {
    float t0, t1; u64 buttons; int aim[2]; float yaw[2], pitch[2];
    int head; int head_yaw, head_pitch; /* "head=yaw,pitch" in degrees */
} BsAutoStep;
static BsAutoStep g_auto[MAX_AUTO];
static int g_auto_count = -1;
static XrTime g_auto_epoch;
static int g_auto_aim[2];
static void auto_load(void) {
    g_auto_count = 0;
    FILE *f = fopen("sdmc:/switch/beat-saber-1408/autoinput.txt", "r");
    if (!f) return;
    static const struct { const char *n; u64 b; } names[] = {
        {"ZR", HidNpadButton_ZR}, {"ZL", HidNpadButton_ZL}, {"R", HidNpadButton_R},
        {"L", HidNpadButton_L}, {"A", HidNpadButton_A}, {"B", HidNpadButton_B},
        {"X", HidNpadButton_X}, {"Y", HidNpadButton_Y}, {"PLUS", HidNpadButton_Plus},
        {"MINUS", HidNpadButton_Minus}, {"UP", HidNpadButton_Up},
        {"DOWN", HidNpadButton_Down}, {"LEFT", HidNpadButton_Left},
        {"RIGHT", HidNpadButton_Right}};
    char line[256];
    while (g_auto_count < MAX_AUTO && fgets(line, sizeof line, f)) {
        BsAutoStep st = {0};
        char *save = NULL, *tok = strtok_r(line, " \t\r\n", &save);
        if (!tok || *tok == '#') continue;
        st.t0 = strtof(tok, NULL);
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (!tok) continue;
        st.t1 = strtof(tok, NULL);
        while ((tok = strtok_r(NULL, " \t\r\n", &save))) {
            if ((tok[0] == 'l' || tok[0] == 'r') && !strncmp(tok + 1, "aim=", 4)) {
                int h = tok[0] == 'r';
                st.aim[h] = sscanf(tok + 5, "%f,%f", &st.yaw[h], &st.pitch[h]) == 2;
                continue;
            }
            if (!strncmp(tok, "head=", 5)) {
                st.head = sscanf(tok + 5, "%d,%d", &st.head_yaw, &st.head_pitch) == 2;
                continue;
            }
            for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
                if (!strcmp(tok, names[i].n)) st.buttons |= names[i].b;
        }
        g_auto[g_auto_count++] = st;
    }
    fclose(f);
    XRLOG("autoinput: %d step(s)\n", g_auto_count);
}
static void auto_apply(XrTime t) {
    if (g_auto_count < 0) {
        /* Default stick aim: both rays converge 2.2 m ahead at eye height,
         * where Beat Saber places its menus. */
        g_hands[0].yaw = -0.05f; g_hands[1].yaw = 0.05f;
        g_hands[0].pitch = g_hands[1].pitch = 0.44f;
        auto_load();
        g_auto_epoch = t;
    }
    float s = (float)(t - g_auto_epoch) * 1e-9f;
    g_auto_aim[0] = g_auto_aim[1] = 0;
    for (int i = 0; i < g_auto_count; ++i) {
        BsAutoStep *st = &g_auto[i];
        if (s < st->t0 || s >= st->t1) continue;
        if (st->t0 >= 0) {
            XRLOG("autoinput step %d active at %.1fs buttons=%#llx\n", i, s,
                  (unsigned long long)st->buttons);
            st->t0 = -st->t0 - 1e-3f; /* log once; keep window via t1 */
        }
        g_buttons |= st->buttons;
        if (st->head) bs_openxr_set_head_angles(st->head_yaw, st->head_pitch);
        for (int h = 0; h < 2; ++h)
            if (st->aim[h]) {
                g_auto_aim[h] = 1; /* scripted aim overrides gyro */
                g_hands[h].yaw = st->yaw[h];
                g_hands[h].pitch = st->pitch[h];
            }
    }
}

/* Returns 1 when the convention was just decided (callers recenter). */
static int motion_detect_convention(Quat prev, Quat cur, const float w[3]) {
    float wm = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (!(wm > 0.2f)) return 0;                      /* too still to tell */
    Quat a = qmul(qconj(prev), cur);                 /* body delta if body->world */
    Quat b = qmul(prev, qconj(cur));                 /* body delta if world->body */
    if (a.w < 0) a = (Quat){-a.x, -a.y, -a.z, -a.w};
    if (b.w < 0) b = (Quat){-b.x, -b.y, -b.z, -b.w};
    float am = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    float bm = sqrtf(b.x * b.x + b.y * b.y + b.z * b.z);
    if (am < 1e-4f || bm < 1e-4f) return 0;          /* same HID sample */
    float ca = (a.x * w[0] + a.y * w[1] + a.z * w[2]) / (am * wm);
    float cb = (b.x * w[0] + b.y * w[1] + b.z * w[2]) / (bm * wm);
    g_motion_conv_score += ca - cb;
    if (g_motion_conv_score > 6.0f || g_motion_conv_score < -6.0f) {
        g_motion_conv = g_motion_conv_score > 0 ? 1 : -1;
        XRLOG("Joy-Con orientation convention: %s\n",
              g_motion_conv > 0 ? "body->world" : "world->body");
        return 1;
    }
    return 0;
}

/* Recenter: the current Joy-Con pose becomes the default aim (pointing at
 * the menus). The heading of its long axis is removed, keeping yaw about
 * gravity independent of how the controller is held. */
static void motion_recenter(BsHand *hand, int h, Quat cur) {
    hand->ref = cur;
    Vec3 fwd = qrot(cur, (Vec3){0, 0, -1});          /* Joy-Con long axis */
    if (fabsf(fwd.y) > 0.9f) fwd = qrot(cur, (Vec3){0, 1, 0});
    float heading = atan2f(-fwd.x, -fwd.z);
    hand->heading = yaw_pitch(-heading, 0);
    hand->base = yaw_pitch(h ? 0.05f : -0.05f, 0.44f);
    hand->calibrated = 1;
    XRLOG("hand %d motion recentered (heading %.0f deg)\n", h, heading * 57.29578f);
}

/* Reset combo: hold both stick clicks for 0.5 s -> recenter both hands. */
static float g_reset_hold;

static void poll_input(void) {
    XrTime t = now_ns();
    float dt = g_last_sync ? (float)(t - g_last_sync) * 1e-9f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_last_sync = t;
    if (!g_pad_ready) {
        padInitializeAny(&g_pad);
        g_pad_ready = 1;
    }
    padUpdate(&g_pad);
    g_buttons = padGetButtons(&g_pad);
    g_sticks[0] = padGetStickPos(&g_pad, 0);
    g_sticks[1] = padGetStickPos(&g_pad, 1);
    auto_apply(t);

    BsJoyconSnapshot snap;
    int have = bs_joycon_poll(&snap) == 0;
    int reset_all = 0;
    if ((g_buttons & HidNpadButton_StickL) && (g_buttons & HidNpadButton_StickR)) {
        float before = g_reset_hold;
        g_reset_hold += dt;
        if (before < 0.5f && g_reset_hold >= 0.5f) {
            reset_all = 1;
            XRLOG("reset combo: recentering both controllers\n");
        }
    } else {
        g_reset_hold = 0;
    }
    for (int h = 0; h < 2; ++h) {
        BsHand *hand = &g_hands[h];
        const BsJoyconHandSample *s = have ? &snap.hands[h] : NULL;
        int motion = s && s->connected &&
                     (s->capabilities & BS_JOYCON_CAP_RELATIVE_ORIENTATION);
        if (motion) {
            const float *o = s->orientation_xyzw;
            Quat raw = {o[0], o[1], o[2], o[3]};
            int recenter = h ? (g_buttons & HidNpadButton_StickR) &&
                               (g_buttons & HidNpadButton_R)
                             : (g_buttons & HidNpadButton_StickL) &&
                               (g_buttons & HidNpadButton_L);
            if (!hand->has_motion || reset_all) recenter = 1; /* (re)connected */
            if (g_motion_conv == 0 && hand->has_prev && motion_detect_convention(
                    hand->prev_raw, raw, s->angular_velocity_rad_s))
                recenter = 1;
            hand->prev_raw = raw;
            hand->has_prev = 1;
            Quat body_to_world = g_motion_conv < 0 ? qconj(raw) : raw;
            Quat cur = sensor_to_xr((const float[4]){body_to_world.x, body_to_world.y,
                                                     body_to_world.z, body_to_world.w});
            if (!hand->calibrated || recenter) {
                motion_recenter(hand, h, cur);
                bs_jc_tracker_reset(&hand->tracker, NULL, h);
                hand->last_motion_t = 0;
            }
            /* World-frame rotation since recenter, expressed with the
             * recenter heading removed, so turning about gravity is always
             * yaw no matter how the Joy-Con is held. */
            Quat d = qmul(cur, qconj(hand->ref));
            hand->motion = qmul(qmul(hand->heading, d), qconj(hand->heading));
            hand->has_motion = 1;
            /* Feed the estimator once per fresh HID sample so integration
             * dt follows the sensor, not the app's sync cadence. */
            if (s->motion_sampling_number != hand->last_motion_seq ||
                !hand->last_motion_t) {
                float mdt = hand->last_motion_t ?
                            (float)(t - hand->last_motion_t) * 1e-9f : 0.0f;
                hand->last_motion_t = t;
                hand->last_motion_seq = s->motion_sampling_number;
                Quat aim = qnorm(qmul(hand->motion, hand->base));
                Quat world = qnorm(qmul(hand->heading, cur));
                bs_jc_tracker_update(&hand->tracker,
                                     (const float[4]){aim.x, aim.y, aim.z, aim.w},
                                     (const float[4]){world.x, world.y, world.z, world.w},
                                     s->acceleration_g, s->angular_velocity_rad_s, mdt);
            }
        } else {
            hand->has_motion = 0;
            hand->has_prev = 0;
            /* No motion sensor: aim with the stick (pointer for menus). */
            float sx = g_sticks[h].x / 32767.0f, sy = g_sticks[h].y / 32767.0f;
            if (fabsf(sx) < 0.15f) sx = 0;
            if (fabsf(sy) < 0.15f) sy = 0;
            hand->yaw -= sx * 1.6f * dt;
            hand->pitch += sy * 1.6f * dt;
            if (hand->yaw > 1.4f) hand->yaw = 1.4f;
            if (hand->yaw < -1.4f) hand->yaw = -1.4f;
            if (hand->pitch > 1.2f) hand->pitch = 1.2f;
            if (hand->pitch < -1.2f) hand->pitch = -1.2f;
        }
    }
}

static Pose hand_pose_world(int side) {
    int h = side == SIDE_LEFT ? 0 : 1;
    Pose head = head_pose_world();
    Quat yaw_only = yaw_pitch(-g_head_yaw_deg * 0.01745329252f, 0);
    /* Seated flat-screen layout: hands low in the view (~29 deg down). */
    Vec3 off = {h ? 0.20f : -0.20f, -0.22f, -0.40f};
    float j = tracking_jitter();
    if (g_hands[h].has_motion && !g_auto_aim[h]) {
        /* Estimated 6DoF: One-Euro filtered, gyro-predicted orientation on
         * a shoulder/elbow/wrist arm model, plus accelerometer residual
         * translation (see joycon_tracker.h). */
        float tq[4], tp[3];
        bs_jc_tracker_pose(&g_hands[h].tracker, tq, tp);
        Quat q = qnorm((Quat){tq[0], tq[1], tq[2], tq[3]});
        Vec3 hand_off = {tp[0] + j, tp[1], tp[2]};
        return (Pose){qmul(yaw_only, q), vadd(head.p, qrot(yaw_only, hand_off))};
    }
    Quat q = yaw_pitch(g_hands[h].yaw + j, g_hands[h].pitch);
    return (Pose){qmul(yaw_only, q), vadd(head.p, qrot(yaw_only, off))};
}

/* Physical control value for an OpenXR input path suffix. */
static int input_value(int side, const char *suffix, float *v, float v2[2]) {
    int l = side == SIDE_LEFT;
    u64 b = g_buttons;
    *v = 0;
    if (!strcmp(suffix, "trigger/value") || !strcmp(suffix, "trigger/click") ||
        !strcmp(suffix, "select/click") || !strcmp(suffix, "trigger/touch")) {
        *v = (b & (l ? HidNpadButton_ZL : HidNpadButton_ZR)) ? 1.0f : 0.0f;
    } else if (!strncmp(suffix, "squeeze/", 8)) {
        *v = (b & (l ? HidNpadButton_L : HidNpadButton_R)) ? 1.0f : 0.0f;
    } else if (!strcmp(suffix, "a/click") || !strcmp(suffix, "x/click")) {
        *v = (b & (l ? HidNpadButton_Down : HidNpadButton_A)) ? 1.0f : 0.0f;
    } else if (!strcmp(suffix, "b/click") || !strcmp(suffix, "y/click")) {
        *v = (b & (l ? HidNpadButton_Left : HidNpadButton_B)) ? 1.0f : 0.0f;
    } else if (!strcmp(suffix, "menu/click") || !strcmp(suffix, "system/click")) {
        *v = (b & (l ? HidNpadButton_Minus : HidNpadButton_Plus)) ? 1.0f : 0.0f;
    } else if (!strcmp(suffix, "thumbstick/click")) {
        *v = (b & (l ? HidNpadButton_StickL : HidNpadButton_StickR)) ? 1.0f : 0.0f;
    } else if (!strcmp(suffix, "thumbstick") || !strcmp(suffix, "thumbstick/x") ||
               !strcmp(suffix, "thumbstick/y")) {
        float x = g_sticks[l ? 0 : 1].x / 32767.0f, y = g_sticks[l ? 0 : 1].y / 32767.0f;
        if (v2) { v2[0] = x; v2[1] = y; }
        *v = suffix[10] == '/' ? (suffix[11] == 'x' ? x : y) : sqrtf(x*x + y*y);
    } else if (!strcmp(suffix, "grip/pose") || !strcmp(suffix, "aim/pose") ||
               !strcmp(suffix, "palm_ext/pose")) {
        *v = 1.0f;
    } else if (strstr(suffix, "/touch") || strstr(suffix, "proximity") ||
               strstr(suffix, "thumbrest")) {
        *v = 0.0f;
    } else {
        return 0;
    }
    return 1;
}

/* Evaluate an action for a side filter (SIDE_NONE = any). */
static int eval_action(BsAction *a, XrPath subaction, float *value, float v2[2],
                       int *side_out) {
    int want = subaction ? path_side(subaction) : SIDE_NONE;
    int active = 0;
    *value = 0;
    if (v2) v2[0] = v2[1] = 0;
    for (uint32_t i = 0; i < g_binding_count; ++i) {
        if (g_bindings[i].action != a) continue;
        if (g_profile && g_bindings[i].profile != g_profile) continue;
        const char *p = path_str(g_bindings[i].path);
        int side = path_side(g_bindings[i].path);
        if (want && side != want) continue;
        const char *in = strstr(p, "/input/");
        if (!in) continue;
        float v = 0, vv[2] = {0, 0};
        if (!input_value(side, in + 7, &v, vv)) continue;
        if (!active || fabsf(v) > fabsf(*value)) {
            *value = v;
            if (v2) { v2[0] = vv[0]; v2[1] = vv[1]; }
            if (side_out) *side_out = side;
        }
        active = 1;
    }
    return active;
}

/* ---- instance -------------------------------------------------------- */
static const char *const g_exts[] = {
    XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    "XR_KHR_android_create_instance",
    XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
    "XR_FB_foveation", "XR_FB_foveation_configuration",
    "XR_FB_swapchain_update_state",
};
static const uint32_t g_ext_versions[] = {
    XR_KHR_opengl_es_enable_SPEC_VERSION, 3, XR_KHR_composition_layer_depth_SPEC_VERSION,
    XR_FB_foveation_SPEC_VERSION, XR_FB_foveation_configuration_SPEC_VERSION,
    XR_FB_swapchain_update_state_SPEC_VERSION,
};

static XrResult XRAPI_CALL rt_xrEnumerateApiLayerProperties(
    uint32_t cap, uint32_t *count, XrApiLayerProperties *props) {
    (void)props;
    return two_call(cap, count, 0);
}

static XrResult XRAPI_CALL rt_xrEnumerateInstanceExtensionProperties(
    const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *props) {
    XRLOG("enumerate instance extensions layer=%s cap=%u\n", layer ? layer : "-", cap);
    uint32_t n = sizeof g_exts / sizeof g_exts[0];
    XrResult r = two_call(cap, count, n);
    if (r || !cap) return r;
    for (uint32_t i = 0; i < n; ++i) {
        snprintf(props[i].extensionName, sizeof props[i].extensionName, "%s", g_exts[i]);
        props[i].extensionVersion = g_ext_versions[i];
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrCreateInstance(const XrInstanceCreateInfo *info,
                                               XrInstance *out) {
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    XRLOG("xrCreateInstance app=%s engine=%s api=%#llx exts=%u\n",
          info->applicationInfo.applicationName, info->applicationInfo.engineName,
          (unsigned long long)info->applicationInfo.apiVersion,
          info->enabledExtensionCount);
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        int ok = 0;
        for (size_t k = 0; k < sizeof g_exts / sizeof g_exts[0]; ++k)
            ok |= !strcmp(info->enabledExtensionNames[i], g_exts[k]);
        XRLOG("  ext %s%s\n", info->enabledExtensionNames[i], ok ? "" : " (UNSUPPORTED)");
        /* OVRPlugin's PreInitialize probes with a Vulkan instance it only
         * queries; accept it so its compositor initializes (GLES is used). */
        if (!ok && strcmp(info->enabledExtensionNames[i], "XR_KHR_vulkan_enable"))
            return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (XR_VERSION_MAJOR(info->applicationInfo.apiVersion) != 1)
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    mutexInit(&g_lock);
    mutexInit(&g_frame_lock);
    condvarInit(&g_frame_cv);
    g_instance_live = 1;
    *out = (XrInstance)&g_instance;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroyInstance(XrInstance inst) {
    (void)inst;
    g_instance_live = 0;
    XRLOG("xrDestroyInstance\n");
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetInstanceProperties(XrInstance inst,
                                                      XrInstanceProperties *p) {
    (void)inst;
    /* OVRPlugin refuses non-Oculus runtimes ("Support Non-Oculus runtime: NO"). */
    p->runtimeVersion = XR_MAKE_VERSION(78, 0, 0);
    snprintf(p->runtimeName, sizeof p->runtimeName, "Oculus");
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrPollEvent(XrInstance inst, XrEventDataBuffer *b) {
    (void)inst;
    mutexLock(&g_lock);
    if (g_ev_head == g_ev_tail) { mutexUnlock(&g_lock); return XR_EVENT_UNAVAILABLE; }
    XrEventDataBuffer *e = &g_events[g_ev_head++ % MAX_EVENTS];
    memcpy(b, e, sizeof(*b));
    mutexUnlock(&g_lock);
    XRLOG("poll event type %d\n", (int)b->type);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrResultToString(XrInstance inst, XrResult r,
                                               char buf[XR_MAX_RESULT_STRING_SIZE]) {
    (void)inst;
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, r == XR_SUCCESS ? "XR_SUCCESS" :
             "XR_RESULT_%d", (int)r);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrStructureTypeToString(XrInstance inst, XrStructureType t,
        char buf[XR_MAX_STRUCTURE_NAME_SIZE]) {
    (void)inst;
    snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_TYPE_%d", (int)t);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetSystem(XrInstance inst, const XrSystemGetInfo *gi,
                                          XrSystemId *id) {
    (void)inst;
    if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *id = 1;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetSystemProperties(XrInstance inst, XrSystemId id,
                                                    XrSystemProperties *p) {
    (void)inst;
    p->systemId = id;
    p->vendorId = 0x057e;
    snprintf(p->systemName, sizeof p->systemName, "Nintendo Switch");
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateEnvironmentBlendModes(XrInstance inst,
        XrSystemId id, XrViewConfigurationType t, uint32_t cap, uint32_t *count,
        XrEnvironmentBlendMode *modes) {
    (void)inst; (void)id; (void)t;
    XrResult r = two_call(cap, count, 1);
    if (!r && cap) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return r;
}

static XrResult XRAPI_CALL rt_xrEnumerateViewConfigurations(XrInstance inst,
        XrSystemId id, uint32_t cap, uint32_t *count, XrViewConfigurationType *t) {
    (void)inst; (void)id;
    XrResult r = two_call(cap, count, 1);
    if (!r && cap) t[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return r;
}

static XrResult XRAPI_CALL rt_xrGetViewConfigurationProperties(XrInstance inst,
        XrSystemId id, XrViewConfigurationType t, XrViewConfigurationProperties *p) {
    (void)inst; (void)id;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    p->viewConfigurationType = t;
    p->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateViewConfigurationViews(XrInstance inst,
        XrSystemId id, XrViewConfigurationType t, uint32_t cap, uint32_t *count,
        XrViewConfigurationView *v) {
    (void)inst; (void)id;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    XrResult r = two_call(cap, count, 2);
    if (r || !cap) return r;
    for (int i = 0; i < 2; ++i) {
        v[i].recommendedImageRectWidth = EYE_W;
        v[i].maxImageRectWidth = EYE_W;
        v[i].recommendedImageRectHeight = EYE_H;
        v[i].maxImageRectHeight = EYE_H;
        v[i].recommendedSwapchainSampleCount = 1;
        v[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetOpenGLESGraphicsRequirementsKHR(XrInstance inst,
        XrSystemId id, XrGraphicsRequirementsOpenGLESKHR *req) {
    (void)inst; (void)id;
    req->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    return XR_SUCCESS;
}

/* ---- session --------------------------------------------------------- */
static XrResult XRAPI_CALL rt_xrCreateSession(XrInstance inst,
        const XrSessionCreateInfo *info, XrSession *out) {
    (void)inst;
    const XrBaseInStructure *n = info->next;
    for (; n; n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR)
            memcpy(&g_binding, n, sizeof g_binding);
    }
    XRLOG("xrCreateSession display=%p context=%p (current ctx %p)\n",
          g_binding.display, g_binding.context, eglGetCurrentContext());
    if (!g_binding.context) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    g_session_live = 1;
    *out = (XrSession)&g_session;
    mutexLock(&g_lock);
    push_state(XR_SESSION_STATE_IDLE);
    push_state(XR_SESSION_STATE_READY);
    mutexUnlock(&g_lock);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroySession(XrSession s) {
    (void)s;
    g_session_live = g_session_running = 0;
    XRLOG("xrDestroySession\n");
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrBeginSession(XrSession s, const XrSessionBeginInfo *bi) {
    (void)s;
    XRLOG("xrBeginSession viewconfig=%d\n", (int)bi->primaryViewConfigurationType);
    g_session_running = 1;
    mutexLock(&g_lock);
    push_state(XR_SESSION_STATE_SYNCHRONIZED);
    push_state(XR_SESSION_STATE_VISIBLE);
    push_state(XR_SESSION_STATE_FOCUSED);
    mutexUnlock(&g_lock);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEndSession(XrSession s) {
    (void)s;
    g_session_running = 0;
    mutexLock(&g_lock);
    push_state(XR_SESSION_STATE_IDLE);
    mutexUnlock(&g_lock);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrRequestExitSession(XrSession s) {
    (void)s;
    mutexLock(&g_lock);
    push_state(XR_SESSION_STATE_STOPPING);
    mutexUnlock(&g_lock);
    return XR_SUCCESS;
}

/* ---- spaces ---------------------------------------------------------- */
static BsSpace *space_alloc(void) {
    mutexLock(&g_lock);
    for (int i = 0; i < MAX_SPACES; ++i)
        if (!g_spaces[i].used) {
            memset(&g_spaces[i], 0, sizeof g_spaces[i]);
            g_spaces[i].used = 1;
            mutexUnlock(&g_lock);
            return &g_spaces[i];
        }
    mutexUnlock(&g_lock);
    return NULL;
}

static XrResult XRAPI_CALL rt_xrEnumerateReferenceSpaces(XrSession s, uint32_t cap,
        uint32_t *count, XrReferenceSpaceType *t) {
    (void)s;
    XrResult r = two_call(cap, count, 3);
    if (!r && cap) {
        t[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
        t[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
        t[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    return r;
}

static XrResult XRAPI_CALL rt_xrCreateReferenceSpace(XrSession s,
        const XrReferenceSpaceCreateInfo *ci, XrSpace *out) {
    (void)s;
    /* Unknown (vendor) types behave like STAGE. */
    BsSpace *sp = space_alloc();
    if (!sp) return XR_ERROR_LIMIT_REACHED;
    sp->kind = 1;
    sp->ref = ci->referenceSpaceType;
    sp->offset = from_xr(ci->poseInReferenceSpace);
    XRLOG("reference space %d -> %p\n", (int)sp->ref, (void *)sp);
    *out = (XrSpace)sp;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetReferenceSpaceBoundsRect(XrSession s,
        XrReferenceSpaceType t, XrExtent2Df *b) {
    (void)s; (void)t;
    b->width = 2.0f;
    b->height = 2.0f;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrCreateActionSpace(XrSession s,
        const XrActionSpaceCreateInfo *ci, XrSpace *out) {
    (void)s;
    BsSpace *sp = space_alloc();
    if (!sp) return XR_ERROR_LIMIT_REACHED;
    sp->kind = 2;
    sp->action = (BsAction *)ci->action;
    sp->sub = ci->subactionPath;
    sp->offset = from_xr(ci->poseInActionSpace);
    *out = (XrSpace)sp;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroySpace(XrSpace s) {
    if (s) ((BsSpace *)s)->used = 0;
    return XR_SUCCESS;
}

static int space_world(BsSpace *sp, Pose *out) {
    Pose base;
    if (sp->kind == 1) {
        switch (sp->ref) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: base = head_pose_world(); break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL:
            base = (Pose){{0, 0, 0, 1}, {0, HEAD_HEIGHT, 0}};
            break;
        default: base = (Pose){{0, 0, 0, 1}, {0, 0, 0}}; break;
        }
    } else {
        float v;
        int side = path_side(sp->sub);
        if (!side) {
            if (!eval_action(sp->action, XR_NULL_PATH, &v, NULL, &side) || !side)
                side = SIDE_RIGHT;
        }
        base = hand_pose_world(side);
    }
    *out = pmul(base, sp->offset);
    return 1;
}

static XrResult XRAPI_CALL rt_xrLocateSpace(XrSpace space, XrSpace basesp, XrTime t,
                                            XrSpaceLocation *loc) {
    (void)t;
    Pose a, b;
    space_world((BsSpace *)space, &a);
    space_world((BsSpace *)basesp, &b);
    loc->pose = to_xr(pmul(pinv(b), a));
    {
        static int logged;
        BsSpace *sp = (BsSpace *)space;
        if (sp->kind == 2) ++g_n_locate_hand;
        static int ref_logged;
        if (sp->kind == 1 && ref_logged < 16) {
            ++ref_logged;
            XRLOG("locate ref %d in ref %d -> p=(%.2f %.2f %.2f) q=(%.2f %.2f %.2f %.2f)\n",
                  (int)sp->ref, ((BsSpace *)basesp)->kind == 1 ? (int)((BsSpace *)basesp)->ref : -1,
                  loc->pose.position.x, loc->pose.position.y, loc->pose.position.z,
                  loc->pose.orientation.x, loc->pose.orientation.y,
                  loc->pose.orientation.z, loc->pose.orientation.w);
        }
        if (sp->kind == 2 && logged < 12) {
            ++logged;
            so_module *cm = so_find_module_by_addr(__builtin_return_address(0));
            XRLOG("locate (from %s+%#lx) base kind=%d ref=%d action %s sub=%s -> p=(%.2f %.2f %.2f) q=(%.2f %.2f %.2f %.2f)\n",
                  cm ? cm->name : "?", cm ? (unsigned long)((uintptr_t)__builtin_return_address(0) - (uintptr_t)cm->load_virtbase) : 0ul, ((BsSpace *)basesp)->kind, (int)((BsSpace *)basesp)->ref,
                  sp->action ? sp->action->name : "?", path_str(sp->sub),
                  loc->pose.position.x, loc->pose.position.y, loc->pose.position.z,
                  loc->pose.orientation.x, loc->pose.orientation.y,
                  loc->pose.orientation.z, loc->pose.orientation.w);
        }
    }
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    for (XrBaseOutStructure *n = loc->next; n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITY) {
            XrSpaceVelocity *v = (XrSpaceVelocity *)n;
            v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT |
                               XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            v->linearVelocity = (XrVector3f){0, 0, 0};
            v->angularVelocity = (XrVector3f){0, 0, 0};
        }
    return XR_SUCCESS;
}

/* OpenXR 1.1 core batch locate. Unity's input provider fetches every
 * controller pose through this (straight from libopenxr_loader's export,
 * not via xrGetInstanceProcAddr); unimplemented, Input System poses stay 0. */
static XrResult XRAPI_CALL rt_xrLocateSpaces(XrSession s, const XrSpacesLocateInfo *li,
                                             XrSpaceLocations *out) {
    (void)s;
    static int logged;
    if (logged < 4) { ++logged; XRLOG("xrLocateSpaces count=%u caller=%p\n", li ? li->spaceCount : 0, __builtin_return_address(0)); }
    if (!li || !out || !li->spaces || out->locationCount < li->spaceCount)
        return XR_ERROR_VALIDATION_FAILURE;
    XrSpaceVelocities *vel = NULL;
    for (XrBaseOutStructure *n = (XrBaseOutStructure *)out->next; n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITIES) vel = (XrSpaceVelocities *)n;
    for (uint32_t i = 0; i < li->spaceCount; ++i) {
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION, NULL, 0, {{0, 0, 0, 1}, {0, 0, 0}}};
        rt_xrLocateSpace(li->spaces[i], li->baseSpace, li->time, &loc);
        out->locations[i].locationFlags = loc.locationFlags;
        out->locations[i].pose = loc.pose;
        if (vel && i < vel->velocityCount) {
            vel->velocities[i].velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT |
                                               XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            vel->velocities[i].linearVelocity = (XrVector3f){0, 0, 0};
            vel->velocities[i].angularVelocity = (XrVector3f){0, 0, 0};
        }
    }
    return XR_SUCCESS;
}
#define rt_xrLocateSpacesKHR rt_xrLocateSpaces

static XrResult XRAPI_CALL rt_xrLocateViews(XrSession s, const XrViewLocateInfo *li,
        XrViewState *st, uint32_t cap, uint32_t *count, XrView *views) {
    (void)s;
    XrResult r = two_call(cap, count, 2);
    if (r || !cap) return r;
    Pose head = head_pose_world(), base;
    space_world((BsSpace *)li->space, &base);
    Pose rel = pmul(pinv(base), head);
    st->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT;
    const float ah = atanf(TAN_HALF_H), av = atanf(TAN_HALF_V);
    for (int i = 0; i < 2; ++i) {
        /* Mono: both eyes at the head, the screen shows the left one. */
        views[i].pose = to_xr(rel);
        views[i].fov = (XrFovf){-ah, ah, av, -av};
    }
    return XR_SUCCESS;
}

/* ---- swapchains ------------------------------------------------------ */
static const int64_t g_formats[] = {
    GL_SRGB8_ALPHA8, GL_RGBA8, GL_DEPTH24_STENCIL8, GL_DEPTH_COMPONENT24,
    GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT32F, GL_DEPTH32F_STENCIL8,
};

static XrResult XRAPI_CALL rt_xrEnumerateSwapchainFormats(XrSession s, uint32_t cap,
        uint32_t *count, int64_t *f) {
    (void)s;
    uint32_t n = sizeof g_formats / sizeof g_formats[0];
    XrResult r = two_call(cap, count, n);
    if (!r && cap) memcpy(f, g_formats, n * sizeof(int64_t));
    return r;
}

static XrResult XRAPI_CALL rt_xrCreateSwapchain(XrSession s,
        const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    (void)s;
    if (!eglGetCurrentContext()) {
        XRLOG("xrCreateSwapchain without a current GL context\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    BsSwapchain *sc = NULL;
    mutexLock(&g_lock);
    for (int i = 0; i < MAX_SWAPCHAINS && !sc; ++i)
        if (!g_swapchains[i].used) { sc = &g_swapchains[i]; memset(sc, 0, sizeof *sc); sc->used = 1; }
    mutexUnlock(&g_lock);
    if (!sc) return XR_ERROR_LIMIT_REACHED;
    sc->count = 3;
    sc->w = ci->width;
    sc->h = ci->height;
    sc->array = ci->arraySize ? ci->arraySize : 1;
    sc->format = ci->format;
    sc->is_array = sc->array > 1;
    GLint prev = 0;
    GLenum target = sc->is_array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    glGetIntegerv(sc->is_array ? GL_TEXTURE_BINDING_2D_ARRAY : GL_TEXTURE_BINDING_2D, &prev);
    glGenTextures(sc->count, sc->tex);
    for (uint32_t i = 0; i < sc->count; ++i) {
        glBindTexture(target, sc->tex[i]);
        if (sc->is_array)
            glTexStorage3D(target, 1, (GLenum)ci->format, sc->w, sc->h, sc->array);
        else
            glTexStorage2D(target, 1, (GLenum)ci->format, sc->w, sc->h);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(target, (GLuint)prev);
    XRLOG("xrCreateSwapchain %ux%u x%u fmt=%#llx samples=%u usage=%#llx -> tex %u,%u,%u err=0x%x\n",
          ci->width, ci->height, sc->array, (unsigned long long)ci->format,
          ci->sampleCount, (unsigned long long)ci->usageFlags,
          sc->tex[0], sc->tex[1], sc->tex[2], glGetError());
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroySwapchain(XrSwapchain h) {
    BsSwapchain *sc = (BsSwapchain *)h;
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    /* Textures intentionally leaked: Eden crashes on in-flight deleted images. */
    sc->used = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap,
        uint32_t *count, XrSwapchainImageBaseHeader *imgs) {
    BsSwapchain *sc = (BsSwapchain *)h;
    XrResult r = two_call(cap, count, sc->count);
    if (r || !cap) return r;
    XrSwapchainImageOpenGLESKHR *gl = (XrSwapchainImageOpenGLESKHR *)imgs;
    for (uint32_t i = 0; i < sc->count; ++i) gl[i].image = sc->tex[i];
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrAcquireSwapchainImage(XrSwapchain h,
        const XrSwapchainImageAcquireInfo *ai, uint32_t *index) {
    (void)ai;
    BsSwapchain *sc = (BsSwapchain *)h;
    *index = sc->next;
    sc->next = (sc->next + 1) % sc->count;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrWaitSwapchainImage(XrSwapchain h,
        const XrSwapchainImageWaitInfo *wi) {
    (void)h; (void)wi;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrReleaseSwapchainImage(XrSwapchain h,
        const XrSwapchainImageReleaseInfo *ri) {
    (void)ri;
    BsSwapchain *sc = (BsSwapchain *)h;
    sc->last = (sc->next + sc->count - 1) % sc->count;
    return XR_SUCCESS;
}

/* ---- frames ---------------------------------------------------------- */
static XrResult XRAPI_CALL rt_xrWaitFrame(XrSession s, const XrFrameWaitInfo *wi,
                                          XrFrameState *fs) {
    (void)s; (void)wi;
    mutexLock(&g_frame_lock);
    /* At most one frame in flight: pace the game loop to presentation. */
    u64 deadline = armGetSystemTick() + armNsToTicks(100000000ULL);
    while (g_frames_waited > g_frames_ended && armGetSystemTick() < deadline)
        condvarWaitTimeout(&g_frame_cv, &g_frame_lock, 20000000ULL);
    ++g_frames_waited;
    mutexUnlock(&g_frame_lock);
    XrTime t = now_ns() + FRAME_NS;
    if (t <= g_last_display_time) t = g_last_display_time + 1;
    g_last_display_time = t;
    fs->predictedDisplayTime = t;
    fs->predictedDisplayPeriod = FRAME_NS;
    fs->shouldRender = g_session_running ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrBeginFrame(XrSession s, const XrFrameBeginInfo *bi) {
    (void)s; (void)bi;
    return XR_SUCCESS;
}

static uint64_t g_end_count;
static XrResult XRAPI_CALL rt_xrEndFrame(XrSession s, const XrFrameEndInfo *ei) {
    (void)s;
    ++g_end_count;
    int presented = 0;
    for (uint32_t i = 0; i < ei->layerCount && !presented; ++i) {
        const XrCompositionLayerBaseHeader *l = ei->layers[i];
        if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
        if (!p->viewCount) continue;
        const XrSwapchainSubImage *si = &p->views[0].subImage;
        BsSwapchain *sc = (BsSwapchain *)si->swapchain;
        if (!sc) continue;
        int w = si->imageRect.extent.width ? si->imageRect.extent.width : (int)sc->w;
        int h = si->imageRect.extent.height ? si->imageRect.extent.height : (int)sc->h;
        int rc = bs_egl_present_texture(sc->tex[sc->last], (int)si->imageArrayIndex,
                                        sc->is_array, si->imageRect.offset.x,
                                        si->imageRect.offset.y, w, h,
                                        (int)sc->w, (int)sc->h);
        presented = rc == 0;
        if (rc && g_end_count < 64) XRLOG("present failed rc=%d\n", rc);
    }
    if (g_end_count == 600 && 0) {
        static const char *const filters[] = {"Vector3Control", "QuaternionControl", NULL};
        bs_il2cpp_probe(filters);
    }
    if ((g_end_count == 300 || g_end_count == 1500) && il2cpp_probe_enabled())
        bs_il2cpp_probe_xr_devices();
    if (g_end_count == 1 || g_end_count == 60 || g_end_count % 3000 == 0)
        XRLOG("xrEndFrame #%llu layers=%u presented=%d input: sync=%lu hand-locate=%lu bool=%lu float=%lu vec2=%lu pose=%lu\n",
              (unsigned long long)g_end_count, ei->layerCount, presented, g_n_sync,
              g_n_locate_hand, g_n_bool, g_n_float, g_n_vec2, g_n_pose);
    {
        static u64 t0, n0, wsum; static u64 tprev;
        u64 now = armGetSystemTick();
        if (!t0) { t0 = now; n0 = g_end_count; }
        if (tprev) { u64 dt = now - tprev; if (dt > wsum) wsum = dt; }
        tprev = now;
        u64 el = armTicksToNs(now - t0);
        if (el >= 5000000000ULL) {
            XRLOG("fps %.1f (worst frame %.1f ms)\n",
                  (double)(g_end_count - n0) * 1e9 / (double)el, armTicksToNs(wsum) / 1e6);
            t0 = now; n0 = g_end_count; wsum = 0;
        }
    }
    mutexLock(&g_frame_lock);
    g_frames_ended = g_frames_waited;
    condvarWakeAll(&g_frame_cv);
    mutexUnlock(&g_frame_lock);
    return XR_SUCCESS;
}

/* ---- paths & actions ------------------------------------------------- */
static XrResult XRAPI_CALL rt_xrStringToPath(XrInstance inst, const char *s, XrPath *p) {
    (void)inst;
    if (!s || s[0] != '/') return XR_ERROR_PATH_FORMAT_INVALID;
    mutexLock(&g_lock);
    *p = path_intern(s);
    mutexUnlock(&g_lock);
    return *p ? XR_SUCCESS : XR_ERROR_PATH_COUNT_EXCEEDED;
}

static XrResult XRAPI_CALL rt_xrPathToString(XrInstance inst, XrPath p, uint32_t cap,
                                             uint32_t *count, char *buf) {
    (void)inst;
    if (!p || p > g_path_count) return XR_ERROR_PATH_INVALID;
    const char *s = path_str(p);
    uint32_t n = (uint32_t)strlen(s) + 1;
    XrResult r = two_call(cap, count, n);
    if (!r && cap) memcpy(buf, s, n);
    return r;
}

static XrResult XRAPI_CALL rt_xrCreateActionSet(XrInstance inst,
        const XrActionSetCreateInfo *ci, XrActionSet *out) {
    (void)inst;
    if (g_set_count >= 64) return XR_ERROR_LIMIT_REACHED;
    BsActionSet *set = &g_sets[g_set_count++];
    snprintf(set->name, sizeof set->name, "%s", ci->actionSetName);
    XRLOG("action set %s\n", set->name);
    *out = (XrActionSet)set;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroyActionSet(XrActionSet s) { (void)s; return XR_SUCCESS; }

static XrResult XRAPI_CALL rt_xrCreateAction(XrActionSet set, const XrActionCreateInfo *ci,
                                             XrAction *out) {
    BsAction *a = NULL;
    mutexLock(&g_lock);
    for (int i = 0; i < MAX_ACTIONS && !a; ++i)
        if (!g_actions[i].used) { a = &g_actions[i]; memset(a, 0, sizeof *a); a->used = 1; }
    mutexUnlock(&g_lock);
    if (!a) return XR_ERROR_LIMIT_REACHED;
    a->set = (BsActionSet *)set;
    a->type = ci->actionType;
    snprintf(a->name, sizeof a->name, "%s", ci->actionName);
    a->sub_count = ci->countSubactionPaths < MAX_SUBACTIONS ? ci->countSubactionPaths
                                                             : MAX_SUBACTIONS;
    for (uint32_t i = 0; i < a->sub_count; ++i) a->sub[i] = ci->subactionPaths[i];
    *out = (XrAction)a;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroyAction(XrAction a) {
    if (a) ((BsAction *)a)->used = 0;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrSuggestInteractionProfileBindings(XrInstance inst,
        const XrInteractionProfileSuggestedBinding *sb) {
    (void)inst;
    const char *prof = path_str(sb->interactionProfile);
    mutexLock(&g_lock);
    for (uint32_t i = 0; i < sb->countSuggestedBindings &&
                         g_binding_count < MAX_BINDINGS; ++i) {
        g_bindings[g_binding_count].action = (BsAction *)sb->suggestedBindings[i].action;
        g_bindings[g_binding_count].path = sb->suggestedBindings[i].binding;
        g_bindings[g_binding_count].profile = sb->interactionProfile;
        ++g_binding_count;
    }
    /* Prefer Touch-style controllers; otherwise the first profile seen. */
    if (strstr(prof, "oculus/touch_controller") ||
        (!g_profile && strstr(prof, "khr/simple_controller") == NULL))
        g_profile = sb->interactionProfile;
    if (!g_profile) g_profile = sb->interactionProfile;
    mutexUnlock(&g_lock);
    XRLOG("bindings for %s: %u (profile now %s)\n", prof, sb->countSuggestedBindings,
          path_str(g_profile));
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrAttachSessionActionSets(XrSession s,
        const XrSessionActionSetsAttachInfo *ai) {
    (void)s;
    XRLOG("attach %u action sets, %u bindings, profile %s\n", ai->countActionSets,
          g_binding_count, path_str(g_profile));
    for (uint32_t i = 0; i < g_binding_count; ++i)
        if (g_bindings[i].profile == g_profile)
            XRLOG("  %s <- %s\n", g_bindings[i].action->name,
                  path_str(g_bindings[i].path));
    XrEventDataInteractionProfileChanged e = {.type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
    e.session = (XrSession)&g_session;
    mutexLock(&g_lock);
    push_event(&e, sizeof e);
    mutexUnlock(&g_lock);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetCurrentInteractionProfile(XrSession s, XrPath user,
        XrInteractionProfileState *st) {
    (void)s;
    st->interactionProfile = path_side(user) ? g_profile : XR_NULL_PATH;
    static int logged;
    if (logged < 8) { ++logged; XRLOG("current interaction profile %s -> %s\n", path_str(user), path_str(st->interactionProfile)); }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrSyncActions(XrSession s, const XrActionsSyncInfo *si) {
    (void)s; (void)si;
    ++g_n_sync;
    /* Diagnostic only: guesses object fields by address and faults on
     * hardware (crash right after the EULA). Opt in with an SD flag file. */
    if ((g_n_sync == 1500 || g_n_sync == 3000) && il2cpp_probe_enabled())
        bs_il2cpp_probe_helper_actions();
    mutexLock(&g_lock);
    poll_input();
    mutexUnlock(&g_lock);
    return XR_SUCCESS;
}

static int side_slot(XrPath sub) { return path_side(sub); }

static XrResult XRAPI_CALL rt_xrGetActionStateBoolean(XrSession s,
        const XrActionStateGetInfo *gi, XrActionStateBoolean *st) {
    (void)s;
    BsAction *a = (BsAction *)gi->action;
    float v;
    int slot = side_slot(gi->subactionPath);
    ++g_n_bool;
    st->isActive = eval_action(a, gi->subactionPath, &v, NULL, NULL);
    st->currentState = v > 0.5f;
    st->changedSinceLastSync = (a->prev[slot][0] > 0.5f) != (v > 0.5f);
    if (st->changedSinceLastSync) a->changed[slot] = now_ns();
    a->prev[slot][0] = v;
    st->lastChangeTime = a->changed[slot];
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetActionStateFloat(XrSession s,
        const XrActionStateGetInfo *gi, XrActionStateFloat *st) {
    (void)s;
    BsAction *a = (BsAction *)gi->action;
    float v;
    int slot = side_slot(gi->subactionPath);
    ++g_n_float;
    st->isActive = eval_action(a, gi->subactionPath, &v, NULL, NULL);
    st->currentState = v;
    st->changedSinceLastSync = a->prev[slot][0] != v;
    if (st->changedSinceLastSync) a->changed[slot] = now_ns();
    a->prev[slot][0] = v;
    st->lastChangeTime = a->changed[slot];
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetActionStateVector2f(XrSession s,
        const XrActionStateGetInfo *gi, XrActionStateVector2f *st) {
    (void)s;
    BsAction *a = (BsAction *)gi->action;
    float v, v2[2];
    int slot = side_slot(gi->subactionPath);
    ++g_n_vec2;
    st->isActive = eval_action(a, gi->subactionPath, &v, v2, NULL);
    st->currentState = (XrVector2f){v2[0], v2[1]};
    st->changedSinceLastSync = a->prev[slot][0] != v2[0] || a->prev[slot][1] != v2[1];
    if (st->changedSinceLastSync) a->changed[slot] = now_ns();
    a->prev[slot][0] = v2[0];
    a->prev[slot][1] = v2[1];
    st->lastChangeTime = a->changed[slot];
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetActionStatePose(XrSession s,
        const XrActionStateGetInfo *gi, XrActionStatePose *st) {
    (void)s;
    float v;
    ++g_n_pose;
    st->isActive = eval_action((BsAction *)gi->action, gi->subactionPath, &v, NULL, NULL);
    static int logged;
    if (logged < 8) { ++logged; XRLOG("pose state %s sub=%s active=%d\n", ((BsAction *)gi->action)->name, path_str(gi->subactionPath), st->isActive); }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateBoundSourcesForAction(XrSession s,
        const XrBoundSourcesForActionEnumerateInfo *ei, uint32_t cap, uint32_t *count,
        XrPath *src) {
    (void)s;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_binding_count; ++i)
        if (g_bindings[i].action == (BsAction *)ei->action &&
            g_bindings[i].profile == g_profile) {
            if (cap && n < cap) src[n] = g_bindings[i].path;
            ++n;
        }
    return two_call(cap, count, n);
}

static XrResult XRAPI_CALL rt_xrGetInputSourceLocalizedName(XrSession s,
        const XrInputSourceLocalizedNameGetInfo *gi, uint32_t cap, uint32_t *count,
        char *buf) {
    (void)s;
    const char *name = path_side(gi->sourcePath) == SIDE_LEFT ? "Left Joy-Con"
                                                                : "Right Joy-Con";
    uint32_t n = (uint32_t)strlen(name) + 1;
    XrResult r = two_call(cap, count, n);
    if (!r && cap) memcpy(buf, name, n);
    return r;
}

static XrResult XRAPI_CALL rt_xrApplyHapticFeedback(XrSession s,
        const XrHapticActionInfo *hi, const XrHapticBaseHeader *h) {
    (void)s; (void)hi; (void)h;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrStopHapticFeedback(XrSession s,
                                                   const XrHapticActionInfo *hi) {
    (void)s; (void)hi;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrInitializeLoaderKHR(const void *info) {
    (void)info;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance inst, const char *name,
                                                    PFN_xrVoidFunction *fn);

/* ---- XR_FB_foveation / swapchain_update_state ------------------------
 * The flat Switch display has no fixed-foveation hardware: profiles are
 * accepted handles and swapchain state updates are no-ops. OVRPlugin
 * (OculusXRPlugin eye layer setup) requires these to succeed. */
static uint64_t g_next_foveation_profile = 1;
static XrResult XRAPI_CALL rt_xrCreateFoveationProfileFB(
    XrSession session, const XrFoveationProfileCreateInfoFB *info,
    XrFoveationProfileFB *profile) {
    (void)session;
    if (!info || !profile) return XR_ERROR_VALIDATION_FAILURE;
    *profile = (XrFoveationProfileFB)(uintptr_t)(0xf0000 + g_next_foveation_profile++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_xrDestroyFoveationProfileFB(XrFoveationProfileFB p) {
    (void)p;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_xrUpdateSwapchainFB(
    XrSwapchain swapchain, const XrSwapchainStateBaseHeaderFB *state) {
    (void)swapchain;
    return state ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}
static XrResult XRAPI_CALL rt_xrGetSwapchainStateFB(
    XrSwapchain swapchain, XrSwapchainStateBaseHeaderFB *state) {
    (void)swapchain;
    return state ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}

/* ---- dispatch -------------------------------------------------------- */
#define F(n) {#n, (PFN_xrVoidFunction)rt_##n}
static const struct { const char *name; PFN_xrVoidFunction fn; } g_funcs[] = {
    F(xrGetInstanceProcAddr), F(xrEnumerateApiLayerProperties),
    F(xrEnumerateInstanceExtensionProperties), F(xrCreateInstance),
    F(xrDestroyInstance), F(xrGetInstanceProperties), F(xrPollEvent),
    F(xrResultToString), F(xrStructureTypeToString), F(xrGetSystem),
    F(xrGetSystemProperties), F(xrEnumerateEnvironmentBlendModes),
    F(xrCreateSession), F(xrDestroySession), F(xrEnumerateReferenceSpaces),
    F(xrCreateReferenceSpace), F(xrGetReferenceSpaceBoundsRect),
    F(xrCreateActionSpace), F(xrLocateSpace), F(xrLocateSpaces), F(xrLocateSpacesKHR),
    F(xrDestroySpace),
    F(xrEnumerateViewConfigurations), F(xrGetViewConfigurationProperties),
    F(xrEnumerateViewConfigurationViews), F(xrEnumerateSwapchainFormats),
    F(xrCreateSwapchain), F(xrDestroySwapchain), F(xrEnumerateSwapchainImages),
    F(xrAcquireSwapchainImage), F(xrWaitSwapchainImage), F(xrReleaseSwapchainImage),
    F(xrBeginSession), F(xrEndSession), F(xrRequestExitSession), F(xrWaitFrame),
    F(xrBeginFrame), F(xrEndFrame), F(xrLocateViews), F(xrStringToPath),
    F(xrPathToString), F(xrCreateActionSet), F(xrDestroyActionSet),
    F(xrCreateAction), F(xrDestroyAction), F(xrSuggestInteractionProfileBindings),
    F(xrAttachSessionActionSets), F(xrGetCurrentInteractionProfile),
    F(xrGetActionStateBoolean), F(xrGetActionStateFloat),
    F(xrGetActionStateVector2f), F(xrGetActionStatePose), F(xrSyncActions),
    F(xrEnumerateBoundSourcesForAction), F(xrGetInputSourceLocalizedName),
    F(xrApplyHapticFeedback), F(xrStopHapticFeedback),
    F(xrGetOpenGLESGraphicsRequirementsKHR), F(xrInitializeLoaderKHR),
    F(xrCreateFoveationProfileFB), F(xrDestroyFoveationProfileFB),
    F(xrUpdateSwapchainFB), F(xrGetSwapchainStateFB),
};
#undef F

static XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance inst, const char *name,
                                                    PFN_xrVoidFunction *fn) {
    (void)inst;
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    static int lookups;
    if (lookups++ < 300) XRLOG("gipa(%p, %s)\n", (void *)inst, name);
    for (size_t i = 0; i < sizeof g_funcs / sizeof g_funcs[0]; ++i)
        if (!strcmp(g_funcs[i].name, name)) { *fn = g_funcs[i].fn; return XR_SUCCESS; }
    static int logged;
    if (logged++ < 64) XRLOG("unsupported function %s\n", name);
    *fn = NULL;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

void *bs_openxr_symbol(const char *name) {
    PFN_xrVoidFunction fn = NULL;
    if (!name || strncmp(name, "xr", 2)) return NULL;
    for (size_t i = 0; i < sizeof g_funcs / sizeof g_funcs[0]; ++i)
        if (!strcmp(g_funcs[i].name, name)) fn = g_funcs[i].fn;
    return (void *)fn;
}

int bs_openxr_hook_loader(so_module *loader) {
    int hooked = 0;
    for (size_t i = 0; i < sizeof g_funcs / sizeof g_funcs[0]; ++i) {
        uintptr_t addr = so_find_module_export(loader, g_funcs[i].name);
        if (!addr) continue;
        uintptr_t staged = addr - (uintptr_t)loader->load_virtbase +
                           (uintptr_t)loader->load_base;
        hook_arm64(staged, (uintptr_t)g_funcs[i].fn);
        ++hooked;
    }
    XRLOG("redirected %d libopenxr_loader exports to the Switch runtime\n", hooked);
    return hooked;
}

/* OVRPlugin focus/presence queries. On Quest these come from the headset's
 * proximity sensor and system overlays; on Switch the player is always
 * present and the app always owns input. Beat Saber deactivates its VR
 * controllers (no pointers, no clicks) while either reports false. */
static int32_t ovrp_true_out(int32_t *out) {
    if (!out) return -1001; /* ovrpFailure_InvalidParameter */
    *out = 1;
    return 0;
}

/* Input focus is reported only once action syncing has run for a while.
 * DeactivateVRControllersOnFocusCapture re-evaluates controller visibility
 * only on focus/presence change events and needs a valid controller pose at
 * that moment; reporting focus from the very first frame leaves the
 * controllers (pointers, clicks) deactivated forever. */
#define BS_INPUT_FOCUS_AFTER_SYNCS 240
static int32_t ovrp_input_focus_out(int32_t *out) {
    if (!out) return -1001;
    *out = g_n_sync >= BS_INPUT_FOCUS_AFTER_SYNCS;
    return 0;
}

int bs_openxr_hook_ovrplugin(so_module *ovrp) {
    static const struct { const char *name; int32_t (*fn)(int32_t *); } hooks[] = {
        {"ovrp_GetUserPresent2", ovrp_true_out},
        {"ovrp_GetAppHasInputFocus", ovrp_input_focus_out},
        {"ovrp_GetAppHasVrFocus2", ovrp_true_out},
    };
    int hooked = 0;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; ++i) {
        uintptr_t addr = so_find_module_export(ovrp, hooks[i].name);
        if (!addr) continue;
        hook_arm64(addr - (uintptr_t)ovrp->load_virtbase + (uintptr_t)ovrp->load_base,
                   (uintptr_t)hooks[i].fn);
        ++hooked;
    }
    XRLOG("OVRPlugin presence/focus hooks: %d\n", hooked);
    return hooked;
}