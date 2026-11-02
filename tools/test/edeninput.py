"""Synthetic keyboard input for eden-cli (SDL frontend), usable from a disconnected / locked session where SendInput does not work.

Eden's default keyboard profile maps Switch buttons to SDL scancodes (= USB HID usages, see user/config/sdl2-config.ini player_0_*). This module posts
WM_KEYDOWN / WM_KEYUP messages straight to the emulator window, with a PS/2 scan code in lParam so that SDL translates them to the right scancode, and tells the
window it has focus first (SDL only delivers key events to a focused window).

  python edeninput.py --title Eden press A [--hold 0.15]        press the button and release it
  python edeninput.py --title Eden hold LSTICK_UP 2.0           hold a direction for 2 seconds
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_SETFOCUS, WM_ACTIVATE, WM_ACTIVATEAPP, WM_SYSKEYDOWN = 0x0007, 0x0006, 0x001C, 0x0104

# Switch button -> (virtual key, PS/2 set 1 scan code, extended). Defaults of Eden's keyboard profile (HID usage in the comment).
KEYS = {
    "A": (0x41, 0x1E, False),          # usage 4
    "B": (0x53, 0x1F, False),          # 22  S
    "X": (0x5A, 0x2C, False),          # 29  Z
    "Y": (0x58, 0x2D, False),          # 27  X
    "L": (0x46, 0x21, False),          # 9   F
    "R": (0x48, 0x23, False),          # 11  H
    "ZL": (0x51, 0x10, False),         # 20  Q
    "ZR": (0x57, 0x11, False),         # 26  W
    "PLUS": (0x4D, 0x32, False),       # 16  M
    "MINUS": (0x4E, 0x31, False),      # 17  N
    "LSTICK": (0x54, 0x14, False),     # 23  T
    "RSTICK": (0x47, 0x22, False),     # 10  G
    "DLEFT": (0x31, 0x02, False),      # 30  1
    "DUP": (0x32, 0x03, False),        # 31  2
    "DRIGHT": (0x42, 0x30, False),     # 5   B
    "LSTICK_UP": (0x26, 0x48, True),   # arrow keys
    "LSTICK_DOWN": (0x28, 0x50, True),
    "LSTICK_LEFT": (0x25, 0x4B, True),
    "LSTICK_RIGHT": (0x27, 0x4D, True),
    "RSTICK_UP": (0x49, 0x17, False),  # I J K L
    "RSTICK_LEFT": (0x4A, 0x24, False),
    "RSTICK_DOWN": (0x4B, 0x25, False),
    "RSTICK_RIGHT": (0x4C, 0x26, False),
}


def find_window(title_part="Eden", pid=None):
    found = []
    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if (pid is None or p.value == pid) and title_part.lower() in buf.value.lower() and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    return found[0] if found else None


def _lparam(scan, ext, up, repeat=1):
    lp = repeat | (scan << 16)
    if ext:
        lp |= 1 << 24
    if up:
        lp |= (1 << 30) | (1 << 31)
    return lp


def focus(hwnd):
    user32.SendMessageW(hwnd, WM_ACTIVATEAPP, 1, 0)
    user32.SendMessageW(hwnd, WM_ACTIVATE, 1, 0)
    user32.SendMessageW(hwnd, WM_SETFOCUS, 0, 0)


def key_down(hwnd, name):
    vk, sc, ext = KEYS[name.upper()]
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, _lparam(sc, ext, False))


def key_up(hwnd, name):
    vk, sc, ext = KEYS[name.upper()]
    user32.PostMessageW(hwnd, WM_KEYUP, vk, _lparam(sc, ext, True))


def press(hwnd, names, hold=0.15):
    if isinstance(names, str):
        names = [names]
    focus(hwnd)
    for n in names:
        key_down(hwnd, n)
    time.sleep(hold)
    for n in names:
        key_up(hwnd, n)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", default="Eden")
    ap.add_argument("--pid", type=int)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("press")
    p.add_argument("keys")
    p.add_argument("--hold", type=float, default=0.15)
    h = sub.add_parser("hold")
    h.add_argument("keys")
    h.add_argument("seconds", type=float)
    a = ap.parse_args(argv)
    hwnd = find_window(a.title, a.pid)
    if not hwnd:
        print("no window", file=sys.stderr)
        return 2
    keys = a.keys.split("+")
    press(hwnd, keys, a.hold if a.cmd == "press" else a.seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
