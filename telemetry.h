/**
 * telemetry.h — In-Place Dynamic Terminal/TTY Logger & Binary Metrics Recorder
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "overlay.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TELEMETRY_MAGIC             "KVMOVL01"
#define TELEMETRY_VERSION           1
#define TELEMETRY_DEFAULT_BIN_PATH  "overlay_metrics.bin"
#define TELEMETRY_EVENT_RING_SIZE   8
#define TELEMETRY_EVENT_STR_LEN     128
#define TELEMETRY_SLOW_FRAME_MS     20.0f

// Flags bitmask in OverlayMetricRecord.flags
#define METRIC_FLAG_IN_WINDOW       (1u << 0)
#define METRIC_FLAG_IN_QUAD1        (1u << 1)
#define METRIC_FLAG_SINGULARITY     (1u << 2)
#define METRIC_FLAG_KB_GRABBED      (1u << 3)

typedef struct {
    char     magic[8];      // "KVMOVL01" (8 bytes)
    uint16_t version;       // TELEMETRY_VERSION (1)
    uint16_t record_size;   // sizeof(OverlayMetricRecord) (64)
    uint32_t session_id;    // Unique run ID so webapp detects restarts
} TelemetryFileHeader;

_Static_assert(sizeof(TelemetryFileHeader) == 16, "TelemetryFileHeader must be 16 bytes");

typedef struct {
    uint32_t frame_idx;
    float    elapsed_sec;
    float    raw_dt_ms;
    float    clamped_dt_ms;
    uint8_t  phase;
    uint8_t  flags;
    int16_t  raw_x;
    int16_t  raw_y;
    uint16_t reserved;
    float    d;
    float    v_theta;
    float    v_r;
    float    omega_ptr;
    float    psi;
    float    ccw_drive;
    float    spring_open;
    float    u;
    float    omega_vis;
    float    r_curr;
} OverlayMetricRecord;

_Static_assert(sizeof(OverlayMetricRecord) == 64, "OverlayMetricRecord must be 64 bytes");

typedef struct {
    // Target binary metrics file
    FILE *bin_fp;
    char bin_path[256];
    uint32_t session_id;
    uint32_t frame_idx;
    float last_bin_flush_sec;

    // Target TTY / terminal stream for dynamic in-place dashboard
    FILE *tty_fp;
    bool owns_tty_fp;
    bool tty_ansi_enabled;
    char tty_label[128];
    char tty_io_buf[4096];
    float last_tty_draw_sec;

    // Static session metadata for header display
    char esp_ip[64];
    char video_dev[128];

    // Rolling 3-frame window & session timing stats
    float dt_ring[3];
    uint32_t dt_ring_count;
    float min_dt_ms;
    float max_dt_ms;
    uint32_t slow_frame_count;

    // Previous state for automatic transition event logging
    bool prev_valid;
    OverlayPhase prev_phase;
    bool prev_in_window;
    bool prev_in_quad1;
    bool prev_kb_grabbed;

    // Rolling event log ring buffer
    char events[TELEMETRY_EVENT_RING_SIZE][TELEMETRY_EVENT_STR_LEN];
    uint32_t event_head;
    uint32_t event_count;
} TelemetryState;

void telemetry_init(TelemetryState *ts,
                    const char *tty_arg,
                    const char *bin_path_arg,
                    const char *esp_ip,
                    const char *video_dev);

void telemetry_log_event(TelemetryState *ts, float elapsed_sec, const char *fmt, ...);

void telemetry_record_frame(TelemetryState *ts,
                            float elapsed_sec,
                            float raw_dt_ms,
                            float clamped_dt_ms,
                            int raw_x,
                            int raw_y,
                            bool kb_grabbed,
                            const OverlayState *ov);

void telemetry_close(TelemetryState *ts);

#endif // TELEMETRY_H

