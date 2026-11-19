#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bakes the icons of the port's own in-game Settings tabs into runtime/games/pkp/ui/pkp_options_icons_data.h.

Each tab gets one Font Awesome Free solid icon (CC BY 4.0), read from the launcher's own copy of the package
(launcher/node_modules/@fortawesome/free-solid-svg-icons, `npm ci` in launcher/ first), so the launcher and the game
use the same artwork. The glyph is rasterised here (a small non-zero scanline fill with supersampling, no extra
dependencies) for the look of the game's own tab icons, which come as pairs of 72 x 72 sprites:
  * not selected: a white silhouette, its details cut out (clear);
  * selected: the glyph in a pale colour of its own over an outline of grey-brown (77, 50, 46), about 4 texels wide around
    the shape and filling its cut-out details.
The header holds two coverage masks per icon (the glyph, and the outline plate: the glyph grown by the rim, its holes filled)
and the colour; ui/pkp_options_icons.cpp composes the two sprites from them as compose_on / compose_off below do.

    python tools/settings_tab_icons.py            # writes the header
    python tools/settings_tab_icons.py --png DIR  # also writes a preview PNG per icon (needs Pillow)
"""
import argparse
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FA_DIR = os.path.join(ROOT, "launcher", "node_modules", "@fortawesome", "free-solid-svg-icons")
OUT = os.path.join(ROOT, "runtime", "games", "pkp", "ui", "pkp_options_icons_data.h")

# The tab (as kTabs in ui/pkp_options.cpp names it) and its icon. Where the launcher has a section for the same settings,
# the icon is the launcher's (launcher/src/settings-schema.js). The colour of the selected look is a pale one per tab, as the
# game's three (grey, lavender, yellow).
TABS = [
    ("Display", "display", (138, 190, 240)),
    ("Performance", "gauge-high", (250, 172, 104)),
    ("Graphics", "wand-magic-sparkles", (242, 156, 204)),
    ("Picture", "circle-half-stroke", (124, 208, 196)),
    ("Keyboard & Mouse", "keyboard", (206, 212, 222)),
    ("Controller", "gamepad", (156, 212, 136)),
    ("Gameplay", "sliders", (250, 214, 110)),
    ("Time", "clock", (178, 166, 236)),
    ("Events", "calendar-days", (246, 140, 136)),
    ("Mods", "puzzle-piece", (134, 214, 166)),
    ("Items", "box-open", (222, 178, 128)),
    ("Online", "globe", (112, 196, 232)),
]

SIZE = 72          # the sprite's side in texels, as the game's tab icons
MARGIN = 6         # clear texels around the glyph's longer side; the outline takes RIM of them
RIM = 4            # the outline's width in texels
SUPER = 8          # supersampling per axis
OUTLINE = (77, 50, 46)


def fa_icon(name):
    camel = "fa" + "".join(p.capitalize() for p in name.split("-"))
    path = os.path.join(FA_DIR, camel + ".js")
    try:
        src = open(path, encoding="utf-8").read()
    except OSError:
        sys.exit(f"{path} is missing: run `npm ci` in launcher/ first")
    w = int(re.search(r"var width = (\d+);", src).group(1))
    h = int(re.search(r"var height = (\d+);", src).group(1))
    d = re.search(r"var svgPathData = '([^']*)';", src).group(1)
    return w, h, d


# ---- SVG path to polygons ----------------------------------------------------------------------------------------------------

TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def tokens(d):
    for t in TOKEN.findall(d):
        yield t


def arc_points(x0, y0, rx, ry, phi, large, sweep, x1, y1, steps=24):
    # SVG implementation notes F.6.5: endpoint to centre parameterisation.
    if rx == 0 or ry == 0:
        return [(x1, y1)]
    rx, ry = abs(rx), abs(ry)
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x0 - x1) / 2, (y0 - y1) / 2
    xp, yp = c * dx + s * dy, -s * dx + c * dy
    lam = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    den = rx * rx * yp * yp + ry * ry * xp * xp
    k = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        k = -k
    cxp, cyp = k * rx * yp / ry, -k * ry * xp / rx
    cx, cy = c * cxp - s * cyp + (x0 + x1) / 2, s * cxp + c * cyp + (y0 + y1) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (xp - cxp) / rx, (yp - cyp) / ry)
    dt = ang((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(2, int(steps * abs(dt) / (math.pi / 2)))
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        pts.append((c * ex - s * ey + cx, s * ex + c * ey + cy))
    return pts


def bezier(p0, p1, p2, p3, steps=16):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def polygons(d):
    toks = list(tokens(d))
    i = 0
    polys, cur = [], []
    x = y = sx = sy = 0.0
    cmd = None
    last_c = None   # the last cubic's second control point (for S)
    last_q = None   # the last quadratic's control point (for T)

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur = []
                x, y = sx, sy
                last_c = last_q = None
                continue
        rel = cmd.islower()
        C = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if C == "M":
            if cur:
                polys.append(cur)
            x, y = num() + ox, num() + oy
            sx, sy = x, y
            cur = [(x, y)]
            cmd = "l" if rel else "L"
            last_c = last_q = None
        elif C == "L":
            x, y = num() + ox, num() + oy
            cur.append((x, y))
            last_c = last_q = None
        elif C == "H":
            x = num() + ox
            cur.append((x, y))
            last_c = last_q = None
        elif C == "V":
            y = num() + oy
            cur.append((x, y))
            last_c = last_q = None
        elif C == "C":
            p1 = (num() + ox, num() + oy)
            p2 = (num() + ox, num() + oy)
            p3 = (num() + ox, num() + oy)
            cur += bezier((x, y), p1, p2, p3)
            x, y = p3
            last_c, last_q = p2, None
        elif C == "S":
            p1 = (2 * x - last_c[0], 2 * y - last_c[1]) if last_c else (x, y)
            p2 = (num() + ox, num() + oy)
            p3 = (num() + ox, num() + oy)
            cur += bezier((x, y), p1, p2, p3)
            x, y = p3
            last_c, last_q = p2, None
        elif C == "Q":
            q = (num() + ox, num() + oy)
            p3 = (num() + ox, num() + oy)
            cur += bezier((x, y), ((x + 2 * q[0]) / 3, (y + 2 * q[1]) / 3), ((p3[0] + 2 * q[0]) / 3, (p3[1] + 2 * q[1]) / 3), p3)
            x, y = p3
            last_c, last_q = None, q
        elif C == "T":
            q = (2 * x - last_q[0], 2 * y - last_q[1]) if last_q else (x, y)
            p3 = (num() + ox, num() + oy)
            cur += bezier((x, y), ((x + 2 * q[0]) / 3, (y + 2 * q[1]) / 3), ((p3[0] + 2 * q[0]) / 3, (p3[1] + 2 * q[1]) / 3), p3)
            x, y = p3
            last_c, last_q = None, q
        elif C == "A":
            rx, ry, phi = num(), num(), num()
            large, sweep = int(num()), int(num())
            x1, y1 = num() + ox, num() + oy
            cur += arc_points(x, y, rx, ry, phi, large, sweep, x1, y1)
            x, y = x1, y1
            last_c = last_q = None
        else:
            sys.exit(f"path command {cmd} is not handled")
    if cur:
        polys.append(cur)
    return polys


# ---- rasterising -----------------------------------------------------------------------------------------------------------

def coverage(polys, size):
    """Non-zero fill of the polygons (already in mask texels) into a size x size list of 0..255, SUPER x SUPER samples per texel."""
    n = size * SUPER
    edges = []
    for poly in polys:
        for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
            if y0 == y1:
                continue
            edges.append((x0 * SUPER, y0 * SUPER, x1 * SUPER, y1 * SUPER, 1 if y1 > y0 else -1))
    hits = [0] * (size * size)
    for row in range(n):
        yc = row + 0.5
        xs = []
        for x0, y0, x1, y1, wdir in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                xs.append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), wdir))
        xs.sort()
        wind = 0
        for k in range(len(xs) - 1):
            wind += xs[k][1]
            if wind == 0:
                continue
            a, b = max(0.0, xs[k][0]), min(float(n), xs[k + 1][0])
            for col in range(max(0, math.ceil(a - 0.5)), min(n, math.ceil(b - 0.5))):
                hits[(row // SUPER) * size + col // SUPER] += 1
    full = SUPER * SUPER
    return [min(255, round(h * 255 / full)) for h in hits]


def mask_of(name):
    w, h, d = fa_icon(name)
    k = (SIZE - 2 * MARGIN) / max(w, h)
    ox, oy = (SIZE - w * k) / 2, (SIZE - h * k) / 2
    polys = [[(ox + px * k, oy + py * k) for px, py in poly] for poly in polygons(d)]
    return coverage(polys, SIZE)


def plate_of(glyph):
    """The outline plate: every hole of the glyph filled, and the glyph grown by RIM texels with a soft edge."""
    n = SIZE
    outside = [False] * (n * n)
    todo = [i for i in range(n * n) if (i % n in (0, n - 1) or i // n in (0, n - 1)) and glyph[i] < 128]
    for i in todo:
        outside[i] = True
    while todo:
        i = todo.pop()
        x, y = i % n, i // n
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            j = ny * n + nx
            if 0 <= nx < n and 0 <= ny < n and not outside[j] and glyph[j] < 128:
                outside[j] = True
                todo.append(j)
    out = [0] * (n * n)
    for y in range(n):
        for x in range(n):
            pa = 0.0 if outside[y * n + x] else 255.0
            for dy in range(-RIM, RIM + 1):
                for dx in range(-RIM, RIM + 1):
                    sx, sy = x + dx, y + dy
                    dist = math.hypot(dx, dy)
                    if 0 <= sx < n and 0 <= sy < n and dist <= RIM + 0.5:
                        pa = max(pa, glyph[sy * n + sx] * min(1.0, RIM + 0.5 - dist))
            out[y * n + x] = round(pa)
    return out


# ---- the two looks (what ui/pkp_options_icons.cpp does with the header; here for the preview) ----------------------------------

def shade(rgb, y):
    k = 1.12 - 0.24 * y / (SIZE - 1)   # lighter at the top, as the game's fills
    return [min(255, round(c * k)) for c in rgb]


def compose_on(rgb, glyph, plate):
    out = []
    for i in range(SIZE * SIZE):
        sa, da = glyph[i] / 255, plate[i] / 255
        oa = sa + da * (1 - sa)
        fill = shade(rgb, i // SIZE)
        out += [round((fill[c] * sa + OUTLINE[c] * da * (1 - sa)) / oa) if oa else 0 for c in range(3)] + [round(oa * 255)]
    return out


def compose_off(glyph):
    out = []
    for v in glyph:
        out += [255, 255, 255, v]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--png", help="also write a preview PNG per icon and look into this folder")
    args = ap.parse_args()
    icons = []
    for tab, icon, rgb in TABS:
        glyph = mask_of(icon)
        icons.append((tab, icon, rgb, glyph, plate_of(glyph)))
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Generated by tools/settings_tab_icons.py from Font Awesome Free 7 solid icons (Fonticons, Inc., CC BY 4.0). Do not edit.",
        "// Per tab of the port's own in-game Settings tabs, in kTabs order: the coverage (0 clear .. 255 covered, rows top to bottom) of the",
        "// glyph and of its outline plate, and the colour of the selected look. ui/pkp_options_icons.cpp composes the two sprites.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace nxrt::games::pkp::options_icons {",
        "",
        f"constexpr int kIconSize = {SIZE};",
        f"constexpr int kIconCount = {len(icons)};",
        f"constexpr uint8_t kOutline[3] = {{{OUTLINE[0]}, {OUTLINE[1]}, {OUTLINE[2]}}};",
        "",
        "struct Icon {",
        "    const char* name;                         // the Font Awesome name",
        "    uint8_t rgb[3];                           // the selected look's colour",
        "    uint8_t glyph[kIconSize * kIconSize];",
        "    uint8_t plate[kIconSize * kIconSize];",
        "};",
        "",
        "constexpr Icon kIcons[kIconCount] = {",
    ]
    for tab, icon, rgb, glyph, plate in icons:
        lines.append(f'    {{"{icon}", {{{rgb[0]}, {rgb[1]}, {rgb[2]}}},   // {tab}')
        for m in (glyph, plate):
            lines.append("     {")
            for r in range(SIZE):
                lines.append("      " + ",".join(str(v) for v in m[r * SIZE:(r + 1) * SIZE]) + ",")
            lines.append("     },")
        lines.append("    },")
    lines += ["};", "", "}  // namespace nxrt::games::pkp::options_icons", ""]
    with open(OUT, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"{len(icons)} icons -> {os.path.relpath(OUT, ROOT)}")
    if args.png:
        from PIL import Image
        os.makedirs(args.png, exist_ok=True)
        for tab, icon, rgb, glyph, plate in icons:
            for look, px in (("on", compose_on(rgb, glyph, plate)), ("off", compose_off(glyph))):
                im = Image.new("RGBA", (SIZE, SIZE))
                im.putdata([tuple(px[i * 4:i * 4 + 4]) for i in range(SIZE * SIZE)])
                im.save(os.path.join(args.png, f"{icon}_{look}.png"))


if __name__ == "__main__":
    main()
