#!/usr/bin/env python3
"""Reference DSP-ADPCM decoder for the game's CSMP audio assets.

A CSMP file (as extracted into work/survey/paks/.../*.CSMP) is laid out as:
  0x00  'RFRM', u32 size (little-endian in these files), 12 zero bytes
  0x14  form type 'CSMP'
  0x18  chunk 'FMTA', u32 size = 5, 5 payload bytes (channel layout; all 370 files read 00 00 00 00 01)
  ...   16 bytes of unidentified gap (constant across the files)
  0x3D  chunk 'DATA', u32 size (little-endian), payload = the DSP-ADPCM stream
  0x45  DATA payload: 16-byte prefix (00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 in all 370 files),
        then the 0x60-byte Nintendo DSP header at payload+0x10,
        then ADPCM frames of 8 bytes each (14 samples per frame) from payload+0x70.
  (other chunks such as CRMS and RAS3 may follow DATA; they are ignored)

DSP header (little-endian), relative to the header start:
  0x00 u32 sample count, 0x04 u32 nibble count, 0x08 u32 sample rate, 0x0C u16 loop flag,
  0x0E u16 format (0 = ADPCM), 0x10 u32 loop start, 0x14 u32 loop end, 0x18 u32 current address,
  0x1C 16 x s16 coefficients, 0x3C u16 gain, 0x3E u16 initial predictor/scale,
  0x40 s16 history 1, 0x42 s16 history 2, 0x44 u16 loop predictor/scale,
  0x46 s16 loop history 1, 0x48 s16 loop history 2.

Each sample: scale = 1 << (header & 15); c1, c2 = coef[2*idx], coef[2*idx+1] with idx = header >> 4;
  sample = clamp16(((nibble * scale) << 11) + 1024 + c1*h1 + c2*h2) >> 11), h2 = h1, h1 = sample.

Usage:
  csmp_ref.py --src <folder with .CSMP> --out <folder> [--count N] [--seed S]
"""
import argparse
import os
import random
import struct
import sys

import numpy as np

RATES_WANTED = (48000, 44100, 32000, 24000, 22050)
DSP_HEADER_LEN = 0x60
DATA_PREFIX_LEN = 0x10          # bytes between DATA payload start and the DSP header
ADPCM_OFFSET = DATA_PREFIX_LEN + DSP_HEADER_LEN   # 0x70 from DATA payload start
FRAME_BYTES = 8
SAMPLES_PER_FRAME = 14


class CsmpError(ValueError):
    pass


def _find_chunk(b, tag, start, limit):
    i = b.find(tag, start, limit)
    if i < 0:
        raise CsmpError("chunk %r not found before 0x%x" % (tag.decode(), limit))
    size = struct.unpack_from("<I", b, i + 4)[0]
    if i + 8 + size > len(b):
        raise CsmpError("chunk %r at 0x%x overruns the file" % (tag.decode(), i))
    return i, size


