/* Real HID bit layout, selection and motion mapping without Nintendo hardware.
 * Build: clang -std=c11 -Wall -Wextra -Werror -O2 tests/test_joycon_bridge.c
 */
#define BS_JOYCON_HOST_TEST 1
#include "../source/joycon_bridge.c"

#include <assert.h>
#include <stdio.h>

static int nearf(float a, float b) { return fabsf(a - b) < 0.0003f; }

static void identity(float d[3][3]) {
    memset(d, 0, sizeof(float) * 9);
    for (int i = 0; i < 3; ++i) d[i][i] = 1.0f;
}

static void reset_frames(void) {
    memset(g_test_frames, 0, sizeof(g_test_frames));
    g_test_frame_count = 0;
}

static BsRawFrame *add_frame(uint32_t id, uint32_t style, uint32_t attributes,
                              uint64_t sample) {
    assert(g_test_frame_count < BS_FRAME_CAP);
    BsRawFrame *f = &g_test_frames[g_test_frame_count++];
    memset(f, 0, sizeof(*f));
    f->id = id;
    f->style = style;
    f->attributes = attributes;
    f->sequence = sample;
    return f;
}

static void motion(BsRawFrame *frame, unsigned hand, uint64_t sequence) {
    BsRawMotion *m = &frame->test_motion[hand];
    m->connected = 1;
    m->sequence = sequence;
    m->acceleration_g[0] = 1.25f + (float)hand;
    m->angular_velocity_rad_s[1] = 2.5f + (float)hand;
    identity(m->direction);
}

static void test_full_pair_imu_and_actions(void) {
    reset_frames();
    BsRawFrame *f = add_frame(0, BS_STYLE_DUAL,
                              BS_ATTR_CONNECTED | BS_ATTR_LEFT | BS_ATTR_RIGHT, 42);
    f->buttons = BS_BTN_ZL | BS_BTN_ZR | BS_BTN_L | BS_BTN_R |
                 BS_BTN_DOWN | BS_BTN_LEFT | BS_BTN_A | BS_BTN_B |
                 BS_BTN_MINUS | BS_BTN_PLUS | BS_BTN_STICK_L | BS_BTN_STICK_R;
    f->left_stick[0] = -32768;
    f->left_stick[1] = 16384;
    f->right_stick[0] = 32767;
    f->right_stick[1] = -10000;
    motion(f, BS_JOYCON_HAND_LEFT, 100);
    motion(f, BS_JOYCON_HAND_RIGHT, 200);
    /* +90 degrees about Z, same convention as the Nintendo direction matrix. */
    const float rot90[3][3] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}};
    memcpy(f->test_motion[1].direction, rot90, sizeof(rot90));
    BsJoyconSnapshot s;
    assert(bs_joycon_poll(&s) == 0);
    assert(s.generation == 1);
    assert(s.hands[0].connected && s.hands[1].connected);
    assert(s.hands[0].npad_id == 0 && s.hands[1].npad_id == 0);
    assert(s.hands[0].connection_epoch == 1 && s.hands[1].connection_epoch == 1);
    assert(s.hands[0].capabilities == 31 && s.hands[1].capabilities == 31);
    assert(s.hands[0].motion_sampling_number == 100);
    assert(s.hands[1].motion_sampling_number == 200);
    assert(s.hands[0].motion_timestamp_ns == s.poll_timestamp_ns);
    assert(s.hands[0].buttons & BS_BTN_ZL);
    assert(!(s.hands[0].buttons & BS_BTN_ZR));
    assert(s.hands[1].buttons & BS_BTN_ZR);
    assert(!(s.hands[1].buttons & BS_BTN_ZL));
    assert(s.hands[0].trigger == 1.0f && s.hands[1].trigger == 1.0f);
    assert(s.hands[0].grip == 1.0f && s.hands[1].grip == 1.0f);
    assert(s.hands[0].primary_pressed && s.hands[1].primary_pressed);
    assert(s.hands[0].secondary_pressed && s.hands[1].secondary_pressed);
    assert(s.hands[0].menu_pressed && s.hands[1].menu_pressed);
    assert(s.hands[0].stick_pressed && s.hands[1].stick_pressed);
    assert(nearf(s.hands[0].stick[0], -1.0f));
    assert(nearf(s.hands[0].stick[1], 16384.0f / 32767.0f));
    assert(nearf(s.hands[1].stick[0], 1.0f));
    assert(nearf(s.hands[0].orientation_xyzw[3], 1.0f));
    assert(nearf(s.hands[1].orientation_xyzw[2], 0.70710678f));
    assert(nearf(s.hands[1].orientation_xyzw[3], 0.70710678f));
    assert(nearf(s.hands[0].acceleration_g[0], 1.25f));
    assert(nearf(s.hands[1].angular_velocity_rad_s[1], 3.5f));
    int64_t first_motion = s.hands[0].motion_timestamp_ns;
    int64_t first_right = s.hands[1].motion_timestamp_ns;
    assert(bs_joycon_poll(&s) == 0);
    assert(s.generation == 1);
    assert(s.poll_timestamp_ns > first_motion);
    assert(s.hands[0].motion_timestamp_ns == first_motion);
    assert(s.hands[1].motion_timestamp_ns == first_right);
    f->test_motion[0].sequence++;
    assert(bs_joycon_poll(&s) == 0);
    assert(s.hands[0].motion_timestamp_ns > first_motion);
    assert(s.hands[1].motion_timestamp_ns == first_right);
    assert(s.generation == 1);
    /* A connected gyro with invalid fused direction must not get a pose flag. */
    memset(f->test_motion[0].direction, 0, sizeof(f->test_motion[0].direction));
    assert(bs_joycon_poll(&s) == 0);
    assert(!(s.hands[0].capabilities & BS_JOYCON_CAP_RELATIVE_ORIENTATION));
    assert(s.hands[0].capabilities & BS_JOYCON_CAP_GYROSCOPE);
    assert(s.hands[1].capabilities & BS_JOYCON_CAP_RELATIVE_ORIENTATION);
    assert(s.generation == 2);
    assert(s.hands[0].orientation_xyzw[3] == 0.0f);
}

