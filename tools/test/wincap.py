"""Window capture for the Eden reference harness (Windows only, ctypes + Pillow).

  python wincap.py --title "Eden" out.png          capture the client area of the first visible top-level window whose title contains the text
  python wincap.py --pid 1234 out.png

Methods (--method): printwindow (PrintWindow with PW_RENDERFULLCONTENT: works for DWM-composed Vulkan/OpenGL windows even when partly covered),
bitblt (copy from the window DC), screen (copy that rectangle from the desktop DC: needs the window visible on top). Default: printwindow, falling back to
the others when the result is all black.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()

PW_CLIENTONLY = 1
PW_RENDERFULLCONTENT = 2
SRCCOPY = 0x00CC0020


def find_windows(title_part=None, pid=None):
    out = []
    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if pid is not None and p.value != pid:
            return True
        if title_part is not None and title_part.lower() not in buf.value.lower():
            return True
        if pid is None and title_part is None:
            return True
        out.append((hwnd, buf.value, p.value))
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    return out


def client_size(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top


def _bitmap_to_image(hdc_mem, hbm, w, h):
    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                    ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bi), 0)
    return Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")


def capture(hwnd, method="printwindow"):
    w, h = client_size(hwnd)
    if w <= 0 or h <= 0:
        raise RuntimeError("window has an empty client area")
    hdc_win = user32.GetDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbm = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    old = gdi32.SelectObject(hdc_mem, hbm)
    try:
        if method == "printwindow":
            ok = user32.PrintWindow(hwnd, hdc_mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT)
            if not ok:
                raise RuntimeError("PrintWindow failed")
        elif method == "bitblt":
            gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_win, 0, 0, SRCCOPY)
        elif method == "screen":
            pt = wt.POINT(0, 0)
            user32.ClientToScreen(hwnd, ctypes.byref(pt))
            hdc_scr = user32.GetDC(0)
            gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_scr, pt.x, pt.y, SRCCOPY)
            user32.ReleaseDC(0, hdc_scr)
        else:
            raise ValueError(method)
        return _bitmap_to_image(hdc_mem, hbm, w, h)
    finally:
        gdi32.SelectObject(hdc_mem, old)
        gdi32.DeleteObject(hbm)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(hwnd, hdc_win)


def is_black(img, thresh=3):
    ex = img.resize((32, 32)).getextrema()
    return all(hi <= thresh for (_, hi) in ex)


def capture_best(hwnd, method=None):
    methods = [method] if method else ["printwindow", "bitblt", "screen"]
    last = None
    for m in methods:
        try:
            img = capture(hwnd, m)
        except Exception as e:  # noqa: BLE001
            last = e
            continue
        if not is_black(img) or m == methods[-1]:
            return img, m
        last = RuntimeError("black")
    if last:
        raise last
    raise RuntimeError("no capture method worked")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--title")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--method", choices=["printwindow", "bitblt", "screen"])
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args(argv)
    wins = find_windows(a.title, a.pid)
    if a.list:
        for h, t, p in wins:
            print(h, p, t)
        return 0
    wins = [w for w in wins if client_size(w[0])[0] > 64]
    if not wins:
        print("no matching window", file=sys.stderr)
        return 2
    img, m = capture_best(wins[0][0], a.method)
    img.save(a.out)
    print(f"{a.out}: {img.size[0]}x{img.size[1]} via {m}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
