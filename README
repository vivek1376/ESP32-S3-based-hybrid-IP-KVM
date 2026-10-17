# ESP32-S3 Hybrid IP-KVM (Low-Latency V4L2 + USB-OTG HID Bridge)

A lightweight, sub-frame-latency hardware IP-KVM built for Linux (X11) desktops using an **ESP32-S3** microcontroller and a **USB V4L2 HDMI Capture Card**.

Unlike software KM switches (*Barrier*, *Synergy*, *Input Leap*) that require installing background daemons on the target machine, this project emulates a **physical composite USB HID Keyboard and Absolute Digitizer Mouse** via the ESP32-S3's native USB-OTG silicon. It works at the BIOS/UEFI level, in GRUB bootloaders, on locked corporate laptops, and across any OS without installing software on the target computer.

---

## System Architecture

```text
+-----------------------------------------------------------------------+
| HOST DESKTOP (Linux / X11)                                            |
|                                                                       |
|  +-----------------------------------------------------------------+  |
|  | kvm_client (C + SDL2 + FFmpeg V4L2)                             |  |
|  |  - Renders 1920x1080 YUYV422 directly to GPU (SDL_PIXELFORMAT)  |  |
|  |  - Seamless 16-bit Absolute Mouse viewport (no cursor locking!) |  |
|  |  - Active X11 Keyboard Grab (forwards Alt+Tab, Super, Alt+F4)   |  |
|  +-----------------------------------------------------------------+  |
+-------------------|---------------------------------------^-----------+
                    | Wi-Fi LAN (UDP Port 4210)             | USB 3.0
                    | Raw Binary HID Frames                 | (/dev/video0)
                    v                                       |
      +---------------------------+           +---------------------------+
      | ESP32-S3 Dev Board        |           | USB HDMI Capture Card     |
      |  - Core 0: Wi-Fi UDP Rx   |           |  - 1920x1080 @ 60fps      |
      |  - Core 1: TinyUSB Stack  |           |  - YUYV422 Uncompressed   |
      |  - GPIO48: WS2812 Status  |           +-------------^-------------+
      +-------------|-------------+                         |
                    | USB-C (Port: "USB" / GPIO19 & 20)     | HDMI Out
                    | Composite USB HID Keyboard + Mouse    |
                    v                                       |
+-----------------------------------------------------------|-----------+
| TARGET COMPUTER / LAPTOP (Any OS / BIOS / Bootloader)                 |
+-----------------------------------------------------------------------+
```

---

## Key Features

* **Zero-Copy GPU Video Rendering:** Streams raw `1920x1080` `yuyv422` video from `/dev/video0` directly into an `SDL_PIXELFORMAT_YUY2` hardware texture without CPU `YUYV -> RGB` conversion overhead.
* **Seamless Desktop Pointer Flow:** Uses USB HID **Absolute Digitizer Coordinates** (`0..32767`) rather than relative mouse deltas. Your cursor glides freely into and out of the 1080p KVM window across a multi-monitor or 4K desktop without needing to lock/unlock the pointer.
* **Full Desktop Environment Key Override:** Acquires an exclusive X11 keyboard grab (`SDL_SetWindowKeyboardGrab`) while focused so window manager shortcuts (`Alt+Tab`, `Super/Windows` key, `Alt+F4`) execute on the target machine rather than the local desktop. Press **`Ctrl + Esc`** at any time to release keyboard focus back to your local window manager.
* **1:1 Hardware Scancode Mapping:** Maps `SDL_Scancode` directly to USB HID Usage Page `0x07` keycodes via `Keyboard.pressRaw()` and `Keyboard.releaseRaw()`, supporting full 6-Key Rollover (6KRO), modifier chords, and held keys without X11 auto-repeat jitter.

---

## Hardware Requirements

1. **ESP32-S3 Development Board (`ESP32-S3-WROOM-1` N16R8 or N8R2)** with Dual USB Type-C ports:
   * **`COM` Port (CH343P USB-to-UART Bridge on GPIO43/44):** Used for flashing firmware and viewing serial debug logs.
   * **`USB` Port (Native USB-OTG on GPIO19 `D-` / GPIO20 `D+`):** Connected to the target computer to emulate a USB HID Keyboard and Mouse.
   * **Onboard WS2812 RGB LED (GPIO48):** Displays real-time connection and input telemetry.
2. **USB V4L2 HDMI Capture Card:** Capable of outputting `1920x1080` in `yuyv422` format (e.g., MS2109 / MS2130 USB 3.0 capture dongles).
3. **Two USB Type-C Data Cables** and **One HDMI Cable**.

