/**
 * kvm_video.c — V4L2/FFmpeg YUYV422 Video Capture Pipeline & Texture Upload
 */

#define _DEFAULT_SOURCE

#include "kvm_video.h"
#include "kvm_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Shared video state between V4L2/FFmpeg reader thread and SDL2 render thread
static uint8_t frame_buf[FRAME_BYTES];
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool new_frame_ready = false;
static const char *video_dev = "/dev/video0";

volatile bool app_running = true;

void kvm_video_set_device(const char *dev_path) {
    if (dev_path && dev_path[0] != '\0') {
        video_dev = dev_path;
    }
}

const char *kvm_video_get_device(void) {
    return video_dev;
}

// Background thread: reads raw 1920x1080 yuyv422 frames from /dev/video0 via ffmpeg pipe
void *video_capture_thread(void *arg) {
    (void)arg;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -loglevel quiet -fflags nobuffer -flags low_delay "
             "-f v4l2 -input_format yuyv422 -video_size %dx%d -i %s "
             "-f rawvideo -pix_fmt yuyv422 -",
             WIN_W, WIN_H, video_dev);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "[ERROR] Failed to open ffmpeg capture pipe\n");
        return NULL;
    }

    uint8_t *temp_buf = malloc(FRAME_BYTES);
    if (!temp_buf) {
        pclose(pipe);
        return NULL;
    }

    while (app_running) {
        size_t total_read = 0;
        while (total_read < FRAME_BYTES && app_running) {
            size_t n = fread(temp_buf + total_read, 1, FRAME_BYTES - total_read, pipe);
            if (n == 0) break;
            total_read += n;
        }
        if (total_read == FRAME_BYTES) {
            pthread_mutex_lock(&frame_mutex);
            memcpy(frame_buf, temp_buf, FRAME_BYTES);
            new_frame_ready = true;
            pthread_mutex_unlock(&frame_mutex);
        } else {
            usleep(10000);
        }
    }

    free(temp_buf);
    pclose(pipe);
    return NULL;
}

// Update GPU Texture if a fresh YUYV422 frame arrived from /dev/video0
void kvm_video_update_texture(SDL_Texture *tex) {
    if (new_frame_ready) {
        pthread_mutex_lock(&frame_mutex);
        SDL_UpdateTexture(tex, NULL, frame_buf, WIN_W * 2);
        new_frame_ready = false;
        pthread_mutex_unlock(&frame_mutex);
    }
}

