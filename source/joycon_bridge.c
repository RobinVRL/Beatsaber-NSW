/* Two independent physical Joy-Con hands for genuine game/XR input.
 * SPDX-License-Identifier: MIT
 */
#include "joycon_bridge.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifndef BS_JOYCON_HOST_TEST
#include <switch.h>
#include "util.h"
#define JCLOG(...) debugPrintf("[joycon] " __VA_ARGS__)
static Mutex g_joycon_mutex;
static void joycon_lock(void) { mutexLock(&g_joycon_mutex); }
static void joycon_unlock(void) { mutexUnlock(&g_joycon_mutex); }
#else
static void joycon_lock(void) {}
static void joycon_unlock(void) {}
#define JCLOG(...) ((void)0)
#endif

/* These constants are the ABI bits of HidNpadStyleTag, HidNpadAttribute and
 * HidNpadButton in libnx services/hid.h. Keeping the portable mapper separate
 * permits host tests without mocking Nintendo's entire SDK. */
#define BS_STYLE_HANDHELD (UINT32_C(1) << 1)
#define BS_STYLE_DUAL     (UINT32_C(1) << 2)
#define BS_STYLE_LEFT     (UINT32_C(1) << 3)
#define BS_STYLE_RIGHT    (UINT32_C(1) << 4)
#define BS_NPAD_HANDHELD  UINT32_C(0x20)
#define BS_ATTR_CONNECTED (UINT32_C(1) << 0)
#define BS_ATTR_LEFT      (UINT32_C(1) << 2)
#define BS_ATTR_RIGHT     (UINT32_C(1) << 4)

#define BS_BTN_A       (UINT64_C(1) << 0)
#define BS_BTN_B       (UINT64_C(1) << 1)
#define BS_BTN_X       (UINT64_C(1) << 2)
#define BS_BTN_Y       (UINT64_C(1) << 3)
#define BS_BTN_STICK_L (UINT64_C(1) << 4)
#define BS_BTN_STICK_R (UINT64_C(1) << 5)
#define BS_BTN_L       (UINT64_C(1) << 6)
#define BS_BTN_R       (UINT64_C(1) << 7)
#define BS_BTN_ZL      (UINT64_C(1) << 8)
#define BS_BTN_ZR      (UINT64_C(1) << 9)
#define BS_BTN_PLUS    (UINT64_C(1) << 10)
#define BS_BTN_MINUS   (UINT64_C(1) << 11)
#define BS_BTN_LEFT    (UINT64_C(1) << 12)
#define BS_BTN_UP      (UINT64_C(1) << 13)
#define BS_BTN_RIGHT   (UINT64_C(1) << 14)
#define BS_BTN_DOWN    (UINT64_C(1) << 15)
#define BS_BTN_LEFT_SL (UINT64_C(1) << 24)
#define BS_BTN_LEFT_SR (UINT64_C(1) << 25)
#define BS_BTN_RIGHT_SL (UINT64_C(1) << 26)
#define BS_BTN_RIGHT_SR (UINT64_C(1) << 27)

#define BS_FRAME_CAP 36
#define BS_GYRO_START_RETRY_NS INT64_C(500000000)

/* Matches the physical axes and sampling metadata of a single libnx reading.
 * direction is Nintendo's measured/fused 3x3 sensor orientation, not XR space. */
typedef struct BsRawMotion {
    uint32_t connected;
    uint64_t sequence;
    float acceleration_g[3];
    float angular_velocity_rad_s[3];
    float direction[3][3];
} BsRawMotion;

typedef struct BsRawFrame {
    uint32_t id;
    uint32_t style;
    uint32_t attributes;
    uint64_t sequence;
    uint64_t buttons;
    int32_t left_stick[2];
    int32_t right_stick[2];
#ifdef BS_JOYCON_HOST_TEST
    BsRawMotion test_motion[BS_JOYCON_HAND_COUNT];
#endif
} BsRawFrame;

