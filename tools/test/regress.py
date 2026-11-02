#!/usr/bin/env python3
"""Run-regression summarizer and comparator.

summarize() reads one run directory (trace.jsonl, result.json, crash.txt,
freeze.txt, frames\\*.png) and returns its metrics as a dict. compare() takes
two such dicts and returns a Verdict with one row per metric.

Module: summarize(), compare(), Verdict, DEFAULT_THRESHOLDS.
CLI:
  python regress.py summarize RUN_DIR [-o out.json]
  python regress.py compare BASE CAND [--json] [--fps-median-drop F] [--fps-p10-drop F]
                                       [--frame-ms-p99-rise F] [--peak-mem-rise F]
    BASE and CAND are each a run directory or a summary JSON written by `summarize -o`.
Exit: summarize 0 = ok, 2 = error. compare 0 = no loss, 1 = loss, 2 = error.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import sys
from typing import Any, Dict, List, Optional, Sequence

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from find_glitches import find_glitches  # noqa: E402

DEFAULT_THRESHOLDS: Dict[str, float] = {
    "fps_median_drop": 0.03,
    "fps_p10_drop": 0.10,
    "frame_ms_p99_rise": 0.15,
    "peak_mem_rise": 0.10,
}
SKIP_WINDOWS = 20
WINDOW = 60


def _is_num(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _read_json_optional(path: str) -> Optional[Dict[str, Any]]:
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return data


def _read_trace(path: str) -> Optional[List[Dict[str, Any]]]:
    """Frame objects only: skips the header line, checkpoint ("label") lines and torn lines."""
    if not os.path.isfile(path):
        return None
    frames: List[Dict[str, Any]] = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue  # a torn last line left by a crash
            if not isinstance(obj, dict) or "trace" in obj or "label" in obj:
                continue
            if isinstance(obj.get("f"), int):
                frames.append(obj)
    return frames


def percentile(values: Sequence[float], pct: float) -> Optional[float]:
    """Linear-interpolation percentile, pct in [0, 100]. None for no values."""
    if not values:
        return None
    s = sorted(values)
    if len(s) == 1:
        return float(s[0])
    k = (len(s) - 1) * pct / 100.0
    lo = math.floor(k)
    hi = min(lo + 1, len(s) - 1)
    return float(s[lo] + (s[hi] - s[lo]) * (k - lo))


def summarize(run_dir: str, skip_windows: int = SKIP_WINDOWS, window: int = WINDOW) -> Dict[str, Any]:
    """Metrics for one run directory. Missing files give None fields; a missing directory raises."""
    if not os.path.isdir(run_dir):
        raise FileNotFoundError(f"run directory not found: {run_dir}")
    if window < 1 or skip_windows < 0:
        raise ValueError("window must be >= 1 and skip_windows >= 0")

    result = _read_json_optional(os.path.join(run_dir, "result.json"))
    trace = _read_trace(os.path.join(run_dir, "trace.jsonl"))
    crash_file = os.path.isfile(os.path.join(run_dir, "crash.txt"))
    freeze_file = os.path.isfile(os.path.join(run_dir, "freeze.txt"))
    frames_dir = os.path.join(run_dir, "frames")

    fps_median = fps_p10 = frame_ms_p99 = peak_mem = fps_windows = None
    trace_frames: Optional[int] = None
    if trace is not None:
        trace_frames = len(trace)
        ms_all = [float(fr["ms"]) for fr in trace if _is_num(fr.get("ms"))]
        mems = [float(fr["mem"]) for fr in trace if _is_num(fr.get("mem"))]
        peak_mem = max(mems) if mems else None
        n_windows = len(ms_all) // window
        # Drop the boot/menu windows only when there are clearly more windows than that.
        drop = skip_windows if n_windows > skip_windows + 3 else 0
        fps = []
        for w in range(n_windows):
            total = sum(ms_all[w * window:(w + 1) * window])
            if total > 0:
                fps.append(window * 1000.0 / total)
        kept_fps = fps[drop:]
        fps_windows = len(kept_fps)
        fps_median = float(statistics.median(kept_fps)) if kept_fps else None
        fps_p10 = percentile(kept_fps, 10)
        frame_ms_p99 = percentile(ms_all[drop * window:], 99)

    verdict = result.get("verdict") if result is not None else None
    crashes = freezes = None
    if result is not None or crash_file:
        crashes = 1 if (verdict == "crash" or crash_file) else 0
    if result is not None or freeze_file:
        freezes = 1 if (verdict == "freeze" or freeze_file) else 0

    cps = result.get("checkpoints") if result is not None else None
    labels = None
    if isinstance(cps, list):
        labels = [str(c["label"]) for c in cps if isinstance(c, dict) and "label" in c]

    return {
        "run_dir": os.path.abspath(run_dir),
        "skip_windows": skip_windows,
        "window": window,
        "frames": result.get("frames", trace_frames) if result is not None else trace_frames,
        "fps_windows": fps_windows,
        "wall_s": result.get("wall_s") if result is not None else None,
        "verdict": verdict,
        "exit_code": result.get("exit_code") if result is not None else None,
        "crashes": crashes,
        "freezes": freezes,
        "fps_median": fps_median,
        "fps_p10": fps_p10,
        "frame_ms_p99": frame_ms_p99,
        "peak_mem_mb": peak_mem,
        "vk_errors": result.get("vk_errors") if result is not None else None,
        "host_exceptions": result.get("host_exceptions", 0) if result is not None else None,
        "glitch_frames": len(find_glitches(frames_dir)) if os.path.isdir(frames_dir) else 0,
        "final_hash": result.get("final_hash") if result is not None else None,
        "deterministic": bool(result.get("deterministic")) if result is not None else None,
        "checkpoint_labels": labels,
    }


class Verdict:
    """Per-metric rows plus the overall result. Each row: metric, base, cand, delta_pct, status (OK/LOSS/NOTE)."""

    def __init__(self, rows: List[Dict[str, Any]]):
        self.rows = rows

    @property
    def loss_count(self) -> int:
        return sum(1 for r in self.rows if r["status"] == "LOSS")

    @property
    def overall(self) -> str:
        return "loss" if self.loss_count else "no loss"

    def to_dict(self) -> Dict[str, Any]:
        return {"overall": self.overall, "loss_count": self.loss_count, "rows": self.rows}


def _delta_pct(b: Any, c: Any) -> Optional[float]:
    if not _is_num(b) or not _is_num(c) or b == 0:
        return None
    return (c - b) / b * 100.0


def compare(base: Dict[str, Any], cand: Dict[str, Any], thresholds: Optional[Dict[str, float]] = None) -> Verdict:
    th = dict(DEFAULT_THRESHOLDS)
    if thresholds:
        th.update(thresholds)
    rows: List[Dict[str, Any]] = []

    def add(metric, b, c, delta, status, **extra):
        row = {"metric": metric, "base": b, "cand": c, "delta_pct": delta, "status": status}
        row.update(extra)
        rows.append(row)

    # Counts: any increase is a loss.
    for key in ("crashes", "freezes", "vk_errors", "host_exceptions", "glitch_frames"):
        b, c = base.get(key), cand.get(key)
        if not _is_num(b) or not _is_num(c):
            continue
        add(key, b, c, _delta_pct(b, c), "LOSS" if c > b else "OK")

    bv, cv = base.get("verdict"), cand.get("verdict")
    if bv is not None and cv is not None:
        add("verdict", bv, cv, None, "LOSS" if (bv == "pass" and cv != "pass") else "OK")

    def pct_check(metric: str, direction: str, limit: float) -> None:
        b, c = base.get(metric), cand.get(metric)
        if not _is_num(b) or not _is_num(c) or b == 0:
            return
        change = (c - b) / b
        bad = (-change > limit) if direction == "down" else (change > limit)
        add(metric, b, c, change * 100.0, "LOSS" if bad else "OK")

    pct_check("fps_median", "down", th["fps_median_drop"])
    pct_check("fps_p10", "down", th["fps_p10_drop"])
    pct_check("frame_ms_p99", "up", th["frame_ms_p99_rise"])
    pct_check("peak_mem_mb", "up", th["peak_mem_rise"])

    bl, cl = base.get("checkpoint_labels"), cand.get("checkpoint_labels")
    if isinstance(bl, list) and isinstance(cl, list):
        missing = sorted(set(bl) - set(cl))
        add("checkpoints", len(bl), len(cl), None, "LOSS" if missing else "OK", missing=missing)

    bh, ch = base.get("final_hash"), cand.get("final_hash")
    if bh and ch and bh != ch:
        both_det = bool(base.get("deterministic")) and bool(cand.get("deterministic"))
        add("final_hash", bh, ch, None, "NOTE" if both_det else "OK")
    elif bh and ch:
        add("final_hash", bh, ch, None, "OK")

    return Verdict(rows)


def _fmt(v: Any) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.3f}"
    s = str(v)
    return s if len(s) <= 16 else s[:13] + "..."


def print_table(verdict: Verdict, out=sys.stdout) -> None:
    head = ["metric", "base", "cand", "delta%", "status"]
    body = []
    for r in verdict.rows:
        d = "-" if r["delta_pct"] is None else f"{r['delta_pct']:+.2f}%"
        body.append([r["metric"], _fmt(r["base"]), _fmt(r["cand"]), d, r["status"]])
    widths = [max(len(str(x)) for x in col) for col in zip(head, *body)] if body else [len(h) for h in head]
    line = "  ".join(h.ljust(w) for h, w in zip(head, widths))
    print(line, file=out)
    print("-" * len(line), file=out)
    for row in body:
        print("  ".join(str(x).ljust(w) for x, w in zip(row, widths)), file=out)
    for r in verdict.rows:
        if r.get("missing"):
            print(f"  missing checkpoints: {', '.join(r['missing'])}", file=out)


def _load_side(arg: str) -> Dict[str, Any]:
    if os.path.isdir(arg):
        return summarize(arg)
    if os.path.isfile(arg):
        data = _read_json_optional(arg)
        if data is None:
            raise ValueError(f"{arg}: not a summary object")
        return data
    raise FileNotFoundError(f"not a run directory or summary file: {arg}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Summarize and compare runtime regression runs.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    ps = sub.add_parser("summarize", help="summarize one run directory")
    ps.add_argument("run_dir")
    ps.add_argument("-o", "--out", help="write the summary JSON here")
    ps.add_argument("--skip-windows", type=int, default=SKIP_WINDOWS)
    ps.add_argument("--window", type=int, default=WINDOW)

    pc = sub.add_parser("compare", help="compare BASE and CAND (run dirs or summary JSON)")
    pc.add_argument("base")
    pc.add_argument("cand")
    pc.add_argument("--json", action="store_true", help="print the verdict as JSON")
    pc.add_argument("--fps-median-drop", type=float, default=None)
    pc.add_argument("--fps-p10-drop", type=float, default=None)
    pc.add_argument("--frame-ms-p99-rise", type=float, default=None)
    pc.add_argument("--peak-mem-rise", type=float, default=None)

    args = ap.parse_args(argv)
    try:
        if args.cmd == "summarize":
            summary = summarize(args.run_dir, args.skip_windows, args.window)
            if args.out:
                with open(args.out, "w", encoding="utf-8") as fh:
                    json.dump(summary, fh, indent=2)
                print(f"wrote {args.out}")
            else:
                print(json.dumps(summary, indent=2))
            return 0

        overrides = {k: v for k, v in {
            "fps_median_drop": args.fps_median_drop,
            "fps_p10_drop": args.fps_p10_drop,
            "frame_ms_p99_rise": args.frame_ms_p99_rise,
            "peak_mem_rise": args.peak_mem_rise,
        }.items() if v is not None}
        verdict = compare(_load_side(args.base), _load_side(args.cand), overrides)
        if args.json:
            print(json.dumps(verdict.to_dict(), indent=2))
        else:
            print_table(verdict)
            n = verdict.loss_count
            print(f"VERDICT: loss ({n} metrics)" if n else "VERDICT: no loss")
        return 1 if verdict.loss_count else 0
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
