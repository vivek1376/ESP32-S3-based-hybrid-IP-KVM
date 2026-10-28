/**
 * overlay.c — [TODO-1] Feedforward Radial Overlay (Subtask 1: Phases 1 & 2)
 */

#include "overlay.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DOT_SEGMENTS 24
#define VERTS_PER_DOT (DOT_SEGMENTS + 1)
#define INDICES_PER_DOT (DOT_SEGMENTS * 3)

static float s_unit_cos[DOT_SEGMENTS];
static float s_unit_sin[DOT_SEGMENTS];
static int s_dot_ring_indices[OVERLAY_NUM_DOTS * INDICES_PER_DOT];
static bool s_geom_initialized = false;

static void init_dot_geometry_tables(void) {
    if (s_geom_initialized) return;

    const float two_pi = 2.0f * (float)M_PI;
    for (int s = 0; s < DOT_SEGMENTS; ++s) {
        float angle = (float)s * (two_pi / (float)DOT_SEGMENTS);
        s_unit_cos[s] = cosf(angle);
        s_unit_sin[s] = sinf(angle);
    }

    int idx = 0;
    for (int k = 0; k < OVERLAY_NUM_DOTS; ++k) {
        int base_v = k * VERTS_PER_DOT;
        for (int s = 0; s < DOT_SEGMENTS; ++s) {
            int next_s = (s + 1) % DOT_SEGMENTS;
            s_dot_ring_indices[idx++] = base_v;
            s_dot_ring_indices[idx++] = base_v + 1 + s;
            s_dot_ring_indices[idx++] = base_v + 1 + next_s;
        }
    }

    s_geom_initialized = true;
}

void overlay_init(OverlayState *ov) {
    memset(ov, 0, sizeof(*ov));
    ov->phase = OVERLAY_DORMANT;
    ov->u = 0.0f;
    ov->omega_vis = 0.0f;
    ov->theta_vis = 0.0f;
    init_dot_geometry_tables();
}

void overlay_update(OverlayState *ov, int raw_x, int raw_y, bool cursor_in_window, float dt) {
    // Clamp dt to [0.001f, 0.100f] per Section 3 specification
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.100f) dt = 0.100f;

    // Quadrant I check: top-right quadrant inside the active 1920x1080 KVM window.
    // Leaving the window onto the host desktop treats pointer as outside Quadrant I.
    bool in_window = cursor_in_window &&
                     (raw_x >= 0 && raw_x < WIN_W &&
                      raw_y >= 0 && raw_y < WIN_H);
    bool in_quadrant_1 = in_window &&
                         (raw_x >= (WIN_W / 2) && raw_y <= (WIN_H / 2));

    // Subtask 1 Phase Transitions: OVERLAY_DORMANT <-> OVERLAY_REELING_IN
    switch (ov->phase) {
        case OVERLAY_DORMANT:
            if (in_quadrant_1) {
                ov->phase = OVERLAY_REELING_IN;
            }
            break;

        case OVERLAY_REELING_IN:
            if (!in_quadrant_1) {
                ov->phase = OVERLAY_DORMANT;
            }
            break;

        default:
            ov->phase = in_quadrant_1 ? OVERLAY_REELING_IN : OVERLAY_DORMANT;
            break;
    }

    // Exponential spin factor (clamped to <= 1.0f for numerical stability)
    float spin_alpha = dt / OVERLAY_SPIN_TAU;
    if (spin_alpha > 1.0f) spin_alpha = 1.0f;

    // Phase Kinematics Update
    switch (ov->phase) {
        case OVERLAY_DORMANT:
            // Compression state u decays toward 0.0f
            ov->u = fmaxf(0.0f, ov->u - OVERLAY_GAMMA_OPEN * dt);
            // Visual rotation speed omega_vis decays exponentially toward 0.0f
            ov->omega_vis += (0.0f - ov->omega_vis) * spin_alpha;
            // Advance rotation angle (CCW in screen space)
            ov->theta_vis -= ov->omega_vis * dt;
            break;

        case OVERLAY_REELING_IN:
            // Compression state u decays toward 0.0f
            ov->u = fmaxf(0.0f, ov->u - OVERLAY_GAMMA_OPEN * dt);
            // omega_vis accelerates exponentially toward OVERLAY_BASE_SPEED
            ov->omega_vis += (OVERLAY_BASE_SPEED - ov->omega_vis) * spin_alpha;
            // Advance rotation angle (CCW in screen space)
            ov->theta_vis -= ov->omega_vis * dt;
            break;

        default:
            break;
    }

    // Keep theta_vis bounded within [-2*pi, 2*pi]
    const float two_pi = 2.0f * (float)M_PI;
    if (ov->theta_vis > two_pi || ov->theta_vis < -two_pi) {
        ov->theta_vis = fmodf(ov->theta_vis, two_pi);
    }
}

void overlay_render(const OverlayState *ov, SDL_Renderer *ren) {
    if (ov->phase == OVERLAY_POPOUT) {
        // Collapsed dotted circle is hidden during OVERLAY_POPOUT (implemented in Subtask 3)
        return;
    }

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    SDL_Color color;
    switch (ov->phase) {
        case OVERLAY_DORMANT:
            // Soft White (200, 230, 255), Alpha 90 (~35%)
            color = (SDL_Color){200, 230, 255, 90};
            break;

        case OVERLAY_REELING_IN:
        default:
            // Light Cyan (100, 210, 255), Alpha 150 (~59%)
            color = (SDL_Color){100, 210, 255, 150};
            break;
    }

    float r_curr = OVERLAY_R0 * (1.0f - ov->u) + OVERLAY_R_MIN * ov->u;
    const float two_pi = 2.0f * (float)M_PI;
    const float step_angle = two_pi / (float)OVERLAY_NUM_DOTS;

    SDL_Vertex verts[OVERLAY_NUM_DOTS * VERTS_PER_DOT];

    for (int k = 0; k < OVERLAY_NUM_DOTS; ++k) {
        float phi_k = ov->theta_vis + (float)k * step_angle;
        float dot_cx = OVERLAY_CENTER_X + r_curr * cosf(phi_k);
        float dot_cy = OVERLAY_CENTER_Y + r_curr * sinf(phi_k);

        int base_v = k * VERTS_PER_DOT;
        verts[base_v].position.x = dot_cx;
        verts[base_v].position.y = dot_cy;
        verts[base_v].color = color;
        verts[base_v].tex_coord.x = 0.0f;
        verts[base_v].tex_coord.y = 0.0f;

        for (int s = 0; s < DOT_SEGMENTS; ++s) {
            SDL_Vertex *v = &verts[base_v + 1 + s];
            v->position.x = dot_cx + OVERLAY_DOT_RADIUS * s_unit_cos[s];
            v->position.y = dot_cy + OVERLAY_DOT_RADIUS * s_unit_sin[s];
            v->color = color;
            v->tex_coord.x = 0.0f;
            v->tex_coord.y = 0.0f;
        }
    }

    SDL_RenderGeometry(ren, NULL,
                       verts, OVERLAY_NUM_DOTS * VERTS_PER_DOT,
                       s_dot_ring_indices, OVERLAY_NUM_DOTS * INDICES_PER_DOT);
}

