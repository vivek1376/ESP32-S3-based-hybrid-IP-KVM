/**
 * overlay.h — [TODO-1] Feedforward Radial Overlay Configuration & Interface
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include "kvm_config.h"

#include <SDL2/SDL.h>
#include <stdbool.h>

// --- [TODO-1] Feedforward Radial Overlay Configuration ---
#define OVERLAY_DIAMETER_RATIO  0.18f                       // 18% of window height (194.4 px)
#define OVERLAY_R0              (WIN_H * OVERLAY_DIAMETER_RATIO * 0.5f) // 97.2 px nominal radius
#define OVERLAY_R_MIN           4.0f                        // Collapsed singularity radius (px)
#define OVERLAY_CENTER_X        1620.0f                     // Top-Right Quadrant X anchor (px)
#define OVERLAY_CENTER_Y        260.0f                      // Top-Right Quadrant Y anchor (px)

// Spatial Hysteresis (Strictly anchored to nominal OVERLAY_R0, never R_curr!)
#define OVERLAY_ZONE_IN         (1.40f * OVERLAY_R0)        // 136.1 px entry threshold
#define OVERLAY_ZONE_OUT        (1.50f * OVERLAY_R0)        // 145.8 px exit threshold (hysteresis)

// Reeling-In Rotation Kinematics
#define OVERLAY_BASE_SPEED      2.20f                       // CCW rotation speed (rad/s)
#define OVERLAY_SPIN_TAU        0.20f                       // Exponential spin-up/down time constant (s)
#define OVERLAY_NUM_DOTS        8                           // Number of dots in the dotted circle
#define OVERLAY_DOT_RADIUS      12.0f                       // Fixed radius of each dot (px)

// 1-Euro Filter Parameters (Pointer Position Smoothing)
#define ONEEURO_MIN_CUTOFF      1.0f                        // Minimum cutoff frequency fc_min (Hz)
#define ONEEURO_BETA            0.007f                      // Speed coefficient beta
#define ONEEURO_D_CUTOFF        1.0f                        // Derivative cutoff frequency (Hz)

// Engagement Control Law Parameters
#define OVERLAY_D_FLOOR         (0.30f * OVERLAY_R0)        // Clamp denominator near center (29.2 px)
#define OVERLAY_PSI_ALPHA       2.50f                       // Radial velocity penalty in psi
#define OVERLAY_PSI_EPSILON     625.0f                      // Low-speed noise squelch (25 px/s)^2
#define OVERLAY_OMEGA_DEADBAND  0.35f                       // Min CCW angular speed to wind (rad/s)
#define OVERLAY_K_IN            0.22f                       // CCW winding gain (1/rad)
#define OVERLAY_GAMMA_OPEN      0.65f                       // Spring restoration rate (1/s)
#define OVERLAY_BETA_STIFF      0.80f                       // Non-linear spring stiffening factor
#define OVERLAY_K_CW            0.00f                       // Active CW unwind gain (0 = use spring only)

// Pop-Out Concentric Annuli Parameters
#define MENU_OPTION_COUNT       2                           // 0: Grayscale Toggle, 1: Save Screenshot
#define POPOUT_EXPLODE_SEC      0.18f                       // 180 ms initial ballistic explosion
#define POPOUT_DWELL_SEC        0.32f                       // 320 ms hover dwell to commit action
#define POPOUT_BAND_WIDTH       52.0f                       // Radial thickness of each annulus (px)
#define POPOUT_BAND_GAP         20.0f                       // Radial gap between annuli (px)
#define POPOUT_START_INNER_R    130.0f                      // Band 0 inner radius after explosion (px)
#define POPOUT_DRIFT_SPEED      45.0f                       // Outward slow drift speed (px/s)
#define POPOUT_BL_MARGIN_RATIO  0.16f                       // 16% of viewport diagonal off bottom-left

typedef enum {
    OVERLAY_DORMANT = 0,
    OVERLAY_REELING_IN,
    OVERLAY_ENGAGEMENT,
    OVERLAY_POPOUT
} OverlayPhase;

typedef struct {
    OverlayPhase phase;
    float u;          // Compression state [0.0f, 1.0f]
    float omega_vis;  // Visual CCW rotation speed (rad/s)
    float theta_vis;  // Visual rotation angle (rad)
} OverlayState;

void overlay_init(OverlayState *ov);
void overlay_update(OverlayState *ov, int raw_x, int raw_y, bool cursor_in_window, float dt);
void overlay_render(const OverlayState *ov, SDL_Renderer *ren);

#endif // OVERLAY_H

