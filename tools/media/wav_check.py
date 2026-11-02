#!/usr/bin/env python3
"""wav_check: decide whether a 16-bit PCM WAV from the game audio engine holds real, clean sound.

Usage:
    python wav_check.py FILE.wav [--rate 48000] [--channels 2] [--min-seconds 10]
                        [--min-active 0.2] [--json]

Default output: one PASS/FAIL summary line, then one line per failed condition and
one per warning. --json prints the full result (verdict, metrics, warnings) as JSON.
Exit codes: 0 PASS, 1 FAIL, 2 unreadable file or bad arguments.

Needs numpy (available in tools\\.venv).
"""

import argparse
import json
import os
import struct
import sys

import numpy as np

WAVE_FORMAT_PCM = 0x0001
WAVE_FORMAT_EXTENSIBLE = 0xFFFE

FULL_SCALE = 32768.0          # int16 LSB -> normalised float (full scale = 1.0)
CLIP_LEVEL = 32767            # |sample| >= this counts as clipped
ACTIVE_WINDOW_S = 0.1         # window length for the active fraction
ACTIVE_RMS_DBFS = -60.0       # a window is active when its RMS is above this
ACTIVE_POWER = 10.0 ** (ACTIVE_RMS_DBFS / 10.0)
PEAK_MIN = 0.01               # peak (0..1) must be above this
CLIP_MAX_FRACTION = 0.0001    # clipped samples must be below 0.01 % of samples
MAX_CLICKS = 3
MAX_DROPOUTS = 3
CLICK_MIN_STEP = 0.5          # |x[n]-x[n-1]| must exceed this (full scale = 1.0) ...
CLICK_RATIO = 8.0             # ... and this multiple of the local mean |difference|
CLICK_NEIGHBOURHOOD = 256     # samples around n used for the local mean (n itself excluded)
DROPOUT_MIN_FRAMES = 64       # run of exact-zero frames that counts as a dropout
DROPOUT_WINDOW_S = 0.005      # the windows either side of the run must be non-silent
FIRST_N = 5                   # how many click / dropout positions to report
DB_FLOOR = -200.0             # reported instead of -infinity for digital silence


class WavError(Exception):
    """The file cannot be read as 16-bit PCM WAV."""


def _parse_fmt(raw):
    if len(raw) < 16:
        raise WavError("fmt chunk too short")
    tag, channels, rate, _byte_rate, block_align, bits = struct.unpack_from("<HHIIHH", raw, 0)
    if tag == WAVE_FORMAT_EXTENSIBLE:
        if len(raw) < 40:
            raise WavError("WAVE_FORMAT_EXTENSIBLE fmt chunk too short")
        sub_tag = struct.unpack_from("<H", raw, 24)[0]  # first two bytes of the SubFormat GUID
        if sub_tag != WAVE_FORMAT_PCM:
            raise WavError(f"extensible sub-format 0x{sub_tag:04X} is not PCM")
        label = "EXTENSIBLE"
    elif tag == WAVE_FORMAT_PCM:
        label = "PCM"
    else:
        raise WavError(f"format tag 0x{tag:04X} is not PCM")
    if bits != 16:
        raise WavError(f"{bits}-bit samples; only 16-bit is supported")
    if channels < 1 or rate < 1:
        raise WavError(f"bad channel count {channels} or sample rate {rate}")
    return {"format": label, "channels": channels, "rate": rate, "block_align": block_align}


def read_wav(path):
    """Parse the headers, derive the real data length from the file size, return int16 samples."""
    warnings = []
    file_size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise WavError("not a RIFF/WAVE file")
        riff_size = struct.unpack_from("<I", head, 4)[0]
        if riff_size + 8 != file_size:
            warnings.append(f"RIFF header says {riff_size + 8} bytes, file is {file_size} bytes")
        fmt = None
        data_off = None
        data_declared = None
        pos = 12
        while pos + 8 <= file_size:
            f.seek(pos)
            cid, csize = struct.unpack("<4sI", f.read(8))
            body = pos + 8
            if cid == b"fmt ":
                fmt = _parse_fmt(f.read(min(csize, file_size - body)))
            elif cid == b"data":
                if fmt is None:
                    raise WavError("data chunk comes before fmt chunk")
                data_off, data_declared = body, csize
                break
            pos = body + csize + (csize & 1)
        if fmt is None:
            raise WavError("no fmt chunk")
        if data_off is None:
            raise WavError("no data chunk")

        channels = fmt["channels"]
        frame_bytes = channels * 2
        if fmt["block_align"] != frame_bytes:
            warnings.append(f"fmt block align {fmt['block_align']} != channels*2 ({frame_bytes}); using {frame_bytes}")
        avail = file_size - data_off
        if data_declared == 0 or data_declared > avail:
            if data_declared == 0:
                warnings.append(f"data chunk size is 0 in the header; using the {avail} bytes that follow it")
            else:
                warnings.append(f"data chunk size is {data_declared} in the header but only {avail} bytes remain; "
                                f"file is truncated, using {avail}")
            nbytes = avail
        else:
            nbytes = data_declared
        if nbytes % frame_bytes:
            warnings.append(f"{nbytes % frame_bytes} trailing byte(s) do not form a whole frame and are ignored")
        frames = nbytes // frame_bytes
        f.seek(data_off)
        raw = f.read(frames * frame_bytes)
    samples = np.frombuffer(raw, dtype="<i2").reshape(frames, channels)
    return {"samples": samples, "rate": fmt["rate"], "channels": channels, "frames": frames,
            "format": fmt["format"], "warnings": warnings}


