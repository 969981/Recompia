#!/usr/bin/env python3
"""Run a TOML suite of scripted runs against the game exe, headless, and print one PASS/FAIL table.

Usage:
    python run_suite.py SUITE.toml [--only a,b] [--tag t] [--exe X] [--keep] [--list] [--jobs N]

Exit:
    0  every selected run is PASS
    1  at least one run is not PASS
    2  suite file / usage error (nothing was run)

Outputs:
    <out_root>\\<run>[_<n>]\\   one dir per attempt (stdout.txt, stderr.txt, mpr.log, result.json, ...)
    <out_root>\\suite_result.json   list of per-run dicts

Suite format and the exit-code contract of the game exe are described in the
brief for this tool; see the module-level constants below for defaults.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import imgdiff  # noqa: E402  (same folder)
import tracediff  # noqa: E402  (same folder)

STATUS_ORDER = ("PASS", "FAIL", "CRASH", "FREEZE", "TIMEOUT", "ERROR")
TOTALS_LABELS = {
    "PASS": "pass", "FAIL": "fail", "CRASH": "crash",
    "FREEZE": "freeze", "TIMEOUT": "timeout", "ERROR": "error",
}
DEFAULT_TIMEOUT = 300.0
DEFAULT_MIN_SSIM = 0.85
NOTE_MAX = 70
RUN_NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


class SuiteError(Exception):
    """Bad suite file or bad command line. Reported with exit code 2."""


# ----------------------------------------------------------------- data model

@dataclass
class RunSpec:
    name: str
    script: Path
    exe: list
    cwd: Path
    timeout: float
    env: dict
    deterministic: bool
    regions: Optional[Path]
    args: list
    expect_exit: int
    repeat: int
    ref_trace: Optional[dict]          # {"ref": Path, "tol": {name: float}, "align": str}
    ref_shots: dict                    # shotname -> {"ref": Path, "min_ssim": float, "mask": Any}
    tags: list


@dataclass
class Attempt:
    outdir: Path
    status: str
    rc: Optional[int]
    wall: float
    frames: Optional[int]
    final_hash: Optional[str]
    reason: str
    notes: list = field(default_factory=list)


@dataclass
class RunResult:
    spec: RunSpec
    status: str = "ERROR"
    frames: Optional[int] = None
    wall: float = 0.0
    reason: str = ""
    notes: list = field(default_factory=list)
    attempts: list = field(default_factory=list)


# --------------------------------------------------------------- suite loading

def _as_str_list(value: Any, what: str) -> list:
    if isinstance(value, str):
        return [value]
    if isinstance(value, list) and all(isinstance(v, str) for v in value):
        return list(value)
    raise SuiteError(f"{what} must be a string or a list of strings")


def _resolve_path(base: Path, value: str) -> Path:
    p = Path(value)
    return p if p.is_absolute() else (base / p)


def _resolve_exe_part(base: Path, value: str) -> str:
    p = Path(value)
    if p.is_absolute():
        return str(p)
    cand = base / p
    if cand.exists():
        return str(cand.resolve())
    return value  # bare command name, looked up on PATH by the OS


def parse_tol(text: Any) -> dict:
    """'pos=0.05,vel=0.1' -> {'pos': 0.05, 'vel': 0.1}. A table is accepted too.

    Keys must be tracediff tolerance names (pos, vel, hp, igt, state).
    """
    if text in (None, ""):
        return {}
    if isinstance(text, dict):
        pairs = [(str(k), v) for k, v in text.items()]
    else:
        pairs = []
        for part in str(text).split(","):
            part = part.strip()
            if not part:
                continue
            if "=" not in part:
                raise SuiteError(f"tol entry must look like name=value, got {part!r}")
            k, v = part.split("=", 1)
            pairs.append((k.strip(), v))
    out = {}
    for k, v in pairs:
        if k not in tracediff.DEFAULT_TOL:
            raise SuiteError(f"tol key {k!r} is not one of {', '.join(tracediff.DEFAULT_TOL)}")
        try:
            out[k] = float(v)
        except (TypeError, ValueError):
            raise SuiteError(f"tol value must be a number, got {k}={v!r}")
    return out


def load_suite(path: Path, exe_override: Optional[str] = None):
    """Return (out_root, [RunSpec, ...]) for the suite file. Raises SuiteError."""
    base = path.parent
    with open(path, "rb") as fh:
        data = tomllib.load(fh)
    defaults = data.get("defaults", {})
    if not isinstance(defaults, dict):
        raise SuiteError("[defaults] must be a table")
    raw_runs = data.get("run", [])
    if not isinstance(raw_runs, list) or not raw_runs:
        raise SuiteError("suite has no [[run]] entries")

    default_cwd = _resolve_path(base, defaults.get("cwd", "."))
    out_root = _resolve_path(base, defaults["out_root"]) if "out_root" in defaults \
        else default_cwd / "suite_out"

    seen = set()
    specs = []
    for raw in raw_runs:
        if not isinstance(raw, dict):
            raise SuiteError("each [[run]] must be a table")

        def get(key, fallback=None):
            return raw.get(key, defaults.get(key, fallback))

        name = raw.get("name")
        if not isinstance(name, str) or not RUN_NAME_RE.match(name):
            raise SuiteError(f"run name missing or not a safe folder name: {name!r}")
        if name in seen:
            raise SuiteError(f"duplicate run name: {name}")
        seen.add(name)

        if "script" not in raw:
            raise SuiteError(f"run {name}: script is required")
        script = _resolve_path(base, str(raw["script"]))
        if not script.is_file():
            raise SuiteError(f"run {name}: script not found: {script}")

        if exe_override is not None:
            exe = [exe_override if not Path(exe_override).exists() else str(Path(exe_override).resolve())]
        else:
            if "exe" not in defaults and "exe" not in raw:
                raise SuiteError(f"run {name}: no exe (set it in [defaults] or on the run)")
            exe_parts = _as_str_list(get("exe"), "exe")
            exe = [_resolve_exe_part(base, p) for p in exe_parts]

        env = {str(k): str(v) for k, v in dict(defaults.get("env", {})).items()}
        env.update({str(k): str(v) for k, v in dict(raw.get("env", {})).items()})

        timeout = float(get("timeout", DEFAULT_TIMEOUT))
        if timeout <= 0:
            raise SuiteError(f"run {name}: timeout must be > 0")

        repeat = int(raw.get("repeat", 1))
        if repeat < 1:
            raise SuiteError(f"run {name}: repeat must be >= 1")

        regions = get("regions")
        args = _as_str_list(raw.get("args", defaults.get("args", [])), "args")

        ref_trace = None
        rt = raw.get("ref_trace")
        if rt is not None:
            if isinstance(rt, str):
                rt = {"ref": rt}
            if not isinstance(rt, dict) or "ref" not in rt:
                raise SuiteError(f"run {name}: ref_trace must be a path or a table with 'ref'")
            ref_trace = {
                "ref": _resolve_path(base, str(rt["ref"])),
                "tol": parse_tol(rt.get("tol")),
                "align": str(rt.get("align", "") or ""),
            }

        ref_shots = {}
        for shot, spec_val in dict(raw.get("ref_shots", {})).items():
            if isinstance(spec_val, str):
                spec_val = {"ref": spec_val}
            if not isinstance(spec_val, dict) or "ref" not in spec_val:
                raise SuiteError(f"run {name}: ref_shots.{shot} must be a path or a table with 'ref'")
            ref_shots[str(shot)] = {
                "ref": _resolve_path(base, str(spec_val["ref"])),
                "min_ssim": float(spec_val.get("min_ssim", DEFAULT_MIN_SSIM)),
                "mask": spec_val.get("mask"),
            }

        specs.append(RunSpec(
            name=name,
            script=script,
            exe=exe,
            cwd=_resolve_path(base, str(get("cwd", "."))),
            timeout=timeout,
            env=env,
            deterministic=bool(get("deterministic", True)),
            regions=_resolve_path(base, str(regions)) if regions else None,
            args=args,
            expect_exit=int(raw.get("expect_exit", 0)),
            repeat=repeat,
            ref_trace=ref_trace,
            ref_shots=ref_shots,
            tags=_as_str_list(raw.get("tags", []), "tags") if raw.get("tags") else [],
        ))
    return out_root, specs


def select_runs(specs, only=None, tags=None):
    chosen = list(specs)
    if only:
        wanted = [n.strip() for n in only if n.strip()]
        known = {s.name for s in specs}
        unknown = [n for n in wanted if n not in known]
        if unknown:
            raise SuiteError(f"--only names not in suite: {', '.join(unknown)}")
        chosen = [s for s in chosen if s.name in set(wanted)]
    if tags:
        chosen = [s for s in chosen if any(t in s.tags for t in tags)]
    return chosen


# ----------------------------------------------------------------- execution

def build_env(spec: RunSpec, outdir: Path) -> dict:
    env = dict(os.environ)
    env.update(spec.env)
    env.setdefault("NXRT_GFX_HEADLESS", "1")  # a suite env entry may override this one
    env["NXRT_TEST_SCRIPT"] = str(spec.script)
    env["NXRT_TEST_OUT"] = str(outdir)
    env["NXRT_TEST_DETERMINISTIC"] = "1" if spec.deterministic else "0"
    env["NXRT_LOG_PATH"] = str(outdir / "mpr.log")
    if spec.regions is not None:
        env["NXRT_TEST_REGIONS"] = str(spec.regions)
    else:
        env.pop("NXRT_TEST_REGIONS", None)
    return env


def _kill_tree(proc: subprocess.Popen) -> None:
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        import signal
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass
    if proc.poll() is None:
        proc.kill()


def _read_result(outdir: Path):
    """Return (dict or None, error text or '')."""
    p = outdir / "result.json"
    if not p.exists():
        return None, ""
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return None, f"result.json unreadable: {e}"
    if not isinstance(data, dict):
        return None, "result.json is not an object"
    return data, ""


def _count_trace_lines(outdir: Path) -> Optional[int]:
    p = outdir / "trace.jsonl"
    if not p.exists():
        return None
    with open(p, "rb") as fh:
        return sum(1 for line in fh if line.strip())


def classify(rc: Optional[int], timed_out: bool, wall: float, spec: RunSpec, outdir: Path) -> Attempt:
    result, result_err = _read_result(outdir)
    frames = result.get("frames") if result and isinstance(result.get("frames"), int) else None
    if frames is None:
        frames = _count_trace_lines(outdir)
    final_hash = result.get("final_hash") if result else None
    if final_hash is not None:
        final_hash = str(final_hash)
    script_reason = str(result.get("reason", "")) if result else ""

    def done(status, reason="", notes=None):
        return Attempt(outdir, status, rc, wall, frames, final_hash, reason, notes or [])

    crash_files = [p.name for p in outdir.glob("*crash.txt")]
    freeze_file = (outdir / "freeze.txt").exists()

    if timed_out:
        return done("TIMEOUT", f"killed after {spec.timeout:g}s")
    if rc == 124:
        return done("TIMEOUT", "exit 124 (deadline)")
    if rc == 4:
        return done("CRASH", "host crash (exit 4)")
    if crash_files:
        return done("CRASH", f"crash file: {crash_files[0]}")
    if rc == 5:
        return done("FREEZE", "watchdog freeze (exit 5)")
    if freeze_file:
        return done("FREEZE", "freeze.txt present")
    if rc in (2, 6):
        why = "usage/parse error" if rc == 2 else "internal test-tool error"
        return done("ERROR", f"exit {rc}: {script_reason or why}")
    if rc in (0, 1):
        if result_err:
            return done("ERROR", result_err)
        if result and rc == 0 and result.get("verdict") not in (None, "pass"):
            return done("ERROR", f"exit 0 but result.json verdict is {result.get('verdict')!r}")
        if rc == 0:
            if spec.expect_exit == 0:
                return done("PASS")
            return done("FAIL", "expected failure but passed")
        # rc == 1: the script failed
        if spec.expect_exit == 1:
            note = f"expected failure: {script_reason}" if script_reason else "expected failure"
            return done("PASS", "", [note])
        return done("FAIL", script_reason or "script failed (exit 1)")
    return done("ERROR", f"unexpected exit code {rc}")


def run_once(spec: RunSpec, outdir: Path) -> Attempt:
    cmd = list(spec.exe) + list(spec.args)
    env = build_env(spec, outdir)
    popen_kwargs = {"start_new_session": True} if os.name != "nt" else {}
    t0 = time.monotonic()
    with open(outdir / "stdout.txt", "wb") as out_f, open(outdir / "stderr.txt", "wb") as err_f:
        try:
            proc = subprocess.Popen(cmd, cwd=str(spec.cwd), env=env,
                                    stdout=out_f, stderr=err_f, **popen_kwargs)
        except OSError as e:
            return Attempt(outdir, "ERROR", None, 0.0, None, None, f"could not start: {e}")
        timed_out = False
        try:
            rc = proc.wait(timeout=spec.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            _kill_tree(proc)
            rc = proc.wait()
    wall = time.monotonic() - t0
    return classify(rc, timed_out, wall, spec, outdir)


def _short(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.6g}"
    if isinstance(v, list):
        return "[" + ", ".join(_short(x) for x in v) + "]"
    return str(v)


def check_trace(spec: RunSpec, outdir: Path):
    """Compare the run's trace.jsonl with ref_trace using tracediff. Return (failure_reason, notes)."""
    rt = spec.ref_trace
    if rt is None:
        return "", []
    if not rt["ref"].exists():
        return "", ["SKIP(ref missing)"]
    cand = outdir / "trace.jsonl"
    if not cand.exists():
        return ("no trace.jsonl written by the run", [])
    try:
        _, ref_objs = tracediff.load_trace(str(rt["ref"]))
        _, run_objs = tracediff.load_trace(str(cand))
        res = tracediff.diff_traces(ref_objs, run_objs,
                                    tol=rt["tol"] or None, align=rt["align"] or None)
    except (OSError, ValueError) as e:
        return (f"trace compare: {e}", [])
    notes = []
    if rt["align"]:
        notes.append(f"trace offset {res.offset} via align {rt['align']}")
    if res.matched_frames == 0:
        return ("trace compare: no frames matched (check align/offset)", notes)
    if res.mismatch_count:
        fd = res.first_divergence
        where = f"frame {fd['frame']}" if fd.get("frame") is not None else f"checkpoint {fd.get('label')}"
        return (f"trace diverges at {where} field {fd['field']}: "
                f"ref={_short(fd['a'])} run={_short(fd['b'])} "
                f"({res.mismatch_count} mismatches)", notes)
    if rt["align"] is None and (res.only_in_a or res.only_in_b):
        return (f"frame count differs: {res.only_in_a} ref frames and "
                f"{res.only_in_b} run frames unmatched", notes)
    return "", notes


