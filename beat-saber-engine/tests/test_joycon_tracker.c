/* Build: clang -std=c11 -Wall -Wextra -Werror -O2 tests/test_joycon_tracker.c \
 *        source/joycon_tracker.c -lm
 */
#include "../source/joycon_tracker.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static const float ID[4] = {0, 0, 0, 1};
/* Sensor frame: XR +Y (up) is Nintendo +Z. At rest the sensor reads +1 g up. */
static const float REST_ACC[3] = {0, 0, 1.0f};
static const float NO_GYRO[3] = {0, 0, 0};

static void test_rest_is_stable_and_matches_layout(void) {
    BsJcTracker t;
    bs_jc_tracker_reset(&t, NULL, 1);
    /* Default aim: pitch up 0.44 rad about +X. */
    float aim[4] = {sinf(0.22f), 0, 0, cosf(0.22f)};
    for (int i = 0; i < 400; ++i)
        bs_jc_tracker_update(&t, aim, ID, REST_ACC, NO_GYRO, 0.005f);
    assert(bs_jc_tracker_is_still(&t));
    float q[4], p[3];
    bs_jc_tracker_pose(&t, q, p);
    assert(fabsf(q[0] - aim[0]) < 1e-3f && fabsf(q[3] - aim[3]) < 1e-3f);
    assert(p[0] > 0.1f && p[0] < 0.3f);            /* right of head */
    assert(p[1] < -0.15f && p[1] > -0.35f);        /* below the eyes */
    assert(p[2] < -0.2f && p[2] > -0.45f);         /* in front */
    assert(fabsf(t.offset[0]) + fabsf(t.offset[1]) + fabsf(t.offset[2]) < 1e-4f);
}

static void test_punch_translates_then_returns(void) {
    BsJcTracker t;
    bs_jc_tracker_reset(&t, NULL, 0);
    for (int i = 0; i < 100; ++i)
        bs_jc_tracker_update(&t, ID, ID, REST_ACC, NO_GYRO, 0.005f);
    /* 100 ms forward (-Z XR == +Y sensor) push of 1.5 g, orientation fixed. */
    const float fwd[3] = {0, 1.5f, 1.0f};
    for (int i = 0; i < 20; ++i)
        bs_jc_tracker_update(&t, ID, ID, fwd, NO_GYRO, 0.005f);
    float q[4], p_push[3];
    bs_jc_tracker_pose(&t, q, p_push);
    assert(t.offset[2] < -0.01f);                  /* moved forward */
    assert(fabsf(t.offset[0]) < 0.005f);
    for (int i = 0; i < 600; ++i)
        bs_jc_tracker_update(&t, ID, ID, REST_ACC, NO_GYRO, 0.005f);
    assert(fabsf(t.offset[2]) < 0.002f);           /* no drift afterwards */
}

static void test_prediction_and_bias(void) {
    BsJcTracker t;
    bs_jc_tracker_reset(&t, NULL, 0);
    /* Constant small gyro bias at rest is learned and not predicted. */
    const float bias[3] = {0.004f, -0.003f, 0.002f};
    for (int i = 0; i < 2000; ++i)
        bs_jc_tracker_update(&t, ID, ID, REST_ACC, bias, 0.005f);
    assert(fabsf(t.gyro_bias[0] - bias[0]) < 5e-4f);
    float q[4], p[3];
    bs_jc_tracker_pose(&t, q, p);
    assert(q[3] > 0.99999f);
    /* Yaw rotation: 0.5 rev/s about sensor +Z (XR +Y) predicts ahead. */
    const float spin[3] = {0, 0, 0.5f};
    for (int i = 0; i < 6; ++i)                   /* 30 ms of rotation */
        bs_jc_tracker_update(&t, ID, ID, REST_ACC, spin, 0.005f);
    bs_jc_tracker_pose(&t, q, p);
    assert(q[1] > 0.02f);
}

static void test_rejects_garbage(void) {
    BsJcTracker t;
    bs_jc_tracker_reset(&t, NULL, 0);
    const float bad[3] = {NAN, 0, 0};
    bs_jc_tracker_update(&t, ID, ID, bad, NO_GYRO, 0.005f);
    bs_jc_tracker_update(&t, ID, ID, REST_ACC, NO_GYRO, -1.0f);
    float q[4], p[3];
    bs_jc_tracker_pose(&t, q, p);
    for (int i = 0; i < 3; ++i) assert(isfinite(p[i]));
}