static void test_partial_dual_and_single_reassignment(void) {
    BsJoyconSnapshot s;
    reset_frames();
    BsRawFrame *pair = add_frame(0, BS_STYLE_DUAL,
                                BS_ATTR_CONNECTED | BS_ATTR_LEFT, 43);
    pair->buttons = BS_BTN_ZL | BS_BTN_ZR;
    motion(pair, 0, 111);
    assert(bs_joycon_poll(&s) == 0);
    assert(s.hands[0].connected);
    assert(!s.hands[1].connected);
    assert(s.hands[1].trigger == 0.0f);
    uint64_t previous_right_epoch = s.hands[1].connection_epoch;
    BsRawFrame *right = add_frame(2, BS_STYLE_RIGHT, BS_ATTR_CONNECTED, 20);
    right->buttons = BS_BTN_ZR | BS_BTN_A;
    motion(right, 1, 77);
    assert(bs_joycon_poll(&s) == 0);
    assert(s.hands[0].npad_id == 0 && s.hands[1].npad_id == 2);
    assert(s.hands[1].connected && s.hands[1].trigger == 1.0f);
    assert(s.hands[1].connection_epoch == previous_right_epoch + 1);
    /* Dropped connection must release all button and motion state. */
    pair->attributes = 0;
    right->attributes = 0;
    assert(bs_joycon_poll(&s) == 0);
    assert(!s.hands[0].connected && !s.hands[1].connected);
    assert(s.hands[0].capabilities == 0 && s.hands[1].capabilities == 0);
    assert(s.hands[0].buttons == 0 && s.hands[1].buttons == 0);
    assert(s.hands[0].motion_timestamp_ns == 0);
    assert(s.hands[1].motion_timestamp_ns == 0);
    uint64_t gone_left_epoch = s.hands[0].connection_epoch;
    reset_frames();
    BsRawFrame *left = add_frame(4, BS_STYLE_LEFT, BS_ATTR_CONNECTED, 3);
    left->buttons = BS_BTN_L | BS_BTN_ZL | BS_BTN_MINUS;
    right = add_frame(5, BS_STYLE_RIGHT, BS_ATTR_CONNECTED, 7);
    right->buttons = BS_BTN_R;
    assert(bs_joycon_poll(&s) == 0);
    assert(s.hands[0].connected && s.hands[1].connected);
    assert(s.hands[0].npad_id == 4 && s.hands[1].npad_id == 5);
    assert(s.hands[0].connection_epoch == gone_left_epoch + 1);
    assert(s.hands[0].trigger == 1.0f && s.hands[1].trigger == 0.0f);
    assert(s.hands[0].capabilities == (BS_JOYCON_CAP_BUTTONS | BS_JOYCON_CAP_STICK));
}