def check_shots(spec: RunSpec, outdir: Path):
    """Return (failure_reason or '', skip_notes list)."""
    fails, skips = [], []
    for shot, cfg in spec.ref_shots.items():
        if not cfg["ref"].exists():
            skips.append("SKIP(ref missing)")
            continue
        cand = outdir / "shots" / f"{shot}.png"
        if not cand.exists():
            fails.append(f"shot {shot} not written")
            continue
        masks = cfg["mask"]
        try:
            res = imgdiff.compare(str(cfg["ref"]), str(cand), masks=masks)
        except (ValueError, OSError) as e:
            fails.append(f"shot {shot}: compare error: {e}")
            continue
        if not res.passed({"min_ssim": cfg["min_ssim"]}):
            ssim = f"{res.ssim:.3f}" if res.ssim is not None else "n/a"
            mae = f"{res.mae:.1f}" if res.mae is not None else "n/a"
            fails.append(f"shot {shot} differs (ssim {ssim} vs min {cfg['min_ssim']}, mae {mae})")
    return ("; ".join(fails), skips)


def run_spec(spec: RunSpec, out_root: Path, keep: bool) -> RunResult:
    rr = RunResult(spec=spec)
    try:
        for i in range(spec.repeat):
            dirname = spec.name if spec.repeat == 1 else f"{spec.name}_{i + 1}"
            outdir = out_root / dirname
            if outdir.exists() and not keep:
                shutil.rmtree(outdir)
            outdir.mkdir(parents=True, exist_ok=True)
            rr.attempts.append(run_once(spec, outdir))

        first = rr.attempts[0]
        rr.frames = first.frames
        rr.wall = sum(a.wall for a in rr.attempts)
        bad = [(i, a) for i, a in enumerate(rr.attempts) if a.status != "PASS"]
        if bad:
            i, a = bad[0]
            rr.status = a.status
            rr.reason = a.reason if spec.repeat == 1 else f"attempt {i + 1}/{spec.repeat}: {a.reason}"
            rr.notes = list(a.notes)
            return rr

        rr.status = "PASS"
        rr.notes = [n for a in rr.attempts for n in a.notes]

        if spec.repeat > 1:
            if spec.deterministic:
                hashes = [a.final_hash for a in rr.attempts]
                if any(h is None for h in hashes):
                    rr.status, rr.reason = "FAIL", "determinism: result.json has no final_hash"
                elif len(set(hashes)) > 1:
                    rr.status, rr.reason = "FAIL", "nondeterministic: final_hash differs between repeats"
            else:
                rr.notes.append("determinism not checked (deterministic=false)")
        if rr.status != "PASS":
            return rr

        # Comparisons run on the first attempt's outputs.
        reasons, skips = [], []
        t_reason, t_notes = check_trace(spec, first.outdir)
        s_reason, s_skip = check_shots(spec, first.outdir)
        for r in (t_reason, s_reason):
            if r:
                reasons.append(r)
        skips += t_notes + s_skip
        rr.notes += skips
        if reasons:
            rr.status = "FAIL"
            rr.reason = reasons[0] if len(reasons) == 1 else "; ".join(reasons)
    except Exception as e:  # runner bug or unexpected OS error: keep the table going
        rr.status = "ERROR"
        rr.reason = f"runner exception: {type(e).__name__}: {e}"
    return rr


