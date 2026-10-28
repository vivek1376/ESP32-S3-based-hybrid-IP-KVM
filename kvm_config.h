/**
 * kvm_config.h — Core Viewport, Frame, and Network Constants for ESP32-S3 IP-KVM
 */

#ifndef KVM_CONFIG_H
#define KVM_CONFIG_H

#define WIN_W       1920
#define WIN_H       1080
#define FRAME_BYTES (WIN_W * WIN_H * 2) // YUYV422 = 2 bytes per pixel (4,147,200 bytes)
#define HID_MAX     32767
#define UDP_PORT    4210

#endif // KVM_CONFIG_H