/* Pitch rotation about +X (XR), as xyzw. */
static void pitch_q(float a, float q[4]) {
    q[0] = sinf(a * 0.5f); q[1] = 0; q[2] = 0; q[3] = cosf(a * 0.5f);
}

/* Overhead swings, repeated hard: forward-up through straight-up and over
 * the top, then back down, at 25 rad/s. The accelerometer reports the
 * centripetal force of a *longer* real arm than the model (mismatch), and
 * clips at 8 g, exactly the conditions that used to fling the hand. */
static void test_large_fast_swings_stay_stable(void) {
    BsJcTracker t;
    bs_jc_tracker_reset(&t, NULL, 1);
    float q[4], p[3], prev[3] = {0, 0, 0};
    const float dt = 0.005f, w = 25.0f, real_radius = 0.75f;
    float angle = 0.44f, dir = 1.0f;
    for (int i = 0; i < 100; ++i) {
        pitch_q(angle, q);
        bs_jc_tracker_update(&t, q, q, REST_ACC, NO_GYRO, dt);
    }
    bs_jc_tracker_pose(&t, q, prev);
    float max_step = 0, max_off = 0;
    for (int i = 0; i < 2400; ++i) {         /* 12 s of continuous swinging */
        angle += dir * w * dt;
        if (angle > 2.6f) dir = -1.0f;        /* ~149 deg: past vertical */
        if (angle < -0.9f) dir = 1.0f;
        pitch_q(angle, q);
        /* Specific force in player frame: +1 g up plus centripetal toward
         * the shoulder (w^2 r) along -forward; expressed in body axes. */
        float fwd_y = sinf(angle), fwd_z = -cosf(angle);
        float cy = 1.0f - w * w * real_radius * fwd_y / 9.80665f;
        float cz = -w * w * real_radius * fwd_z / 9.80665f;
        /* Rotate player -> body (inverse pitch) then XR -> sensor axes. */
        float c = cosf(angle), s = sinf(angle);
        float by = c * cy + s * cz, bz = -s * cy + c * cz;
        float acc[3] = {0, -bz, by};
        for (int k = 0; k < 3; ++k) {
            if (acc[k] > 8.0f) acc[k] = 8.0f;
            if (acc[k] < -8.0f) acc[k] = -8.0f;
        }
        float gyro[3] = {dir * w / (2.0f * 3.14159265f), 0, 0};
        bs_jc_tracker_update(&t, q, q, acc, gyro, dt);
        bs_jc_tracker_pose(&t, q, p);
        float dx = p[0] - prev[0], dy = p[1] - prev[1], dz = p[2] - prev[2];
        float step = sqrtf(dx * dx + dy * dy + dz * dz);
        if (step > max_step) max_step = step;
        float o = sqrtf(t.offset[0] * t.offset[0] + t.offset[1] * t.offset[1] +
                        t.offset[2] * t.offset[2]);
        if (o > max_off) max_off = o;
        for (int k = 0; k < 3; ++k) {
            assert(isfinite(p[k]));
            prev[k] = p[k];
        }
    }
    printf("  swing: max step %.1f mm/sample, max inertial offset %.1f mm\n",
           max_step * 1000.0f, max_off * 1000.0f);
    /* Hand tip moves ~0.6 m * 25 rad/s * 5 ms = 75 mm per sample, plus the
     * capped-prediction jump at direction reversal. No teleports. */
    assert(max_step < 0.16f);
    assert(max_off < 0.04f);
    /* Stop: settles to the arm model with no leftover offset. */
    pitch_q(0.44f, q);
    for (int i = 0; i < 400; ++i)
        bs_jc_tracker_update(&t, q, q, REST_ACC, NO_GYRO, dt);
    assert(sqrtf(t.offset[0] * t.offset[0] + t.offset[1] * t.offset[1] +
                 t.offset[2] * t.offset[2]) < 0.002f);
}

/* Pointing straight up/down used to flip the Euler yaw by 180 degrees. */
static void test_arm_model_continuous_through_vertical(void) {
    BsJcTracker t;
    BsJcTrackerConfig cfg;
    bs_jc_tracker_default_config(&cfg, 0);
    cfg.inertial_gain = 0;
    cfg.predict_s = 0;
    bs_jc_tracker_reset(&t, &cfg, 0);
    float q[4], p[3], prev[3];
    pitch_q(1.40f, q);
    bs_jc_tracker_update(&t, q, q, REST_ACC, NO_GYRO, 0.0f);
    bs_jc_tracker_pose(&t, q, prev);
    for (int i = 1; i <= 200; ++i) {
        float a = 1.40f + 0.0017f * (float)i;   /* 1.40 -> 1.74 rad, crosses pi/2 */
        pitch_q(a, q);
        bs_jc_tracker_reset(&t, &cfg, 0);
        bs_jc_tracker_update(&t, q, q, REST_ACC, NO_GYRO, 0.0f);
        bs_jc_tracker_pose(&t, q, p);
        float dx = p[0] - prev[0], dy = p[1] - prev[1], dz = p[2] - prev[2];
        assert(sqrtf(dx * dx + dy * dy + dz * dz) < 0.003f);
        for (int k = 0; k < 3; ++k) prev[k] = p[k];
    }
}

