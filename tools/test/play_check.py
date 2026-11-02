#!/usr/bin/env python3
"""Cross-run checks for the play-mode oracles (work area S8). run_suite.py proves each run passes; this proves what only a comparison can.

  python play_check.py suite <out_root>      after: run_suite.py suites/play.toml   (out_root = work/suite_out_play)
  python play_check.py mouse <run_dir> [--counts N] [--yaw-rad R] [--tol T]
                                             after a gameplay run of scripts/play_mouse_gameplay.nxs: the yaw the trace shows turned by
                                             counts * R radians (right = decreasing yaw)

Exit code 0 when every check passes, 1 when one fails, 2 on usage errors.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import imgdiff  # noqa: E402

RESULTS = []


def check(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def ssim(a: Path, b: Path):
    if not a.exists() or not b.exists():
        return None
    r = imgdiff.compare(str(a), str(b))
    return r.ssim


def suite(root: Path) -> None:
    sh = lambda run, shot: root / run / "shots" / f"{shot}.png"
    idle_same = ssim(sh("title_idle", "before"), sh("title_idle", "after"))
    check("title idle: nothing pressed, the screen does not advance", idle_same is not None and idle_same >= 0.999, f"ssim {idle_same}")
    pad_adv = ssim(sh("title_pad", "before"), sh("title_pad", "after"))
    check("title pad: A advances title -> menu", pad_adv is not None and pad_adv <= 0.95, f"ssim before/after {pad_adv}")
    for run in ("title_key_return", "title_key_space"):
        s = ssim(sh("title_pad", "after"), sh(run, "after"))
        check(f"{run}: keyboard press gives the same screen as the pad press", s is not None and s >= 0.99, f"ssim {s}")
        adv = ssim(sh(run, "before"), sh(run, "after"))
        check(f"{run}: the keyboard run advanced", adv is not None and adv <= 0.95, f"ssim before/after {adv}")
    for run in ("window_lifecycle", "window_fps_limiter"):
        log = root / run / "mpr.log"
        if not log.exists():
            check(f"{run}: log present", False, str(log))
            continue
        text = log.read_text(encoding="utf-8", errors="replace")
        errs = [l for l in text.splitlines() if re.search(r"\] E t\d+ ", l) and "svc:" not in l]
        check(f"{run}: no error lines in the log", not errs, errs[0][:160] if errs else "")
    log = root / "window_lifecycle" / "mpr.log"
    if log.exists():
        text = log.read_text(encoding="utf-8", errors="replace")
        steps = re.findall(r"selftest: step \d+: (\S+)", text)
        check("window_lifecycle: every self-test step ran", "close" in steps and steps.count("expect_mode") >= 5, f"{len(steps)} steps")
        check("window_lifecycle: closing the window ended the process cleanly", "ending the process (pass): window closed" in text)
        check("window_lifecycle: Alt+Enter toggled fullscreen", text.count("overlay") >= 0 and steps.count("altenter") == 2)
    log = root / "window_fps_limiter" / "mpr.log"
    if log.exists():
        text = log.read_text(encoding="utf-8", errors="replace")
        fps = [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r"selftest: fps=([\d.]+) over [\d.]+ s \(cap (\d+)\)", text)]
        for cap in (30, 60):
            got = [(f, l) for (f, c2), l in zip(fps, [x for x in re.findall(r"late p99 ([\d.]+) ms", text)]) if c2 == cap]
            check(f"frame limiter at {cap} fps starts waited frames on schedule (late p99 <= 0.5 ms)", bool(got) and float(got[-1][1]) <= 0.5, f"measured {got}")
        for cap in (120, 144):
            got = [f for f, c2 in fps if c2 == cap]
            print(f"INFO  cap {cap}: the game delivered {got} fps (it cannot yet exceed its own frame time at the title screen, see S8.md)")


def mouse(run: Path, counts: int, yaw_rad: float, tol: float) -> None:
    trace = run / "trace.jsonl"
    frames = [json.loads(l) for l in trace.read_text().splitlines() if l.strip()]
    yaws = [(f["f"], f["yaw"]) for f in frames if "yaw" in f]
    if len(yaws) < 10:
        check("mouse: trace has yaw samples", False, f"{len(yaws)} samples (gameplay not reached?)")
        return
    # the burst is the largest cumulative yaw change over ten consecutive frames
    best, best_i = 0.0, 0
    for i in range(len(yaws) - 12):
        d = sum(((yaws[j + 1][1] - yaws[j][1] + math.pi) % (2 * math.pi)) - math.pi for j in range(i, i + 12))
        if abs(d) > abs(best):
            best, best_i = d, i
    expect = -counts * yaw_rad
    check("mouse: yaw turned by counts * radians per count", abs(best - expect) <= abs(expect) * tol + 1e-4, f"turned {best:.6f} rad, expected {expect:.6f} at frame {yaws[best_i][0]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("suite")
    s.add_argument("root")
    m = sub.add_parser("mouse")
    m.add_argument("run")
    m.add_argument("--counts", type=int, default=1000)
    m.add_argument("--yaw-rad", type=float, default=0.000767945)
    m.add_argument("--tol", type=float, default=0.02)
    a = ap.parse_args()
    if a.cmd == "suite":
        suite(Path(a.root))
    else:
        mouse(Path(a.run), a.counts, a.yaw_rad, a.tol)
    bad = [r for r in RESULTS if not r[1]]
    print(f"{len(RESULTS) - len(bad)} of {len(RESULTS)} checks passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