static int g_initialized;
static BsJoyconSnapshot g_snapshot;

#ifndef BS_JOYCON_HOST_TEST
typedef struct BsImuHandle {
    int started;
    HidSixAxisSensorHandle handle;
    uint32_t id;
    uint32_t style;
    int64_t retry_after_ns;
} BsImuHandle;
static BsImuHandle g_imus[BS_JOYCON_HAND_COUNT];

_Static_assert((unsigned)HidNpadStyleTag_NpadJoyDual == BS_STYLE_DUAL,
               "NpadJoyDual style must match HID ABI");
_Static_assert((unsigned)HidNpadAttribute_IsLeftConnected == BS_ATTR_LEFT,
               "left connected flag must match HID ABI");
_Static_assert((uint64_t)HidNpadButton_RightSR == BS_BTN_RIGHT_SR,
               "Joy-Con button bits must match HID ABI");

static int64_t joycon_timestamp(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static size_t backend_read_frames(BsRawFrame *frames, size_t cap) {
    static const uint32_t styles[] = {
        BS_STYLE_HANDHELD, BS_STYLE_DUAL, BS_STYLE_LEFT, BS_STYLE_RIGHT
    };
    size_t total = 0;
    for (unsigned slot = 0; slot < 9 && total < cap; ++slot) {
        HidNpadIdType id = slot == 0 ? HidNpadIdType_Handheld :
                          (HidNpadIdType)(slot - 1);
        uint32_t style_set = hidGetNpadStyleSet(id);
        for (unsigned j = 0; j < sizeof(styles) / sizeof(styles[0]) && total < cap; ++j) {
            uint32_t style = styles[j];
            if (!(style_set & style)) continue;
            if ((slot == 0) != (style == BS_STYLE_HANDHELD)) continue;
            HidNpadCommonState state = {0};
            size_t received = 0;
            switch (style) {
                case BS_STYLE_HANDHELD:
                    received = hidGetNpadStatesHandheld(id, &state, 1); break;
                case BS_STYLE_DUAL:
                    received = hidGetNpadStatesJoyDual(id, &state, 1); break;
                case BS_STYLE_LEFT:
                    received = hidGetNpadStatesJoyLeft(id, &state, 1); break;
                case BS_STYLE_RIGHT:
                    received = hidGetNpadStatesJoyRight(id, &state, 1); break;
                default: break;
            }
            if (received != 1 || !(state.attributes & HidNpadAttribute_IsConnected))
                continue;
            BsRawFrame *frame = &frames[total++];
            memset(frame, 0, sizeof(*frame));
            frame->id = (uint32_t)id;
            frame->style = style;
            frame->attributes = state.attributes;
            frame->sequence = state.sampling_number;
            frame->buttons = state.buttons;
            frame->left_stick[0] = state.analog_stick_l.x;
            frame->left_stick[1] = state.analog_stick_l.y;
            frame->right_stick[0] = state.analog_stick_r.x;
            frame->right_stick[1] = state.analog_stick_r.y;
        }
    }
    return total;
}

static void backend_stop_motion(unsigned hand) {
    BsImuHandle *imu = &g_imus[hand];
    if (imu->started) {
        JCLOG("hand %u motion stopped (npad %u style %#x)\n", hand, imu->id, imu->style);
        (void)hidStopSixAxisSensor(imu->handle);
    }
    memset(imu, 0, sizeof(*imu));
}

static int backend_read_motion(const BsRawFrame *frame, unsigned hand,
                               int64_t now, BsRawMotion *out) {
    if (frame->style == BS_STYLE_HANDHELD) return 0;
    BsImuHandle *imu = &g_imus[hand];
    if (imu->started && (imu->id != frame->id || imu->style != frame->style))
        backend_stop_motion(hand);
    if (!imu->started) {
        if (now && imu->retry_after_ns > now) return 0;
        HidSixAxisSensorHandle handles[2] = {{0}, {0}};
        int count = frame->style == BS_STYLE_DUAL ? 2 : 1;
        Result rc = hidGetSixAxisSensorHandles(handles, count,
                          (HidNpadIdType)frame->id, (HidNpadStyleTag)frame->style);
        HidSixAxisSensorHandle handle = handles[frame->style == BS_STYLE_DUAL ? hand : 0];
        Result handles_rc = rc;
        if (R_SUCCEEDED(rc)) rc = hidStartSixAxisSensor(handle);
        static int fail_logs;
        if (R_FAILED(rc)) {
            if (fail_logs++ < 24)
                JCLOG("hand %u motion start FAILED npad %u style %#x handles_rc=%#x start_rc=%#x\n",
                      hand, frame->id, frame->style, handles_rc, rc);
            imu->retry_after_ns = now ? now + BS_GYRO_START_RETRY_NS : 0;
            return 0;
        }
        JCLOG("hand %u motion started npad %u style %#x handle=%#x\n",
              hand, frame->id, frame->style, handle.type_value);
        /* Horizon's default (Standard) zero-drift filter swallows small and
         * moderate rotations; yaw has no gravity reference to recover it,
         * so horizontal motion felt muted. The tracker removes drift itself. */
        Result zrc = hidSetGyroscopeZeroDriftMode(handle, HidGyroscopeZeroDriftMode_Loose);
        if (R_FAILED(zrc)) JCLOG("hand %u zero-drift mode not set: %#x\n", hand, zrc);
        imu->handle = handle;
        imu->id = frame->id;
        imu->style = frame->style;
        imu->started = 1;
    }
    HidSixAxisSensorState state = {0};
    size_t got = hidGetSixAxisSensorStates(imu->handle, &state, 1);
    if (got != 1 || !(state.attributes & HidSixAxisSensorAttribute_IsConnected)) {
        static int lost_logs;
        if (lost_logs++ < 24)
            JCLOG("hand %u motion lost: states=%zu attributes=%#x\n",
                  hand, got, (unsigned)state.attributes);
        backend_stop_motion(hand);
        imu->retry_after_ns = now ? now + BS_GYRO_START_RETRY_NS : 0;
        return 0;
    }
    out->connected = 1;
    out->sequence = state.sampling_number;
    out->acceleration_g[0] = state.acceleration.x;
    out->acceleration_g[1] = state.acceleration.y;
    out->acceleration_g[2] = state.acceleration.z;
    out->angular_velocity_rad_s[0] = state.angular_velocity.x;
    out->angular_velocity_rad_s[1] = state.angular_velocity.y;
    out->angular_velocity_rad_s[2] = state.angular_velocity.z;
    memcpy(out->direction, state.direction.direction, sizeof(out->direction));
    return 1;
}
#else
/* Host tests inject the same Npad frames the portable mapping code receives.
 * The public header has no dependency on these test-only fixtures. */
static BsRawFrame g_test_frames[BS_FRAME_CAP];
static size_t g_test_frame_count;
static int64_t g_test_time_ns = 1000000000;
static int64_t joycon_timestamp(void) {
    g_test_time_ns += 5000000;
    return g_test_time_ns;
}
static size_t backend_read_frames(BsRawFrame *frames, size_t cap) {
    size_t count = g_test_frame_count < cap ? g_test_frame_count : cap;
    memcpy(frames, g_test_frames, count * sizeof(*frames));
    return count;
}
static void backend_stop_motion(unsigned hand) { (void)hand; }
static int backend_read_motion(const BsRawFrame *frame, unsigned hand,
                               int64_t now, BsRawMotion *out) {
    (void)now;
    *out = frame->test_motion[hand];
    return out->connected != 0;
}
#endif

static float joycon_stick_axis(int32_t raw) {
    if (raw >= 32767) return 1.0f;
    if (raw <= -32767) return -1.0f;
    return (float)raw / 32767.0f;
}

/* Reject zeroed/degenerate HID direction matrices. The resulting orientation
 * remains in Nintendo's IMU coordinate frame; XR must calibrate its own space. */
static int joycon_quaternion(const float m[3][3], float q[4]) {
    for (unsigned i = 0; i < 3; ++i) {
        float norm = 0;
        for (unsigned j = 0; j < 3; ++j) {
            if (!isfinite(m[j][i])) return 0;
            norm += m[j][i] * m[j][i];
        }
        if (norm < 0.7f || norm > 1.3f) return 0;
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = i + 1; j < 3; ++j) {
            float dot = 0;
            for (unsigned k = 0; k < 3; ++k) dot += m[k][i] * m[k][j];
            if (fabsf(dot) > 0.2f) return 0;
        }
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
              - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
              + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!isfinite(det) || det < 0.7f || det > 1.3f) return 0;
    float trace = m[0][0] + m[1][1] + m[2][2];
    float s;
    if (trace > 0.0f) {
        s = sqrtf(trace + 1.0f) * 2.0f;
        if (s <= 0.0f) return 0;
        q[3] = 0.25f * s;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        if (s <= 0.0f) return 0;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        if (s <= 0.0f) return 0;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        if (s <= 0.0f) return 0;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25f * s;
    }
    float length2 = q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
    if (!isfinite(length2) || length2 < 0.01f) return 0;
    float inv = 1.0f / sqrtf(length2);
    for (unsigned i = 0; i < 4; ++i) q[i] *= inv;
    if (q[3] < 0.0f) for (unsigned i = 0; i < 4; ++i) q[i] = -q[i];
    return 1;
}

