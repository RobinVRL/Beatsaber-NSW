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

int main(void) {
    test_rest_is_stable_and_matches_layout();
    test_punch_translates_then_returns();
    test_prediction_and_bias();
    test_rejects_garbage();
    puts("Joy-Con 6DoF tracker tests passed");
    return 0;
}
