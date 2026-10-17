#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

const char* WIFI_SSID = "SpectrumSetup-F7-II";
const char* WIFI_PASS = "hotelquaint425";
const uint16_t UDP_PORT = 4210;

#define LED_PIN_A 48
#define LED_PIN_B 47

// Packet Type Opcodes
#define PKT_MOUSE_MOVE   0x01
#define PKT_MOUSE_BTN    0x02
#define PKT_KEY_EVENT    0x03
#define PKT_RELEASE_ALL  0xFF

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
WiFiUDP udp;

uint8_t buf[16];

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(LED_PIN_A, r, g, b);
  // rgbLedWrite(LED_PIN_B, r, g, b);
}

void setup() {
  Serial.begin(115200);
  setLed(30, 15, 0); // Orange during initialization

  // 1. Initialize TinyUSB HID Composite Device on GPIO19 (D-) & GPIO20 (D+)
  Keyboard.begin();
  Mouse.begin();
  USB.begin();

  // 2. Initialize Wi-Fi in low-latency mode
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Keep radio awake for sub-5ms packet latency
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println("\n==========================================");
  Serial.println("ESP32-S3 USB-OTG HID KVM Bridge Ready!");
  Serial.print("Listening IP : ");
  Serial.println(WiFi.localIP());
  Serial.printf("UDP Port     : %u\n", UDP_PORT);
  Serial.println("==========================================");

  udp.begin(UDP_PORT);
  setLed(0, 20, 0); // Solid Green = Ready
}

void loop() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  int len = udp.read(buf, sizeof(buf));
  if (len < 1) return;

  uint8_t pktType = buf[0];

  switch (pktType) {
    case PKT_MOUSE_MOVE: {
      if (len >= 5) {
        int8_t dx    = (int8_t)buf[1];
        int8_t dy    = (int8_t)buf[2];
        int8_t wheel = (int8_t)buf[3];
        int8_t pan   = (int8_t)buf[4];
        Mouse.move(dx, dy, wheel, pan);
      }
      break;
    }

    case PKT_MOUSE_BTN: {
      if (len >= 3) {
        uint8_t btn    = buf[1]; // 1=Left, 2=Right, 4=Middle, 8=Back, 16=Forward
        uint8_t action = buf[2]; // 1=Press, 0=Release
        if (action == 1) {
          Mouse.press(btn);
          setLed(0, 0, 40); // Flash Blue on click
        } else {
          Mouse.release(btn);
          setLed(0, 20, 0);
        }
      }
      break;
    }

    case PKT_KEY_EVENT: {
      if (len >= 3) {
        uint8_t hidKeycode = buf[1];
        uint8_t action     = buf[2]; // 1=Press, 0=Release
        if (action == 1) {
          Keyboard.pressRaw(hidKeycode);
          setLed(40, 0, 40); // Flash Purple on keypress
        } else {
          Keyboard.releaseRaw(hidKeycode);
          setLed(0, 20, 0);
        }
      }
      break;
    }

    case PKT_RELEASE_ALL: {
      Keyboard.releaseAll();
      Mouse.release(MOUSE_ALL);
      setLed(0, 20, 0);
      Serial.println(" Panic/Unfocus: Released all keys and mouse buttons.");
      break;
    }
  }
}