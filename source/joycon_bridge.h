/* Physical Joy-Con input for hand-specific game/XR action adapters.
 * Motion is Nintendo controller-relative. No world-space 6DoF position exists.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_JOYCON_BRIDGE_H
#define BS_JOYCON_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BS_JOYCON_HAND_LEFT = 0, BS_JOYCON_HAND_RIGHT = 1,
       BS_JOYCON_HAND_COUNT = 2 };

enum {
    BS_JOYCON_CAP_BUTTONS              = 1u << 0,
    BS_JOYCON_CAP_STICK                = 1u << 1,
    BS_JOYCON_CAP_ACCELEROMETER        = 1u << 2,
    BS_JOYCON_CAP_GYROSCOPE            = 1u << 3,
    BS_JOYCON_CAP_RELATIVE_ORIENTATION = 1u << 4
};

typedef struct BsJoyconHandSample {
    uint32_t connected;
    uint32_t capabilities;
    uint32_t npad_id;             /* libnx HidNpadIdType, 0..7 or 0x20 */
    uint32_t npad_style;          /* libnx HidNpadStyleTag, exactly one bit */
    uint64_t connection_epoch;    /* changes on disconnect, reassignment */
    uint64_t button_sampling_number;
    uint64_t motion_sampling_number;
    int64_t motion_timestamp_ns;  /* CLOCK_MONOTONIC on first read of sample */
    uint64_t buttons;             /* native HidNpadButton bits, filtered by hand */
    float stick[2];               /* Nintendo thumbstick [-1, 1] */
    /* Digital Joy-Con controls, exposed as 0 or 1 for XR float actions.
     * Left:  trigger=ZL, grip=L, primary=Down, secondary=Left, menu=Minus.
     * Right: trigger=ZR, grip=R, primary=A, secondary=B, menu=Plus.
     * Neither trigger nor grip has analog travel on a Joy-Con. */
    float trigger;
    float grip;
    uint32_t primary_pressed;
    uint32_t secondary_pressed;
    uint32_t menu_pressed;
    uint32_t stick_pressed;
    float acceleration_g[3];     /* raw sensor units: standard gravity */
    float angular_velocity_rad_s[3];
    /* Valid only with BS_JOYCON_CAP_RELATIVE_ORIENTATION.
     * xyzw quaternion from Nintendo's 3x3 direction matrix; in Nintendo's
     * sensor frame, without an OpenXR/world calibration or position. */
    float orientation_xyzw[4];
} BsJoyconHandSample;

typedef struct BsJoyconSnapshot {
    uint64_t generation;         /* increments when connection/caps change */
    int64_t poll_timestamp_ns;   /* CLOCK_MONOTONIC at read, 0 if unavailable */
    BsJoyconHandSample hands[BS_JOYCON_HAND_COUNT];
} BsJoyconSnapshot;

/* Call once after libnx input setup (padConfigureInput with at least 2 players
 * and HidNpadStyleSet_NpadStandard). Neither function changes Npad assignment. */
int bs_joycon_init(void);
/* Poll HID and publish an atomic, thread-safe two-hand snapshot. Returns 0 on
 * success, -EINVAL for NULL output, -ENODEV before initialization. An absent
 * controller is reflected by hands[i].connected=0, not a poll error. */
int bs_joycon_poll(BsJoyconSnapshot *out);
/* Copy the most recently published snapshot without touching HID. */
int bs_joycon_get_snapshot(BsJoyconSnapshot *out);
void bs_joycon_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
