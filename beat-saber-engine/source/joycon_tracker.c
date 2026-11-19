/* Estimated 6DoF Joy-Con hand tracking. See joycon_tracker.h.
 * SPDX-License-Identifier: MIT
 */
#include "joycon_tracker.h"

#include <math.h>
#include <string.h>

#define JT_G 9.80665f
#define JT_PI 3.14159265f

/* Rest detection: residual rotation below this and |accel| within this of
 * 1 g, sustained for JT_STILL_S, enables gyro-bias learning and ZUPT. */
#define JT_STILL_GYRO_RAD_S 0.10f
#define JT_STILL_ACCEL_G    0.05f
#define JT_STILL_S          0.15f
#define JT_BIAS_TAU_S       1.5f
#define JT_GRAVITY_TAU_S    0.6f
#define JT_ACC_LP_TAU_S     0.025f
#define JT_RESIDUAL_DEADBAND 0.35f   /* m/s^2: below this treat as noise */
#define JT_MAX_DT           0.1f
/* Large-swing robustness. */
#define JT_SWING_RAD_S      4.0f     /* rotation where translation trust halves */
#define JT_ACCEL_CLIP_G     7.5f     /* Joy-Con accelerometer range is +-8 g */
#define JT_GYRO_CLIP_RAD_S  33.0f    /* gyro range +-2000 dps = 34.9 rad/s */
#define JT_CLIP_HOLD_S      0.12f
#define JT_PREDICT_MAX_RAD  0.20f

typedef struct { float x, y, z, w; } Q;
typedef struct { float x, y, z; } V;

static Q qm(Q a, Q b) {
    return (Q){a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
               a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
               a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
               a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
static Q qc(Q a) { return (Q){-a.x, -a.y, -a.z, a.w}; }
static Q qn(Q a) {
    float n = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z + a.w*a.w);
    if (!(n > 1e-6f) || !isfinite(n)) return (Q){0, 0, 0, 1};
    return (Q){a.x/n, a.y/n, a.z/n, a.w/n};
}
static V qr(Q q, V v) {
    Q r = qm(qm(q, (Q){v.x, v.y, v.z, 0}), qc(q));
    return (V){r.x, r.y, r.z};
}
static Q qload(const float *f) { return qn((Q){f[0], f[1], f[2], f[3]}); }
static void qstore(Q q, float *f) { f[0] = q.x; f[1] = q.y; f[2] = q.z; f[3] = q.w; }
static V vload(const float *f) { return (V){f[0], f[1], f[2]}; }
static void vstore(V v, float *f) { f[0] = v.x; f[1] = v.y; f[2] = v.z; }
static V va(V a, V b) { return (V){a.x+b.x, a.y+b.y, a.z+b.z}; }
static V vs(V a, V b) { return (V){a.x-b.x, a.y-b.y, a.z-b.z}; }
static V vk(V a, float k) { return (V){a.x*k, a.y*k, a.z*k}; }
static float vlen(V a) { return sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); }
static V vlerp(V a, V b, float t) { return va(a, vk(vs(b, a), t)); }

/* Nintendo sensor axes -> OpenXR axes (x->x, y->-z, z->y), matching the
 * quaternion relabelling done by the runtime's sensor_to_xr(). */
static V sensor_vec_to_xr(const float *s) { return (V){s[0], s[2], -s[1]}; }

/* Rotation by angle |w|*t about w (world/left-multiplied). */
static Q qexp(V w, float t) {
    float a = vlen(w) * t;
    if (!(a > 1e-7f)) return (Q){0, 0, 0, 1};
    float s = sinf(a * 0.5f) / vlen(w);
    return (Q){w.x * s, w.y * s, w.z * s, cosf(a * 0.5f)};
}

static Q qslerp(Q a, Q b, float t) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (d < 0) { b = (Q){-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f)
        return qn((Q){a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
                      a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t});
    float th = acosf(d), s = sinf(th);
    float wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    return (Q){a.x*wa + b.x*wb, a.y*wa + b.y*wb, a.z*wa + b.z*wb, a.w*wa + b.w*wb};
}

static float lp_alpha(float dt, float tau) {
    return dt <= 0 ? 0.0f : 1.0f - expf(-dt / tau);
}

