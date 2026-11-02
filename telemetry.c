/**
 * telemetry.c — In-Place Dynamic Terminal/TTY Logger & Binary Metrics Recorder
 */

#define _DEFAULT_SOURCE

#include "telemetry.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ANSI escape helpers
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_DIM     "\033[2m"
#define ANSI_CYAN    "\033[1;36m"
#define ANSI_MAGENTA "\033[1;35m"
#define ANSI_YELLOW  "\033[1;33m"
#define ANSI_GREEN       "\033[1;32m"
#define ANSI_RED         "\033[1;31m"
#define ANSI_WHITE       "\033[0;37m"
#define ANSI_PALE_YELLOW "\033[38;5;229m"
#define ANSI_DARK_BAR    "\033[38;5;238m"

static void resolve_tty_path(const char *arg, char *out, size_t out_sz) {
    if (!arg || arg[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (arg[0] == '/') {
        snprintf(out, out_sz, "%s", arg);
        return;
    }
    if (strncmp(arg, "pts/", 4) == 0 || strncmp(arg, "tty", 3) == 0) {
        snprintf(out, out_sz, "/dev/%s", arg);
        return;
    }
    bool all_digits = true;
    for (const char *p = arg; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        snprintf(out, out_sz, "/dev/pts/%s", arg);
    } else {
        snprintf(out, out_sz, "%s", arg);
    }
}

static void append_str(char *buf, size_t buf_sz, size_t *pos, const char *s) {
    while (*s != '\0' && *pos + 1 < buf_sz) {
        buf[(*pos)++] = *s++;
    }
}

static void build_unicode_bar(float norm_val, char *buf, size_t buf_sz, int width) {
    if (norm_val < 0.0f) norm_val = 0.0f;
    if (norm_val > 1.0f) norm_val = 1.0f;
    int filled = (int)(norm_val * (float)width + 0.5f);
    if (filled > width) filled = width;

    size_t pos = 0;
    if (filled > 0) {
        append_str(buf, buf_sz, &pos, ANSI_PALE_YELLOW);
        for (int i = 0; i < filled; ++i) {
            // U+258C LEFT HALF BLOCK '▌' (UTF-8: 0xE2 0x96 0x8C)
            append_str(buf, buf_sz, &pos, "\xe2\x96\x8c");
        }
    }
    if (filled < width) {
        append_str(buf, buf_sz, &pos, ANSI_DARK_BAR);
        for (int i = filled; i < width; ++i) {
            append_str(buf, buf_sz, &pos, "\xe2\x96\x8c");
        }
    }
    append_str(buf, buf_sz, &pos, ANSI_RESET);
    buf[pos] = '\0';
}

static const char *phase_ansi_color(OverlayPhase phase) {
    switch (phase) {
        case OVERLAY_DORMANT:    return ANSI_WHITE;
        case OVERLAY_REELING_IN: return ANSI_CYAN;
        case OVERLAY_ENGAGEMENT: return ANSI_MAGENTA;
        case OVERLAY_POPOUT:     return ANSI_YELLOW;
        default:                 return ANSI_RESET;
    }
}

void telemetry_log_event(TelemetryState *ts, float elapsed_sec, const char *fmt, ...) {
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    uint32_t slot = ts->event_head % TELEMETRY_EVENT_RING_SIZE;
    snprintf(ts->events[slot], TELEMETRY_EVENT_STR_LEN,
             "[%8.3fs] %s", elapsed_sec, msg);
    ts->event_head++;
    if (ts->event_count < TELEMETRY_EVENT_RING_SIZE) {
        ts->event_count++;
    }
}

void telemetry_init(TelemetryState *ts,
                    const char *tty_arg,
                    const char *bin_path_arg,
                    const char *esp_ip,
                    const char *video_dev) {
    memset(ts, 0, sizeof(*ts));
    ts->min_dt_ms = 1e9f;
    ts->max_dt_ms = 0.0f;

    snprintf(ts->esp_ip, sizeof(ts->esp_ip), "%s", esp_ip ? esp_ip : "unknown");
    snprintf(ts->video_dev, sizeof(ts->video_dev), "%s", video_dev ? video_dev : "/dev/video0");
    snprintf(ts->bin_path, sizeof(ts->bin_path), "%s",
             (bin_path_arg && bin_path_arg[0] != '\0') ? bin_path_arg : TELEMETRY_DEFAULT_BIN_PATH);

    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    ts->session_id = (uint32_t)now_ts.tv_sec ^ (uint32_t)(now_ts.tv_nsec >> 10) ^ (uint32_t)getpid();

    // 1. Open binary metrics output file and write 16-byte header
    ts->bin_fp = fopen(ts->bin_path, "wb");
    if (ts->bin_fp) {
        TelemetryFileHeader hdr;
        memcpy(hdr.magic, TELEMETRY_MAGIC, 8);
        hdr.version = TELEMETRY_VERSION;
        hdr.record_size = (uint16_t)sizeof(OverlayMetricRecord);
        hdr.session_id = ts->session_id;
        fwrite(&hdr, sizeof(hdr), 1, ts->bin_fp);
        fflush(ts->bin_fp);
    } else {
        fprintf(stderr, "[WARN] Failed to open binary metrics file '%s': %s\n",
                ts->bin_path, strerror(errno));
    }

    // 2. Open target TTY (or fall back to stdout)
    if (tty_arg && tty_arg[0] != '\0') {
        char resolved[128];
        resolve_tty_path(tty_arg, resolved, sizeof(resolved));
        FILE *fp = fopen(resolved, "w");
        if (fp) {
            ts->tty_fp = fp;
            ts->owns_tty_fp = true;
            ts->tty_ansi_enabled = true;
            snprintf(ts->tty_label, sizeof(ts->tty_label), "%s", resolved);
            printf("[INFO] Dynamic in-place telemetry active on %s | Binary metrics: %s\n",
                   resolved, ts->bin_path);
            fflush(stdout);
        } else {
            fprintf(stderr, "[WARN] Could not open TTY '%s' (%s); falling back to stdout.\n",
                    resolved, strerror(errno));
        }
    }

    if (!ts->tty_fp) {
        if (isatty(STDOUT_FILENO)) {
            ts->tty_fp = stdout;
            ts->owns_tty_fp = false;
            ts->tty_ansi_enabled = true;
            const char *cur_tty = ttyname(STDOUT_FILENO);
            snprintf(ts->tty_label, sizeof(ts->tty_label), "%s",
                     cur_tty ? cur_tty : "stdout");
        }
    }

    if (ts->tty_fp && ts->tty_ansi_enabled) {
        // Use full block buffering so each dashboard redraw flushes in a single write
        setvbuf(ts->tty_fp, ts->tty_io_buf, _IOFBF, sizeof(ts->tty_io_buf));
        fprintf(ts->tty_fp, "\033[2J\033[H\033[?25l");
        fflush(ts->tty_fp);
    }

    telemetry_log_event(ts, 0.0f, "INIT  : Telemetry started (session=0x%08X)", ts->session_id);
}

static void render_tty_dashboard(TelemetryState *ts,
                                 float elapsed_sec,
                                 float raw_dt_ms,
                                 float clamped_dt_ms,
                                 float rolling_fps,
                                 int raw_x,
                                 int raw_y,
                                 bool kb_grabbed,
                                 const OverlayState *ov) {
    FILE *fp = ts->tty_fp;
    if (!fp || !ts->tty_ansi_enabled) return;

    const OverlayDiagnostics *dg = &ov->diag;

    char psi_bar[160];
    char u_bar[160];
    build_unicode_bar(dg->psi, psi_bar, sizeof(psi_bar), 24);
    build_unicode_bar(ov->u, u_bar, sizeof(u_bar), 24);

    const char *zone_tag = "OUTSIDE ZONE_REEL";
    const char *zone_col = ANSI_DIM;
    if (!dg->in_window) {
        zone_tag = "OUTSIDE WINDOW";
        zone_col = ANSI_DIM;
    } else if (dg->d <= OVERLAY_ZONE_IN) {
        zone_tag = "INSIDE ZONE_IN";
        zone_col = ANSI_MAGENTA;
    } else if (dg->d <= OVERLAY_ZONE_OUT) {
        zone_tag = "HYSTERESIS BAND";
        zone_col = ANSI_YELLOW;
    } else if (dg->d <= OVERLAY_ZONE_REEL) {
        zone_tag = "INSIDE ZONE_REEL";
        zone_col = ANSI_CYAN;
    }

    float slow_pct = (ts->frame_idx > 0)
        ? (100.0f * (float)ts->slow_frame_count / (float)ts->frame_idx)
        : 0.0f;

    // Move cursor to top-left without clearing first (zero-flicker overwrite)
    fprintf(fp, "\033[H");
    fprintf(fp, ANSI_BOLD "=== ESP32-S3 IP-KVM | Radial Overlay Live Telemetry ===" ANSI_RESET "\033[K\n");
    fprintf(fp, "Target: %-15s | Video: %-12s | TTY: %s\033[K\n",
            ts->esp_ip, ts->video_dev, ts->tty_label);
    fprintf(fp, "KB Grab: %s%-9s" ANSI_RESET " | Metrics: %-20s | Frames: %7u | Uptime: %7.2fs\033[K\n",
            kb_grabbed ? ANSI_GREEN : ANSI_YELLOW,
            kb_grabbed ? "GRABBED" : "UNGRABBED",
            ts->bin_path, ts->frame_idx, elapsed_sec);
    fprintf(fp, "\033[K\n");

    fprintf(fp, ANSI_BOLD "[Frame Timing]" ANSI_RESET "\033[K\n");
    fprintf(fp, "  FPS (3-frame): %6.1f Hz  | dt_raw: %7.2f ms | dt_clamp: %6.2f ms\033[K\n",
            rolling_fps, raw_dt_ms, clamped_dt_ms);
    fprintf(fp, "  Min / Max dt : %6.2f / %6.2f ms | Slow (>%.0fms): %s%5u (%.2f%%)" ANSI_RESET "\033[K\n",
            (ts->min_dt_ms < 1e8f) ? ts->min_dt_ms : 0.0f,
            ts->max_dt_ms,
            TELEMETRY_SLOW_FRAME_MS,
            (ts->slow_frame_count > 0) ? ANSI_YELLOW : ANSI_GREEN,
            ts->slow_frame_count, slow_pct);
    fprintf(fp, "\033[K\n");

    fprintf(fp, ANSI_BOLD "[State & Pointer]" ANSI_RESET "\033[K\n");
    fprintf(fp, "  Phase        : %s>> %-20s <<" ANSI_RESET "\033[K\n",
            phase_ansi_color(ov->phase), overlay_phase_name(ov->phase));
    fprintf(fp, "  Spatial Gate : in_window=%s%-3s" ANSI_RESET " | in_reel_zone=%s%-3s" ANSI_RESET "\033[K\n",
            dg->in_window ? ANSI_GREEN : ANSI_DIM, dg->in_window ? "YES" : "NO",
            dg->in_reel_zone ? ANSI_CYAN : ANSI_DIM, dg->in_reel_zone ? "YES" : "NO");
    fprintf(fp, "  Pointer Pos  : raw=(%5d, %5d) px | 1-Euro=(%6.1f, %6.1f) px\033[K\n",
            raw_x, raw_y, dg->x_hat, dg->y_hat);
    fprintf(fp, "  Pointer Vel  : vx =%+8.1f px/s    | vy    =%+8.1f px/s\033[K\n",
            dg->vx_hat, dg->vy_hat);
    fprintf(fp, "\033[K\n");

    fprintf(fp, ANSI_BOLD "[Polar Kinematics]" ANSI_RESET "  (Anchor: %.0f, %.0f | IN: %.1f | OUT: %.1f | REEL: %.1f)\033[K\n",
            OVERLAY_CENTER_X, OVERLAY_CENTER_Y, OVERLAY_ZONE_IN, OVERLAY_ZONE_OUT, OVERLAY_ZONE_REEL);
    fprintf(fp, "  Radius d     : %7.1f px (d_scaled: %5.1f px) %s[%s]" ANSI_RESET "\033[K\n",
            dg->d, dg->d_scaled, zone_col, zone_tag);
    fprintf(fp, "  Velocities   : v_theta=%+8.1f px/s (CCW+) | v_r=%+8.1f px/s\033[K\n",
            dg->v_theta, dg->v_r);
    fprintf(fp, "  omega_ptr    : %+7.2f rad/s (deadband: %.2f rad/s)\033[K\n",
            dg->omega_ptr, OVERLAY_OMEGA_DEADBAND);
    fprintf(fp, "  Circularity  :   %5.3f  %s  (psi)\033[K\n",
            dg->psi, psi_bar);
    fprintf(fp, "\033[K\n");

    fprintf(fp, ANSI_BOLD "[Control Law & Visual Ring]" ANSI_RESET "\033[K\n");
    fprintf(fp, "  Rates (1/s)  : ccw_drive=%+6.3f | spring_open=-%5.3f | du/dt=%s%+6.3f" ANSI_RESET "\033[K\n",
            dg->ccw_drive, dg->spring_open,
            (dg->du_dt > 0.005f) ? ANSI_GREEN : ((dg->du_dt < -0.005f) ? ANSI_RED : ANSI_DIM),
            dg->du_dt);
    fprintf(fp, "  Compression u:   %5.3f  %s  | R_curr: %5.1f px\033[K\n",
            ov->u, u_bar, dg->r_curr);
    fprintf(fp, "  Visual Spin  : omega_vis=%6.2f rad/s | Singularities: %u\033[K\n",
            ov->omega_vis, dg->singularity_count);
    fprintf(fp, "\033[K\n");

    fprintf(fp, ANSI_BOLD "[Recent Events]" ANSI_RESET "\033[K\n");
    for (uint32_t i = 0; i < TELEMETRY_EVENT_RING_SIZE; ++i) {
        if (i < ts->event_count) {
            uint32_t idx = (ts->event_head - 1u - i) % TELEMETRY_EVENT_RING_SIZE;
            fprintf(fp, "  %s\033[K\n", ts->events[idx]);
        } else {
            fprintf(fp, "  " ANSI_DIM "---" ANSI_RESET "\033[K\n");
        }
    }
    fprintf(fp, "\033[J");
    fflush(fp);
}

void telemetry_record_frame(TelemetryState *ts,
                            float elapsed_sec,
                            float raw_dt_ms,
                            float clamped_dt_ms,
                            int raw_x,
                            int raw_y,
                            bool kb_grabbed,
                            const OverlayState *ov) {
    const OverlayDiagnostics *dg = &ov->diag;

    // 1. Update timing stats & 3-frame rolling FPS
    if (ts->frame_idx > 0) {
        if (raw_dt_ms < ts->min_dt_ms) ts->min_dt_ms = raw_dt_ms;
        if (raw_dt_ms > ts->max_dt_ms) ts->max_dt_ms = raw_dt_ms;
        if (raw_dt_ms > TELEMETRY_SLOW_FRAME_MS) ts->slow_frame_count++;
    }
    ts->dt_ring[ts->dt_ring_count % 3u] = raw_dt_ms;
    ts->dt_ring_count++;

    uint32_t win_n = (ts->dt_ring_count < 3u) ? ts->dt_ring_count : 3u;
    float sum_dt = 0.0f;
    for (uint32_t i = 0; i < win_n; ++i) {
        sum_dt += ts->dt_ring[i];
    }
    float avg_dt_ms = (win_n > 0 && sum_dt > 1e-4f) ? (sum_dt / (float)win_n) : 16.67f;
    float rolling_fps = 1000.0f / avg_dt_ms;

    // 2. Detect discrete state transitions & events
    bool event_occurred = false;
    if (!ts->prev_valid) {
        ts->prev_phase = ov->phase;
        ts->prev_in_window = dg->in_window;
        ts->prev_in_reel_zone = dg->in_reel_zone;
        ts->prev_kb_grabbed = kb_grabbed;
        ts->prev_valid = true;
        event_occurred = true;
    } else {
        if (dg->in_window != ts->prev_in_window) {
            if (dg->in_window) {
                telemetry_log_event(ts, elapsed_sec,
                                    "WINDOW: Pointer entered KVM window at (%d, %d)",
                                    raw_x, raw_y);
            } else {
                telemetry_log_event(ts, elapsed_sec,
                                    "WINDOW: Pointer left KVM window -> host desktop");
            }
            ts->prev_in_window = dg->in_window;
            ts->prev_in_reel_zone = dg->in_reel_zone;
            event_occurred = true;
        } else if (dg->in_reel_zone != ts->prev_in_reel_zone) {
            telemetry_log_event(ts, elapsed_sec,
                                "REEL  : %s ZONE_REEL (d=%.1f px) at (%d, %d)",
                                dg->in_reel_zone ? "Entered" : "Exited",
                                dg->d, raw_x, raw_y);
            ts->prev_in_reel_zone = dg->in_reel_zone;
            event_occurred = true;
        }

        if (ov->phase != ts->prev_phase) {
            telemetry_log_event(ts, elapsed_sec,
                                "PHASE : %s -> %s (d=%.1f px, u=%.2f)",
                                overlay_phase_name(ts->prev_phase),
                                overlay_phase_name(ov->phase),
                                dg->d, ov->u);
            ts->prev_phase = ov->phase;
            event_occurred = true;
        }

        if (kb_grabbed != ts->prev_kb_grabbed) {
            telemetry_log_event(ts, elapsed_sec,
                                "INPUT : X11 Keyboard Grab %s",
                                kb_grabbed ? "ENGAGED" : "RELEASED");
            ts->prev_kb_grabbed = kb_grabbed;
            event_occurred = true;
        }
    }

    if (dg->singularity_triggered) {
        telemetry_log_event(ts, elapsed_sec,
                            "EVENT : Singularity #%u triggered! (u >= 1.00)",
                            dg->singularity_count);
        event_occurred = true;
    }

    ts->frame_idx++;

    // 3. Write 64-byte binary metric record
    if (ts->bin_fp) {
        uint8_t flags = 0;
        if (dg->in_window)             flags |= METRIC_FLAG_IN_WINDOW;
        if (dg->in_reel_zone)          flags |= METRIC_FLAG_IN_REEL_ZONE;
        if (dg->singularity_triggered) flags |= METRIC_FLAG_SINGULARITY;
        if (kb_grabbed)                flags |= METRIC_FLAG_KB_GRABBED;

        OverlayMetricRecord rec;
        rec.frame_idx     = ts->frame_idx;
        rec.elapsed_sec   = elapsed_sec;
        rec.raw_dt_ms     = raw_dt_ms;
        rec.clamped_dt_ms = clamped_dt_ms;
        rec.phase         = (uint8_t)ov->phase;
        rec.flags         = flags;
        rec.raw_x         = (int16_t)raw_x;
        rec.raw_y         = (int16_t)raw_y;
        rec.reserved      = 0;
        rec.d             = dg->d;
        rec.v_theta       = dg->v_theta;
        rec.v_r           = dg->v_r;
        rec.omega_ptr     = dg->omega_ptr;
        rec.psi           = dg->psi;
        rec.ccw_drive     = dg->ccw_drive;
        rec.spring_open   = dg->spring_open;
        rec.u             = ov->u;
        rec.omega_vis     = ov->omega_vis;
        rec.r_curr        = dg->r_curr;

        fwrite(&rec, sizeof(rec), 1, ts->bin_fp);
        if ((elapsed_sec - ts->last_bin_flush_sec) >= 0.25f || event_occurred) {
            fflush(ts->bin_fp);
            ts->last_bin_flush_sec = elapsed_sec;
        }
    }

    // 4. Refresh dynamic in-place TTY view at ~20 Hz (or immediately on discrete events)
    if (event_occurred || (elapsed_sec - ts->last_tty_draw_sec) >= 0.05f) {
        render_tty_dashboard(ts, elapsed_sec, raw_dt_ms, clamped_dt_ms,
                             rolling_fps, raw_x, raw_y, kb_grabbed, ov);
        ts->last_tty_draw_sec = elapsed_sec;
    }
}

void telemetry_close(TelemetryState *ts) {
    if (ts->bin_fp) {
        fflush(ts->bin_fp);
        fclose(ts->bin_fp);
        ts->bin_fp = NULL;
    }

    if (ts->tty_fp) {
        if (ts->tty_ansi_enabled) {
            fprintf(ts->tty_fp, "\033[?25h" ANSI_RESET "\n[INFO] Telemetry stopped.\n");
            fflush(ts->tty_fp);
        }
        if (ts->owns_tty_fp) {
            fclose(ts->tty_fp);
        }
        ts->tty_fp = NULL;
    }
}

