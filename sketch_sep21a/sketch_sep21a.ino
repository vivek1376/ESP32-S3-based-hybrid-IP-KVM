#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

const char* WIFI_SSID = "SpectrumSetup-F7-II";
const char* WIFI_PASS = "hotelquaint425";
const uint16_t UDP_PORT = 4210;

// Confirmed on your N16R8 schematic (Page 4): WS2812 data-in is wired to GPIO48
#define LED_PIN 48

// Packet Opcodes
#define PKT_MOUSE_BTN    0x02
#define PKT_KEY_EVENT    0x03
#define PKT_MOUSE_ABS    0x04
#define PKT_RELEASE_ALL  0xFF

USBHIDKeyboard Keyboard;
USBHIDAbsoluteMouse AbsMouse; // Uses 16-bit [0..32767] absolute HID coordinates
WiFiUDP udp;

uint8_t buf[16];

inline void setLed(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(LED_PIN, r, g, b);
}

void setup() {
  Serial.begin(115200);
  setLed(30, 15, 0); // Orange: Booting

  // Initialize Composite USB HID (Keyboard + Absolute Digitizer Mouse)
  Keyboard.begin();
  AbsMouse.begin();
  USB.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Low-latency mode: disable RF sleep between DTIM beacons
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println("\n================================================");
  Serial.println("ESP32-S3 N16R8 Seamless Absolute HID KVM Ready!");
  Serial.printf("PSRAM Size   : %u bytes\n", ESP.getPsramSize());
  Serial.print("Listening IP : ");
  Serial.println(WiFi.localIP());
  Serial.printf("UDP Port     : %u\n", UDP_PORT);
  Serial.println("================================================");

  udp.begin(UDP_PORT);
  setLed(0, 20, 0); // Green: Ready
}

void loop() {
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  int len = udp.read(buf, sizeof(buf));
  if (len < 1) return;

  uint8_t pktType = buf[0];

  switch (pktType) {
    case PKT_MOUSE_ABS: {
      // 7-byte packet: [0x04, x_lo, x_hi, y_lo, y_hi, wheel, pan]
      if (len >= 7) {
        // Xtensa LX7 is Little-Endian; reconstruct signed 16-bit integers
        int16_t abs_x = (int16_t)((uint16_t)buf[1] | ((uint16_t)buf[2] << 8));
        int16_t abs_y = (int16_t)((uint16_t)buf[3] | ((uint16_t)buf[4] << 8));
        int8_t  wheel = (int8_t)buf[5];
        int8_t  pan   = (int8_t)buf[6];

        AbsMouse.move(abs_x, abs_y, wheel, pan);
      }
      break;
    }

    case PKT_MOUSE_BTN: {
      if (len >= 3) {
        uint8_t btn    = buf[1]; // 1=Left, 2=Right, 4=Middle
        uint8_t action = buf[2]; // 1=Press, 0=Release
        if (action == 1) {
          AbsMouse.press(btn);
          setLed(0, 0, 40); // Flash Blue on mouse button down
        } else {
          AbsMouse.release(btn);
          setLed(0, 20, 0);
        }
      }
      break;
    }

    case PKT_KEY_EVENT: {
      if (len >= 3) {
        uint8_t hidKeycode = buf[1];
        uint8_t action     = buf[2];
        if (action == 1) {
          Keyboard.pressRaw(hidKeycode);
          setLed(40, 0, 40); // Flash Purple on key down
        } else {
          Keyboard.releaseRaw(hidKeycode);
          setLed(0, 20, 0);
        }
      }
      break;
    }

    case PKT_RELEASE_ALL: {
      Keyboard.releaseAll();
      AbsMouse.release(MOUSE_ALL);
      setLed(0, 20, 0);
      break;
    }
  }
}