def parse_csmp(path):
    """Parse one CSMP file. Raises CsmpError if the header does not parse."""
    with open(path, "rb") as f:
        b = f.read()
    if b[0:4] != b"RFRM":
        raise CsmpError("no RFRM tag")
    form = b.find(b"CSMP", 0, 0x40)
    if form < 0:
        raise CsmpError("no CSMP form type in the first 64 bytes")
    fmta_at, fmta_size = _find_chunk(b, b"FMTA", form + 4, 0x80)
    fmta = b[fmta_at + 8: fmta_at + 8 + fmta_size]
    if fmta_size != 5:
        raise CsmpError("FMTA payload is %d bytes, expected 5" % fmta_size)
    # The channel count and the layout mask (1 front left, 2 front right, 4 centre, 8 LFE, 0x33 quad, 0x3f 5.1) are the two bytes at +0x18
    # of the FMTA chunk, after the 5-byte payload and its padding: 01 04 for a mono centre sample, 02 03 for stereo. (The bytes at +8
    # read the same in every file.)
    channels = b[fmta_at + 0x18]
    layout = b[fmta_at + 0x19]

    data_at, data_size = _find_chunk(b, b"DATA", fmta_at + 8 + fmta_size, len(b))
    payload = b[data_at + 8: data_at + 8 + data_size]
    if len(payload) < ADPCM_OFFSET:
        raise CsmpError("DATA payload (%d bytes) shorter than prefix+header" % len(payload))
    prefix = payload[:DATA_PREFIX_LEN]
    h = payload[DATA_PREFIX_LEN:DATA_PREFIX_LEN + DSP_HEADER_LEN]
    sample_count, nibble_count, rate, loop_flag, fmt, loop_start, loop_end, cur_addr = \
        struct.unpack_from("<IIIHHIII", h, 0)
    coefs = list(struct.unpack_from("<16h", h, 0x1C))
    gain, ps, hist1, hist2, loop_ps, loop_hist1, loop_hist2 = \
        struct.unpack_from("<HHhhHhh", h, 0x3C)
    if fmt != 0:
        raise CsmpError("DSP format %d, expected 0 (ADPCM)" % fmt)
    if sample_count == 0:
        raise CsmpError("sample count is 0")
    frames = -(-sample_count // SAMPLES_PER_FRAME)
    adpcm = payload[ADPCM_OFFSET:]
    if len(adpcm) < frames * FRAME_BYTES:
        raise CsmpError("ADPCM data %d bytes, need %d for %d samples"
                        % (len(adpcm), frames * FRAME_BYTES, sample_count))
    # nibble count: observed as floor(samples*16/14)+1, or exact, within [0, 3) of samples*16/14
    slack = nibble_count - sample_count * 16 / 14
    return {
        "path": path,
        "channels": channels,
        "fmta_layout": layout,
        "rate": rate,
        "sample_count": sample_count,
        "nibble_count": nibble_count,
        "nibble_ok": 0 <= slack < 3,
        "loop_flag": loop_flag,
        "loop_start": loop_start,
        "loop_end": loop_end,
        "cur_addr": cur_addr,
        "coefs": coefs,
        "gain": gain,
        "ps": ps,
        "hist1": hist1,
        "hist2": hist2,
        "loop_ps": loop_ps,
        "loop_hist1": loop_hist1,
        "loop_hist2": loop_hist2,
        "prefix_hex": prefix.hex(),
        "data_size": data_size,
        "data_payload": payload,
        "adpcm_bytes": adpcm,
    }


def decode_dsp_adpcm(adpcm_bytes, coefs, sample_count, hist1=0, hist2=0):
    """Decode sample_count samples of DSP-ADPCM. Returns numpy int16 array."""
    frames = -(-sample_count // SAMPLES_PER_FRAME)
    if len(adpcm_bytes) < frames * FRAME_BYTES:
        raise CsmpError("need %d bytes of ADPCM, have %d" % (frames * FRAME_BYTES, len(adpcm_bytes)))
    # The header holds 8 coefficient pairs (16 values). idx = header >> 4 can reach 15, so pairs
    # 8..15 point past the table. Those reads are given the value 0 (same as the runtime port's
    # GetCoefficientAtIndex); the CLI reports how many frames need this.
    c = [int(x) for x in coefs] + [0] * 16
    out = []
    append = out.append
    h1, h2 = int(hist1), int(hist2)
    for f in range(frames):
        base = f * FRAME_BYTES
        hdr = adpcm_bytes[base]
        scale = 1 << (hdr & 15)
        idx = hdr >> 4
        c1 = c[2 * idx]
        c2 = c[2 * idx + 1]
        for j in range(1, FRAME_BYTES):
            byte = adpcm_bytes[base + j]
            for nib in (byte >> 4, byte & 15):
                if nib >= 8:
                    nib -= 16
                v = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2
                s = v >> 11
                if s > 32767:
                    s = 32767
                elif s < -32768:
                    s = -32768
                append(s)
                h2 = h1
                h1 = s
    return np.array(out[:sample_count], dtype="<i2")


def count_out_of_table_frames(adpcm_bytes, sample_count):
    """Frames whose header selects coefficient pair 8..15 (outside the 16-value table)."""
    frames = -(-sample_count // SAMPLES_PER_FRAME)
    return sum(1 for f in range(frames) if adpcm_bytes[f * FRAME_BYTES] >> 4 >= 8)


def _pick(candidates, n, seed):
    """Deterministic spread: quotas per rate, then one random file per size stratum."""
    rng = random.Random(seed)
    by_rate = {}
    for c in candidates:
        by_rate.setdefault(c["rate"], []).append(c)
    rates = [r for r in RATES_WANTED if r in by_rate]
    total = sum(len(by_rate[r]) for r in rates)
    if n >= total:
        return sorted(candidates, key=lambda c: c["name"])
    quota = {r: min(len(by_rate[r]), max(1, round(n * len(by_rate[r]) / total))) for r in rates}
    while sum(quota.values()) > n:
        r = max((r for r in rates if quota[r] > 1), key=lambda r: quota[r], default=None)
        if r is None:
            break
        quota[r] -= 1
    while sum(quota.values()) < n:
        r = max((r for r in rates if quota[r] < len(by_rate[r])),
                key=lambda r: len(by_rate[r]) - quota[r], default=None)
        if r is None:
            break
        quota[r] += 1
    chosen = []
    for r in rates:
        group = sorted(by_rate[r], key=lambda c: (c["size"], c["name"]))
        q, m = quota[r], len(group)
        for k in range(q):
            lo = k * m // q
            hi = max(lo + 1, (k + 1) * m // q)
            chosen.append(group[rng.randrange(lo, hi)])
    return sorted(chosen, key=lambda c: c["name"])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="folder with .CSMP files")
    ap.add_argument("--out", required=True, help="output folder")
    ap.add_argument("--count", type=int, default=60)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args(argv)

    names = sorted(n for n in os.listdir(args.src) if n.upper().endswith(".CSMP"))
    parsed, failed = [], []
    for n in names:
        p = os.path.join(args.src, n)
        try:
            info = parse_csmp(p)
        except CsmpError as e:
            failed.append((n, str(e)))
            continue
        info["name"] = os.path.splitext(n)[0]
        info["size"] = os.path.getsize(p)
        parsed.append(info)

    multi = [c for c in parsed if c["channels"] != 1]
    mono = [c for c in parsed if c["channels"] == 1]
    other_rate = [c for c in mono if c["rate"] not in RATES_WANTED]
    cands = [c for c in mono if c["rate"] in RATES_WANTED]
    bad_nib = [c["name"] for c in parsed if not c["nibble_ok"]]
    oob = {c["name"]: count_out_of_table_frames(c["adpcm_bytes"], c["sample_count"]) for c in mono}

    chosen = _pick(cands, args.count, args.seed)
    os.makedirs(args.out, exist_ok=True)
    ids = []
    for c in chosen:
        base = os.path.join(args.out, c["name"])
        with open(base + ".csmp", "wb") as f:
            f.write(c["data_payload"])
        with open(base + ".info", "w", encoding="ascii") as f:
            vals = [c["sample_count"], c["rate"], c["loop_flag"]] + c["coefs"] + \
                   [c["ps"], c["hist1"], c["hist2"]]
            f.write(" ".join(str(v) for v in vals) + "\n")
        pcm = decode_dsp_adpcm(c["adpcm_bytes"], c["coefs"], c["sample_count"], c["hist1"], c["hist2"])
        with open(base + ".ref.pcm", "wb") as f:
            f.write(pcm.tobytes())
        ids.append(c["name"])
    with open(os.path.join(args.out, "index.txt"), "w", encoding="ascii") as f:
        f.write("\n".join(ids) + "\n")

    rates = {}
    for c in chosen:
        rates[c["rate"]] = rates.get(c["rate"], 0) + 1
    print("scanned %d .CSMP files in %s" % (len(names), args.src))
    print("header parse failures: %d %s" % (len(failed), failed[:10]))
    print("skipped multichannel (channels != 1): %d" % len(multi))
    print("mono at rates outside the spread list: %d %s" % (
        len(other_rate), sorted({c["rate"] for c in other_rate})))
    print("nibble count outside expected slack: %d" % len(bad_nib))
    print("selected %d files, rates %s" % (len(chosen), dict(sorted(rates.items(), reverse=True))))
    print("files with out-of-table frames (pair 8..15): %d of %d mono; selected: %d"
          % (sum(1 for v in oob.values() if v), len(mono), sum(1 for c in chosen if oob[c["name"]])))
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
