/**
 * overlay.c — [TODO-1] Feedforward Radial Overlay (4-Phase State Machine & Translucent Renderer)
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

#define ANNULUS_VERTS   ((POPOUT_ANNULUS_SEGMENTS + 1) * 2)
#define ANNULUS_INDICES (POPOUT_ANNULUS_SEGMENTS * 6)

static float s_unit_cos[DOT_SEGMENTS];
static float s_unit_sin[DOT_SEGMENTS];
static int s_dot_ring_indices[OVERLAY_NUM_DOTS * INDICES_PER_DOT];

static float s_annulus_cos[POPOUT_ANNULUS_SEGMENTS + 1];
static float s_annulus_sin[POPOUT_ANNULUS_SEGMENTS + 1];
static int s_annulus_indices[ANNULUS_INDICES];

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

    // Precompute 90-subdivision unit circle & shared-vertex quad strip indices for pop-out annuli
    for (int s = 0; s < POPOUT_ANNULUS_SEGMENTS; ++s) {
        float angle = (float)s * (two_pi / (float)POPOUT_ANNULUS_SEGMENTS);
        s_annulus_cos[s] = cosf(angle);
        s_annulus_sin[s] = sinf(angle);
    }
    // Exact wraparound at 2*pi to prevent any floating-point seam gap
    s_annulus_cos[POPOUT_ANNULUS_SEGMENTS] = s_annulus_cos[0];
    s_annulus_sin[POPOUT_ANNULUS_SEGMENTS] = s_annulus_sin[0];

    int a_idx = 0;
    for (int s = 0; s < POPOUT_ANNULUS_SEGMENTS; ++s) {
        int i0 = 2 * s;
        int i1 = 2 * s + 1;
        int i2 = 2 * (s + 1);
        int i3 = 2 * (s + 1) + 1;

        s_annulus_indices[a_idx++] = i0;
        s_annulus_indices[a_idx++] = i1;
        s_annulus_indices[a_idx++] = i3;

        s_annulus_indices[a_idx++] = i0;
        s_annulus_indices[a_idx++] = i3;
        s_annulus_indices[a_idx++] = i2;
    }

    s_geom_initialized = true;
}

static void render_annulus(SDL_Renderer *ren, float r_in, float r_out, SDL_Color color) {
    if (color.a == 0 || r_out <= r_in + 0.25f) return;
    if (r_in < 0.0f) r_in = 0.0f;

    SDL_Vertex verts[ANNULUS_VERTS];
    for (int s = 0; s <= POPOUT_ANNULUS_SEGMENTS; ++s) {
        float c = s_annulus_cos[s];
        float sn = s_annulus_sin[s];

        SDL_Vertex *vin = &verts[2 * s];
        vin->position.x = OVERLAY_CENTER_X + r_in * c;
        vin->position.y = OVERLAY_CENTER_Y + r_in * sn;
        vin->color = color;
        vin->tex_coord.x = 0.0f;
        vin->tex_coord.y = 0.0f;

        SDL_Vertex *vout = &verts[2 * s + 1];
        vout->position.x = OVERLAY_CENTER_X + r_out * c;
        vout->position.y = OVERLAY_CENTER_Y + r_out * sn;
        vout->color = color;
        vout->tex_coord.x = 0.0f;
        vout->tex_coord.y = 0.0f;
    }

    SDL_RenderGeometry(ren, NULL, verts, ANNULUS_VERTS, s_annulus_indices, ANNULUS_INDICES);
}

static void trail_update(OverlayTrail *tr, float x, float y, bool in_window, float dt) {
    // 1. Age existing points and evict expired tail points
    for (uint32_t i = 0; i < tr->count; ++i) {
        uint32_t slot = (tr->head + i) % OVERLAY_TRAIL_CAPACITY;
        tr->pts[slot].age_sec += dt;
    }
    while (tr->count > 0) {
        if (tr->pts[tr->head].age_sec >= OVERLAY_TRAIL_LIFETIME) {
            tr->head = (tr->head + 1u) % OVERLAY_TRAIL_CAPACITY;
            tr->count--;
        } else {
            break;
        }
    }

    // 2. Break stroke cleanly when pointer leaves window
    if (!in_window) {
        tr->active_stroke = false;
        return;
    }

    if (!tr->active_stroke) {
        tr->stroke_id++;
        tr->active_stroke = true;
    }

    // 3. Only append a new vertex if moved >= OVERLAY_TRAIL_MIN_STEP in current stroke
    if (tr->count > 0) {
        uint32_t last_slot = (tr->head + tr->count - 1u) % OVERLAY_TRAIL_CAPACITY;
        const OverlayTrailPoint *last = &tr->pts[last_slot];
        if (last->stroke_id == tr->stroke_id) {
            float dist = hypotf(x - last->x, y - last->y);
            if (dist < OVERLAY_TRAIL_MIN_STEP) {
                return;
            }
        }
    }

    if (tr->count == OVERLAY_TRAIL_CAPACITY) {
        tr->head = (tr->head + 1u) % OVERLAY_TRAIL_CAPACITY;
        tr->count--;
    }

    uint32_t write_slot = (tr->head + tr->count) % OVERLAY_TRAIL_CAPACITY;
    tr->pts[write_slot].x = x;
    tr->pts[write_slot].y = y;
    tr->pts[write_slot].age_sec = 0.0f;
    tr->pts[write_slot].stroke_id = tr->stroke_id;
    tr->count++;
}

static void trail_render(const OverlayTrail *tr, SDL_Renderer *ren,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t max_alpha) {
    if (tr->count < 2) return;

    SDL_Vertex verts[OVERLAY_TRAIL_CAPACITY * 2];
    int indices[(OVERLAY_TRAIL_CAPACITY - 1) * 6];
    int v_count = 0;
    int i_count = 0;

    uint32_t i = 0;
    while (i < tr->count) {
        uint32_t run_start = i;
        uint32_t sid = tr->pts[(tr->head + run_start) % OVERLAY_TRAIL_CAPACITY].stroke_id;
        i++;
        while (i < tr->count &&
               tr->pts[(tr->head + i) % OVERLAY_TRAIL_CAPACITY].stroke_id == sid) {
            i++;
        }
        uint32_t run_len = i - run_start;
        if (run_len < 2) continue;

        for (uint32_t j = 0; j < run_len; ++j) {
            const OverlayTrailPoint *curr =
                &tr->pts[(tr->head + run_start + j) % OVERLAY_TRAIL_CAPACITY];

            float tx = 1.0f, ty = 0.0f;
            if (j == 0) {
                const OverlayTrailPoint *next =
                    &tr->pts[(tr->head + run_start + j + 1u) % OVERLAY_TRAIL_CAPACITY];
                float dx = next->x - curr->x;
                float dy = next->y - curr->y;
                float len = fmaxf(hypotf(dx, dy), 1e-4f);
                tx = dx / len;
                ty = dy / len;
            } else if (j == run_len - 1u) {
                const OverlayTrailPoint *prev =
                    &tr->pts[(tr->head + run_start + j - 1u) % OVERLAY_TRAIL_CAPACITY];
                float dx = curr->x - prev->x;
                float dy = curr->y - prev->y;
                float len = fmaxf(hypotf(dx, dy), 1e-4f);
                tx = dx / len;
                ty = dy / len;
            } else {
                const OverlayTrailPoint *prev =
                    &tr->pts[(tr->head + run_start + j - 1u) % OVERLAY_TRAIL_CAPACITY];
                const OverlayTrailPoint *next =
                    &tr->pts[(tr->head + run_start + j + 1u) % OVERLAY_TRAIL_CAPACITY];
                float dx0 = curr->x - prev->x;
                float dy0 = curr->y - prev->y;
                float l0 = fmaxf(hypotf(dx0, dy0), 1e-4f);
                float dx1 = next->x - curr->x;
                float dy1 = next->y - curr->y;
                float l1 = fmaxf(hypotf(dx1, dy1), 1e-4f);
                float sx = (dx0 / l0) + (dx1 / l1);
                float sy = (dy0 / l0) + (dy1 / l1);
                float slen = hypotf(sx, sy);
                if (slen > 1e-3f) {
                    tx = sx / slen;
                    ty = sy / slen;
                } else {
                    tx = dx0 / l0;
                    ty = dy0 / l0;
                }
            }

            float nx = -ty;
            float ny = tx;

            float life = 1.0f - (curr->age_sec / OVERLAY_TRAIL_LIFETIME);
            if (life < 0.0f) life = 0.0f;
            if (life > 1.0f) life = 1.0f;

            float half_w = 0.5f + 1.8f * life;
            uint8_t a = (uint8_t)((float)max_alpha * (life * life) + 0.5f);
            SDL_Color col = {r, g, b, a};

            int v_curr = v_count;
            verts[v_count].position.x = curr->x + nx * half_w;
            verts[v_count].position.y = curr->y + ny * half_w;
            verts[v_count].color = col;
            verts[v_count].tex_coord.x = 0.0f;
            verts[v_count].tex_coord.y = 0.0f;
            v_count++;

            verts[v_count].position.x = curr->x - nx * half_w;
            verts[v_count].position.y = curr->y - ny * half_w;
            verts[v_count].color = col;
            verts[v_count].tex_coord.x = 0.0f;
            verts[v_count].tex_coord.y = 0.0f;
            v_count++;

            if (j > 0) {
                int v_prev = v_curr - 2;
                indices[i_count++] = v_prev;
                indices[i_count++] = v_prev + 1;
                indices[i_count++] = v_curr;

                indices[i_count++] = v_prev + 1;
                indices[i_count++] = v_curr + 1;
                indices[i_count++] = v_curr;
            }
        }
    }

    if (i_count > 0) {
        SDL_RenderGeometry(ren, NULL, verts, v_count, indices, i_count);
    }
}

void overlay_init(OverlayState *ov) {
    memset(ov, 0, sizeof(*ov));
    ov->phase = OVERLAY_DORMANT;
    ov->u = 0.0f;
    ov->omega_vis = 0.0f;
    ov->theta_vis = 0.0f;

    // Geometry Note (Section 2): Bottom-Left Horizon (POPOUT_MAX_HORIZON)
    float d_diag = hypotf((float)WIN_W, (float)WIN_H);
    float d_bl   = hypotf(OVERLAY_CENTER_X, (float)WIN_H - OVERLAY_CENTER_Y);
    ov->r_horizon = d_bl - POPOUT_BL_MARGIN_RATIO * d_diag;

    ov->dwell_band = -1;
    ov->dwell_timer = 0.0f;
    ov->diag.r_curr = OVERLAY_R0;
    ov->diag.committed_option = -1;
    one_euro_2d_reset(&ov->filter);
    init_dot_geometry_tables();
}

void overlay_update(OverlayState *ov, int raw_x, int raw_y, bool cursor_in_window, float dt) {
    // Clamp dt to [0.001f, 0.100f] per Section 3 specification
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.100f) dt = 0.100f;

    // Window bounds check: leaving the 1920x1080 KVM window onto the host desktop
    // transitions the overlay to OVERLAY_DORMANT and resets the 1-Euro filter.
    bool in_window = cursor_in_window &&
                     (raw_x >= 0 && raw_x < WIN_W &&
                      raw_y >= 0 && raw_y < WIN_H);

    // Section 3: Real-Time 2D 1-Euro Filter Update (Phase-Consistent C1 Kinematics)
    float x_hat = (float)raw_x;
    float y_hat = (float)raw_y;
    float x_mid = (float)raw_x;
    float y_mid = (float)raw_y;
    float vx_hat = 0.0f;
    float vy_hat = 0.0f;

    if (in_window) {
        one_euro_2d_update(&ov->filter,
                           (float)raw_x, (float)raw_y, dt,
                           ONEEURO_MIN_CUTOFF, ONEEURO_BETA,
                           ONEEURO_D_CUTOFF, ONEEURO_SYNC_CUTOFF,
                           &x_hat, &y_hat, &x_mid, &y_mid, &vx_hat, &vy_hat);
    } else {
        // Reset filter when pointer exits onto host desktop to prevent re-entry velocity spikes
        one_euro_2d_reset(&ov->filter);
    }

    // Update translucent fading pointer trails (raw & 1-Euro filtered)
    trail_update(&ov->raw_trail, (float)raw_x, (float)raw_y, in_window, dt);
    trail_update(&ov->filt_trail, x_hat, y_hat, in_window, dt);

    // Section 4: First-Order Polar Kinematics (Screen Coordinates: +x Right, +y Down)
    // Keep true Euclidean distance d for physical screen hit-zones (ENGAGEMENT_ZONE_IN / OUT),
    // and compute all polar kinematics from isotropic radially sqrt-scaled (dx_scaled, dy_scaled)
    // so radial distance scales sub-linearly as sqrt(d * R_ref) without angular distortion.
    float dx = x_hat - OVERLAY_CENTER_X;
    float dy = y_hat - OVERLAY_CENTER_Y;
    float d  = hypotf(dx, dy);

    float dx_mid    = x_mid - OVERLAY_CENTER_X;
    float dy_mid    = y_mid - OVERLAY_CENTER_Y;
    float d_mid     = hypotf(dx_mid, dy_mid);

    float scale_r   = (d_mid > 1e-4f) ? sqrtf(OVERLAY_R_SCALE_REF / d_mid) : 1.0f;
    float dx_scaled = dx_mid * scale_r;
    float dy_scaled = dy_mid * scale_r;
    float d_scaled  = hypotf(dx_scaled, dy_scaled);
    float d_floor_s = sqrtf(OVERLAY_D_FLOOR * OVERLAY_R_SCALE_REF);

    float d_safe    = fmaxf(d_scaled, 1e-4f);
    float v_theta   = (dy_scaled * vx_hat - dx_scaled * vy_hat) / d_safe;
    float v_r       = (dx_scaled * vx_hat + dy_scaled * vy_hat) / d_safe;
    float omega_ptr = v_theta / fmaxf(d_scaled, d_floor_s);

    float v_theta_sq = v_theta * v_theta;
    float v_r_sq     = v_r * v_r;
    float psi = v_theta_sq /
                (v_theta_sq + OVERLAY_PSI_ALPHA * v_r_sq + OVERLAY_PSI_EPSILON);

    // Section 5: Phase Transitions (Strictly anchored to nominal OVERLAY_R0, never R_curr!)
    // Reeling-in is gated by radial proximity OVERLAY_REELING_ZONE (+24% beyond OVERLAY_ENGAGEMENT_ZONE_OUT),
    // clipped cleanly to the active KVM window bounds.
    bool in_reel_zone = in_window && (d <= OVERLAY_REELING_ZONE);

    switch (ov->phase) {
        case OVERLAY_DORMANT:
            if (in_reel_zone) {
                ov->phase = (d <= OVERLAY_ENGAGEMENT_ZONE_IN) ? OVERLAY_ENGAGEMENT : OVERLAY_REELING_IN;
            }
            break;

        case OVERLAY_REELING_IN:
            if (!in_reel_zone) {
                ov->phase = OVERLAY_DORMANT;
            } else if (d <= OVERLAY_ENGAGEMENT_ZONE_IN) {
                ov->phase = OVERLAY_ENGAGEMENT;
            }
            break;

        case OVERLAY_ENGAGEMENT:
            // Hysteresis retention while inside window and d <= OVERLAY_ENGAGEMENT_ZONE_OUT
            if (!in_window) {
                ov->phase = OVERLAY_DORMANT;
            } else if (d > OVERLAY_ENGAGEMENT_ZONE_OUT) {
                ov->phase = in_reel_zone ? OVERLAY_REELING_IN : OVERLAY_DORMANT;
            }
            break;

        case OVERLAY_POPOUT:
            // Pop-out annuli continue drifting toward R_horizon regardless of cursor zone/window
            break;

        default:
            ov->phase = in_reel_zone ? OVERLAY_REELING_IN : OVERLAY_DORMANT;
            break;
    }

    // Exponential spin factor (clamped to <= 1.0f for numerical stability)
    float spin_alpha = dt / OVERLAY_SPIN_TAU;
    if (spin_alpha > 1.0f) spin_alpha = 1.0f;

    float ccw_drive = 0.0f;
    float spring_open = 0.0f;
    float du_dt = 0.0f;
    bool singularity_triggered = false;
    bool popout_expired = false;
    int committed_option = -1;

    // Phase Kinematics & Control Law Update
    switch (ov->phase) {
        case OVERLAY_DORMANT:
            if (ov->u > 0.0f) {
                spring_open = OVERLAY_GAMMA_OPEN;
                du_dt = -OVERLAY_GAMMA_OPEN;
            }
            // Compression state u decays toward 0.0f
            ov->u = fmaxf(0.0f, ov->u - OVERLAY_GAMMA_OPEN * dt);
            // Visual rotation speed omega_vis decays exponentially toward 0.0f
            ov->omega_vis += (0.0f - ov->omega_vis) * spin_alpha;
            // Advance rotation angle (CCW in screen space)
            ov->theta_vis -= ov->omega_vis * dt;
            break;

        case OVERLAY_REELING_IN:
            if (ov->u > 0.0f) {
                spring_open = OVERLAY_GAMMA_OPEN;
                du_dt = -OVERLAY_GAMMA_OPEN;
            }
            // Compression state u decays toward 0.0f
            ov->u = fmaxf(0.0f, ov->u - OVERLAY_GAMMA_OPEN * dt);
            // omega_vis accelerates exponentially toward OVERLAY_BASE_SPEED
            ov->omega_vis += (OVERLAY_BASE_SPEED - ov->omega_vis) * spin_alpha;
            // Advance rotation angle (CCW in screen space)
            ov->theta_vis -= ov->omega_vis * dt;
            break;

        case OVERLAY_ENGAGEMENT: {
            // Phase 3 Differential Winding & Non-Linear Spring Control Law
            ccw_drive        = OVERLAY_K_IN * psi * fmaxf(0.0f, omega_ptr - OVERLAY_OMEGA_DEADBAND);
            spring_open      = OVERLAY_GAMMA_OPEN * (1.0f + OVERLAY_BETA_STIFF * ov->u);
            float cw_unwind  = OVERLAY_K_CW * fmaxf(0.0f, -omega_ptr);
            du_dt            = ccw_drive - spring_open - cw_unwind;

            ov->u += du_dt * dt;
            if (ov->u < 0.0f) ov->u = 0.0f;

            // Maintain uniform spin speed at OVERLAY_BASE_SPEED ([TODO-3] spin-up deferred)
            ov->omega_vis = OVERLAY_BASE_SPEED;
            ov->theta_vis -= ov->omega_vis * dt;

            // Singularity Trigger (u >= 1.0f) -> Transition to Phase 4 (OVERLAY_POPOUT)
            if (ov->u >= 1.0f) {
                ov->u = 1.0f;
                singularity_triggered = true;
                ov->diag.singularity_count++;
                ov->popout_elapsed = 0.0f;
                ov->dwell_band = -1;
                ov->dwell_timer = 0.0f;
                for (int i = 0; i < MENU_OPTION_COUNT; ++i) {
                    ov->band_r_in[i] = 0.0f;
                    ov->band_r_out[i] = 0.0f;
                }
                ov->omega_vis = 0.0f;
                ov->phase = OVERLAY_POPOUT;
            }
            break;
        }

        case OVERLAY_POPOUT: {
            ov->popout_elapsed += dt;

            if (ov->popout_elapsed < POPOUT_EXPLODE_SEC) {
                // Sub-Phase 4A — Initial Ballistic Explosion (180 ms ease-out cubic)
                float p = ov->popout_elapsed / POPOUT_EXPLODE_SEC;
                float inv = 1.0f - p;
                float s = 1.0f - (inv * inv * inv);

                for (int i = 0; i < MENU_OPTION_COUNT; ++i) {
                    float target_in  = POPOUT_START_INNER_R +
                                       (float)i * (POPOUT_BAND_WIDTH + POPOUT_BAND_GAP);
                    float target_out = target_in + POPOUT_BAND_WIDTH;
                    ov->band_r_in[i]  = s * target_in;
                    ov->band_r_out[i] = s * target_out;
                }

                // Selection disabled during Sub-Phase 4A
                ov->dwell_band = -1;
                ov->dwell_timer = 0.0f;
            } else {
                // Sub-Phase 4B — Outward Drift & 320 ms Hover Dwell
                float t_drift = ov->popout_elapsed - POPOUT_EXPLODE_SEC;
                for (int i = 0; i < MENU_OPTION_COUNT; ++i) {
                    float base_in = POPOUT_START_INNER_R +
                                    (float)i * (POPOUT_BAND_WIDTH + POPOUT_BAND_GAP);
                    ov->band_r_in[i]  = base_in + POPOUT_DRIFT_SPEED * t_drift;
                    ov->band_r_out[i] = ov->band_r_in[i] + POPOUT_BAND_WIDTH;
                }

                // Hover Hit-Test: only active while cursor is inside KVM window
                int hovered = -1;
                if (in_window) {
                    for (int i = 0; i < MENU_OPTION_COUNT; ++i) {
                        if (d >= ov->band_r_in[i] && d <= ov->band_r_out[i]) {
                            hovered = i;
                            break;
                        }
                    }
                }

                bool finished = false;
                if (hovered >= 0) {
                    if (ov->dwell_band == hovered) {
                        ov->dwell_timer += dt;
                    } else {
                        ov->dwell_band = hovered;
                        ov->dwell_timer = 0.0f;
                    }

                    if (ov->dwell_timer >= POPOUT_DWELL_SEC) {
                        committed_option = hovered;
                        ov->diag.option_commit_count[hovered]++;
                        finished = true;
                    }
                } else {
                    ov->dwell_band = -1;
                    ov->dwell_timer = 0.0f;
                }

                // Expiration when outer band reaches R_horizon
                if (!finished && ov->band_r_out[MENU_OPTION_COUNT - 1] >= ov->r_horizon) {
                    popout_expired = true;
                    finished = true;
                }

                if (finished) {
                    ov->u = 0.0f;
                    ov->dwell_band = -1;
                    ov->dwell_timer = 0.0f;
                    ov->popout_elapsed = 0.0f;
                    if (in_reel_zone) {
                        ov->phase = (d <= OVERLAY_ENGAGEMENT_ZONE_IN)
                                        ? OVERLAY_ENGAGEMENT
                                        : OVERLAY_REELING_IN;
                    } else {
                        ov->phase = OVERLAY_DORMANT;
                    }
                }
            }
            break;
        }

        default:
            break;
    }

    // Keep theta_vis bounded within [-2*pi, 2*pi]
    const float two_pi = 2.0f * (float)M_PI;
    if (ov->theta_vis > two_pi || ov->theta_vis < -two_pi) {
        ov->theta_vis = fmodf(ov->theta_vis, two_pi);
    }

    float uc = fminf(fmaxf(ov->u, 0.0f), 1.0f);
    float r_curr = OVERLAY_R0 * (1.0f - uc) + OVERLAY_R_MIN * uc;

    // Store per-frame diagnostic snapshot for terminal & binary telemetry
    ov->diag.x_hat = x_hat;
    ov->diag.y_hat = y_hat;
    ov->diag.vx_hat = vx_hat;
    ov->diag.vy_hat = vy_hat;
    ov->diag.d = d;
    ov->diag.d_scaled = d_scaled;
    ov->diag.v_theta = v_theta;
    ov->diag.v_r = v_r;
    ov->diag.omega_ptr = omega_ptr;
    ov->diag.psi = psi;
    ov->diag.ccw_drive = ccw_drive;
    ov->diag.spring_open = spring_open;
    ov->diag.du_dt = du_dt;
    ov->diag.r_curr = r_curr;
    ov->diag.in_window = in_window;
    ov->diag.in_reel_zone = in_reel_zone;
    ov->diag.singularity_triggered = singularity_triggered;
    ov->diag.popout_expired = popout_expired;
    ov->diag.committed_option = committed_option;
}

void overlay_render(const OverlayState *ov, SDL_Renderer *ren) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // Render translucent fading pointer trails:
    //  1) Raw mouse pointer       -> Pale Yellow (254, 243, 162)
    //  2) 1-Euro filtered pointer -> Pale Violet (214, 188, 250)
    trail_render(&ov->raw_trail, ren, 254, 243, 162, 165);
    trail_render(&ov->filt_trail, ren, 214, 188, 250, 185);

    if (ov->phase == OVERLAY_POPOUT) {
        // Section 6.1 & 6.3: Collapsed dotted circle is hidden during OVERLAY_POPOUT.
        // Render 90-subdivision concentric annuli with linear drift fade (130 -> 0)
        // and radial fill sweep (alpha 220) during active hover dwell.
        static const uint8_t k_band_rgb[MENU_OPTION_COUNT][3] = {
            { 40, 200, 220}, // Band 0 (Grayscale Toggle): Cool Teal
            {255, 180,  50}  // Band 1 (Save Screenshot) : Warm Gold
        };

        float start_outer_max = POPOUT_START_INNER_R +
                                (float)(MENU_OPTION_COUNT - 1) * (POPOUT_BAND_WIDTH + POPOUT_BAND_GAP) +
                                POPOUT_BAND_WIDTH;
        float cur_outer_max = ov->band_r_out[MENU_OPTION_COUNT - 1];
        float drift_span = fmaxf(1.0f, ov->r_horizon - start_outer_max);
        float drift_frac = 0.0f;
        if (ov->popout_elapsed >= POPOUT_EXPLODE_SEC) {
            drift_frac = (cur_outer_max - start_outer_max) / drift_span;
            drift_frac = fminf(fmaxf(drift_frac, 0.0f), 1.0f);
        }
        float base_alpha_f = 130.0f * (1.0f - drift_frac);
        uint8_t base_alpha = (uint8_t)(base_alpha_f + 0.5f);

        for (int i = 0; i < MENU_OPTION_COUNT; ++i) {
            float r_in  = ov->band_r_in[i];
            float r_out = ov->band_r_out[i];
            if (r_out <= r_in + 0.25f) continue;

            uint8_t cr = k_band_rgb[i][0];
            uint8_t cg = k_band_rgb[i][1];
            uint8_t cb = k_band_rgb[i][2];

            if (ov->dwell_band == i) {
                float dwell_prog = fminf(fmaxf(ov->dwell_timer / POPOUT_DWELL_SEC, 0.0f), 1.0f);
                float r_sweep = r_in + (r_out - r_in) * dwell_prog;

                // Keep unswept outer track clearly legible even late in drift
                uint8_t track_alpha = (uint8_t)(fmaxf(base_alpha_f, 75.0f) + 0.5f);
                SDL_Color sweep_col = {cr, cg, cb, 220};
                SDL_Color track_col = {cr, cg, cb, track_alpha};

                render_annulus(ren, r_in, r_sweep, sweep_col);
                render_annulus(ren, r_sweep, r_out, track_col);
            } else {
                SDL_Color band_col = {cr, cg, cb, base_alpha};
                render_annulus(ren, r_in, r_out, band_col);
            }
        }
        return;
    }

    float uc = fminf(fmaxf(ov->u, 0.0f), 1.0f);

    SDL_Color color;
    switch (ov->phase) {
        case OVERLAY_DORMANT:
            // Soft White (200, 230, 255), Alpha 90 (~35%)
            color = (SDL_Color){200, 230, 255, 90};
            break;

        case OVERLAY_REELING_IN:
            // Light Cyan (100, 210, 255), Alpha 150 (~59%)
            color = (SDL_Color){100, 210, 255, 150};
            break;

        case OVERLAY_ENGAGEMENT:
        default: {
            // Smooth RGB shift from Light Cyan (100, 210, 255) -> Magenta (255, 100, 200),
            // Alpha scales as 160 + 80 * u
            uint8_t r = (uint8_t)(100.0f + (255.0f - 100.0f) * uc + 0.5f);
            uint8_t g = (uint8_t)(210.0f + (100.0f - 210.0f) * uc + 0.5f);
            uint8_t b = (uint8_t)(255.0f + (200.0f - 255.0f) * uc + 0.5f);
            uint8_t a = (uint8_t)(160.0f + 80.0f * uc + 0.5f);
            color = (SDL_Color){r, g, b, a};
            break;
        }
    }

    float r_curr = OVERLAY_R0 * (1.0f - uc) + OVERLAY_R_MIN * uc;
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
