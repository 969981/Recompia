#!/usr/bin/env python3
"""Per-frame trace comparator (tracediff).

Compares two JSON Lines traces: one object per game frame, keyed by "f". Written
by our recompiled runtime ("nxrt", also writes "h") or a reference emulator
("eden"). Checkpoint lines (carrying "label") are compared by label, not frame.

Module: load_trace(), diff_traces(), format_report().
CLI:    python tracediff.py A.jsonl B.jsonl [--tol pos=0.05,vel=0.1,hp=0.5,igt=0.02]
        [--offset N] [--align first-change:area] [--fields pos,area] [--exact-hash]
        [--max-report 20] [--json]
Exit:   0 = passed, 1 = mismatches (or no frames matched), 2 = usage or file error.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import sys
from collections import deque
from typing import Any, Dict, List, Optional, Sequence, Tuple

DEFAULT_TOL: Dict[str, float] = {"pos": 0.05, "vel": 0.10, "hp": 0.5, "igt": 0.02, "state": 0.0}
DEFAULT_FIELDS: Tuple[str, ...] = ("area", "world", "pos", "vel", "hp", "igt", "state")
ALIGN_FIELDS: Tuple[str, ...] = ("area", "world", "igt", "state", "hp")
HASH_FIELD = "h"
STRING_FIELDS = frozenset({"area", "world", HASH_FIELD})
MAX_STORED = 1000


@dataclasses.dataclass
class DiffResult:
    offset: int = 0
    matched_frames: int = 0
    compared: int = 0                 # field comparisons actually made
    skipped: int = 0                  # field pairs skipped because one side lacks it
    checkpoints_matched: int = 0
    checkpoint_only_in_a: int = 0
    checkpoint_only_in_b: int = 0
    mismatch_count: int = 0
    mismatches: List[Dict[str, Any]] = dataclasses.field(default_factory=list)
    first_divergence: Optional[Dict[str, Any]] = None
    max_delta: Dict[str, float] = dataclasses.field(default_factory=dict)
    only_in_a: int = 0                # frames in A with no counterpart in B
    only_in_b: int = 0                # frames in B with no counterpart in A
    passed: bool = False


def _is_int(x: Any) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


def _is_num(x: Any) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _all_num(seq: Sequence[Any]) -> bool:
    return all(_is_num(x) for x in seq)


def load_trace(path: str) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    """Read a JSON Lines trace and return (header, objects).

    The header is the first non-blank line when it has no "f" key, else {}.
    Every other non-blank line is returned in file order. Objects without "f"
    stay in the list but diff_traces ignores them.

    Raises OSError if the file cannot be read, and ValueError naming the
    physical line number if a line is not a JSON object.
    """
    header: Dict[str, Any] = {}
    objects: List[Dict[str, Any]] = []
    first = True
    with open(path, "r", encoding="utf-8-sig") as fh:
        for lineno, raw in enumerate(fh, start=1):
            text = raw.strip()
            if not text:
                continue
            try:
                obj = json.loads(text)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: malformed JSON ({exc.msg})") from None
            if not isinstance(obj, dict):
                raise ValueError(f"{path}:{lineno}: expected a JSON object")
            if first:
                first = False
                if "f" not in obj:
                    header = obj
                    continue
            objects.append(obj)
    return header, objects


def _index(objs: Sequence[Dict[str, Any]]):
    """Split objects into {frame: obj} (first occurrence wins) and checkpoint list."""
    frames: Dict[int, Dict[str, Any]] = {}
    checkpoints: List[Dict[str, Any]] = []
    for obj in objs:
        if "label" in obj:
            checkpoints.append(obj)
            continue
        f = obj.get("f")
        if _is_int(f):
            frames.setdefault(f, obj)
    return frames, checkpoints


def _num_delta(x: float, y: float) -> float:
    if x == y:
        return 0.0
    if math.isnan(x) and math.isnan(y):
        return 0.0
    if math.isnan(x) or math.isnan(y):
        return math.inf
    return abs(x - y)


def _value_mismatch(va: Any, vb: Any, tol: float) -> Tuple[bool, Optional[float]]:
    """Return (is_mismatch, delta). delta is None when the values are not numeric."""
    if _is_num(va) and _is_num(vb):
        d = _num_delta(float(va), float(vb))
        return d > tol, d
    if isinstance(va, list) and isinstance(vb, list) and _all_num(va) and _all_num(vb):
        if len(va) != len(vb):
            return True, None
        d = max((_num_delta(float(x), float(y)) for x, y in zip(va, vb)), default=0.0)
        return d > tol, d
    return va != vb, None


def _coerce(field: str, text: str) -> Any:
    if field in STRING_FIELDS:
        return text.strip().lower()
    if field == "state":
        return int(text)
    return float(text)


def _parse_align(spec: str) -> Tuple[str, str, Any]:
    if spec.startswith("first-change:"):
        field, mode, raw = spec[len("first-change:"):], "change", None
    elif spec.startswith("first:") and "=" in spec:
        body = spec[len("first:"):]
        field, raw = body.split("=", 1)
        mode = "equals"
    else:
        raise ValueError(
            f"bad align spec {spec!r}; use first-change:<field> or first:<field>=<value>")
    if field not in ALIGN_FIELDS:
        raise ValueError(f"align field must be one of {', '.join(ALIGN_FIELDS)}; got {field!r}")
    target = None
    if mode == "equals":
        try:
            target = _coerce(field, raw)
        except ValueError:
            raise ValueError(f"align value {raw!r} is not valid for field {field!r}") from None
    return field, mode, target


def _find_change(frames: Dict[int, Dict[str, Any]], field: str, mode: str, target: Any) -> Optional[int]:
    first: Any = None
    have_first = False
    for f in sorted(frames):
        v = frames[f].get(field)
        if v is None:
            continue
        if mode == "equals":
            if isinstance(target, str):
                if isinstance(v, str) and v.lower() == target:
                    return f
            elif _is_num(v) and float(v) == float(target):
                return f
            continue
        if not have_first:
            first, have_first = v, True
            continue
        if v != first:
            return f
    return None


def _align_offset(a_frames: Dict[int, Dict[str, Any]], b_frames: Dict[int, Dict[str, Any]], spec: str) -> int:
    field, mode, target = _parse_align(spec)
    fa = _find_change(a_frames, field, mode, target)
    if fa is None:
        raise ValueError(f"align: no point matching {spec!r} in trace A")
    fb = _find_change(b_frames, field, mode, target)
    if fb is None:
        raise ValueError(f"align: no point matching {spec!r} in trace B")
    # b's frame f+offset corresponds to a's frame f, so the event at fb is fa + offset.
    return fb - fa


def diff_traces(a: Sequence[Dict[str, Any]], b: Sequence[Dict[str, Any]],
                tol: Optional[Dict[str, float]] = None, offset: int = 0,
                align: Optional[str] = None, fields: Optional[Sequence[str]] = None,
                exact_hash: bool = False) -> DiffResult:
    """Compare two lists of frame dicts. b's frame f+offset corresponds to a's frame f.

    align (mutually exclusive with a non-zero offset) computes the offset from the
    first change of a field, or the first frame where it equals a value.
    exact_hash compares only "h", for equality. Fields missing on either side are skipped.
    """
    if align is not None and offset != 0:
        raise ValueError("align and a non-zero offset cannot be combined")
    if exact_hash:
        use_fields = [HASH_FIELD]
    elif fields is None:
        use_fields = list(DEFAULT_FIELDS)
    else:
        use_fields = list(fields)
        if not use_fields:
            raise ValueError("fields list is empty")
    tols: Dict[str, float] = dict(DEFAULT_TOL)
    if tol:
        tols.update(tol)

    a_frames, a_cps = _index(a)
    b_frames, b_cps = _index(b)
    if align is not None:
        offset = _align_offset(a_frames, b_frames, align)

    res = DiffResult(offset=offset)
    found: List[Tuple[Tuple[int, int, int], Dict[str, Any]]] = []

    def compare(key: Tuple[int, int, int], frame: Optional[int], ea: Dict[str, Any],
                eb: Dict[str, Any], extra: Optional[Dict[str, Any]] = None) -> None:
        for name in use_fields:
            va = ea.get(name)
            vb = eb.get(name)
            if va is None or vb is None:
                res.skipped += 1
                continue
            res.compared += 1
            bad, delta = _value_mismatch(va, vb, tols.get(name, 0.0))
            if delta is not None and (name not in res.max_delta or delta > res.max_delta[name]):
                res.max_delta[name] = delta
            if bad:
                rec: Dict[str, Any] = {"frame": frame, "field": name, "a": va, "b": vb, "delta": delta}
                if extra:
                    rec.update(extra)
                res.mismatch_count += 1
                found.append((key, rec))

    for fa in sorted(a_frames):
        eb = b_frames.get(fa + offset)
        if eb is None:
            res.only_in_a += 1
            continue
        res.matched_frames += 1
        compare((fa, 0, 0), fa, a_frames[fa], eb)
    res.only_in_b = sum(1 for fb in b_frames if (fb - offset) not in a_frames)

    # Checkpoints: pair by label name, k-th occurrence in A with k-th occurrence in B.
    def label_key(obj: Dict[str, Any]) -> str:
        return json.dumps(obj.get("label"), sort_keys=True)

    pending: Dict[str, deque] = {}
    for cp in b_cps:
        pending.setdefault(label_key(cp), deque()).append(cp)
    for seq, cp in enumerate(a_cps):
        q = pending.get(label_key(cp))
        if not q:
            res.checkpoint_only_in_a += 1
            continue
        partner = q.popleft()
        res.checkpoints_matched += 1
        fa = cp.get("f") if _is_int(cp.get("f")) else None
        key = (fa if fa is not None else -1, 1, seq)
        compare(key, fa, cp, partner, {"label": cp.get("label")})
    res.checkpoint_only_in_b = sum(len(q) for q in pending.values())

    found.sort(key=lambda kv: kv[0])
    res.mismatches = [rec for _, rec in found[:MAX_STORED]]
    res.first_divergence = res.mismatches[0] if res.mismatches else None
    res.passed = res.mismatch_count == 0 and res.matched_frames > 0
    return res


def _fmt(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.6g}"
    if isinstance(v, list):
        return "[" + ", ".join(_fmt(x) for x in v) + "]"
    return str(v)


def _frame_text(m: Dict[str, Any]) -> str:
    text = "-" if m.get("frame") is None else str(m["frame"])
    if "label" in m:
        text += f" [{m['label']}]"
    return text


def format_report(result: DiffResult, max_rows: int = 20) -> str:
    lines = [
        f"result: {'PASS' if result.passed else 'FAIL'}",
        f"offset used: {result.offset}",
        f"matched frames: {result.matched_frames}  (only in A: {result.only_in_a}, only in B: {result.only_in_b})",
        f"checkpoints matched: {result.checkpoints_matched}  "
        f"(only in A: {result.checkpoint_only_in_a}, only in B: {result.checkpoint_only_in_b})",
        f"field comparisons: {result.compared}  (skipped, field missing on one side: {result.skipped})",
        f"mismatches: {result.mismatch_count}",
    ]
    if result.matched_frames == 0:
        lines.append("no frames matched: check --offset / --align")
    fd = result.first_divergence
    if fd is not None:
        where = f"frame {_frame_text(fd)}"
        delta = "" if fd["delta"] is None else f" delta={fd['delta']:.6g}"
        lines.append(f"first divergence: {where} field {fd['field']}: a={_fmt(fd['a'])} b={_fmt(fd['b'])}{delta}")
    if result.max_delta:
        lines.append("max delta: " + ", ".join(f"{k}={v:.6g}" for k, v in sorted(result.max_delta.items())))
    if result.mismatches and max_rows > 0:
        shown = result.mismatches[:max_rows]
        rows = [("frame", "field", "a", "b", "delta")]
        for m in shown:
            rows.append((_frame_text(m), m["field"], _fmt(m["a"]), _fmt(m["b"]),
                         "-" if m["delta"] is None else f"{m['delta']:.6g}"))
        widths = [max(len(r[i]) for r in rows) for i in range(5)]
        lines.append("")
        for r in rows:
            lines.append("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(r)).rstrip())
        more = result.mismatch_count - len(shown)
        if more > 0:
            lines.append(f"... and {more} more")
    return "\n".join(lines)


def parse_tol(text: str) -> Dict[str, float]:
    out: Dict[str, float] = {}
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "=" not in part:
            raise ValueError(f"bad --tol entry {part!r}; use field=value")
        key, raw = part.split("=", 1)
        try:
            value = float(raw)
        except ValueError:
            raise ValueError(f"bad --tol value in {part!r}") from None
        if value < 0:
            raise ValueError(f"--tol value must be >= 0: {part!r}")
        out[key.strip()] = value
    return out


def main(argv: Optional[Sequence[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Compare two per-frame JSON Lines traces.")
    p.add_argument("a", help="trace A (JSON Lines)")
    p.add_argument("b", help="trace B (JSON Lines)")
    p.add_argument("--tol", default=None, help="tolerances, e.g. pos=0.05,vel=0.1,hp=0.5,igt=0.02")
    p.add_argument("--offset", type=int, default=0, help="b frame f+N corresponds to a frame f")
    p.add_argument("--align", default=None, help="first-change:<field> or first:<field>=<value>")
    p.add_argument("--fields", default=None, help="comma list of fields to compare")
    p.add_argument("--exact-hash", action="store_true", help="compare only 'h', for equality")
    p.add_argument("--max-report", type=int, default=20, help="mismatch rows to print")
    p.add_argument("--json", action="store_true", help="print the result as JSON")
    args = p.parse_args(argv)

    try:
        tol = parse_tol(args.tol) if args.tol is not None else None
        fields = [s.strip() for s in args.fields.split(",") if s.strip()] if args.fields is not None else None
        _, frames_a = load_trace(args.a)
        _, frames_b = load_trace(args.b)
        res = diff_traces(frames_a, frames_b, tol=tol, offset=args.offset, align=args.align,
                          fields=fields, exact_hash=args.exact_hash)
    except (OSError, ValueError) as exc:
        print(f"tracediff: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(dataclasses.asdict(res), indent=2))
    else:
        print(format_report(res, max_rows=args.max_report))
    return 0 if res.passed else 1


if __name__ == "__main__":
    sys.exit(main())
