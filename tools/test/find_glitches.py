#!/usr/bin/env python3
"""Flag glitched frames in a set of PNG frame dumps.

A frame is flagged when:
  - near-black pixels (all channels <= 8) cover more than `black_frac` of its
    area while the median frame of the set has less than half of that fraction
    (exploded geometry or a missing draw); or
  - it is one colour (every channel's std < 1.0) while the median frame is not.

Module: find_glitches(paths_or_dir, black_frac=0.02) -> list of dicts.
CLI:    python find_glitches.py DIR_OR_PNG [PNG ...] [--black-frac 0.02] [--json]
Exit:   0 = nothing flagged, 1 = flagged frames, 2 = usage or file error.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
from typing import Any, Dict, List, Sequence, Tuple, Union

import numpy as np
from PIL import Image

NEAR_BLACK = 8
FLAT_STD = 1.0


def _collect(paths_or_dir: Union[str, os.PathLike, Sequence[Any]]) -> List[str]:
    """A directory gives its *.png files sorted by name; a list gives those files sorted by basename."""
    if isinstance(paths_or_dir, (str, os.PathLike)):
        p = os.fspath(paths_or_dir)
        if os.path.isdir(p):
            names = sorted(n for n in os.listdir(p) if n.lower().endswith(".png"))
            return [os.path.join(p, n) for n in names]
        if os.path.isfile(p):
            return [p]
        raise FileNotFoundError(f"no such directory or PNG file: {p}")
    paths = [os.fspath(x) for x in paths_or_dir]
    for p in paths:
        if not os.path.isfile(p):
            raise FileNotFoundError(f"no such PNG file: {p}")
    return sorted(paths, key=os.path.basename)


def _frame_stats(path: str) -> Tuple[float, float]:
    """Return (near-black area fraction, largest per-channel std) for one frame."""
    with Image.open(path) as im:
        arr = np.asarray(im.convert("RGB"))
    px = arr.reshape(-1, 3)
    black = float(np.all(px <= NEAR_BLACK, axis=1).mean())
    std = float(px.std(axis=0).max())
    return black, std


def find_glitches(paths_or_dir, black_frac: float = 0.02) -> List[Dict[str, Any]]:
    """Return one dict per flagged frame: path, index, reason, black_frac, std, median_black_frac, median_std."""
    paths = _collect(paths_or_dir)
    if not paths:
        return []
    stats = [_frame_stats(p) for p in paths]
    med_black = statistics.median([b for b, _ in stats])
    med_std = statistics.median([s for _, s in stats])
    flagged: List[Dict[str, Any]] = []
    for i, (path, (black, std)) in enumerate(zip(paths, stats)):
        if black > black_frac and med_black < black_frac / 2.0:
            reason = "near_black"
        elif std < FLAT_STD and med_std >= FLAT_STD:
            reason = "flat"
        else:
            continue
        flagged.append({
            "path": path,
            "index": i,
            "reason": reason,
            "black_frac": black,
            "std": std,
            "median_black_frac": med_black,
            "median_std": med_std,
        })
    return flagged


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Flag glitched (near-black or flat) frames in PNG dumps.")
    ap.add_argument("paths", nargs="+", help="a directory of PNGs, or one or more PNG files")
    ap.add_argument("--black-frac", type=float, default=0.02)
    ap.add_argument("--json", action="store_true", help="print the flagged list as JSON")
    args = ap.parse_args(argv)
    try:
        target = args.paths[0] if len(args.paths) == 1 else args.paths
        flagged = find_glitches(target, args.black_frac)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(flagged, indent=2))
    elif not flagged:
        print("no glitched frames")
    else:
        for f in flagged:
            print(f"{f['index']:5d}  {f['reason']:<10}  {os.path.basename(f['path'])}  "
                  f"black={f['black_frac']:.4f}  std={f['std']:.2f}")
        print(f"{len(flagged)} flagged")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