# ---------------------------------------------------------------- reporting

def _trim(text: str) -> str:
    text = " ".join(text.split())
    return text if len(text) <= NOTE_MAX else text[:NOTE_MAX - 3] + "..."


def _notes_text(rr: RunResult) -> str:
    parts = ([rr.reason] if rr.reason else []) + [n for n in rr.notes if n != rr.reason]
    return _trim("; ".join(parts))


def format_table(results) -> str:
    headers = ["NAME", "STATUS", "FRAMES", "WALL(s)"]
    rows = []
    for rr in results:
        rows.append([rr.spec.name, rr.status,
                     str(rr.frames) if rr.frames is not None else "-",
                     f"{rr.wall:.1f}", _notes_text(rr)])
    widths = [max(len(headers[i]), *(len(r[i]) for r in rows)) if rows else len(headers[i])
              for i in range(4)]

    def line(cells):
        padded = [cells[i].ljust(widths[i]) for i in range(4)]
        return "  ".join(padded + [cells[4]]).rstrip()

    out = [line(headers + ["NOTES"]), line(["-" * w for w in widths] + [""])]
    out += [line(r) for r in rows]
    counts = {s: 0 for s in STATUS_ORDER}
    for rr in results:
        counts[rr.status] = counts.get(rr.status, 0) + 1
    totals = ", ".join(f"{counts[s]} {TOTALS_LABELS[s]}" for s in
                       ("PASS", "FAIL", "CRASH", "FREEZE", "TIMEOUT", "ERROR"))
    out.append("")
    out.append(f"{len(results)} runs: {totals}")
    return "\n".join(out)