static uint64_t joycon_hand_buttons(uint64_t buttons, unsigned hand) {
    const uint64_t left = BS_BTN_LEFT | BS_BTN_UP | BS_BTN_RIGHT | BS_BTN_DOWN |
        BS_BTN_L | BS_BTN_ZL | BS_BTN_MINUS | BS_BTN_STICK_L |
        BS_BTN_LEFT_SL | BS_BTN_LEFT_SR | (UINT64_C(15) << 16);
    const uint64_t right = BS_BTN_A | BS_BTN_B | BS_BTN_X | BS_BTN_Y |
        BS_BTN_R | BS_BTN_ZR | BS_BTN_PLUS | BS_BTN_STICK_R |
        BS_BTN_RIGHT_SL | BS_BTN_RIGHT_SR | (UINT64_C(15) << 20);
    return buttons & (hand == BS_JOYCON_HAND_LEFT ? left : right);
}

/* Only actually connected sides are eligible. For paired Joy-Cons, the Npad
 * attribute bits distinguish physically connected left and right halves. */
static int joycon_side_present(const BsRawFrame *frame, unsigned hand) {
    if (!(frame->attributes & BS_ATTR_CONNECTED)) return 0;
    if (frame->style == BS_STYLE_DUAL || frame->style == BS_STYLE_HANDHELD)
        return !!(frame->attributes & (hand == BS_JOYCON_HAND_LEFT ?
                                       BS_ATTR_LEFT : BS_ATTR_RIGHT));
    return frame->style == (hand == BS_JOYCON_HAND_LEFT ? BS_STYLE_LEFT : BS_STYLE_RIGHT);
}

