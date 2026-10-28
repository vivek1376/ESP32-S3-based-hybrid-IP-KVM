/**
 * kvm_hid.h — ESP32-S3 UDP Binary HID Protocol & Socket Interface
 */

#ifndef KVM_HID_H
#define KVM_HID_H

#include <netinet/in.h>
#include <stdint.h>

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

typedef struct {
    int sock;
    struct sockaddr_in addr;
} KvmHid;

int kvm_hid_init(KvmHid *hid, const char *esp_ip, uint16_t port);
void kvm_hid_close(KvmHid *hid);

// Send absolute mouse coordinates + scroll wheel over UDP
void send_mouse_abs(const KvmHid *hid, int x, int y, int8_t wheel, int8_t pan);
void send_mouse_btn(const KvmHid *hid, uint8_t btn_mask, uint8_t pressed);
void send_key_event(const KvmHid *hid, uint8_t hid_code, uint8_t pressed);
void send_release_all(const KvmHid *hid);
uint8_t sdl_btn_to_hid(uint8_t sdl_btn);

#endif // KVM_HID_H

