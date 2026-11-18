/* Estimated 6DoF hand pose from a single Joy-Con IMU.
 *
 * Joy-Con 2 has no outside-in/inside-out optical reference (the mouse sensor
 * only works on a surface), so true absolute position is unobservable from
 * its accelerometer + gyroscope alone: double integration drifts by metres
 * within seconds. This tracker therefore fuses three estimators:
 *
 *  1. Orientation: Horizon's fused attitude, filtered by an adaptive
 *     One-Euro slerp (no jitter at rest, no lag in swings) and extrapolated
 *     by the bias-corrected gyro to hide input->photon latency.
 *  2. Biomechanical arm model: shoulder -> elbow -> wrist chain whose elbow
 *     follows part of the hand's yaw/pitch, giving a plausible position for
 *     every orientation (the low-frequency / absolute part of position).
 *  3. Inertial residual: gravity-free accelerometer minus the acceleration
 *     the arm model already explains, double-integrated with a leaky,
 *     spring-damped integrator and zero-velocity updates at rest. This adds
 *     genuine translational motion (punches, lateral slashes, reaching)
 *     on top of the arm model, then decays back to it without drift.
 *
 * Frames: OpenXR axes (+Y up, -Z forward), relative to the player's
 * recentered heading. Positions are metres relative to the head origin.
 * Portable C11 so it can be host-tested without libnx.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_JOYCON_TRACKER_H
#define BS_JOYCON_TRACKER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BsJcTrackerConfig {
    float shoulder[3];          /* hand-side shoulder, head-relative (m) */
    float upper_arm;            /* shoulder -> elbow (m) */
    float forearm;              /* elbow -> grip (m) */
    float elbow_ratio;          /* fraction of the hand's swing the elbow takes */
    float smooth_min_cutoff_hz; /* One-Euro: cutoff at rest */
    float smooth_beta;          /* One-Euro: cutoff gain per rad/s */
    float predict_s;            /* gyro extrapolation horizon */
    float gyro_scale;           /* sensor units -> rad/s (libnx: rev/s) */
    float inertial_gain;        /* 0 disables the accelerometer residual */
    float inertial_vel_tau_s;   /* velocity leak time constant */
    float inertial_pos_tau_s;   /* spring back to the arm model */
    float inertial_max_m;       /* clamp on residual offset */
} BsJcTrackerConfig;

typedef struct BsJcTracker {
    BsJcTrackerConfig cfg;
    int hand;                   /* 0 left, 1 right */
    int initialized;
    float q_filt[4];            /* smoothed aim orientation (xyzw) */
    float omega_player[3];      /* bias-corrected angular velocity, rad/s */
    float gyro_bias[3];         /* sensor-frame bias, sensor units */
    float gravity[3];           /* player-frame gravity estimate, g */
    float still_time_s;
    float model_pos[3][3];      /* last 3 arm-model positions (newest [0]) */
    float model_dt[2];          /* intervals between those positions */
    int model_count;
    float unreliable_s;         /* hold-off after sensor clipping */
    float model_acc[3];         /* low-passed arm-model acceleration */
    float lin_acc[3];           /* low-passed gravity-free acceleration */
    float vel[3];               /* residual velocity, m/s */
    float offset[3];            /* residual position, m */
} BsJcTracker;

void bs_jc_tracker_default_config(BsJcTrackerConfig *cfg, int hand);
void bs_jc_tracker_reset(BsJcTracker *t, const BsJcTrackerConfig *cfg, int hand);

/* q_aim:   rendered aim orientation in the player frame (xyzw).
 * q_world: true sensor attitude in the player frame (gravity is +Y), used
 *          for gravity removal; usually q_aim without the cosmetic aim base.
 * accel_g / gyro: raw Nintendo sensor-frame samples. dt: seconds since the
 * previous update (0 or a repeated HID sample is accepted). */
void bs_jc_tracker_update(BsJcTracker *t, const float q_aim[4],
                          const float q_world[4], const float accel_g[3],
                          const float gyro[3], float dt);

/* Predicted pose: orientation (xyzw) and head-relative position (m). */
void bs_jc_tracker_pose(const BsJcTracker *t, float q_out[4], float p_out[3]);

/* 1 while the controller has been at rest long enough for ZUPT. */
int bs_jc_tracker_is_still(const BsJcTracker *t);

#ifdef __cplusplus
}
#endif

#endif