static float yaw_of(const float q[4]) {     /* heading of -Z after rotation */
    float fx = -2.0f * (q[0] * q[2] + q[3] * q[1]);
    float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    return atan2f(-fx, -fz);
}

/* Horizontal slashes: Horizon's yaw trails the true motion by 50 ms (its
 * gyro-only yaw smoothing). The gyro reports the truth. Output yaw must
 * follow the truth, not Horizon. */
static void test_horizontal_follows_gyro_not_lagging_fusion(void) {
    BsJcTracker t;
    BsJcTrackerConfig cfg;
    bs_jc_tracker_default_config(&cfg, 1);
    cfg.predict_s = 0;          /* measure the filter itself */
    bs_jc_tracker_reset(&t, &cfg, 1);
    const float dt = 0.005f, lag = 0.050f, amp = 1.0f, freq = 1.5f;
    float q[4], p[3];
    for (int i = 0; i < 100; ++i) {
        float id[4] = {0, 0, 0, 1};
        bs_jc_tracker_update(&t, id, id, REST_ACC, NO_GYRO, dt);
    }
    double err_out = 0, err_fusion = 0;
    int n = 0;
    for (int i = 0; i < 1200; ++i) {
        float time = (float)i * dt;
        float yaw_true = amp * sinf(2 * 3.14159265f * freq * time);
        float yaw_lag = amp * sinf(2 * 3.14159265f * freq * (time - lag));
        float rate = amp * 2 * 3.14159265f * freq * cosf(2 * 3.14159265f * freq * time);
        float fused[4] = {0, sinf(yaw_lag * 0.5f), 0, cosf(yaw_lag * 0.5f)};
        float world[4] = {0, sinf(yaw_true * 0.5f), 0, cosf(yaw_true * 0.5f)};
        /* XR +Y == sensor +Z */
        float gyro[3] = {0, 0, rate / (2.0f * 3.14159265f)};
        bs_jc_tracker_update(&t, fused, world, REST_ACC, gyro, dt);
        if (i < 400) continue;  /* settle */
        bs_jc_tracker_pose(&t, q, p);
        err_out += fabsf(yaw_of(q) - yaw_true);
        err_fusion += fabsf(yaw_lag - yaw_true);
        ++n;
    }
    err_out /= n;
    err_fusion /= n;
    printf("  horizontal: mean yaw error %.1f deg (fusion alone %.1f deg)\n",
           err_out * 57.29578, err_fusion * 57.29578);
    assert(err_out < err_fusion * 0.4);
}

/* Gyro reported in rad/s while the config assumes rev/s: the tracker must
 * learn the real scale from Horizon's rotation. */
static void test_gyro_scale_self_calibrates(void) {
    BsJcTracker t;
    bs_jc_tracker_reset(&t, NULL, 0);
    const float dt = 0.005f, w = 4.0f;
    float yaw = 0;
    for (int i = 0; i < 1500; ++i) {
        float dir = ((i / 100) & 1) ? -1.0f : 1.0f;
        yaw += dir * w * dt;
        float q[4] = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
        float gyro[3] = {0, 0, dir * w};               /* rad/s, not rev/s */
        bs_jc_tracker_update(&t, q, q, REST_ACC, gyro, dt);
    }
    printf("  gyro scale learned %.3f (true 1.0, assumed %.3f)\n",
           t.gyro_scale_est, t.cfg.gyro_scale);
    assert(fabsf(t.gyro_scale_est - 1.0f) < 0.08f);
}

int main(void) {
    test_horizontal_follows_gyro_not_lagging_fusion();
    test_gyro_scale_self_calibrates();
    test_arm_model_continuous_through_vertical();
    test_large_fast_swings_stay_stable();
    test_rest_is_stable_and_matches_layout();
    test_punch_translates_then_returns();
    test_prediction_and_bias();
    test_rejects_garbage();
    puts("Joy-Con 6DoF tracker tests passed");
    return 0;
}