def result_dicts(results):
    out = []
    for rr in results:
        first = rr.attempts[0] if rr.attempts else None
        out.append({
            "name": rr.spec.name,
            "status": rr.status,
            "frames": rr.frames,
            "wall_s": round(rr.wall, 3),
            "reason": rr.reason,
            "notes": rr.notes,
            "tags": rr.spec.tags,
            "outdir": str(first.outdir) if first else None,
            "expect_exit": rr.spec.expect_exit,
            "attempts": [{
                "outdir": str(a.outdir), "status": a.status, "rc": a.rc,
                "wall_s": round(a.wall, 3), "frames": a.frames,
                "final_hash": a.final_hash, "reason": a.reason, "notes": a.notes,
            } for a in rr.attempts],
        })
    return out


# --------------------------------------------------------------------- CLI

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Run a suite of scripted game runs and print one PASS/FAIL table.")
    ap.add_argument("suite", help="suite TOML file")
    ap.add_argument("--only", default="", help="comma-separated run names to run")
    ap.add_argument("--tag", action="append", default=[], help="run only runs carrying this tag (repeatable)")
    ap.add_argument("--exe", help="override the exe for every run (one path)")
    ap.add_argument("--keep", action="store_true",
                    help="do not clear existing run output folders before running")
    ap.add_argument("--list", action="store_true", help="print the selected run names and exit")
    ap.add_argument("--jobs", type=int, default=1, help="runs in parallel (default 1)")
    args = ap.parse_args(argv)

    try:
        if args.jobs < 1:
            raise SuiteError("--jobs must be >= 1")
        suite_path = Path(args.suite).resolve()
        if not suite_path.is_file():
            raise SuiteError(f"suite file not found: {suite_path}")
        out_root, specs = load_suite(suite_path, args.exe)
        only = [n for n in args.only.split(",")] if args.only else None
        chosen = select_runs(specs, only, args.tag)
        if not chosen:
            raise SuiteError("no runs selected")
    except (SuiteError, tomllib.TOMLDecodeError, OSError, ValueError, TypeError, KeyError) as e:
        print(f"run_suite: {e}", file=sys.stderr)
        return 2

    if args.list:
        for s in chosen:
            print(s.name)
        return 0

    out_root.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda s: run_spec(s, out_root, args.keep), chosen))

    (out_root / "suite_result.json").write_text(
        json.dumps(result_dicts(results), indent=2), encoding="utf-8")

    print(format_table(results))
    return 0 if all(rr.status == "PASS" for rr in results) else 1


if __name__ == "__main__":
    sys.exit(main())
