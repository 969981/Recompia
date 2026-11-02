#!/usr/bin/env python3
"""Perceptual screenshot comparator.

Compares two captures (files or numpy arrays) after resizing both to a common
size. Reports SSIM (on luma), MAE (RGB), histogram correlation and the fraction
of pixels that changed noticeably. Pixels covered by masks are ignored by every
metric.

Module: compare(), diff_image(), load_masks_json().
CLI:    python imgdiff.py A.png B.png [--mask SPEC ...] [--size WxH] [...]
Exit:   0 = all thresholds hold, 1 = a threshold failed, 2 = usage or file error.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Sequence

import numpy as np
from PIL import Image

DEFAULT_SIZE = (320, 180)
DEFAULT_THRESHOLDS: Dict[str, float] = {
    "min_ssim": 0.90,
    "max_mae": 12.0,
    "min_hist": 0.80,
    "max_changed": 0.15,
}
ALL_METRICS = ("ssim", "mae", "hist_corr", "changed_frac")
CHANGED_DELTA = 24      # max-channel difference that counts as a changed pixel
HIST_BINS = 32          # per channel
WIN = 7                 # SSIM window (uniform)
LUMA_W = np.array([0.299, 0.587, 0.114], dtype=np.float64)

# Preset masks are fractions (x0, y0, x1, y1) of width/height.
PRESETS: Dict[str, List[tuple]] = {
    "none": [],
    "hud": [
        (0.0, 0.0, 1.0, 0.14),    # top strip
        (0.0, 0.78, 1.0, 1.0),    # bottom strip
        (0.0, 0.14, 0.20, 0.78),  # left of middle band
        (0.80, 0.14, 1.0, 0.78),  # right of middle band
    ],
}

ImageSrc = Any  # path, os.PathLike, PIL.Image or uint8 numpy array


@dataclass
class Result:
    ssim: Optional[float] = None         # None = not requested; NaN = not computable
    mae: Optional[float] = None
    hist_corr: Optional[float] = None
    changed_frac: Optional[float] = None
    notes: List[str] = field(default_factory=list)

    def passed(self, thresholds: Optional[Dict[str, float]] = None) -> bool:
        """True when every computed metric meets its threshold.

        A metric that was not requested is skipped. A metric that could not be
        computed (NaN) fails.
        """
        t = dict(DEFAULT_THRESHOLDS)
        if thresholds:
            unknown = set(thresholds) - set(DEFAULT_THRESHOLDS)
            if unknown:
                raise ValueError(f"unknown threshold(s): {sorted(unknown)}")
            t.update(thresholds)
        checks = []
        if self.ssim is not None:
            checks.append(self.ssim >= t["min_ssim"])
        if self.mae is not None:
            checks.append(self.mae <= t["max_mae"])
        if self.hist_corr is not None:
            checks.append(self.hist_corr >= t["min_hist"])
        if self.changed_frac is not None:
            checks.append(self.changed_frac <= t["max_changed"])
        return all(checks)

    def to_dict(self) -> Dict[str, Any]:
        def clean(v: Optional[float]) -> Optional[float]:
            if v is None or (isinstance(v, float) and math.isnan(v)):
                return None
            return float(v)

        return {
            "ssim": clean(self.ssim),
            "mae": clean(self.mae),
            "hist_corr": clean(self.hist_corr),
            "changed_frac": clean(self.changed_frac),
            "notes": list(self.notes),
        }


# ---------------------------------------------------------------- image input

def _open_rgb(src: ImageSrc) -> Image.Image:
    """Return an RGB PIL image. Alpha is composited on black."""
    if isinstance(src, np.ndarray):
        arr = np.ascontiguousarray(src)
        if arr.dtype != np.uint8:
            raise ValueError("array input must be uint8")
        if not ((arr.ndim == 2) or (arr.ndim == 3 and arr.shape[2] in (3, 4))):
            raise ValueError(f"array input must be (H,W), (H,W,3) or (H,W,4), got {arr.shape}")
        img = Image.fromarray(arr)
    elif isinstance(src, Image.Image):
        img = src
    elif isinstance(src, (str, os.PathLike)):
        if not os.path.isfile(src):
            raise FileNotFoundError(f"no such image file: {src}")
        with Image.open(src) as im:
            img = im.copy()
    else:
        raise TypeError(f"unsupported image input type: {type(src).__name__}")

    if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        return Image.alpha_composite(bg, rgba).convert("RGB")
    return img.convert("RGB")


def _check_size(size: Sequence[int]) -> tuple:
    W, H = int(size[0]), int(size[1])
    if W < WIN or H < WIN:
        raise ValueError(f"size must be at least {WIN}x{WIN}, got {W}x{H}")
    return W, H


def _resize_to(img: Image.Image, size: tuple) -> np.ndarray:
    """Resize to (W,H) and return uint8 (H,W,3). BOX when shrinking, LANCZOS otherwise."""
    W, H = size
    w, h = img.size
    if (w, h) == (W, H):
        return np.asarray(img, dtype=np.uint8)
    resample = Image.Resampling.BOX if (w >= W and h >= H) else Image.Resampling.LANCZOS
    return np.asarray(img.resize((W, H), resample), dtype=np.uint8)


# ---------------------------------------------------------------------- masks

def _is_number(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _is_rect(v: Any) -> bool:
    return isinstance(v, (tuple, list)) and len(v) == 4 and all(_is_number(x) for x in v)


def _rect_mask(rect: Sequence[float], W: int, H: int) -> np.ndarray:
    if len(rect) != 4:
        raise ValueError(f"rect needs 4 values, got {rect!r}")
    x0, y0, x1, y1 = (float(v) for v in rect)
    for v in (x0, y0, x1, y1):
        if not (0.0 <= v <= 1.0):
            raise ValueError(f"mask rect values must be fractions 0..1, got {rect!r}")
    if x1 <= x0 or y1 <= y0:
        raise ValueError(f"mask rect must have x1>x0 and y1>y0, got {rect!r}")
    c0, r0 = round(x0 * W), round(y0 * H)
    c1, r1 = round(x1 * W), round(y1 * H)
    m = np.zeros((H, W), dtype=bool)
    m[r0:r1, c0:c1] = True
    return m


def _gray_to_mask(nonzero: np.ndarray, W: int, H: int) -> np.ndarray:
    """nonzero: (h,w) bool. Nearest-resize to (H,W)."""
    if nonzero.shape == (H, W):
        return nonzero.copy()
    img = Image.fromarray((nonzero * 255).astype(np.uint8))
    out = img.resize((W, H), Image.Resampling.NEAREST)
    return np.asarray(out) > 0


def _array_mask(arr: np.ndarray, W: int, H: int) -> np.ndarray:
    if arr.ndim == 3:
        nz = (arr != 0).any(axis=2)
    elif arr.ndim == 2:
        nz = arr != 0
    else:
        raise ValueError(f"mask array must be 2D or 3D, got {arr.shape}")
    return _gray_to_mask(nz, W, H)


def _image_mask(path: str, W: int, H: int) -> np.ndarray:
    gray = _open_rgb(path).convert("L")
    return _gray_to_mask(np.asarray(gray) > 0, W, H)


def _spec_to_mask(spec: Any, W: int, H: int) -> np.ndarray:
    if isinstance(spec, str):
        if spec in PRESETS:
            m = np.zeros((H, W), dtype=bool)
            for r in PRESETS[spec]:
                m |= _rect_mask(r, W, H)
            return m
        if os.path.exists(spec):
            return _image_mask(spec, W, H)
        raise ValueError(f"unknown mask preset or missing mask file: {spec!r}")
    if isinstance(spec, dict):
        if "rect" not in spec:
            raise ValueError(f"dict mask spec needs a 'rect' key: {spec!r}")
        return _rect_mask(spec["rect"], W, H)
    if _is_rect(spec):
        return _rect_mask(spec, W, H)
    if isinstance(spec, np.ndarray):
        return _array_mask(spec, W, H)
    if isinstance(spec, os.PathLike):
        return _image_mask(os.fspath(spec), W, H)
    raise TypeError(f"unsupported mask spec: {spec!r}")


def _normalise_masks(masks: Any) -> List[Any]:
    if masks is None:
        return []
    if isinstance(masks, (str, dict, np.ndarray, os.PathLike)) or _is_rect(masks):
        return [masks]
    return list(masks)


def _ignore_mask(masks: Any, W: int, H: int) -> np.ndarray:
    ign = np.zeros((H, W), dtype=bool)
    for spec in _normalise_masks(masks):
        ign |= _spec_to_mask(spec, W, H)
    return ign


# -------------------------------------------------------------------- metrics

def _box_sum(a: np.ndarray, k: int) -> np.ndarray:
    """Sum over every k x k window (valid positions) via an integral image."""
    c = np.zeros((a.shape[0] + 1, a.shape[1] + 1), dtype=np.float64)
    c[1:, 1:] = np.cumsum(np.cumsum(a, axis=0), axis=1)
    return c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]


def _ssim_luma(A: np.ndarray, B: np.ndarray, valid: np.ndarray, notes: List[str]) -> float:
    H, W = valid.shape
    ya = A.astype(np.float64) @ LUMA_W
    yb = B.astype(np.float64) @ LUMA_W
    n = float(WIN * WIN)
    sx = _box_sum(ya, WIN) / n
    sy = _box_sum(yb, WIN) / n
    sxx = np.maximum(_box_sum(ya * ya, WIN) / n - sx * sx, 0.0)
    syy = np.maximum(_box_sum(yb * yb, WIN) / n - sy * sy, 0.0)
    sxy = _box_sum(ya * yb, WIN) / n - sx * sy
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    smap = ((2 * sx * sy + c1) * (2 * sxy + c2)) / ((sx * sx + sy * sy + c1) * (sxx + syy + c2))

    # A window counts only if all 49 of its pixels are unmasked.
    bad_count = _box_sum((~valid).astype(np.float64), WIN)
    full = bad_count == 0
    if full.any():
        vals = smap[full]
    else:
        # Mask leaves no fully-unmasked window: fall back to windows whose centre is unmasked.
        h = WIN // 2
        centre = valid[h:H - h, h:W - h]
        vals = smap[centre]
        notes.append("ssim: no fully unmasked 7x7 window, used centre-unmasked windows")
    if vals.size == 0:
        notes.append("ssim: no window fits in the unmasked area")
        return float("nan")
    return float(vals.mean())


def _hist_vector(X: np.ndarray, valid: np.ndarray) -> np.ndarray:
    px = X[valid].astype(np.int64)                 # (N,3)
    bins = (px * HIST_BINS) // 256                 # 0..HIST_BINS-1
    h = np.zeros((3, HIST_BINS), dtype=np.float64)
    for c in range(3):
        h[c] = np.bincount(bins[:, c], minlength=HIST_BINS)[:HIST_BINS]
    return h.ravel()


def _pearson(x: np.ndarray, y: np.ndarray) -> float:
    xc = x - x.mean()
    yc = y - y.mean()
    nx = float(np.sqrt(np.dot(xc, xc)))
    ny = float(np.sqrt(np.dot(yc, yc)))
    if nx == 0.0 or ny == 0.0:
        # Flat histogram(s): correlation is undefined. Identical -> 1, otherwise 0.
        return 1.0 if np.array_equal(x, y) else 0.0
    return float(min(1.0, max(-1.0, np.dot(xc, yc) / (nx * ny))))


# ------------------------------------------------------------------ public API

def compare(a: ImageSrc, b: ImageSrc, masks: Any = None, size: Sequence[int] = DEFAULT_SIZE,
            metrics: Iterable[str] = ALL_METRICS) -> Result:
    """Compare two captures. See module docstring for the spec."""
    W, H = _check_size(size)
    metrics = tuple(metrics)
    bad = set(metrics) - set(ALL_METRICS)
    if bad:
        raise ValueError(f"unknown metric(s): {sorted(bad)}")

    pa = _open_rgb(a)
    pb = _open_rgb(b)
    notes: List[str] = []
    ratio_a = pa.width / pa.height
    ratio_b = pb.width / pb.height
    if abs(ratio_a / ratio_b - 1.0) > 0.02:
        notes.append("aspect mismatch")

    A = _resize_to(pa, (W, H))
    B = _resize_to(pb, (W, H))
    valid = ~_ignore_mask(masks, W, H)

    res = Result(notes=notes)
    if not valid.any():
        notes.append("fully masked")
        res.ssim = 1.0 if "ssim" in metrics else None
        res.mae = 0.0 if "mae" in metrics else None
        res.hist_corr = 1.0 if "hist_corr" in metrics else None
        res.changed_frac = 0.0 if "changed_frac" in metrics else None
        return res

    if "ssim" in metrics:
        res.ssim = _ssim_luma(A, B, valid, notes)
    if "mae" in metrics or "changed_frac" in metrics:
        diff = np.abs(A.astype(np.int16) - B.astype(np.int16))   # (H,W,3)
        if "mae" in metrics:
            res.mae = float(diff[valid].mean())
        if "changed_frac" in metrics:
            maxd = diff.max(axis=2)
            res.changed_frac = float((maxd[valid] > CHANGED_DELTA).mean())
    if "hist_corr" in metrics:
        res.hist_corr = _pearson(_hist_vector(A, valid), _hist_vector(B, valid))
    return res


def diff_image(a: ImageSrc, b: ImageSrc, masks: Any = None,
               size: Sequence[int] = DEFAULT_SIZE) -> np.ndarray:
    """Visual diff at `size`: dimmed grey of b, red where pixels changed, masked area dark blue."""
    W, H = _check_size(size)
    A = _resize_to(_open_rgb(a), (W, H))
    B = _resize_to(_open_rgb(b), (W, H))
    ign = _ignore_mask(masks, W, H)
    d = np.abs(A.astype(np.int16) - B.astype(np.int16)).max(axis=2).astype(np.float64)
    grey = (B.astype(np.float64) @ LUMA_W) * 0.35
    out = np.zeros((H, W, 3), dtype=np.float64)
    out[..., 0] = grey + 4.0 * d
    out[..., 1] = grey * 0.6
    out[..., 2] = grey * 0.6
    out = np.clip(out, 0, 255).astype(np.uint8)
    out[ign] = (0, 0, 90)
    return out


def load_masks_json(path: str, names: Optional[Iterable[str]] = None) -> List[tuple]:
    """Read {"name": [[x0,y0,x1,y1], ...], ...} and return a flat list of rect masks.

    `names` optionally limits the result to those keys.
    """
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError("mask JSON must be an object mapping names to lists of rects")
    wanted = set(names) if names is not None else None
    out: List[tuple] = []
    for name, rects in data.items():
        if wanted is not None and name not in wanted:
            continue
        if not isinstance(rects, list):
            raise ValueError(f"mask '{name}': expected a list of rects")
        for r in rects:
            if not _is_rect(r):
                raise ValueError(f"mask '{name}': each rect needs 4 numbers, got {r!r}")
            out.append(tuple(float(v) for v in r))
    return out


# ------------------------------------------------------------------------- CLI

def _size_arg(text: str) -> tuple:
    m = re.fullmatch(r"(\d+)x(\d+)", text.strip().lower())
    if not m:
        raise argparse.ArgumentTypeError("size must look like 320x180")
    return int(m.group(1)), int(m.group(2))


def _mask_arg(text: str) -> Any:
    parts = text.split(",")
    if len(parts) == 4:
        try:
            return tuple(float(p) for p in parts)
        except ValueError:
            pass
    return text  # preset name or mask image path, resolved by compare()


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Perceptual screenshot comparator.")
    p.add_argument("a", help="reference image")
    p.add_argument("b", help="candidate image")
    p.add_argument("--mask", action="append", default=[], metavar="SPEC",
                   help='"x0,y0,x1,y1" fractions, a preset (hud, none), or a mask image path; repeatable')
    p.add_argument("--size", type=_size_arg, default=DEFAULT_SIZE, metavar="WxH")
    p.add_argument("--min-ssim", type=float, default=DEFAULT_THRESHOLDS["min_ssim"])
    p.add_argument("--max-mae", type=float, default=DEFAULT_THRESHOLDS["max_mae"])
    p.add_argument("--min-hist", type=float, default=DEFAULT_THRESHOLDS["min_hist"])
    p.add_argument("--max-changed", type=float, default=DEFAULT_THRESHOLDS["max_changed"])
    p.add_argument("--diff-out", metavar="PATH", help="write the diff image (PNG)")
    p.add_argument("--json", action="store_true", help="print a JSON object instead of one line")
    args = p.parse_args(argv)

    thresholds = {
        "min_ssim": args.min_ssim,
        "max_mae": args.max_mae,
        "min_hist": args.min_hist,
        "max_changed": args.max_changed,
    }
    masks = [_mask_arg(m) for m in args.mask]
    try:
        res = compare(args.a, args.b, masks=masks, size=args.size)
        if args.diff_out:
            Image.fromarray(diff_image(args.a, args.b, masks=masks, size=args.size)).save(args.diff_out)
    except (OSError, ValueError, TypeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    ok = res.passed(thresholds)
    for note in res.notes:
        print(f"note: {note}", file=sys.stderr)
    if args.json:
        payload = res.to_dict()
        payload["passed"] = ok
        payload["thresholds"] = thresholds
        print(json.dumps(payload))
    else:
        def fmt(v: Optional[float], spec: str) -> str:
            return "nan" if v is None or math.isnan(v) else format(v, spec)
        print(f"{'PASS' if ok else 'FAIL'} ssim={fmt(res.ssim, '.3f')} mae={fmt(res.mae, '.1f')} "
              f"hist={fmt(res.hist_corr, '.2f')} changed={fmt(res.changed_frac, '.3f')}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