static void test_handheld_never_claims_two_independent_imus(void) {
    reset_frames();
    BsRawFrame *handheld = add_frame(BS_NPAD_HANDHELD, BS_STYLE_HANDHELD,
                                     BS_ATTR_CONNECTED | BS_ATTR_LEFT | BS_ATTR_RIGHT, 12);
    handheld->buttons = BS_BTN_ZL | BS_BTN_ZR;
    motion(handheld, 0, 101);
    motion(handheld, 1, 102);
    BsJoyconSnapshot s;
    assert(bs_joycon_poll(&s) == 0);
    for (unsigned i = 0; i < BS_JOYCON_HAND_COUNT; ++i) {
        assert(s.hands[i].connected);
        assert(s.hands[i].npad_id == BS_NPAD_HANDHELD);
        assert(s.hands[i].capabilities == (BS_JOYCON_CAP_BUTTONS | BS_JOYCON_CAP_STICK));
        assert(s.hands[i].orientation_xyzw[3] == 0.0f);
        assert(s.hands[i].motion_sampling_number == 0);
    }
    /* Even with paired Joy-Cons, their shared handheld input is not 2 IMUs. */
}

static void test_sample_rollback_and_unavailable(void) {
    reset_frames();
    BsRawFrame *left = add_frame(0, BS_STYLE_LEFT, BS_ATTR_CONNECTED, 1000);
    BsJoyconSnapshot s;
    assert(bs_joycon_poll(&s) == 0);
    uint64_t epoch = s.hands[0].connection_epoch;
    left->sequence = 5; /* same Npad slot, new attachment */
    assert(bs_joycon_poll(&s) == 0);
    assert(s.hands[0].connection_epoch == epoch + 1);
    assert(bs_joycon_poll(NULL) == -EINVAL);
    BsJoyconSnapshot copied;
    assert(bs_joycon_get_snapshot(&copied) == 0);
    assert(!memcmp(&copied, &s, sizeof(s)));
    bs_joycon_shutdown();
    assert(bs_joycon_get_snapshot(&copied) == -ENODEV);
    assert(bs_joycon_poll(&copied) == -ENODEV);
    assert(!copied.hands[0].connected);
}

int main(void) {
    BsJoyconSnapshot empty;
    assert(bs_joycon_poll(&empty) == -ENODEV);
    assert(bs_joycon_init() == 0);
    assert(bs_joycon_init() == 0);
    test_full_pair_imu_and_actions();
    test_partial_dual_and_single_reassignment();
    test_handheld_never_claims_two_independent_imus();
    test_sample_rollback_and_unavailable();
    puts("Joy-Con dual/single/handheld HID mapping tests passed");
    return 0;
}