static void joycon_select(const BsRawFrame *frames, size_t count,
                          const BsRawFrame *selected[BS_JOYCON_HAND_COUNT]) {
    selected[0] = selected[1] = NULL;
    /* Prefer a complete pair belonging to the same logical Nintendo player. */
    for (size_t i = 0; i < count; ++i)
        if ((frames[i].style == BS_STYLE_DUAL ||
             frames[i].style == BS_STYLE_HANDHELD) &&
            joycon_side_present(&frames[i], 0) &&
            joycon_side_present(&frames[i], 1)) {
            selected[0] = selected[1] = &frames[i];
            return;
        }
    /* Partial dual/handheld pairs still provide their physically attached hand. */
    for (size_t i = 0; i < count; ++i)
        if (frames[i].style == BS_STYLE_DUAL || frames[i].style == BS_STYLE_HANDHELD)
            for (unsigned hand = 0; hand < BS_JOYCON_HAND_COUNT; ++hand)
                if (!selected[hand] && joycon_side_present(&frames[i], hand))
                    selected[hand] = &frames[i];
    for (size_t i = 0; i < count; ++i)
        for (unsigned hand = 0; hand < BS_JOYCON_HAND_COUNT; ++hand)
            if (!selected[hand] && joycon_side_present(&frames[i], hand))
                selected[hand] = &frames[i];
}

