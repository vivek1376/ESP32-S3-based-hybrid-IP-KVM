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
 *   make
 *   gcc -O3 kvm_client.c kvm_video.c kvm_hid.c overlay.c telemetry.c -o kvm_client $(sdl2-config --cflags --libs) -lpthread -lm
 *
 * Run:
 *   ./kvm_client [esp_ip] [video_dev] [tty_device]
 *   ./kvm_client 192.168.1.150 /dev/video0 --tty /dev/pts/3 --metrics overlay_metrics.bin
 */

#include "kvm_config.h"
#include "kvm_hid.h"
#include "kvm_video.h"
#include "overlay.h"
#include "telemetry.h"

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog) {
    printf("Usage: %s [esp_ip] [video_dev] [tty_device] [options]\n"
           "\n"
           "Positional arguments (all optional):\n"
           "  esp_ip                 ESP32-S3 target IP (default: 192.168.1.150)\n"
           "  video_dev              V4L2 video capture device (default: /dev/video0)\n"
           "  tty_device             Optional TTY for live in-place dashboard (e.g. /dev/pts/3)\n"
           "\n"
           "Options:\n"
           "  -t, --tty <tty>        Target TTY for live in-place logging (e.g. /dev/pts/3, pts/3, 3)\n"
           "  -m, --metrics <path>   Binary metrics output file (default: overlay_metrics.bin)\n"
           "  -h, --help             Show this help message\n",
           prog);
}

