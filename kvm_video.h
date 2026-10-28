/**
 * kvm_video.h — V4L2/FFmpeg YUYV422 Video Capture Pipeline & Texture Upload
 */

#ifndef KVM_VIDEO_H
#define KVM_VIDEO_H

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>

extern volatile bool app_running;

void kvm_video_set_device(const char *dev_path);
const char *kvm_video_get_device(void);

// Background thread: reads raw 1920x1080 yuyv422 frames from /dev/video0 via ffmpeg pipe
void *video_capture_thread(void *arg);

// Update GPU Texture if a fresh YUYV422 frame arrived from /dev/video0
void kvm_video_update_texture(SDL_Texture *tex);

#endif // KVM_VIDEO_H