void bs_jc_tracker_default_config(BsJcTrackerConfig *cfg, int hand) {
    memset(cfg, 0, sizeof(*cfg));
    /* Tuned so the recentered default aim lands near the previous fixed
     * elbow model's hand position (seated flat-screen layout). */
    cfg->shoulder[0] = hand ? 0.19f : -0.19f;
    cfg->shoulder[1] = -0.08f;
    cfg->shoulder[2] = -0.05f;
    cfg->upper_arm = 0.27f;
    cfg->forearm = 0.33f;
    cfg->elbow_ratio = 0.40f;
    cfg->elbow_yaw_ratio = 0.55f;
    cfg->tilt_tau_s = 0.25f;
    cfg->yaw_tau_s = 2.0f;
    /* Gyro integration is already smooth; this filter now only has to hide
     * residual noise, so it can open up sooner (less lag in swings). */
    cfg->smooth_min_cutoff_hz = 1.5f;
    cfg->smooth_beta = 12.0f;
    cfg->predict_s = 0.030f;
    cfg->gyro_scale = 2.0f * JT_PI;       /* libnx reports revolutions/s */
    cfg->inertial_gain = 1.0f;
    cfg->inertial_vel_tau_s = 0.30f;
    cfg->inertial_pos_tau_s = 0.45f;
    cfg->inertial_max_m = 0.30f;
}

void bs_jc_tracker_reset(BsJcTracker *t, const BsJcTrackerConfig *cfg, int hand) {
    memset(t, 0, sizeof(*t));
    if (cfg) t->cfg = *cfg;
    else bs_jc_tracker_default_config(&t->cfg, hand);
    t->hand = hand;
    t->q_filt[3] = 1.0f;
    t->q_est[3] = 1.0f;
    t->q_prev_aim[3] = 1.0f;
    t->gravity[1] = 1.0f;
    t->gyro_scale_est = t->cfg.gyro_scale;
}

