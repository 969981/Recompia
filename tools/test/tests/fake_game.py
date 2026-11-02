#!/usr/bin/env python3
"""Stand-in for the game executable, used by run_suite.py's unit tests.

Reads the NXRT_TEST_* environment, writes result.json / trace.jsonl /
shots/a.png into NXRT_TEST_OUT and exits with the game's exit-code contract.
The behaviour is chosen by the first line of the script file:

    # fake: <mode>

Modes:
    pass            exit 0, verdict pass
    fail            exit 1, verdict fail
    crash           exit 4, writes crash.txt
    crashfile       exit 1, writes boot_crash.txt (a *_crash.txt file, classified CRASH)
    freeze          exit 5, writes freeze.txt
    hang            never exits (the runner must kill it on timeout)
    badscript       exit 2, usage / parse error, writes nothing
    internal        exit 6, internal test-tool error
    deadline        exit 124
    odd             exit 3 (anything outside the contract)
    nondeterministic exit 0, verdict pass, final_hash differs on every run

Optional knobs from the environment (set via the suite's env table):
    FAKE_FRAMES       frame count (default 120)
    FAKE_TRACE_SHIFT  float added to every trace position (default 0)
    FAKE_FRAME_OFFSET int added to every trace frame number f (default 0)
    FAKE_SHOT_RGB     "r,g,b" colour of shots/a.png (default 128,128,128)
"""
import hashlib
import json
import os
import re
import sys
import time
import uuid
from pathlib import Path

MODE_RE = re.compile(r"#\s*fake:\s*([A-Za-z_]+)")


def write_trace(out: Path, frames: int, shift: float, offset: int = 0) -> None:
    """One record per frame keyed by "f" (tracediff's key). offset renumbers frames: f = i + offset."""
    with open(out / "trace.jsonl", "w", encoding="utf-8") as f:
        for i in range(frames):
            rec = {"f": i + offset, "pos": [round(i * 0.1 + shift, 6), 0.0, 1.0],
                   "state": 0 if i < 10 else 1}  # one change, at frame 10, for align tests
            f.write(json.dumps(rec) + "\n")


def write_shot(out: Path, rgb: str) -> None:
    from PIL import Image

    r, g, b = (int(v) for v in rgb.split(","))
    (out / "shots").mkdir(parents=True, exist_ok=True)
    Image.new("RGB", (64, 36), (r, g, b)).save(out / "shots" / "a.png")


def write_result(out: Path, verdict: str, frames: int, final_hash: str, reason: str = "") -> None:
    data = {"verdict": verdict, "frames": frames, "final_hash": final_hash}
    if reason:
        data["reason"] = reason
    (out / "result.json").write_text(json.dumps(data), encoding="utf-8")


def main() -> int:
    script = os.environ.get("NXRT_TEST_SCRIPT", "")
    out_s = os.environ.get("NXRT_TEST_OUT", "")
    if not script or not out_s:
        print("fake_game: NXRT_TEST_SCRIPT / NXRT_TEST_OUT not set", file=sys.stderr)
        return 6
    out = Path(out_s)
    out.mkdir(parents=True, exist_ok=True)

    try:
        first = Path(script).read_text(encoding="utf-8").splitlines()[0]
    except (OSError, IndexError):
        first = ""
    m = MODE_RE.search(first)
    mode = m.group(1) if m else "badscript"

    frames = int(os.environ.get("FAKE_FRAMES", "120"))
    shift = float(os.environ.get("FAKE_TRACE_SHIFT", "0"))
    frame_offset = int(os.environ.get("FAKE_FRAME_OFFSET", "0"))
    shot_rgb = os.environ.get("FAKE_SHOT_RGB", "128,128,128")
    script_digest = hashlib.sha256(Path(script).read_bytes()).hexdigest()

    if mode == "badscript":
        print("fake_game: script parse error", file=sys.stderr)
        return 2
    if mode == "hang":
        (out / "hang_pid.txt").write_text(str(os.getpid()), encoding="utf-8")
        while True:
            time.sleep(0.2)
    if mode == "crash":
        (out / "crash.txt").write_text("fake crash\n", encoding="utf-8")
        return 4
    if mode == "crashfile":
        (out / "boot_crash.txt").write_text("fake named crash\n", encoding="utf-8")
        return 1
    if mode == "freeze":
        (out / "freeze.txt").write_text("fake freeze\n", encoding="utf-8")
        return 5
    if mode == "internal":
        return 6
    if mode == "deadline":
        return 124
    if mode == "odd":
        return 3

    if mode == "fail":
        write_result(out, "fail", frames, script_digest, reason="fake assert failed")
        write_trace(out, frames, shift, frame_offset)
        write_shot(out, shot_rgb)
        return 1
    if mode == "pass":
        write_result(out, "pass", frames, script_digest)
    elif mode == "nondeterministic":
        write_result(out, "pass", frames, uuid.uuid4().hex)
    else:
        print(f"fake_game: unknown mode {mode!r}", file=sys.stderr)
        return 2
    write_trace(out, frames, shift, frame_offset)
    write_shot(out, shot_rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