---

## Binary Wire Protocol (UDP Port `4210`)

All packets are transmitted over UDP in **Little-Endian** byte order, prefixed by a 1-byte Opcode:

| Opcode | Packet Name | Size | Binary Payload Layout | Description |
| :--- | :--- | :--- | :--- | :--- |
| **`0x04`** | `PKT_MOUSE_ABS` | 7 Bytes | `[0x04, x_lo, x_hi, y_lo, y_hi, wheel, pan]` | `abs_x`, `abs_y` as `int16_t` ($0 \dots 32767$); `wheel`, `pan` as `int8_t` ($-127 \dots +127$). |
| **`0x02`** | `PKT_MOUSE_BTN` | 3 Bytes | `[0x02, btn_mask, action]` | `btn_mask`: `0x01`=Left, `0x02`=Right, `0x04`=Middle, `0x08`=Back, `0x10`=Fwd. `action`: `1`=Press, `0`=Release. |
| **`0x03`** | `PKT_KEY_EVENT` | 3 Bytes | `[0x03, hid_scancode, action]` | `hid_scancode`: USB HID Usage Page `0x07` (`0x04`–`0xE7`). `action`: `1`=Press, `0`=Release. |
| **`0xFF`** | `PKT_RELEASE_ALL` | 1 Byte | `[0xFF]` | Immediately releases all held keys and mouse buttons (triggered on unfocus or `Ctrl+Esc`). |

### Onboard RGB LED (GPIO48) Status Codes
* **Solid Orange:** Initializing USB-OTG HID stack & connecting to 2.4 GHz Wi-Fi.
* **Solid Green:** Connected to Wi-Fi & listening for UDP packets.
* **Blue Flash:** Mouse button press active.
* **Purple Flash:** Keyboard key press active.

---

## Setup & Installation

### 1. Flashing the ESP32-S3 Firmware (Arduino IDE)

1. Install **Arduino IDE 2.x** and add the official Espressif Boards Manager URL in **File > Preferences**:
   ```text
   [https://espressif.github.io/arduino-esp32/package_esp32_index.json](https://espressif.github.io/arduino-esp32/package_esp32_index.json)
   ```
2. Install the **`esp32` by Espressif Systems** package (v3.x+) via Boards Manager.
3. Connect the ESP32-S3's **`COM`** port to your PC and configure the **Tools** menu:
   * **Board:** `"ESP32S3 Dev Module"`
   * **USB Mode:** `"USB-OTG (TinyUSB)"` *(Required for USB HID enumeration)*
   * **USB CDC On Boot:** `"Disabled"`
   * **Flash Size:** `"16MB (128Mb)"` *(for N16R8)* or `"8MB (64Mb)"` *(for N8R2)*
   * **PSRAM:** `"OPI PSRAM"` *(for N16R8)* or `"QSPI PSRAM"` *(for N8R2)*
4. Open the firmware sketch, enter your 2.4 GHz Wi-Fi credentials (`WIFI_SSID` and `WIFI_PASS`), and click **Upload**.
5. Open the **Serial Monitor** (`115200` baud) to note the IP address assigned to your ESP32-S3, then plug a USB-C cable from the ESP32-S3's **`USB`** port into your target laptop.

### 2. Compiling the Linux Host Client (`kvm_client`)

Install the required dependencies on Debian / Ubuntu / Linux Mint:

```bash
sudo apt update && sudo apt install -y libsdl2-dev ffmpeg gcc
```

Compile the client with optimizations enabled:

```bash
gcc -O3 kvm_client.c -o kvm_client $(sdl2-config --cflags --libs) -lpthread
```

---

## Usage

Connect the target computer's HDMI output to your capture card (`/dev/video0`) and launch `kvm_client` with your ESP32-S3's IP address:

```bash
./kvm_client 192.168.1.150 /dev/video0
```

### Controls
* **Mouse Movement:** Move your cursor freely across the `1920x1080` window—no locking or capture key needed.
* **Engage Keyboard Grab:** Click anywhere inside the KVM window. All keyboard combinations (including `Alt+Tab` and `Super`) are now sent exclusively to the remote computer.
* **Detach Keyboard Grab (`Ctrl + Esc`):** Press **`Ctrl + Esc`** to release the X11 keyboard grab back to your local Linux desktop and automatically clear all held keys on the remote target.

---

## License

MIT License
