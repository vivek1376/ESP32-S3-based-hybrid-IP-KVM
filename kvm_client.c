/**
 * kvm_client.c — High-Performance 1080p YUYV422 IP-KVM Client for ESP32-S3
 *
 * Features:
 *  - Direct GPU rendering of 1920x1080 YUYV422 (/dev/video0) via SDL_PIXELFORMAT_YUY2
 *  - Seamless 16-bit Absolute HID Mouse [0..32767] (free cursor flow on 4K desktop)
 *  - X11 Exclusive Keyboard Grab (Overrides Cinnamon Alt+Tab, Super, Alt+F4)
 *  - Click inside window engages Keyboard Grab; Ctrl+Esc detaches Keyboard Grab
 *  - SDL_Scancode maps 1:1 to USB HID Usage Page 0x07 Keycodes!
 *
 * Compile:
 *   gcc -O3 kvm_client.c -o kvm_client $(sdl2-config --cflags --libs) -lpthread
 *
 * Run:
 *   ./kvm_client 192.168.1.150 /dev/video0
 */

#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WIN_W 1920
#define WIN_H 1080
#define FRAME_BYTES (WIN_W * WIN_H * 2) // YUYV422 = 2 bytes per pixel (4,147,200 bytes)
#define HID_MAX 32767
#define UDP_PORT 4210

// ESP32-S3 Wire Protocol Opcodes
#define PKT_MOUSE_BTN   0x02
#define PKT_KEY_EVENT   0x03
#define PKT_MOUSE_ABS   0x04
#define PKT_RELEASE_ALL 0xFF

// TinyUSB Mouse Button Masks
#define HID_BTN_LEFT    0x01
#define HID_BTN_RIGHT   0x02
#define HID_BTN_MIDDLE  0x04
#define HID_BTN_BACK    0x08
#define HID_BTN_FORWARD 0x10

// Shared video state between V4L2/FFmpeg reader thread and SDL2 render thread
static uint8_t frame_buf[FRAME_BYTES];
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool new_frame_ready = false;
static volatile bool app_running = true;
static const char *video_dev = "/dev/video0";

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

// Send absolute mouse coordinates + scroll wheel over UDP
static inline void send_mouse_abs(int sock, struct sockaddr_in *addr, int x, int y, int8_t wheel, int8_t pan) {
    if (x < 0) x = 0;
    if (x >= WIN_W) x = WIN_W - 1;
    if (y < 0) y = 0;
    if (y >= WIN_H) y = WIN_H - 1;

    int16_t hid_x = (int16_t)(((int64_t)x * HID_MAX) / (WIN_W - 1));
    int16_t hid_y = (int16_t)(((int64_t)y * HID_MAX) / (WIN_H - 1));

    uint8_t pkt[7];
    pkt[0] = PKT_MOUSE_ABS;
    pkt[1] = (uint8_t)(hid_x & 0xFF);
    pkt[2] = (uint8_t)((hid_x >> 8) & 0xFF);
    pkt[3] = (uint8_t)(hid_y & 0xFF);
    pkt[4] = (uint8_t)((hid_y >> 8) & 0xFF);
    pkt[5] = (uint8_t)wheel;
    pkt[6] = (uint8_t)pan;

    sendto(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)addr, sizeof(*addr));
}

static inline void send_mouse_btn(int sock, struct sockaddr_in *addr, uint8_t btn_mask, uint8_t pressed) {
    uint8_t pkt[3] = {PKT_MOUSE_BTN, btn_mask, pressed};
    sendto(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)addr, sizeof(*addr));
}

static inline void send_key_event(int sock, struct sockaddr_in *addr, uint8_t hid_code, uint8_t pressed) {
    uint8_t pkt[3] = {PKT_KEY_EVENT, hid_code, pressed};
    sendto(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)addr, sizeof(*addr));
}

static inline void send_release_all(int sock, struct sockaddr_in *addr) {
    uint8_t pkt[1] = {PKT_RELEASE_ALL};
    sendto(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)addr, sizeof(*addr));
}

static uint8_t sdl_btn_to_hid(uint8_t sdl_btn) {
    switch (sdl_btn) {
        case SDL_BUTTON_LEFT:   return HID_BTN_LEFT;
        case SDL_BUTTON_RIGHT:  return HID_BTN_RIGHT;
        case SDL_BUTTON_MIDDLE: return HID_BTN_MIDDLE;
        case SDL_BUTTON_X1:     return HID_BTN_BACK;
        case SDL_BUTTON_X2:     return HID_BTN_FORWARD;
        default:                return 0;
    }
}

static void update_title(SDL_Window *win, const char *ip, bool kb_grabbed) {
    char title[256];
    if (kb_grabbed) {
        snprintf(title, sizeof(title),
                 "[KEYBOARD GRABBED - Ctrl+Esc to Detach] ESP32-S3 IP-KVM (%s) - %s",
                 ip, video_dev);
    } else {
        snprintf(title, sizeof(title),
                 "[UNGRABBED - Click Window to Grab Keyboard] ESP32-S3 IP-KVM (%s) - %s",
                 ip, video_dev);
    }
    SDL_SetWindowTitle(win, title);
}