static void joycon_map_hand(BsJoyconHandSample *out,
                            const BsJoyconHandSample *old,
                            const BsRawFrame *frame, unsigned hand, int64_t now) {
    memset(out, 0, sizeof(*out));
    out->connection_epoch = old->connection_epoch;
    if (!frame) {
        if (old->connected) ++out->connection_epoch;
        backend_stop_motion(hand);
        return;
    }
    out->connected = 1;
    out->npad_id = frame->id;
    out->npad_style = frame->style;
    out->capabilities = BS_JOYCON_CAP_BUTTONS | BS_JOYCON_CAP_STICK;
    out->button_sampling_number = frame->sequence;
    if (!old->connected || old->npad_id != frame->id ||
        old->npad_style != frame->style || frame->sequence < old->button_sampling_number) {
        ++out->connection_epoch;
        /* A sampling counter rollback indicates a reconnection even when the
         * Npad slot and style stayed the same between two application polls. */
        backend_stop_motion(hand);
    }
    out->buttons = joycon_hand_buttons(frame->buttons, hand);
    const int32_t *stick = hand == BS_JOYCON_HAND_LEFT ?
                          frame->left_stick : frame->right_stick;
    out->stick[0] = joycon_stick_axis(stick[0]);
    out->stick[1] = joycon_stick_axis(stick[1]);
    uint64_t buttons = out->buttons;
    out->trigger = !!(buttons & (hand == BS_JOYCON_HAND_LEFT ? BS_BTN_ZL : BS_BTN_ZR));
    out->grip = !!(buttons & (hand == BS_JOYCON_HAND_LEFT ? BS_BTN_L : BS_BTN_R));
    out->primary_pressed = !!(buttons & (hand == BS_JOYCON_HAND_LEFT ? BS_BTN_DOWN : BS_BTN_A));
    out->secondary_pressed = !!(buttons & (hand == BS_JOYCON_HAND_LEFT ? BS_BTN_LEFT : BS_BTN_B));
    out->menu_pressed = !!(buttons & (hand == BS_JOYCON_HAND_LEFT ? BS_BTN_MINUS : BS_BTN_PLUS));
    out->stick_pressed = !!(buttons & (hand == BS_JOYCON_HAND_LEFT ? BS_BTN_STICK_L : BS_BTN_STICK_R));

    /* Handheld provides only a combined sensor: it cannot represent two
     * independently tracked handheld Joy-Cons and must remain controls-only. */
    if (frame->style == BS_STYLE_HANDHELD) {
        backend_stop_motion(hand);
        return;
    }
    BsRawMotion motion = {0};
    if (!backend_read_motion(frame, hand, now, &motion) || !motion.connected)
        return;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(motion.acceleration_g[i]) ||
            !isfinite(motion.angular_velocity_rad_s[i])) return;
    out->capabilities |= BS_JOYCON_CAP_ACCELEROMETER | BS_JOYCON_CAP_GYROSCOPE;
    out->motion_sampling_number = motion.sequence;
    if (old->connected && old->npad_id == frame->id &&
        old->npad_style == frame->style &&
        (old->capabilities & BS_JOYCON_CAP_GYROSCOPE) &&
        old->motion_sampling_number == motion.sequence)
        out->motion_timestamp_ns = old->motion_timestamp_ns;
    else
        out->motion_timestamp_ns = now;
    memcpy(out->acceleration_g, motion.acceleration_g, sizeof(out->acceleration_g));
    memcpy(out->angular_velocity_rad_s, motion.angular_velocity_rad_s,
           sizeof(out->angular_velocity_rad_s));
    if (joycon_quaternion((const float (*)[3])motion.direction,
                          out->orientation_xyzw))
        out->capabilities |= BS_JOYCON_CAP_RELATIVE_ORIENTATION;
}

