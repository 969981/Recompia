#!/usr/bin/env python3
"""Compare two raw s16le interleaved PCM files, channel by channel.

Usage: compare_pcm.py A.pcm B.pcm --channels 6 --rate 48000 [--max-lag 4096] [--min-corr 0.99]

Lag convention: lag d means B[i + d] lines up with A[i]. A file that is B delayed by 100
samples gives lag +100.
Levels are in int16 units; a channel below 4 LSB RMS counts as silent (the noise floor of the decoders).
Correlation uses the loudest 10 seconds of each channel (by file A), FFT based, normalised over the overlap.
RMS and the silence test use the whole file.

Exit 0 = PASS, 1 = FAIL, 2 = unreadable input.
"""
import argparse
import os
import sys

import numpy as np

SEG_SECONDS = 10.0
SILENT_RMS = 4.0   # a few LSB: the noise floor, where two decoders round differently
MAX_LEN_DIFF = 3000
MAX_LAG_SPREAD = 2


def load(path, channels):
    size = os.path.getsize(path)
    if size % (2 * channels):
        raise ValueError("%s: %d bytes is not a whole number of %d-channel s16 frames"
                         % (path, size, channels))
    raw = np.fromfile(path, dtype="<i2")
    return raw.reshape(-1, channels).astype(np.float64)


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if x.size else 0.0


def best_lag(a, b, max_lag):
    """a, b: equal-length 1-D float arrays. Returns (lag, corr) with B[i+lag] best matching A[i]."""
    n = a.size
    m = min(max_lag, n - 1)
    nfft = 1
    while nfft < 2 * n:
        nfft *= 2
    cross = np.fft.irfft(np.conj(np.fft.rfft(a, nfft)) * np.fft.rfft(b, nfft), nfft)
    lags = np.arange(-m, m + 1)
    num = cross[lags % nfft]  # index k >= 0 holds lag k, index nfft+k holds lag k < 0

    ca = np.concatenate(([0.0], np.cumsum(a * a)))
    cb = np.concatenate(([0.0], np.cumsum(b * b)))
    pos = lags >= 0
    ea = np.where(pos, ca[np.clip(n - lags, 0, n)], ca[n] - ca[np.clip(-lags, 0, n)])
    eb = np.where(pos, cb[n] - cb[np.clip(lags, 0, n)], cb[np.clip(n + lags, 0, n)])
    den = np.sqrt(ea * eb)
    with np.errstate(divide="ignore", invalid="ignore"):
        corr = np.where(den > 0, num / den, np.nan)
    if np.all(np.isnan(corr)):
        return 0, float("nan")
    k = int(np.nanargmax(corr))
    return int(lags[k]), float(corr[k])


def rel_error(a, b, lag):
    """RMS(A - B shifted by lag) / RMS(A), over the overlap."""
    n = a.size
    if lag >= 0:
        sa, sb = a[:n - lag], b[lag:]
    else:
        e = -lag
        sa, sb = a[e:], b[:n - e]
    ra = rms(sa)
    if ra == 0.0:
        return float("nan")
    return rms(sa - sb) / ra


def fmt(x, spec):
    return "n/a" if x != x else format(x, spec)


def compare(path_a, path_b, channels=6, rate=48000, max_lag=4096, min_corr=0.99):
    """Returns dict: passed, lines (report text), failures (list of strings), rows (per channel)."""
    A = load(path_a, channels)
    B = load(path_b, channels)
    nA, nB = A.shape[0], B.shape[0]
    failures = []
    lines = [
        "A: %s" % path_a,
        "B: %s" % path_b,
        "length A: %d samples (%.3f s)" % (nA, nA / rate),
        "length B: %d samples (%.3f s)" % (nB, nB / rate),
    ]
    if abs(nA - nB) > MAX_LEN_DIFF:
        failures.append("length difference %d samples exceeds %d (A=%d, B=%d)"
                        % (abs(nA - nB), MAX_LEN_DIFF, nA, nB))

    n = min(nA, nB)
    seg = min(int(SEG_SECONDS * rate), n)
    if seg < 2:
        failures.append("too short to correlate (%d samples)" % n)
        seg = 0

    rows = []
    active_lags = []
    lines.append("")
    lines.append("%3s %12s %12s %7s %10s %10s  %s" % ("ch", "rms A", "rms B", "lag", "corr", "rel err", "status"))
    for c in range(channels):
        ra_full = rms(A[:, c])
        rb_full = rms(B[:, c])
        a_act = ra_full >= SILENT_RMS
        b_act = rb_full >= SILENT_RMS
        if not (a_act or b_act):
            status = "silent"
            rows.append((c, ra_full, rb_full, None, None, None, status))
        elif not (a_act and b_act):
            status = "FAIL one-sided"
            failures.append("ch %d: active in only one file (rms A=%.2f, rms B=%.2f)"
                            % (c, ra_full, rb_full))
            rows.append((c, ra_full, rb_full, None, None, None, status))
        else:
            e = np.concatenate(([0.0], np.cumsum(A[:n, c] ** 2)))
            start = int(np.argmax(e[seg:] - e[:n + 1 - seg])) if n > seg else 0   # loudest window
            a = A[start:start + seg, c]
            b = B[start:start + seg, c]
            lag, corr = best_lag(a, b, max_lag)
            rel = rel_error(a, b, lag)
            ok = corr >= min_corr  # NaN compares False, so it fails
            status = "PASS" if ok else "FAIL corr"
            if not ok:
                failures.append("ch %d: corr %s below min-corr %.4f" % (c, fmt(corr, ".6f"), min_corr))
            active_lags.append((c, lag))
            rows.append((c, ra_full, rb_full, lag, corr, rel, status))
        r = rows[-1]
        lag_s = "-" if r[3] is None else "%d" % r[3]
        corr_s = "-" if r[4] is None else fmt(r[4], ".6f")
        rel_s = "-" if r[5] is None else fmt(r[5], ".4f")
        lines.append("%3d %12.2f %12.2f %7s %10s %10s  %s" % (c, r[1], r[2], lag_s, corr_s, rel_s, status))

    if active_lags:
        lags = [l for _, l in active_lags]
        spread = max(lags) - min(lags)
        if spread > MAX_LAG_SPREAD:
            failures.append("lags differ across active channels by %d samples (max %d): %s"
                            % (spread, MAX_LAG_SPREAD,
                               ", ".join("ch%d=%d" % cl for cl in active_lags)))
        lines.append("")
        lines.append("active channels: %d, lag spread: %d samples" % (len(active_lags), spread))
    else:
        lines.append("")
        lines.append("no active channels (all silent): lag and corr checks not applicable")

    passed = not failures
    return {"passed": passed, "lines": lines, "failures": failures, "rows": rows}


def main(argv=None):
    ap = argparse.ArgumentParser(description="Channel-wise comparison of two s16le PCM files.")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--channels", type=int, default=6)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--max-lag", type=int, default=4096)
    ap.add_argument("--min-corr", type=float, default=0.99)
    args = ap.parse_args(argv)
    try:
        res = compare(args.a, args.b, args.channels, args.rate, args.max_lag, args.min_corr)
    except (OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    print("\n".join(res["lines"]))
    print("")
    print("RESULT: PASS" if res["passed"] else "RESULT: FAIL")
    for f in res["failures"]:
        print("  - " + f)
    return 0 if res["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