def _runs(mask):
    """Start indices and exclusive end indices of each run of True in a 1-D bool array."""
    edges = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    return np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)


def _active_fraction(power, win):
    """Fraction of non-overlapping windows whose RMS is above ACTIVE_RMS_DBFS."""
    n_win = power.size // win
    if n_win == 0:
        return 0.0
    window_power = power[: n_win * win].reshape(n_win, win).mean(axis=1)
    return float(np.count_nonzero(window_power > ACTIVE_POWER)) / n_win


def _click_indices(x):
    """Sample indices where a discontinuity click is detected in one normalised channel."""
    n = x.size
    d = np.zeros(n)
    d[1:] = np.abs(np.diff(x))
    cs = np.concatenate(([0.0], np.cumsum(d)))
    half = CLICK_NEIGHBOURHOOD // 2
    idx = np.arange(n)
    lo = np.maximum(0, idx - half)
    hi = np.minimum(n, idx + half + 1)
    count = hi - lo - 1                       # neighbours, centre sample excluded
    local_sum = cs[hi] - cs[lo] - d
    local_mean = np.where(count > 0, local_sum / np.maximum(count, 1), 0.0)
    return np.flatnonzero((d > CLICK_MIN_STEP) & (d > CLICK_RATIO * local_mean))


def _block(xf, xi, zero_mask, rate):
    """Metrics for a set of columns: xf normalised float (frames, k), xi int16 (frames, k).

    zero_mask marks the frames counted as digital silence (all columns zero).
    """
    win = max(1, round(rate * ACTIVE_WINDOW_S))
    dwin = max(1, round(rate * DROPOUT_WINDOW_S))
    sq = xf * xf
    if xi.size:
        peak = float(np.max(np.abs(xi.astype(np.int32)))) / FULL_SCALE
        mean_sq = float(sq.mean())
        dc = float(xf.mean())
    else:
        peak = mean_sq = dc = 0.0
    rms_dbfs = 10.0 * np.log10(mean_sq) if mean_sq > 0 else DB_FLOOR
    clipped = int(np.count_nonzero((xi >= CLIP_LEVEL) | (xi <= -CLIP_LEVEL)))
    starts, ends = _runs(zero_mask)
    longest = int((ends - starts).max()) if starts.size else 0

    drops = []
    n_frames = zero_mask.size
    for s, e in zip(starts.tolist(), ends.tolist()):
        if e - s < DROPOUT_MIN_FRAMES:
            continue
        before = zero_mask[max(0, s - dwin):s]
        after = zero_mask[e:min(n_frames, e + dwin)]
        if before.size and after.size and not before.all() and not after.all():
            drops.append(s)

    return {
        "peak": peak,
        "rms_dbfs": float(rms_dbfs),
        "active_fraction": _active_fraction(sq.mean(axis=1), win),
        "longest_silence_s": longest / rate,
        "clipped_samples": clipped,
        "dc_offset": dc,
        "dropouts": {"count": len(drops), "first_s": [s / rate for s in drops[:FIRST_N]]},
    }


def analyse(samples, rate):
    """Return (whole-file metrics, list of per-channel metrics) for int16 samples (frames, channels)."""
    xf = samples.astype(np.float64) / FULL_SCALE
    n_channels = samples.shape[1]
    whole = _block(xf, samples, np.all(samples == 0, axis=1), rate)

    per_channel = []
    events = []
    for c in range(n_channels):
        blk = _block(xf[:, c:c + 1], samples[:, c:c + 1], samples[:, c] == 0, rate)
        idx = _click_indices(xf[:, c])
        events.extend((int(i), c) for i in idx)
        blk["clicks"] = {"count": int(idx.size),
                         "first": [{"time_s": float(i) / rate, "channel": c} for i in idx[:FIRST_N]]}
        per_channel.append(blk)

    events.sort()
    whole["clicks"] = {"count": len(events),
                       "first": [{"time_s": i / rate, "channel": c} for i, c in events[:FIRST_N]]}
    return whole, per_channel


