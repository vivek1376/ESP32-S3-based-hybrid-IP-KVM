#!/usr/bin/env python3
"""
ESP32-S3 Network KVM Transmitter GUI for Linux (X11 / Debian Cinnamon)
Fixed 1920x1080 HUD that displays real-time input telemetry and relays
raw USB HID keycodes and mouse deltas over UDP.
"""

import sys
import socket
import struct
import tkinter as tk
from collections import deque

# ================= CONFIGURATION =================
DEFAULT_ESP_IP = "192.168.1.150"  # Can also be passed via CLI: python3 kvm_sender.py 192.168.1.X
UDP_PORT = 4210
WIN_W, WIN_H = 1920, 1080
CENTER_X, CENTER_Y = WIN_W // 2, WIN_H // 2

# Packet Opcodes matching ESP32 firmware
PKT_MOUSE_MOVE  = 0x01
PKT_MOUSE_BTN   = 0x02
PKT_KEY_EVENT   = 0x03
PKT_RELEASE_ALL = 0xFF

# Mouse button masks matching TinyUSB
MOUSE_LEFT   = 0x01
MOUSE_RIGHT  = 0x02
MOUSE_MIDDLE = 0x04

# X11 Keysym Name -> USB HID Usage ID (Page 0x07) Mapping Table
KEYSYM_TO_HID = {
    # Letters a-z
    **{chr(ord('a') + i): 0x04 + i for i in range(26)},
    **{chr(ord('A') + i): 0x04 + i for i in range(26)},
    # Numbers 1-9, 0 (and their shifted symbols so releasing while Shift is held still maps cleanly)
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
    # Control & Whitespace
    'Return': 0x28, 'Escape': 0x29, 'BackSpace': 0x2A, 'Tab': 0x2B, 'space': 0x2C,
    # Punctuation
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
    # Function keys F1-F11 (F12 is reserved as local Capture Release hotkey)
    **{f'F{i}': 0x3A + (i - 1) for i in range(1, 12)},
    # Navigation & Editing
    'Insert': 0x49, 'Home': 0x4A, 'Prior': 0x4B,  # Page Up
    'Delete': 0x4C, 'End': 0x4D, 'Next': 0x4E,    # Page Down
    'Right': 0x4F, 'Left': 0x50, 'Down': 0x51, 'Up': 0x52,
    # Modifiers (0xE0 - 0xE7 in USB HID)
    'Control_L': 0xE0, 'Shift_L': 0xE1, 'Alt_L': 0xE2, 'Super_L': 0xE3,
    'Control_R': 0xE4, 'Shift_R': 0xE5, 'Alt_R': 0xE6, 'Super_R': 0xE7,
}


