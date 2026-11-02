/**
 * overlay.h — [TODO-1] Feedforward Radial Overlay Configuration & Interface
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include "kvm_config.h"
#include "one_euro.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

// --- [TODO-1] Feedforward Radial Overlay Configuration ---
#define OVERLAY_DIAMETER_RATIO  0.18f                       // 18% of window height (194.4 px)
#define OVERLAY_R0              (WIN_H * OVERLAY_DIAMETER_RATIO * 0.5f) // 97.2 px nominal radius
#define OVERLAY_R_MIN           4.0f                        // Collapsed singularity radius (px)
#define OVERLAY_CENTER_X        1620.0f                     // Top-Right Quadrant X anchor (px)
#define OVERLAY_CENTER_Y        260.0f                      // Top-Right Quadrant Y anchor (px)

// Spatial Hysteresis & Reeling-In Zones (Strictly anchored to nominal OVERLAY_R0, never R_curr!)
#define OVERLAY_ENGAGEMENT_ZONE_IN   (1.82f * OVERLAY_R0)                  // 176.9 px entry threshold (+30% from 1.40*R0)
#define OVERLAY_ENGAGEMENT_ZONE_OUT  (1.95f * OVERLAY_R0)                  // 189.5 px exit threshold (+30% from 1.50*R0)
#define OVERLAY_REELING_ZONE         (1.24f * OVERLAY_ENGAGEMENT_ZONE_OUT) // 235.0 px reeling-in radius (+24% over ZONE_OUT)

// Reeling-In Rotation Kinematics
#define OVERLAY_BASE_SPEED      2.20f                       // CCW rotation speed (rad/s)
#define OVERLAY_SPIN_TAU        0.20f                       // Exponential spin-up/down time constant (s)
#define OVERLAY_NUM_DOTS        8                           // Number of dots in the dotted circle
#define OVERLAY_DOT_RADIUS      8.0f                        // Fixed radius of each dot (px)

// 1-Euro Filter Parameters (Pointer Position Smoothing)
#define ONEEURO_MIN_CUTOFF      1.0f                        // Minimum cutoff frequency fc_min (Hz)
#define ONEEURO_BETA            0.007f                      // Speed coefficient beta
#define ONEEURO_D_CUTOFF        1.0f                        // Derivative cutoff frequency (Hz)
#define ONEEURO_SYNC_CUTOFF     10.0f                       // Matched C1 kinematic post-filter cutoff (Hz)

// Engagement Control Law Parameters
#define OVERLAY_D_FLOOR         (0.30f * OVERLAY_R0)        // Clamp denominator near center (29.2 px)
#define OVERLAY_R_SCALE_REF     (0.45f * OVERLAY_R0)        // Radial sqrt-scaling reference radius (43.7 px)
#define OVERLAY_PSI_ALPHA       2.50f                       // Radial velocity penalty in psi
#define OVERLAY_PSI_EPSILON     625.0f                      // Low-speed noise squelch (25 px/s)^2
#define OVERLAY_OMEGA_DEADBAND  0.35f                       // Min CCW angular speed to wind (rad/s)
#define OVERLAY_K_IN            0.22f                       // CCW winding gain (1/rad)
#define OVERLAY_GAMMA_OPEN      0.35f                       // Spring restoration rate (1/s)
#define OVERLAY_BETA_STIFF      0.40f                       // Non-linear spring stiffening factor
#define OVERLAY_K_CW            0.00f                       // Active CW unwind gain (0 = use spring only)

// Pop-Out Concentric Annuli Parameters
#define MENU_OPTION_COUNT       2                           // 0: Grayscale Toggle, 1: Save Screenshot
#define POPOUT_EXPLODE_SEC      0.18f                       // 180 ms initial ballistic explosion
#define POPOUT_DWELL_SEC        0.32f                       // 320 ms hover dwell to commit action
#define POPOUT_BAND_WIDTH       52.0f                       // Radial thickness of each annulus (px)
#define POPOUT_BAND_GAP         20.0f                       // Radial gap between annuli (px)
#define POPOUT_START_INNER_R    190.0f                      // Band 0 inner radius after explosion (px), outside ENGAGEMENT_ZONE_OUT
#define POPOUT_DRIFT_SPEED      45.0f                       // Outward slow drift speed (px/s)
#define POPOUT_BL_MARGIN_RATIO  0.16f                       // 16% of viewport diagonal off bottom-left
#define POPOUT_ANNULUS_SEGMENTS 90                          // Angular quad subdivisions across 2*pi

// Pointer Debug Trail Parameters (Raw = Pale Yellow, Filtered = Pale Violet)
#define OVERLAY_TRAIL_CAPACITY  160                         // Max trail vertices buffered (~2.5s @ 60Hz)
#define OVERLAY_TRAIL_LIFETIME  0.85f                       // Trail fade-out duration (s)
#define OVERLAY_TRAIL_MIN_STEP  0.75f                       // Min movement (px) to append a new segment

typedef enum {
    OVERLAY_DORMANT = 0,
    OVERLAY_REELING_IN,
    OVERLAY_ENGAGEMENT,
    OVERLAY_POPOUT
} OverlayPhase;

static inline const char *overlay_phase_name(OverlayPhase phase) {
    switch (phase) {
        case OVERLAY_DORMANT:    return "OVERLAY_DORMANT";
        case OVERLAY_REELING_IN: return "OVERLAY_REELING_IN";
        case OVERLAY_ENGAGEMENT: return "OVERLAY_ENGAGEMENT";
        case OVERLAY_POPOUT:     return "OVERLAY_POPOUT";
        default:                 return "UNKNOWN";
    }
}

typedef struct {
    float x_hat;
    float y_hat;
    float vx_hat;
    float vy_hat;
    float d;
    float d_scaled;
    float v_theta;
    float v_r;
    float omega_ptr;
    float psi;
    float ccw_drive;
    float spring_open;
    float du_dt;
    float r_curr;
    bool in_window;
    bool in_reel_zone;
    bool singularity_triggered;
    bool popout_expired;
    int committed_option;     // -1 if none this frame; 0 = Band 0, 1 = Band 1
    uint32_t singularity_count;
    uint32_t option_commit_count[MENU_OPTION_COUNT];
} OverlayDiagnostics;

typedef struct {
    float x;
    float y;
    float age_sec;
    uint32_t stroke_id;
} OverlayTrailPoint;

typedef struct {
    OverlayTrailPoint pts[OVERLAY_TRAIL_CAPACITY];
    uint32_t head;       // Index of oldest active point
    uint32_t count;      // Number of active points (0..OVERLAY_TRAIL_CAPACITY)
    uint32_t stroke_id;  // Increments on window leave/re-entry to break strokes cleanly
    bool active_stroke;
} OverlayTrail;

typedef struct {
    OverlayPhase phase;
    float u;                                  // Compression state [0.0f, 1.0f]
    float omega_vis;                          // Visual CCW rotation speed (rad/s)
    float theta_vis;                          // Visual rotation angle (rad)
    float popout_elapsed;                     // Elapsed time in OVERLAY_POPOUT (s)
    float r_horizon;                          // Max bottom-left drift horizon radius (1463.2 px)
    float band_r_in[MENU_OPTION_COUNT];       // Current inner radius of each pop-out annulus (px)
    float band_r_out[MENU_OPTION_COUNT];      // Current outer radius of each pop-out annulus (px)
    int dwell_band;                           // Currently hovered band index (-1 if none)
    float dwell_timer;                        // Hover dwell accumulator on dwell_band (s)
    OneEuroFilter2D filter;                   // 2D 1-Euro pointer position & velocity filter
    OverlayDiagnostics diag;                  // Per-frame kinematic & control telemetry snapshot
    OverlayTrail raw_trail;                   // Pale yellow fading trail for raw pointer
    OverlayTrail filt_trail;                  // Pale violet fading trail for 1-Euro filtered pointer
} OverlayState;

void overlay_init(OverlayState *ov);
void overlay_update(OverlayState *ov, int raw_x, int raw_y, bool cursor_in_window, float dt);
void overlay_render(const OverlayState *ov, SDL_Renderer *ren);

#endif // OVERLAY_H
