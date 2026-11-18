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
    cfg->shoulder[1] = -0.12f;
    cfg->shoulder[2] = 0.02f;
    cfg->upper_arm = 0.27f;
    cfg->forearm = 0.33f;
    cfg->elbow_yaw_ratio = 0.45f;
    cfg->elbow_pitch_ratio = 0.30f;
    cfg->smooth_min_cutoff_hz = 1.2f;
    cfg->smooth_beta = 6.0f;
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
    t->gravity[1] = 1.0f;
}

/* Shoulder -> elbow -> grip. The elbow swings with a fraction of the hand's
 * heading and elevation, as real arms do, instead of staying fixed. */
static V arm_model(const BsJcTrackerConfig *c, Q q) {
    V f = qr(q, (V){0, 0, -1});
    float yaw = atan2f(-f.x, -f.z);
    float fy = f.y > 1 ? 1 : f.y < -1 ? -1 : f.y;
    float pitch = asinf(fy);
    float ey = yaw * c->elbow_yaw_ratio * 0.5f, ep = pitch * c->elbow_pitch_ratio * 0.5f;
    Q qy = {0, sinf(ey), 0, cosf(ey)};
    Q qp = {sinf(ep), 0, 0, cosf(ep)};
    V elbow = va(vload(c->shoulder), qr(qm(qy, qp), (V){0, -c->upper_arm, 0}));
    return va(elbow, qr(q, (V){0, 0, -c->forearm}));
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
        vstore(a_player, t->gravity);
        t->initialized = 1;
    }

    /* Bias-corrected angular velocity, expressed in the player frame. */
    float g_corr[3] = {(gyro[0] - t->gyro_bias[0]) * c->gyro_scale,
                       (gyro[1] - t->gyro_bias[1]) * c->gyro_scale,
                       (gyro[2] - t->gyro_bias[2]) * c->gyro_scale};
    V w_player = qr(q_world, sensor_vec_to_xr(g_corr));
    float w_mag = vlen(w_player);

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
    vstore(w_player, t->omega_player);

    /* One-Euro orientation filter: cutoff rises with angular speed. */
    if (dt > 0) {
        float cutoff = c->smooth_min_cutoff_hz + c->smooth_beta * w_mag;
        float tau = 1.0f / (2.0f * JT_PI * cutoff);
        float alpha = 1.0f / (1.0f + tau / dt);
        qstore(qn(qslerp(qload(t->q_filt), q_aim, alpha)), t->q_filt);
    }

    /* Inertial residual: what the accelerometer measures beyond the arm
     * model's own (rotation-induced) acceleration is real translation. */
    if (dt <= 0) return;
    V model = arm_model(c, qload(t->q_filt));
    memmove(t->model_pos[1], t->model_pos[0], sizeof(t->model_pos[0]) * 2);
    vstore(model, t->model_pos[0]);
    if (t->model_count < 3) { ++t->model_count; return; }

    V p0 = vload(t->model_pos[0]), p1 = vload(t->model_pos[1]), p2 = vload(t->model_pos[2]);
    V a_model = vk(va(vs(p0, vk(p1, 2.0f)), p2), 1.0f / (dt * dt));
    V grav = vload(t->gravity);
    /* Sign of gravity tells whether the sensor reports specific force
     * (+1 g up at rest) or the gravity vector itself. */
    float sign = grav.y >= 0 ? 1.0f : -1.0f;
    V a_lin = vk(vs(a_player, grav), sign * JT_G);
    float k = lp_alpha(dt, JT_ACC_LP_TAU_S);
    vstore(vlerp(vload(t->model_acc), a_model, k), t->model_acc);
    vstore(vlerp(vload(t->lin_acc), a_lin, k), t->lin_acc);
    V residual = vk(vs(vload(t->lin_acc), vload(t->model_acc)), c->inertial_gain);
    float rmag = vlen(residual);
    if (rmag < JT_RESIDUAL_DEADBAND) residual = (V){0, 0, 0};
    else residual = vk(residual, (rmag - JT_RESIDUAL_DEADBAND) / rmag);

    V vel = vload(t->vel), off = vload(t->offset);
    vel = va(vel, vk(residual, dt));
    vel = vk(vel, expf(-dt / c->inertial_vel_tau_s));
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
    V w = vload(t->omega_player);
    q = qn(qm(qexp(w, c->predict_s), q));
    V p = arm_model(c, q);
    p = va(p, va(vload(t->offset), vk(vload(t->vel), c->predict_s)));
    qstore(q, q_out);
    vstore(p, p_out);
}

int bs_jc_tracker_is_still(const BsJcTracker *t) {
    return t->still_time_s >= JT_STILL_S;
}
