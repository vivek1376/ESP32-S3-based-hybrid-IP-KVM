/**
 * kvm_hid.c — ESP32-S3 UDP Binary HID Protocol & Socket Implementation
 */

#include "kvm_hid.h"
#include "kvm_config.h"

#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int kvm_hid_init(KvmHid *hid, const char *esp_ip, uint16_t port) {
    // Setup non-blocking UDP socket
    hid->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (hid->sock < 0) {
        return -1;
    }

    memset(&hid->addr, 0, sizeof(hid->addr));
    hid->addr.sin_family = AF_INET;
    hid->addr.sin_port = htons(port);
    inet_pton(AF_INET, esp_ip, &hid->addr.sin_addr);
    return 0;
}

void kvm_hid_close(KvmHid *hid) {
    if (hid->sock >= 0) {
        close(hid->sock);
        hid->sock = -1;
    }
}

// Send absolute mouse coordinates + scroll wheel over UDP
void send_mouse_abs(const KvmHid *hid, int x, int y, int8_t wheel, int8_t pan) {
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

    sendto(hid->sock, pkt, sizeof(pkt), MSG_DONTWAIT,
           (const struct sockaddr *)&hid->addr, sizeof(hid->addr));
}

void send_mouse_btn(const KvmHid *hid, uint8_t btn_mask, uint8_t pressed) {
    uint8_t pkt[3] = {PKT_MOUSE_BTN, btn_mask, pressed};
    sendto(hid->sock, pkt, sizeof(pkt), MSG_DONTWAIT,
           (const struct sockaddr *)&hid->addr, sizeof(hid->addr));
}

void send_key_event(const KvmHid *hid, uint8_t hid_code, uint8_t pressed) {
    uint8_t pkt[3] = {PKT_KEY_EVENT, hid_code, pressed};
    sendto(hid->sock, pkt, sizeof(pkt), MSG_DONTWAIT,
           (const struct sockaddr *)&hid->addr, sizeof(hid->addr));
}

void send_release_all(const KvmHid *hid) {
    uint8_t pkt[1] = {PKT_RELEASE_ALL};
    sendto(hid->sock, pkt, sizeof(pkt), MSG_DONTWAIT,
           (const struct sockaddr *)&hid->addr, sizeof(hid->addr));
}

uint8_t sdl_btn_to_hid(uint8_t sdl_btn) {
    switch (sdl_btn) {
        case SDL_BUTTON_LEFT:   return HID_BTN_LEFT;
        case SDL_BUTTON_RIGHT:  return HID_BTN_RIGHT;
        case SDL_BUTTON_MIDDLE: return HID_BTN_MIDDLE;
        case SDL_BUTTON_X1:     return HID_BTN_BACK;
        case SDL_BUTTON_X2:     return HID_BTN_FORWARD;
        default:                return 0;
    }
}

