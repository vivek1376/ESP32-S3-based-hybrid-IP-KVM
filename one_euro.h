/**
 * one_euro.h — Real-Time 2D 1-Euro Filter for Quantized X11 Pointer Smoothing
 *
 * Implements Section 3 of the [TODO-1] Feedforward Radial Overlay Specification.
 */

#ifndef ONE_EURO_H
#define ONE_EURO_H

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x_prev;
    float dx_prev;
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
                                    float min_cutoff, float beta, float d_cutoff,
                                    float *out_x_hat, float *out_dx_hat) {
    // Derivative estimation & filtering
    float raw_dx = (x_raw - f->x_prev) / dt;
    float a_d = one_euro_alpha(d_cutoff, dt);
    float dx_hat = a_d * raw_dx + (1.0f - a_d) * f->dx_prev;

    // Adaptive cutoff & position filtering
    float cutoff = min_cutoff + beta * fabsf(dx_hat);
    float a_x = one_euro_alpha(cutoff, dt);
    float x_hat = a_x * x_raw + (1.0f - a_x) * f->x_prev;

    f->x_prev = x_hat;
    f->dx_prev = dx_hat;

    *out_x_hat = x_hat;
    *out_dx_hat = dx_hat;
}

static inline void one_euro_2d_reset(OneEuroFilter2D *f) {
    f->x.x_prev = 0.0f;
    f->x.dx_prev = 0.0f;
    f->y.x_prev = 0.0f;
    f->y.dx_prev = 0.0f;
    f->initialized = false;
}

static inline void one_euro_2d_update(OneEuroFilter2D *f,
                                      float raw_x, float raw_y, float dt,
                                      float min_cutoff, float beta, float d_cutoff,
                                      float *out_x, float *out_y,
                                      float *out_vx, float *out_vy) {
    if (!f->initialized) {
        f->x.x_prev = raw_x;
        f->x.dx_prev = 0.0f;
        f->y.x_prev = raw_y;
        f->y.dx_prev = 0.0f;
        f->initialized = true;

        *out_x = raw_x;
        *out_y = raw_y;
        *out_vx = 0.0f;
        *out_vy = 0.0f;
        return;
    }

    one_euro_1d_step(&f->x, raw_x, dt, min_cutoff, beta, d_cutoff, out_x, out_vx);
    one_euro_1d_step(&f->y, raw_y, dt, min_cutoff, beta, d_cutoff, out_y, out_vy);
}

#endif // ONE_EURO_H

