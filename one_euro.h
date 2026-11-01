/**
 * one_euro.h — Phase-Consistent 2D 1-Euro Filter for Quantized X11 Pointer Kinematics
 *
 * Fixes two compounding control-dynamics defects in naive 1-Euro differentiation:
 *  1. Computes the internal speed-estimator finite difference from consecutive RAW
 *     samples (x_raw - x_raw_prev) / dt per Casiez et al. (CHI 2012), preventing
 *     the (1 + tau_x / dt) gain inflation caused by subtracting lagged x_hat.
 *  2. Derives kinematic velocity directly from a matched C1 LTI post-stage on x_hat
 *     paired with the chord midpoint 0.5 * (x_sync + x_sync_prev), eliminating the
 *     45-65 deg phase lag between position and the 1 Hz internal speed envelope that
 *     otherwise leaked tangential orbital motion into spurious radial velocity v_r.
 */

#ifndef ONE_EURO_H
#define ONE_EURO_H

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x_raw_prev;   // Previous raw sample (for true Casiez derivative)
    float x_prev;       // Previous 1-Euro adaptive-filtered position
    float dx_prev;      // Previous 1 Hz speed estimate (for adaptive cutoff only)
    float x_sync_prev;  // Previous C1-smoothed phase-locked position
} OneEuroFilter1D;

typedef struct {
    OneEuroFilter1D x;
    OneEuroFilter1D y;
    bool initialized;
} OneEuroFilter2D;

static inline float one_euro_alpha(float cutoff_hz, float dt) {
    float tau = 1.0f / (2.0f * (float)M_PI * cutoff_hz);
    return 1.0f / (1.0f + tau / dt);
}

static inline void one_euro_1d_step(OneEuroFilter1D *f, float x_raw, float dt,
                                    float min_cutoff, float beta,
                                    float d_cutoff, float sync_cutoff,
                                    float *out_x_sync, float *out_x_mid,
                                    float *out_vx_sync) {
    // 1. Internal derivative estimation & filtering (solely for adaptive cutoff modulation)
    float raw_dx = (x_raw - f->x_raw_prev) / dt;
    float a_d = one_euro_alpha(d_cutoff, dt);
    float dx_est = a_d * raw_dx + (1.0f - a_d) * f->dx_prev;

    // 2. Speed-adaptive 1-Euro position low-pass filtering
    float cutoff = min_cutoff + beta * fabsf(dx_est);
    float a_x = one_euro_alpha(cutoff, dt);
    float x_hat = a_x * x_raw + (1.0f - a_x) * f->x_prev;

    // 3. Matched C1 post-stage + midpoint chord kinematics (zero r-vs-v phase lag)
    float a_sync = one_euro_alpha(sync_cutoff, dt);
    float x_sync = a_sync * x_hat + (1.0f - a_sync) * f->x_sync_prev;
    float vx_sync = (x_sync - f->x_sync_prev) / dt;
    float x_mid = 0.5f * (x_sync + f->x_sync_prev);

    f->x_raw_prev = x_raw;
    f->x_prev = x_hat;
    f->dx_prev = dx_est;
    f->x_sync_prev = x_sync;

    *out_x_sync = x_sync;
    *out_x_mid = x_mid;
    *out_vx_sync = vx_sync;
}

static inline void one_euro_2d_reset(OneEuroFilter2D *f) {
    f->x.x_raw_prev = 0.0f;
    f->x.x_prev = 0.0f;
    f->x.dx_prev = 0.0f;
    f->x.x_sync_prev = 0.0f;

    f->y.x_raw_prev = 0.0f;
    f->y.x_prev = 0.0f;
    f->y.dx_prev = 0.0f;
    f->y.x_sync_prev = 0.0f;

    f->initialized = false;
}

static inline void one_euro_2d_update(OneEuroFilter2D *f,
                                      float raw_x, float raw_y, float dt,
                                      float min_cutoff, float beta,
                                      float d_cutoff, float sync_cutoff,
                                      float *out_x, float *out_y,
                                      float *out_x_mid, float *out_y_mid,
                                      float *out_vx, float *out_vy) {
    if (!f->initialized) {
        f->x.x_raw_prev = raw_x;
        f->x.x_prev = raw_x;
        f->x.dx_prev = 0.0f;
        f->x.x_sync_prev = raw_x;

        f->y.x_raw_prev = raw_y;
        f->y.x_prev = raw_y;
        f->y.dx_prev = 0.0f;
        f->y.x_sync_prev = raw_y;

        f->initialized = true;

        *out_x = raw_x;
        *out_y = raw_y;
        *out_x_mid = raw_x;
        *out_y_mid = raw_y;
        *out_vx = 0.0f;
        *out_vy = 0.0f;
        return;
    }

    one_euro_1d_step(&f->x, raw_x, dt, min_cutoff, beta, d_cutoff, sync_cutoff,
                     out_x, out_x_mid, out_vx);
    one_euro_1d_step(&f->y, raw_y, dt, min_cutoff, beta, d_cutoff, sync_cutoff,
                     out_y, out_y_mid, out_vy);
}

#endif // ONE_EURO_H
