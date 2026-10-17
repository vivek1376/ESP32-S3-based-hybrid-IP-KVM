#!/usr/bin/env python3
"""
Seamless Windowed ESP32-S3 KVM Transmitter (Absolute HID Digitizer Mode)
Mouse flows freely across your 4K Debian desktop into/out of the 1080p viewport.
"""

import sys
import socket
import struct
import tkinter as tk
from collections import deque

# ================= CONFIGURATION =================
DEFAULT_ESP_IP = "192.168.0.234"
UDP_PORT = 4210
WIN_W, WIN_H = 1920, 1080
HID_MAX = 32767  # USBHIDAbsoluteMouse logical coordinate range: 0..32767

# Packet Opcodes
PKT_MOUSE_BTN   = 0x02
PKT_KEY_EVENT   = 0x03
PKT_MOUSE_ABS   = 0x04
PKT_RELEASE_ALL = 0xFF

# Mouse button masks
MOUSE_LEFT   = 0x01
MOUSE_RIGHT  = 0x02
MOUSE_MIDDLE = 0x04

# X11 Keysym Name -> USB HID Usage ID (Page 0x07)
KEYSYM_TO_HID = {
    **{chr(ord('a') + i): 0x04 + i for i in range(26)},
    **{chr(ord('A') + i): 0x04 + i for i in range(26)},
    '1': 0x1E, 'exclam': 0x1E,
    '2': 0x1F, 'at': 0x1F,
    '3': 0x20, 'numbersign': 0x20,
    '4': 0x21, 'dollar': 0x21,
    '5': 0x22, 'percent': 0x22,
    '6': 0x23, 'asciicircum': 0x23,
    '7': 0x24, 'ampersand': 0x24,
    '8': 0x25, 'asterisk': 0x25,
    '9': 0x26, 'parenleft': 0x26,
    '0': 0x27, 'parenright': 0x27,
    'Return': 0x28, 'Escape': 0x29, 'BackSpace': 0x2A, 'Tab': 0x2B, 'space': 0x2C,
    'minus': 0x2D, 'underscore': 0x2D,
    'equal': 0x2E, 'plus': 0x2E,
    'bracketleft': 0x2F, 'braceleft': 0x2F,
    'bracketright': 0x30, 'braceright': 0x30,
    'backslash': 0x31, 'bar': 0x31,
    'semicolon': 0x33, 'colon': 0x33,
    'apostrophe': 0x34, 'quotedbl': 0x34,
    'grave': 0x35, 'asciitilde': 0x35,
    'comma': 0x36, 'less': 0x36,
    'period': 0x37, 'greater': 0x37,
    'slash': 0x38, 'question': 0x38,
    'Caps_Lock': 0x39,
    # All F1-F12 keys are now forwarded directly to the remote laptop!
    **{f'F{i}': 0x3A + (i - 1) for i in range(1, 13)},
    'Insert': 0x49, 'Home': 0x4A, 'Prior': 0x4B,
    'Delete': 0x4C, 'End': 0x4D, 'Next': 0x4E,
    'Right': 0x4F, 'Left': 0x50, 'Down': 0x51, 'Up': 0x52,
    'Control_L': 0xE0, 'Shift_L': 0xE1, 'Alt_L': 0xE2, 'Super_L': 0xE3,
    'Control_R': 0xE4, 'Shift_R': 0xE5, 'Alt_R': 0xE6, 'Super_R': 0xE7,
}