class KVMRelayApp:
    def __init__(self, root: tk.Tk, esp_ip: str):
        self.root = root
        self.esp_ip = esp_ip
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)

        self.captured = False
        self.ignore_warp_event = False
        self.held_hid_keys = set()
        self.pending_releases = {}  # keycode -> after_id (for X11 auto-repeat debounce)
        self.keycode_to_hid = {}    # Hardware X11 keycode -> HID code cache
        self.event_log = deque(maxlen=18)
        self.packets_sent = 0
        self.last_dx, self.last_dy = 0, 0
        self.mouse_buttons_held = set()

        self._setup_window()
        self._bind_events()
        self._render_hud()

    def _setup_window(self):
        self.root.title(f"ESP32-S3 Network KVM Relay -> {self.esp_ip}:{UDP_PORT}")
        self.root.geometry(f"{WIN_W}x{WIN_H}+0+0")
        self.root.minsize(WIN_W, WIN_H)
        self.root.maxsize(WIN_W, WIN_H)
        self.root.configure(bg="#0d1117")

        self.canvas = tk.Canvas(
            self.root, width=WIN_W, height=WIN_H,
            bg="#0d1117", highlightthickness=0
        )
        self.canvas.pack(fill=tk.BOTH, expand=True)

    def _bind_events(self):
        # Click inside window to engage Mouse/Keyboard Capture
        self.canvas.bind("<ButtonPress-1>", self._on_mouse_down)
        self.canvas.bind("<ButtonRelease-1>", self._on_mouse_up)
        self.canvas.bind("<ButtonPress-2>", self._on_mouse_down)
        self.canvas.bind("<ButtonRelease-2>", self._on_mouse_up)
        self.canvas.bind("<ButtonPress-3>", self._on_mouse_down)
        self.canvas.bind("<ButtonRelease-3>", self._on_mouse_up)

        # Linux X11 Scroll Wheel uses Button-4 (Up) and Button-5 (Down)
        self.canvas.bind("<Button-4>", lambda e: self._on_scroll(e, +1))
        self.canvas.bind("<Button-5>", lambda e: self._on_scroll(e, -1))

        self.canvas.bind("<Motion>", self._on_mouse_move)
        self.root.bind("<KeyPress>", self._on_key_down)
        self.root.bind("<KeyRelease>", self._on_key_up)
        self.root.bind("<FocusOut>", self._on_focus_out)

    # ---------------- NETWORK TRANSMISSION ----------------
    def _send_udp(self, payload: bytes):
        try:
            self.sock.sendto(payload, (self.esp_ip, UDP_PORT))
            self.packets_sent += 1
        except OSError:
            pass

    def _log(self, msg: str):
        self.event_log.appendleft(msg)

    # ---------------- CAPTURE CONTROL ----------------
    def _engage_capture(self):
        if self.captured:
            return
        self.captured = True
        self.root.config(cursor="none")  # Hide local cursor while controlling remote PC
        self.ignore_warp_event = True
        self.root.event_generate("<Motion>", warp=True, x=CENTER_X, y=CENTER_Y)
        self._log(">>> CAPTURE ENGAGED (Press F12 to release cursor)")
        self._render_hud()

    def _release_capture(self, reason="F12 pressed"):
        if not self.captured:
            return
        self.captured = False
        self.root.config(cursor="")
        self.held_hid_keys.clear()
        self.mouse_buttons_held.clear()
        self._send_udp(struct.pack("B", PKT_RELEASE_ALL))
        self._log(f"<<< CAPTURE RELEASED ({reason}) - Sent PKT_RELEASE_ALL")
        self._render_hud()

    def _on_focus_out(self, _event):
        if self.captured:
            self._release_capture("Window lost focus")

    # ---------------- MOUSE HANDLERS ----------------
    def _tk_btn_to_hid(self, num: int) -> int:
        return {1: MOUSE_LEFT, 2: MOUSE_MIDDLE, 3: MOUSE_RIGHT}.get(num, 0)

    def _on_mouse_down(self, event):
        if not self.captured:
            self._engage_capture()
            return
        btn = self._tk_btn_to_hid(event.num)
        if btn:
            self.mouse_buttons_held.add(event.num)
            self._send_udp(struct.pack("BBB", PKT_MOUSE_BTN, btn, 1))
            self._log(f"MOUSE DOWN : Button {event.num} (HID mask 0x{btn:02X})")
            self._render_hud()

    def _on_mouse_up(self, event):
        if not self.captured:
            return
        btn = self._tk_btn_to_hid(event.num)
        if btn:
            self.mouse_buttons_held.discard(event.num)
            self._send_udp(struct.pack("BBB", PKT_MOUSE_BTN, btn, 0))
            self._log(f"MOUSE UP   : Button {event.num} (HID mask 0x{btn:02X})")
            self._render_hud()

    def _on_scroll(self, _event, direction: int):
        if not self.captured:
            return
        self._send_udp(struct.pack("Bbbbb", PKT_MOUSE_MOVE, 0, 0, direction, 0))
        self._log(f"MOUSE WHEEL: {'UP (+1)' if direction > 0 else 'DOWN (-1)'}")
        self._render_hud()

    def _on_mouse_move(self, event):
        if not self.captured:
            return
        if self.ignore_warp_event:
            self.ignore_warp_event = False
            return

        dx = event.x - CENTER_X
        dy = event.y - CENTER_Y
        if dx == 0 and dy == 0:
            return

        # Clamp deltas to signed 8-bit integer range [-127, 127]
        dx_clamped = max(-127, min(127, dx))
        dy_clamped = max(-127, min(127, dy))

        self.last_dx, self.last_dy = dx_clamped, dy_clamped
        self._send_udp(struct.pack("Bbbbb", PKT_MOUSE_MOVE, dx_clamped, dy_clamped, 0, 0))

        # Warp cursor back to center so it never hits the 1920x1080 screen border
        self.ignore_warp_event = True
        self.root.event_generate("<Motion>", warp=True, x=CENTER_X, y=CENTER_Y)
        self._render_hud()

    # ---------------- KEYBOARD HANDLERS ----------------
    def _on_key_down(self, event):
        # F12 is our local escape hatch to release mouse capture
        if event.keysym == "F12":
            self._release_capture("F12 Hotkey")
            return

        if not self.captured:
            return

        # If X11 auto-repeat scheduled a fake KeyRelease for this physical keycode, cancel it!
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
        if not self.captured or event.keysym == "F12":
            return

        # Defer release by 5ms to detect X11 synthetic auto-repeat KeyRelease/KeyPress pairs
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

        # Top Status Banner
        banner_color = "#1f6feb" if self.captured else "#30363d"
        c.create_rectangle(0, 0, WIN_W, 85, fill=banner_color, outline="")
        status_txt = (
            "ACTIVE CAPTURE — Relaying all input to ESP32 (Press F12 to unlock cursor)"
            if self.captured else
            "STANDBY — Click anywhere inside this window to capture Mouse & Keyboard"
        )
        c.create_text(
            40, 42, anchor="w", text=status_txt,
            fill="#ffffff", font=("Monospace", 18, "bold")
        )
        c.create_text(
            WIN_W - 40, 42, anchor="e",
            text=f"Target: {self.esp_ip}:{UDP_PORT}  |  UDP Packets: {self.packets_sent}",
            fill="#c9d1d9", font=("Monospace", 14)
        )

        # Left Panel: Live State Telemetry
        c.create_text(50, 140, anchor="nw", text="LIVE INPUT STATE", fill="#58a6ff", font=("Monospace", 16, "bold"))
        held_hex = ", ".join(f"0x{k:02X}" for k in sorted(self.held_hid_keys)) or "None"
        btns_str = ", ".join(str(b) for b in sorted(self.mouse_buttons_held)) or "None"

        telemetry = (
            f"Last Mouse Delta (dx, dy) : ({self.last_dx:+4d}, {self.last_dy:+4d})\n"
            f"Mouse Buttons Held        : [{btns_str}]\n"
            f"Active USB HID Keycodes   : [{held_hex}]"
        )
        c.create_text(50, 185, anchor="nw", text=telemetry, fill="#e6edf3", font=("Monospace", 15))

        # Right Panel: Event Stream Log
        c.create_text(50, 320, anchor="nw", text="PACKET TELEMETRY STREAM", fill="#58a6ff", font=("Monospace", 16, "bold"))
        log_text = "\n".join(self.event_log)
        c.create_text(50, 365, anchor="nw", text=log_text, fill="#8b949e", font=("Monospace", 14))


if __name__ == "__main__":
    target_ip = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ESP_IP
    root = tk.Tk()
    app = KVMRelayApp(root, target_ip)
    root.mainloop()