def _clean(obj):
    """Make the result JSON-safe: numpy scalars to Python, floats rounded."""
    if isinstance(obj, dict):
        return {k: _clean(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_clean(v) for v in obj]
    if isinstance(obj, np.integer):
        return int(obj)
    if isinstance(obj, (float, np.floating)):
        return round(float(obj), 6)
    return obj


def check_file(path, rate=None, channels=None, min_seconds=10.0, min_active=0.2):
    """Analyse one file and apply the PASS rules. Raises WavError or OSError if unreadable."""
    wav = read_wav(path)
    sr, ch, frames = wav["rate"], wav["channels"], wav["frames"]
    whole, per_channel = analyse(wav["samples"], sr)
    duration = frames / sr
    total = frames * ch
    clipped = whole["clipped_samples"]
    n_clicks = whole["clicks"]["count"]
    n_drop = whole["dropouts"]["count"]

    failed = []
    if rate is not None and sr != rate:
        failed.append(f"sample rate {sr} Hz, expected {rate} Hz")
    if channels is not None and ch != channels:
        failed.append(f"channels {ch}, expected {channels}")
    if duration < min_seconds:
        failed.append(f"duration {duration:.2f} s, need at least {min_seconds:g} s")
    if whole["active_fraction"] < min_active:
        failed.append(f"active fraction {whole['active_fraction']:.3f}, need at least {min_active:g}")
    for c, blk in enumerate(per_channel):
        if blk["active_fraction"] < min_active:
            failed.append(f"channel {c} active fraction {blk['active_fraction']:.3f}, need at least {min_active:g}")
    if whole["peak"] <= PEAK_MIN:
        failed.append(f"peak {whole['peak']:.4f}, need above {PEAK_MIN:g}")
    for c, blk in enumerate(per_channel):
        if blk["peak"] <= PEAK_MIN:
            failed.append(f"channel {c} peak {blk['peak']:.4f}, need above {PEAK_MIN:g}")
    if total and clipped >= CLIP_MAX_FRACTION * total:
        failed.append(f"clipped samples {clipped} ({100.0 * clipped / total:.4f} %), "
                      f"need below {100.0 * CLIP_MAX_FRACTION:g} %")
    if n_clicks > MAX_CLICKS:
        failed.append(f"clicks {n_clicks}, allowed at most {MAX_CLICKS}")
    if n_drop > MAX_DROPOUTS:
        failed.append(f"dropouts {n_drop}, allowed at most {MAX_DROPOUTS}")

    verdict = "FAIL" if failed else "PASS"
    summary = (f"{verdict}  {os.path.basename(path)}  {ch} ch, {sr} Hz, {duration:.2f} s, "
               f"peak {whole['peak']:.4f}, RMS {whole['rms_dbfs']:.1f} dBFS, "
               f"active {whole['active_fraction']:.2f}, clicks {n_clicks}, dropouts {n_drop}, "
               f"clipped {clipped}")
    result = {
        "file": path,
        "verdict": verdict,
        "summary": summary,
        "failed": failed,
        "warnings": wav["warnings"],
        "format": {"tag": wav["format"], "bits": 16},
        "sample_rate": sr,
        "channels": ch,
        "frames": frames,
        "duration_s": duration,
        "expected": {"sample_rate": rate, "channels": channels,
                     "min_seconds": min_seconds, "min_active": min_active},
        "whole": whole,
        "per_channel": per_channel,
    }
    return _clean(result)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Check a 16-bit PCM WAV for real, clean sound.")
    ap.add_argument("file", help="path to the .wav file")
    ap.add_argument("--rate", type=int, help="expected sample rate in Hz (not checked when omitted)")
    ap.add_argument("--channels", type=int, help="expected channel count (not checked when omitted)")
    ap.add_argument("--min-seconds", type=float, default=10.0, help="minimum duration in seconds (default 10)")
    ap.add_argument("--min-active", type=float, default=0.2,
                    help="minimum fraction of 100 ms windows above -60 dBFS (default 0.2)")
    ap.add_argument("--json", action="store_true", help="print the full result as JSON")
    args = ap.parse_args(argv)

    try:
        result = check_file(args.file, args.rate, args.channels, args.min_seconds, args.min_active)
    except (OSError, WavError) as exc:
        print(f"UNREADABLE  {args.file}: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(result["summary"])
        for item in result["failed"]:
            print(f"  failed: {item}")
        for item in result["warnings"]:
            print(f"  warning: {item}")
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