static V vcross(V a, V b) {
    return (V){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static float vdot(V a, V b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

/* Shortest-arc rotation taking unit vector a to unit vector b. */
static Q qarc(V a, V b) {
    float d = vdot(a, b);
    if (d < -0.9999f) {                       /* opposite: any perpendicular */
        V axis = vcross(a, (V){1, 0, 0});
        if (vlen(axis) < 1e-3f) axis = vcross(a, (V){0, 1, 0});
        axis = vk(axis, 1.0f / vlen(axis));
        return (Q){axis.x, axis.y, axis.z, 0};
    }
    V c = vcross(a, b);
    return qn((Q){c.x, c.y, c.z, 1.0f + d});
}

/* Shoulder -> elbow -> grip. The elbow follows a fraction of the hand's
 * *swing* (direction change from the default aim; wrist twist ignored).
 * Built from a shortest-arc quaternion rather than Euler yaw/pitch, so it is
 * continuous when the Joy-Con passes straight up/down in overhead swings
 * (atan2 yaw flips by 180 degrees there and made the hand teleport). */
static V arm_model(const BsJcTrackerConfig *c, Q q) {
    static const V f0 = {0.0f, 0.42594f, -0.90475f};   /* default aim, 0.44 rad up */
    V f = qr(q, (V){0, 0, -1});
    Q swing = qarc(f0, f);
    /* Swing axis near vertical = horizontal slash: let the elbow take more
     * of it. Weighted by axis, so still continuous everywhere. */
    float sn = sqrtf(swing.x * swing.x + swing.y * swing.y + swing.z * swing.z);
    float horiz = sn > 1e-6f ? (swing.y / sn) * (swing.y / sn) : 0.0f;
    float ratio = c->elbow_ratio + (c->elbow_yaw_ratio - c->elbow_ratio) * horiz;
    Q elbow_rot = qslerp((Q){0, 0, 0, 1}, swing, ratio);
    V elbow = va(vload(c->shoulder), qr(elbow_rot, (V){0, -c->upper_arm, 0}));
    return va(elbow, vk(f, c->forearm));
}

void bs_jc_tracker_update(BsJcTracker *t, const float q_aim_f[4],
                          const float q_world_f[4], const float accel_g[3],
                          const float gyro[3], float dt) {
    const BsJcTrackerConfig *c = &t->cfg;
    if (!(dt >= 0) || !isfinite(dt)) dt = 0;
    if (dt > JT_MAX_DT) dt = JT_MAX_DT;
    for (int i = 0; i < 3; ++i)
        if (!isfinite(accel_g[i]) || !isfinite(gyro[i])) return;
    Q q_aim = qload(q_aim_f), q_world = qload(q_world_f);

    V a_player = qr(q_world, sensor_vec_to_xr(accel_g));
    if (!t->initialized) {
        qstore(q_aim, t->q_filt);
        qstore(q_aim, t->q_est);
        qstore(q_aim, t->q_prev_aim);
        vstore(a_player, t->gravity);
        t->initialized = 1;
    }

    /* Self-calibrate gyro units against Horizon's own rotation, so a wrong
     * unit assumption can't make gyro-driven rotation too fast or slow.
     * Only sustained, unclipped rotation is used. */
    float raw[3] = {gyro[0] - t->gyro_bias[0], gyro[1] - t->gyro_bias[1],
                    gyro[2] - t->gyro_bias[2]};
    float raw_mag = sqrtf(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
    {
        Q pa = qload(t->q_prev_aim);
        float d = fabsf(pa.x*q_aim.x + pa.y*q_aim.y + pa.z*q_aim.z + pa.w*q_aim.w);
        float ang = 2.0f * acosf(d > 1.0f ? 1.0f : d);
        float rate = raw_mag * t->gyro_scale_est;
        int clipped = vlen(a_player) > JT_ACCEL_CLIP_G;
        for (int i = 0; i < 3; ++i)
            if (fabsf(gyro[i]) * t->gyro_scale_est > JT_GYRO_CLIP_RAD_S) clipped = 1;
        if (dt > 0 && !clipped && rate > 1.5f && ang < 1.5f) {
            t->scale_h = t->scale_h * 0.99f + ang;
            t->scale_g = t->scale_g * 0.99f + raw_mag * dt;
            if (t->scale_g > 0.3f) {
                float s = t->scale_h / t->scale_g;
                if (s > 0.01f && s < 12.0f) t->gyro_scale_est = s;
            }
        }
        qstore(q_aim, t->q_prev_aim);
    }

    /* Bias-corrected angular velocity, expressed in the player frame. */
    float g_corr[3] = {raw[0] * t->gyro_scale_est, raw[1] * t->gyro_scale_est,
                       raw[2] * t->gyro_scale_est};
    V w_player = qr(q_world, sensor_vec_to_xr(g_corr));
    float w_mag = vlen(w_player);

    /* Gyro-driven orientation. Horizon's fused attitude re-anchors tilt
     * fast (gravity makes it trustworthy) but yaw only slowly: its yaw is
     * gyro-only and passes through its smoothing / zero-drift filter, which
     * made horizontal motion lag. Swing-twist split of the error about
     * world up separates the two. Large errors (after clipping) snap back. */
    if (dt > 0) {
        Q e = qload(t->q_est);
        e = qn(qm(qexp(w_player, dt), e));
        Q err = qm(q_aim, qc(e));
        if (err.w < 0) err = (Q){-err.x, -err.y, -err.z, -err.w};
        Q twist = qn((Q){0, err.y, 0, err.w});
        Q tilt = qm(err, qc(twist));
        float err_ang = 2.0f * acosf(err.w > 1.0f ? 1.0f : err.w);
        float at = lp_alpha(dt, c->tilt_tau_s), ay = lp_alpha(dt, c->yaw_tau_s);
        if (err_ang > 0.5f) { at = ay = lp_alpha(dt, 0.05f); }
        Q ident = {0, 0, 0, 1};
        e = qn(qm(qm(qslerp(ident, tilt, at), qslerp(ident, twist, ay)), e));
        qstore(e, t->q_est);
    }

    /* Stillness: learns gyro bias and gravity, and gates zero-velocity. */
    float a_mag = vlen(a_player);
    if (w_mag < JT_STILL_GYRO_RAD_S && fabsf(a_mag - 1.0f) < JT_STILL_ACCEL_G)
        t->still_time_s += dt;
    else
        t->still_time_s = 0;
    int still = t->still_time_s >= JT_STILL_S;
    if (still) {
        float kb = lp_alpha(dt, JT_BIAS_TAU_S);
        for (int i = 0; i < 3; ++i) t->gyro_bias[i] += (gyro[i] - t->gyro_bias[i]) * kb;
        vstore(vlerp(vload(t->gravity), a_player, lp_alpha(dt, JT_GRAVITY_TAU_S)),
               t->gravity);
    }
    /* Prediction velocity: lightly smoothed so an abrupt direction change
     * doesn't flip the extrapolation from +cap to -cap in one sample. */
    vstore(vlerp(vload(t->omega_player), w_player, lp_alpha(dt, 0.015f)),
           t->omega_player);

    /* One-Euro orientation filter: cutoff rises with angular speed. */
    if (dt > 0) {
        float cutoff = c->smooth_min_cutoff_hz + c->smooth_beta * w_mag;
        float tau = 1.0f / (2.0f * JT_PI * cutoff);
        float alpha = 1.0f / (1.0f + tau / dt);
        qstore(qn(qslerp(qload(t->q_filt), qload(t->q_est), alpha)), t->q_filt);
    }

    /* Inertial residual: what the accelerometer measures beyond the arm
     * model's own (rotation-induced) acceleration is real translation. */
    if (dt <= 0) return;
    V model = arm_model(c, qload(t->q_filt));
    memmove(t->model_pos[1], t->model_pos[0], sizeof(t->model_pos[0]) * 2);
    vstore(model, t->model_pos[0]);
    t->model_dt[1] = t->model_dt[0];
    t->model_dt[0] = dt;
    if (t->model_count < 3) { ++t->model_count; return; }

    /* Non-uniform second difference: poll intervals are not constant. */
    V p0 = vload(t->model_pos[0]), p1 = vload(t->model_pos[1]), p2 = vload(t->model_pos[2]);
    float d0 = t->model_dt[0], d1 = t->model_dt[1] > 0 ? t->model_dt[1] : dt;
    V a_model = vk(vs(vk(vs(p0, p1), 1.0f / d0), vk(vs(p1, p2), 1.0f / d1)),
                   2.0f / (d0 + d1));

    /* Reliability gate. In fast rotation the accelerometer is dominated by
     * centripetal terms (w^2 * r): any arm-length mismatch, at 20 rad/s
     * even 5 cm, reads as ~20 m/s^2 of fake translation, and big swings
     * also clip the +-8 g accelerometer / +-2000 dps gyro. Translation is
     * therefore only trusted while rotation is moderate, and fully
     * suppressed (with a hold-off for the low-pass tails) when clipping. */
    int saturated = a_mag > JT_ACCEL_CLIP_G;
    for (int i = 0; i < 3; ++i)
        if (fabsf(gyro[i]) * c->gyro_scale > JT_GYRO_CLIP_RAD_S) saturated = 1;
    if (saturated) t->unreliable_s = JT_CLIP_HOLD_S;
    else if (t->unreliable_s > 0) t->unreliable_s -= dt;
    float wr = w_mag / JT_SWING_RAD_S;
    float gate = t->unreliable_s > 0 ? 0.0f : 1.0f / (1.0f + wr * wr);
    V grav = vload(t->gravity);
    /* Sign of gravity tells whether the sensor reports specific force
     * (+1 g up at rest) or the gravity vector itself. */
    float sign = grav.y >= 0 ? 1.0f : -1.0f;
    V a_lin = vk(vs(a_player, grav), sign * JT_G);
    float k = lp_alpha(dt, JT_ACC_LP_TAU_S);
    vstore(vlerp(vload(t->model_acc), a_model, k), t->model_acc);
    vstore(vlerp(vload(t->lin_acc), a_lin, k), t->lin_acc);
    V residual = vk(vs(vload(t->lin_acc), vload(t->model_acc)), c->inertial_gain * gate);
    float rmag = vlen(residual);
    if (rmag < JT_RESIDUAL_DEADBAND) residual = (V){0, 0, 0};
    else residual = vk(residual, (rmag - JT_RESIDUAL_DEADBAND) / rmag);

    V vel = vload(t->vel), off = vload(t->offset);
    vel = va(vel, vk(residual, dt));
    /* Leak faster while untrusted so no stale velocity carries through. */
    float vtau = c->inertial_vel_tau_s * (0.15f + 0.85f * gate);
    vel = vk(vel, expf(-dt / vtau));
    if (still) vel = (V){0, 0, 0};                   /* ZUPT */
    off = va(off, vk(vel, dt));
    off = vk(off, expf(-dt / c->inertial_pos_tau_s)); /* spring to arm model */
    float omag = vlen(off);
    if (omag > c->inertial_max_m) {
        off = vk(off, c->inertial_max_m / omag);
        vel = (V){0, 0, 0};
    }
    if (!isfinite(vlen(vel)) || !isfinite(vlen(off))) {
        vel = off = (V){0, 0, 0};
    }
    vstore(vel, t->vel);
    vstore(off, t->offset);
}

void bs_jc_tracker_pose(const BsJcTracker *t, float q_out[4], float p_out[3]) {
    const BsJcTrackerConfig *c = &t->cfg;
    Q q = qload(t->q_filt);
    /* Cap the extrapolated angle: uncapped, 30 ms at swing speed is ~40
     * degrees of lead that visibly overshoots when the swing stops. */
    V w = vload(t->omega_player);
    float wm = vlen(w), horizon = c->predict_s;
    if (wm * horizon > JT_PREDICT_MAX_RAD) horizon = JT_PREDICT_MAX_RAD / wm;
    q = qn(qm(qexp(w, horizon), q));
    V p = arm_model(c, q);
    p = va(p, va(vload(t->offset), vk(vload(t->vel), c->predict_s)));
    qstore(q, q_out);
    vstore(p, p_out);
}

int bs_jc_tracker_is_still(const BsJcTracker *t) {
    return t->still_time_s >= JT_STILL_S;
}