static void update_title(SDL_Window *win, const char *ip, bool kb_grabbed) {
    char title[256];
    const char *video_dev = kvm_video_get_device();
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
    const char *esp_ip = "192.168.1.150";
    const char *tty_arg = NULL;
    const char *metrics_arg = TELEMETRY_DEFAULT_BIN_PATH;
    int pos_idx = 0;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if ((strcmp(arg, "-t") == 0 || strcmp(arg, "--tty") == 0) && i + 1 < argc) {
            tty_arg = argv[++i];
        } else if (strncmp(arg, "--tty=", 6) == 0) {
            tty_arg = arg + 6;
        } else if ((strcmp(arg, "-m") == 0 || strcmp(arg, "--metrics") == 0) && i + 1 < argc) {
            metrics_arg = argv[++i];
        } else if (strncmp(arg, "--metrics=", 10) == 0) {
            metrics_arg = arg + 10;
        } else if (arg[0] != '-') {
            if (pos_idx == 0) {
                esp_ip = arg;
            } else if (pos_idx == 1) {
                kvm_video_set_device(arg);
            } else if (pos_idx == 2) {
                tty_arg = arg;
            }
            pos_idx++;
        } else {
            fprintf(stderr, "[WARN] Ignoring unknown option: %s\n", arg);
        }
    }

    // Setup non-blocking UDP socket
    KvmHid hid;
    if (kvm_hid_init(&hid, esp_ip, UDP_PORT) != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize UDP socket\n");
        return 1;
    }

    // Crucial SDL2 Hints for X11 / Cinnamon:
    // 1. Allow grabbing the keyboard without confining the mouse cursor
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    // 2. Prevent Cinnamon compositor from bypassing or throttling us
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        kvm_hid_close(&hid);
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
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // SDL_PIXELFORMAT_YUY2 accepts raw YUYV422 directly on the GPU!
    SDL_Texture *tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H
    );

    // Start grabbing keyboard immediately on launch
    bool kb_grabbed = true;
    SDL_SetWindowKeyboardGrab(win, SDL_TRUE);
    update_title(win, esp_ip, kb_grabbed);

    // Initialize [TODO-1] Feedforward Radial Overlay & Live Telemetry
    OverlayState overlay;
    overlay_init(&overlay);

    TelemetryState telemetry;
    telemetry_init(&telemetry, tty_arg, metrics_arg, esp_ip, kvm_video_get_device());

    int mouse_x = 0, mouse_y = 0;
    bool cursor_in_window = (SDL_GetMouseFocus() == win);
    if (cursor_in_window) {
        SDL_GetMouseState(&mouse_x, &mouse_y);
    }

    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    uint64_t start_counter = SDL_GetPerformanceCounter();
    uint64_t prev_counter = start_counter;

    // Launch V4L2 capture thread
    pthread_t cap_tid;
    pthread_create(&cap_tid, NULL, video_capture_thread, NULL);

    SDL_Event ev;
    while (app_running) {
        float cur_elapsed = (float)(SDL_GetPerformanceCounter() - start_counter) / (float)perf_freq;

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    app_running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_ENTER) {
                        int gx = 0, gy = 0, wx = 0, wy = 0;
                        SDL_GetGlobalMouseState(&gx, &gy);
                        SDL_GetWindowPosition(win, &wx, &wy);
                        mouse_x = gx - wx;
                        mouse_y = gy - wy;
                        cursor_in_window = true;
                    } else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                        cursor_in_window = false;
                    } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        if (kb_grabbed) {
                            kb_grabbed = false;
                            SDL_SetWindowKeyboardGrab(win, SDL_FALSE);
                            update_title(win, esp_ip, kb_grabbed);
                        }
                        send_release_all(&hid);
                    }
                    break;

                // Seamless Absolute Mouse Motion (works whether keyboard is grabbed or not!)
                case SDL_MOUSEMOTION:
                    mouse_x = ev.motion.x;
                    mouse_y = ev.motion.y;
                    cursor_in_window = (mouse_x >= 0 && mouse_x < WIN_W &&
                                        mouse_y >= 0 && mouse_y < WIN_H);
                    send_mouse_abs(&hid, ev.motion.x, ev.motion.y, 0, 0);
                    break;

                case SDL_MOUSEBUTTONDOWN: {
                    mouse_x = ev.button.x;
                    mouse_y = ev.button.y;
                    cursor_in_window = (mouse_x >= 0 && mouse_x < WIN_W &&
                                        mouse_y >= 0 && mouse_y < WIN_H);
                    // If keyboard isn't grabbed yet, clicking inside the window grabs it
                    // AND still forwards the click seamlessly to the remote laptop!
                    if (!kb_grabbed) {
                        kb_grabbed = true;
                        SDL_SetWindowKeyboardGrab(win, SDL_TRUE);
                        update_title(win, esp_ip, kb_grabbed);
                    }
                    send_mouse_abs(&hid, ev.button.x, ev.button.y, 0, 0);
                    uint8_t btn = sdl_btn_to_hid(ev.button.button);
                    if (btn) send_mouse_btn(&hid, btn, 1);
                    break;
                }

                case SDL_MOUSEBUTTONUP: {
                    mouse_x = ev.button.x;
                    mouse_y = ev.button.y;
                    cursor_in_window = (mouse_x >= 0 && mouse_x < WIN_W &&
                                        mouse_y >= 0 && mouse_y < WIN_H);
                    send_mouse_abs(&hid, ev.button.x, ev.button.y, 0, 0);
                    uint8_t btn = sdl_btn_to_hid(ev.button.button);
                    if (btn) send_mouse_btn(&hid, btn, 0);
                    break;
                }

                case SDL_MOUSEWHEEL: {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    int8_t wheel = (int8_t)ev.wheel.y;
                    int8_t pan   = (int8_t)ev.wheel.x;
                    send_mouse_abs(&hid, mx, my, wheel, pan);
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
                        send_release_all(&hid);
                        update_title(win, esp_ip, kb_grabbed);
                        telemetry_log_event(&telemetry, cur_elapsed,
                                            "INPUT : Ctrl+Esc -> Keyboard grab released to Cinnamon");
                        break;
                    }

                    // SDL_Scancode values (4..231) are identical to USB HID Usage Page 0x07!
                    if (sc >= 0x04 && sc <= 0xE7) {
                        send_key_event(&hid, (uint8_t)sc, 1);
                    }
                    break;
                }

                case SDL_KEYUP: {
                    if (!kb_grabbed) break;
                    SDL_Scancode sc = ev.key.keysym.scancode;
                    if (sc >= 0x04 && sc <= 0xE7) {
                        send_key_event(&hid, (uint8_t)sc, 0);
                    }
                    break;
                }
            }
        }

        // Compute true raw frame delta time and clamped kinematic dt [0.001f, 0.100f]
        uint64_t now_counter = SDL_GetPerformanceCounter();
        float raw_dt = (float)(now_counter - prev_counter) / (float)perf_freq;
        float elapsed_sec = (float)(now_counter - start_counter) / (float)perf_freq;
        prev_counter = now_counter;

        float dt = raw_dt;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.100f) dt = 0.100f;

        overlay_update(&overlay, mouse_x, mouse_y, cursor_in_window, dt);
        telemetry_record_frame(&telemetry, elapsed_sec,
                               raw_dt * 1000.0f, dt * 1000.0f,
                               mouse_x, mouse_y, kb_grabbed, &overlay);

        // Update GPU Texture if a fresh YUYV422 frame arrived from /dev/video0
        kvm_video_update_texture(tex);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        overlay_render(&overlay, ren);
        SDL_RenderPresent(ren);
    }

    telemetry_close(&telemetry);
    send_release_all(&hid);
    SDL_SetWindowKeyboardGrab(win, SDL_FALSE);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    kvm_hid_close(&hid);
    return 0;
}