int main(int argc, char *argv[]) {
    const char *esp_ip = (argc > 1) ? argv[1] : "192.168.1.150";
    if (argc > 2) video_dev = argv[2];

    // Setup non-blocking UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in esp_addr;
    memset(&esp_addr, 0, sizeof(esp_addr));
    esp_addr.sin_family = AF_INET;
    esp_addr.sin_port = htons(UDP_PORT);
    inet_pton(AF_INET, esp_ip, &esp_addr.sin_addr);

    // Crucial SDL2 Hints for X11 / Cinnamon:
    // 1. Allow grabbing the keyboard without confining the mouse cursor
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    // 2. Prevent Cinnamon compositor from bypassing or throttling us
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    // Create fixed 1920x1080 window (remove SDL_WINDOW_BORDERLESS if you want a titlebar to drag it)
    SDL_Window *win = SDL_CreateWindow(
        "ESP32-S3 IP-KVM",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS
    );

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // SDL_PIXELFORMAT_YUY2 accepts raw YUYV422 directly on the GPU!
    SDL_Texture *tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H
    );

    // Start grabbing keyboard immediately on launch
    bool kb_grabbed = true;
    SDL_SetWindowKeyboardGrab(win, SDL_TRUE);
    update_title(win, esp_ip, kb_grabbed);

    // Launch V4L2 capture thread
    pthread_t cap_tid;
    pthread_create(&cap_tid, NULL, video_capture_thread, NULL);

    SDL_Event ev;
    while (app_running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    app_running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        if (kb_grabbed) {
                            kb_grabbed = false;
                            SDL_SetWindowKeyboardGrab(win, SDL_FALSE);
                            update_title(win, esp_ip, kb_grabbed);
                        }
                        send_release_all(sock, &esp_addr);
                    }
                    break;

                // Seamless Absolute Mouse Motion (works whether keyboard is grabbed or not!)
                case SDL_MOUSEMOTION:
                    send_mouse_abs(sock, &esp_addr, ev.motion.x, ev.motion.y, 0, 0);
                    break;

                case SDL_MOUSEBUTTONDOWN: {
                    // If keyboard isn't grabbed yet, clicking inside the window grabs it
                    // AND still forwards the click seamlessly to the remote laptop!
                    if (!kb_grabbed) {
                        kb_grabbed = true;
                        SDL_SetWindowKeyboardGrab(win, SDL_TRUE);
                        update_title(win, esp_ip, kb_grabbed);
                    }
                    send_mouse_abs(sock, &esp_addr, ev.button.x, ev.button.y, 0, 0);
                    uint8_t btn = sdl_btn_to_hid(ev.button.button);
                    if (btn) send_mouse_btn(sock, &esp_addr, btn, 1);
                    break;
                }

                case SDL_MOUSEBUTTONUP: {
                    send_mouse_abs(sock, &esp_addr, ev.button.x, ev.button.y, 0, 0);
                    uint8_t btn = sdl_btn_to_hid(ev.button.button);
                    if (btn) send_mouse_btn(sock, &esp_addr, btn, 0);
                    break;
                }

                case SDL_MOUSEWHEEL: {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    int8_t wheel = (int8_t)ev.wheel.y;
                    int8_t pan   = (int8_t)ev.wheel.x;
                    send_mouse_abs(sock, &esp_addr, mx, my, wheel, pan);
                    break;
                }

                case SDL_KEYDOWN: {
                    if (!kb_grabbed || ev.key.repeat != 0) break; // Ignore OS auto-repeat!

                    SDL_Scancode sc = ev.key.keysym.scancode;
                    SDL_Keymod mod  = SDL_GetModState();

                    // Escape Hatch: Ctrl + Esc detaches X11 Keyboard Grab
                    if (sc == SDL_SCANCODE_ESCAPE && (mod & KMOD_CTRL)) {
                        kb_grabbed = false;
                        SDL_SetWindowKeyboardGrab(win, SDL_FALSE);
                        send_release_all(sock, &esp_addr);
                        update_title(win, esp_ip, kb_grabbed);
                        printf("[INFO] Ctrl+Esc pressed: Keyboard grab released back to Cinnamon.\n");
                        break;
                    }

                    // SDL_Scancode values (4..231) are identical to USB HID Usage Page 0x07!
                    if (sc >= 0x04 && sc <= 0xE7) {
                        send_key_event(sock, &esp_addr, (uint8_t)sc, 1);
                    }
                    break;
                }

                case SDL_KEYUP: {
                    if (!kb_grabbed) break;
                    SDL_Scancode sc = ev.key.keysym.scancode;
                    if (sc >= 0x04 && sc <= 0xE7) {
                        send_key_event(sock, &esp_addr, (uint8_t)sc, 0);
                    }
                    break;
                }
            }
        }

        // Update GPU Texture if a fresh YUYV422 frame arrived from /dev/video0
        if (new_frame_ready) {
            pthread_mutex_lock(&frame_mutex);
            SDL_UpdateTexture(tex, NULL, frame_buf, WIN_W * 2);
            new_frame_ready = false;
            pthread_mutex_unlock(&frame_mutex);
        }

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    send_release_all(sock, &esp_addr);
    SDL_SetWindowKeyboardGrab(win, SDL_FALSE);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    close(sock);
    return 0;
}