int bs_joycon_init(void) {
    joycon_lock();
    if (g_initialized) {
        joycon_unlock();
        return 0;
    }
#ifndef BS_JOYCON_HOST_TEST
    if (R_FAILED(hidInitialize())) {
        joycon_unlock();
        return -EIO;
    }
    hidInitializeNpad();
#endif
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_initialized = 1;
    joycon_unlock();
    return 0;
}

int bs_joycon_poll(BsJoyconSnapshot *out) {
    if (!out) return -EINVAL;
    joycon_lock();
    if (!g_initialized) {
        memset(out, 0, sizeof(*out));
        joycon_unlock();
        return -ENODEV;
    }
    BsRawFrame frames[BS_FRAME_CAP];
    size_t count = backend_read_frames(frames, BS_FRAME_CAP);
    const BsRawFrame *selected[BS_JOYCON_HAND_COUNT];
    joycon_select(frames, count, selected);
    BsJoyconSnapshot next = {0};
    next.poll_timestamp_ns = joycon_timestamp();
    for (unsigned hand = 0; hand < BS_JOYCON_HAND_COUNT; ++hand)
        joycon_map_hand(&next.hands[hand], &g_snapshot.hands[hand],
                        selected[hand], hand, next.poll_timestamp_ns);
    next.generation = g_snapshot.generation;
    for (unsigned hand = 0; hand < BS_JOYCON_HAND_COUNT; ++hand) {
        const BsJoyconHandSample *prev = &g_snapshot.hands[hand];
        const BsJoyconHandSample *curr = &next.hands[hand];
        if (prev->connected != curr->connected ||
            prev->connection_epoch != curr->connection_epoch ||
            prev->capabilities != curr->capabilities) {
            ++next.generation;
            break;
        }
    }
    if (next.generation != g_snapshot.generation) {
        static int change_logs;
        if (change_logs++ < 48) {
            JCLOG("change #%llu: %zu npad frame(s)\n",
                  (unsigned long long)next.generation, count);
            for (size_t i = 0; i < count; ++i)
                JCLOG("  frame npad %u style %#x attributes %#x\n",
                      frames[i].id, frames[i].style, frames[i].attributes);
            for (unsigned hand = 0; hand < BS_JOYCON_HAND_COUNT; ++hand)
                JCLOG("  hand %u connected=%u npad %u style %#x caps %#x\n", hand,
                      (unsigned)next.hands[hand].connected, (unsigned)next.hands[hand].npad_id,
                      (unsigned)next.hands[hand].npad_style,
                      (unsigned)next.hands[hand].capabilities);
        }
    }
    g_snapshot = next;
    *out = next;
    joycon_unlock();
    return 0;
}

int bs_joycon_get_snapshot(BsJoyconSnapshot *out) {
    if (!out) return -EINVAL;
    joycon_lock();
    *out = g_snapshot;
    int result = g_initialized ? 0 : -ENODEV;
    joycon_unlock();
    return result;
}

void bs_joycon_shutdown(void) {
    joycon_lock();
    if (g_initialized) {
        for (unsigned hand = 0; hand < BS_JOYCON_HAND_COUNT; ++hand)
            backend_stop_motion(hand);
#ifndef BS_JOYCON_HOST_TEST
        hidExit();
#endif
    }
    g_initialized = 0;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    joycon_unlock();
}