class SeamlessKVMApp:
    def __init__(self, root: tk.Tk, esp_ip: str):
        self.root = root
        self.esp_ip = esp_ip
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)

        self.has_kb_focus = False
        self.pointer_inside = False
        self.win_x, self.win_y = 0, 0
        self.hid_x, self.hid_y = 0, 0

        self.held_hid_keys = set()
        self.pending_releases = {}
        self.keycode_to_hid = {}
        self.mouse_buttons_held = set()
        self.event_log = deque(maxlen=18)
        self.packets_sent = 0

        self._setup_window()
        self._bind_events()
        self._render_hud()

    def _setup_window(self):
        self.root.title(f"ESP32-S3 Seamless KVM Viewport (1920x1080) -> {self.esp_ip}:{UDP_PORT}")
        self.root.geometry(f"{WIN_W}x{WIN_H}")
        self.root.minsize(WIN_W, WIN_H)
        self.root.maxsize(WIN_W, WIN_H)
        self.root.configure(bg="#0d1117")

        self.canvas = tk.Canvas(
            self.root, width=WIN_W, height=WIN_H,
            bg="#0d1117", highlightthickness=0, cursor="tcross"
        )
        self.canvas.pack(fill=tk.BOTH, expand=True)

    def _bind_events(self):
        # Seamless pointer entry/exit & motion
        self.canvas.bind("<Enter>", self._on_pointer_enter)
        self.canvas.bind("<Leave>", self._on_pointer_leave)
        self.canvas.bind("<Motion>", self._on_mouse_move)

        # Mouse clicks & scroll wheel
        self.canvas.bind("<ButtonPress-1>", self._on_mouse_down)
        self.canvas.bind("<ButtonRelease-1>", self._on_mouse_up)
        self.canvas.bind("<ButtonPress-2>", self._on_mouse_down)
        self.canvas.bind("<ButtonRelease-2>", self._on_mouse_up)
        self.canvas.bind("<ButtonPress-3>", self._on_mouse_down)
        self.canvas.bind("<ButtonRelease-3>", self._on_mouse_up)
        self.canvas.bind("<Button-4>", lambda e: self._on_scroll(e, +1))
        self.canvas.bind("<Button-5>", lambda e: self._on_scroll(e, -1))

        # Window keyboard focus tracking
        self.root.bind("<FocusIn>", self._on_focus_in)
        self.root.bind("<FocusOut>", self._on_focus_out)
        self.root.bind("<KeyPress>", self._on_key_down)
        self.root.bind("<KeyRelease>", self._on_key_up)

    # ---------------- NETWORK & COORDINATE SCALING ----------------
    def _send_udp(self, payload: bytes):
        try:
            self.sock.sendto(payload, (self.esp_ip, UDP_PORT))
            self.packets_sent += 1
        except OSError:
            pass

    def _update_and_send_abs_pos(self, x: int, y: int, wheel: int = 0):
        # Clamp window coordinates to [0, WIN_W-1] and [0, WIN_H-1]
        cx = max(0, min(WIN_W - 1, x))
        cy = max(0, min(WIN_H - 1, y))
        self.win_x, self.win_y = cx, cy

        # Scale proportionally to 16-bit USB HID Absolute range [0, 32767]
        self.hid_x = int((cx * HID_MAX) // (WIN_W - 1))
        self.hid_y = int((cy * HID_MAX) // (WIN_H - 1))

        # Pack Little-Endian: uint8 type, int16 abs_x, int16 abs_y, int8 wheel, int8 pan
        pkt = struct.pack("<Bhhbb", PKT_MOUSE_ABS, self.hid_x, self.hid_y, wheel, 0)
        self._send_udp(pkt)

    def _log(self, msg: str):
        self.event_log.appendleft(msg)

    # ---------------- FOCUS & SEAMLESS BOUNDARY HANDLERS ----------------
    def _on_pointer_enter(self, event):
        self.pointer_inside = True
        self._update_and_send_abs_pos(event.x, event.y)
        self._render_hud()

    def _on_pointer_leave(self, _event):
        self.pointer_inside = False
        self._render_hud()

    def _on_focus_in(self, _event):
        if not self.has_kb_focus:
            self.has_kb_focus = True
            self._log("WINDOW FOCUSED : Keyboard input now relaying to ESP32")
            self._render_hud()

    def _on_focus_out(self, _event):
        if self.has_kb_focus:
            self.has_kb_focus = False
            self.held_hid_keys.clear()
            self.mouse_buttons_held.clear()
            # Ensure no modifier keys stay stuck on the remote laptop when Alt-Tabbing or clicking away
            self._send_udp(struct.pack("B", PKT_RELEASE_ALL))
            self._log("WINDOW UNFOCUSED: Released all held keys/buttons")
            self._render_hud()

    # ---------------- MOUSE HANDLERS ----------------
    def _tk_btn_to_hid(self, num: int) -> int:
        return {1: MOUSE_LEFT, 2: MOUSE_MIDDLE, 3: MOUSE_RIGHT}.get(num, 0)

    def _on_mouse_move(self, event):
        self.pointer_inside = True
        self._update_and_send_abs_pos(event.x, event.y, wheel=0)
        self._render_hud()

    def _on_mouse_down(self, event):
        # Ensure window grabs keyboard focus on click AND immediately passes the click through
        self.root.focus_set()
        self._update_and_send_abs_pos(event.x, event.y)

        btn = self._tk_btn_to_hid(event.num)
        if btn:
            self.mouse_buttons_held.add(event.num)
            self._send_udp(struct.pack("BBB", PKT_MOUSE_BTN, btn, 1))
            self._log(f"MOUSE DOWN : Btn {event.num} @ ({self.win_x}, {self.win_y}) -> HID ({self.hid_x}, {self.hid_y})")
            self._render_hud()

    def _on_mouse_up(self, event):
        self._update_and_send_abs_pos(event.x, event.y)
        btn = self._tk_btn_to_hid(event.num)
        if btn:
            self.mouse_buttons_held.discard(event.num)
            self._send_udp(struct.pack("BBB", PKT_MOUSE_BTN, btn, 0))
            self._log(f"MOUSE UP   : Btn {event.num} @ ({self.win_x}, {self.win_y})")
            self._render_hud()

    def _on_scroll(self, event, direction: int):
        self._update_and_send_abs_pos(event.x, event.y, wheel=direction)
        self._log(f"MOUSE WHEEL: {'UP (+1)' if direction > 0 else 'DOWN (-1)'} @ ({self.win_x}, {self.win_y})")
        self._render_hud()

    # ---------------- KEYBOARD HANDLERS ----------------
    def _on_key_down(self, event):
        if not self.has_kb_focus:
            return

        if event.keycode in self.pending_releases:
            self.root.after_cancel(self.pending_releases.pop(event.keycode))
            return

        hid_code = KEYSYM_TO_HID.get(event.keysym)
        if hid_code is None:
            self._log(f"UNMAPPED KEY: keysym='{event.keysym}' (keycode={event.keycode})")
            self._render_hud()
            return

        self.keycode_to_hid[event.keycode] = hid_code
        if hid_code not in self.held_hid_keys:
            self.held_hid_keys.add(hid_code)
            self._send_udp(struct.pack("BBB", PKT_KEY_EVENT, hid_code, 1))
            self._log(f"KEY PRESS  : '{event.keysym}' -> HID 0x{hid_code:02X}")
            self._render_hud()

    def _on_key_up(self, event):
        if not self.has_kb_focus:
            return
        after_id = self.root.after(
            5, lambda kc=event.keycode, ks=event.keysym: self._commit_key_up(kc, ks)
        )
        self.pending_releases[event.keycode] = after_id

    def _commit_key_up(self, x11_keycode: int, keysym: str):
        self.pending_releases.pop(x11_keycode, None)
        hid_code = self.keycode_to_hid.get(x11_keycode) or KEYSYM_TO_HID.get(keysym)
        if hid_code and hid_code in self.held_hid_keys:
            self.held_hid_keys.discard(hid_code)
            self._send_udp(struct.pack("BBB", PKT_KEY_EVENT, hid_code, 0))
            self._log(f"KEY RELEASE: '{keysym}' -> HID 0x{hid_code:02X}")
            self._render_hud()

    # ---------------- HUD RENDERING ----------------
    def _render_hud(self):
        c = self.canvas
        c.delete("all")

        # Visual crosshair guides tracking the pointer inside the 1080p viewport
        if self.pointer_inside:
            c.create_line(self.win_x, 85, self.win_x, WIN_H, fill="#1f2937", dash=(4, 4))
            c.create_line(0, self.win_y, WIN_W, self.win_y, fill="#1f2937", dash=(4, 4))

        # Top Status Banner
        banner_color = "#1f6feb" if self.has_kb_focus else "#30363d"
        c.create_rectangle(0, 0, WIN_W, 85, fill=banner_color, outline="")

        focus_status = "FOCUSED (Keyboard + Mouse Active)" if self.has_kb_focus else "UNFOCUSED (Hover moves mouse; Click to focus keyboard)"
        ptr_status = "INSIDE VIEWPORT" if self.pointer_inside else "OUTSIDE ON 4K DESKTOP"

        c.create_text(
            40, 42, anchor="w",
            text=f"{focus_status}   |   Pointer: {ptr_status}",
            fill="#ffffff", font=("Monospace", 16, "bold")
        )
        c.create_text(
            WIN_W - 40, 42, anchor="e",
            text=f"Target: {self.esp_ip}:{UDP_PORT}  |  Packets: {self.packets_sent}",
            fill="#c9d1d9", font=("Monospace", 14)
        )

        # Live State Telemetry
        c.create_text(50, 140, anchor="nw", text="SEAMLESS ABSOLUTE DIGITIZER TELEMETRY", fill="#58a6ff", font=("Monospace", 16, "bold"))
        held_hex = ", ".join(f"0x{k:02X}" for k in sorted(self.held_hid_keys)) or "None"
        btns_str = ", ".join(str(b) for b in sorted(self.mouse_buttons_held)) or "None"

        telemetry = (
            f"1080p Window Pos (X, Y)   : ({self.win_x:4d}, {self.win_y:4d})\n"
            f"16-bit USB HID Pos (X, Y) : ({self.hid_x:5d}, {self.hid_y:5d})  [Range 0..32767]\n"
            f"Mouse Buttons Held        : [{btns_str}]\n"
            f"Active USB HID Keycodes   : [{held_hex}]"
        )
        c.create_text(50, 185, anchor="nw", text=telemetry, fill="#e6edf3", font=("Monospace", 15))

        # Packet Event Log
        c.create_text(50, 340, anchor="nw", text="PACKET TELEMETRY STREAM", fill="#58a6ff", font=("Monospace", 16, "bold"))
        c.create_text(50, 385, anchor="nw", text="\n".join(self.event_log), fill="#8b949e", font=("Monospace", 14))


if __name__ == "__main__":
    target_ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ESP_IP
    root = tk.Tk()
    app = SeamlessKVMApp(root, target_ip)
    root.mainloop()
