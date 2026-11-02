"""Survey every FMV0 movie asset: extract with retrotool, probe with ffprobe, write movies.tsv.

Run from the project folder: tools/.venv/Scripts/python.exe tools/survey/movies.py
"""
import collections
import glob
import json
import os
import shutil
import struct
import subprocess
import sys

def _project_root():
    """The project folder: NXRT_ROOT, else the nearest parent of this file that holds both tools/ and work/."""
    import os, pathlib
    env = os.environ.get('NXRT_ROOT')
    if env:
        return pathlib.Path(env).as_posix()
    here = pathlib.Path(__file__).resolve()
    for p in here.parents:
        if (p / 'tools').is_dir() and (p / 'work').is_dir():
            return p.as_posix()
    return here.parents[2].as_posix()


ROOT = _project_root()
PAKS = ROOT + "/work/survey/paks"
OUT = ROOT + "/work/survey/fmv"
TSV = OUT + "/movies.tsv"
RETROTOOL = ROOT + "/toolchain/retrotool/retrotool.exe"

# The Gyan 8.1.2 build (WinGet) is preferred; the PATH entry resolves to an ancient 2013 build.
FFPROBE_CANDIDATES = [
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Links", "ffprobe.exe"),
    shutil.which("ffprobe") or "",
]

COLS = [
    "asset_file", "pak_folder", "fmv0_bytes", "extracted_file", "extracted_bytes",
    "container_ext", "format_name", "duration_s", "overall_bit_rate",
    "video_streams", "v_codec", "v_profile", "v_level", "v_width", "v_height",
    "r_frame_rate", "avg_frame_rate", "pix_fmt", "has_b_frames", "refs", "v_bit_rate",
    "color_range", "color_space", "color_transfer",
    "audio_track_count", "audio_detail", "subtitle_streams", "data_streams", "error",
]


def find_ffprobe():
    for c in FFPROBE_CANDIDATES:
        if c and os.path.isfile(c):
            return c
    raise SystemExit("no ffprobe found")


FFPROBE = find_ffprobe()


def detect_ext(path):
    with open(path, "rb") as f:
        head = f.read(64)
    if len(head) >= 12 and head[4:8] == b"ftyp":
        return "mov" if head[8:12] == b"qt  " else "mp4"
    if head[:4] == b"\x1a\x45\xdf\xa3":
        return "webm" if b"webm" in head else "mkv"
    if head[:4] == b"RIFF" and head[8:12] == b"AVI ":
        return "avi"
    if head[:4] == b"OggS":
        return "ogg"
    return "bin"


def extract(src, stem):
    tmp = os.path.join(OUT, stem + ".tmp.bin")
    r = subprocess.run([RETROTOOL, "fmv0", "extract", src, tmp], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(tmp) or os.path.getsize(tmp) == 0:
        if os.path.isfile(tmp):
            os.remove(tmp)
        raise RuntimeError("retrotool rc=%d: %s %s" % (r.returncode, r.stdout.strip(), r.stderr.strip()))
    ext = detect_ext(tmp)
    dest = os.path.join(OUT, stem + "." + ext)
    if os.path.exists(dest):
        os.remove(dest)
    os.replace(tmp, dest)
    return dest, ext


def probe(path):
    r = subprocess.run(
        [FFPROBE, "-v", "error", "-show_format", "-show_streams", "-of", "json", path],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if r.returncode != 0:
        raise RuntimeError("ffprobe rc=%d: %s" % (r.returncode, r.stderr.strip()))
    return json.loads(r.stdout)


def clean(v):
    return "" if v is None else str(v).replace("\t", " ").replace("\n", " ").replace("\r", " ")


def row_for(src, pak_folder):
    row = {c: "" for c in COLS}
    stem = os.path.splitext(os.path.basename(src))[0]
    row["asset_file"] = os.path.basename(src)
    row["pak_folder"] = pak_folder
    row["fmv0_bytes"] = os.path.getsize(src)
    try:
        dest, ext = extract(src, stem)
        row["extracted_file"] = os.path.basename(dest)
        row["extracted_bytes"] = os.path.getsize(dest)
        row["container_ext"] = ext
    except Exception as e:
        row["error"] = "extract: " + str(e)
        return row
    try:
        j = probe(dest)
        fmt = j.get("format", {})
        row["format_name"] = fmt.get("format_name", "")
        row["duration_s"] = fmt.get("duration", "")
        row["overall_bit_rate"] = fmt.get("bit_rate", "")
        streams = j.get("streams", [])
        vids = [s for s in streams if s.get("codec_type") == "video"]
        auds = [s for s in streams if s.get("codec_type") == "audio"]
        subs = [s for s in streams if s.get("codec_type") == "subtitle"]
        datas = [s for s in streams if s.get("codec_type") == "data"]
        row["video_streams"] = len(vids)
        if vids:
            v = vids[0]
            row["v_codec"] = v.get("codec_name", "")
            row["v_profile"] = v.get("profile", "")
            row["v_level"] = v.get("level", "")
            row["v_width"] = v.get("width", "")
            row["v_height"] = v.get("height", "")
            row["r_frame_rate"] = v.get("r_frame_rate", "")
            row["avg_frame_rate"] = v.get("avg_frame_rate", "")
            row["pix_fmt"] = v.get("pix_fmt", "")
            row["has_b_frames"] = v.get("has_b_frames", "")
            row["refs"] = v.get("refs", "")
            row["v_bit_rate"] = v.get("bit_rate", "")
            row["color_range"] = v.get("color_range", "")
            row["color_space"] = v.get("color_space", "")
            row["color_transfer"] = v.get("color_transfer", "")
        row["audio_track_count"] = len(auds)
        row["audio_detail"] = "; ".join(
            "%s/%s/ch=%s/sr=%s/br=%s" % (
                a.get("codec_name", ""), a.get("profile", ""), a.get("channels", ""),
                a.get("sample_rate", ""), a.get("bit_rate", ""),
            ) for a in auds
        )
        row["subtitle_streams"] = len(subs)
        row["data_streams"] = len(datas)
    except Exception as e:
        row["error"] = "probe: " + str(e)
    return row


def audio_key(row):
    # Derived from audio_detail, codec + channels + sample rate only.
    if not row["audio_detail"]:
        return "none"
    parts = []
    for item in row["audio_detail"].split("; "):
        codec, _profile, ch, sr, _br = item.split("/")
        parts.append("%s/%sch/%sHz" % (codec, ch.split("=")[1], sr.split("=")[1]))
    return "; ".join(parts)


def mp4_top_boxes(path, limit=40):
    out = []
    total = os.path.getsize(path)
    with open(path, "rb") as f:
        off = 0
        while off + 8 <= total and len(out) < limit:
            f.seek(off)
            size, typ = struct.unpack(">I4s", f.read(8))
            hl = 8
            if size == 1:
                size = struct.unpack(">Q", f.read(8))[0]
                hl = 16
            elif size == 0:
                size = total - off
            if size < hl:
                out.append((typ.decode("latin1"), off, size, "bad size"))
                break
            out.append((typ.decode("latin1"), off, size))
            off += size
    return out


def hexdump(data, base=0):
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hx = " ".join("%02x" % b for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print("%08x  %-47s  %s" % (base + i, hx, asc))


def print_header_report(srcs):
    print("\n== FMV0 header: first 64 bytes of 2 files")
    for s in srcs[:2]:
        with open(s, "rb") as f:
            head = f.read(64)
        print(os.path.basename(s), "\n  " + head.hex(" "))
    print("\n== hexdump 0..128 of", os.path.basename(srcs[0]))
    with open(srcs[0], "rb") as f:
        hexdump(f.read(128))
    print("\n== RFRM/FMV0/FOOT/AINF tag offsets in", os.path.basename(srcs[0]))
    with open(srcs[0], "rb") as f:
        blob = f.read()
    for tag in (b"RFRM", b"FMV0", b"FOOT", b"AINF"):
        offs = []
        i = blob.find(tag)
        while i != -1 and len(offs) < 8:
            offs.append(i)
            i = blob.find(tag, i + 1)
        print("  %s: %s" % (tag.decode(), offs))
    print("\n== top-level MP4 boxes in extracted video of", os.path.basename(srcs[0]))
    ex = [p for p in glob.glob(OUT + "/" + os.path.splitext(os.path.basename(srcs[0]))[0] + ".*")
          if not p.endswith(".tmp.bin")]
    if ex:
        for b in mp4_top_boxes(ex[0]):
            print("  ", b)


def main():
    os.makedirs(OUT, exist_ok=True)
    srcs = sorted(glob.glob(PAKS + "/**/*.FMV0", recursive=True))
    srcs = sorted(set(srcs + glob.glob(PAKS + "/**/*.fmv0", recursive=True)))
    print("found %d FMV0 files" % len(srcs))
    print("ffprobe:", FFPROBE)
    rows = []
    for s in srcs:
        rel_dir = os.path.relpath(os.path.dirname(s), PAKS).replace("\\", "/")
        r = row_for(s, rel_dir)
        rows.append(r)
        status = "ERR " + r["error"] if r["error"] else "ok"
        print("%-45s %s" % (r["asset_file"], status))
    with open(TSV, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(COLS) + "\n")
        for r in rows:
            f.write("\t".join(clean(r[c]) for c in COLS) + "\n")
    print("wrote %s (%d rows)" % (TSV, len(rows)))

    if srcs:
        print_header_report(srcs)

    # Summary
    ok = [r for r in rows if not r["error"]]
    print("\n== summary: %d rows, %d with errors" % (len(rows), len(rows) - len(ok)))
    combos = collections.Counter()
    for r in ok:
        key = (
            r["format_name"],
            "%s %s L%s" % (r["v_codec"], r["v_profile"], r["v_level"]),
            "%sx%s" % (r["v_width"], r["v_height"]),
            r["r_frame_rate"],
            audio_key(r),
        )
        combos[key] += 1
    print("distinct combinations (container | video codec profile level | WxH | r_fps | audio codec/ch/Hz):", len(combos))
    for key, n in combos.most_common():
        print("  %3d  %s" % (n, " | ".join(key)))

    def stats(vals):
        vals = [float(v) for v in vals if v not in ("", None)]
        if not vals:
            return "n/a"
        return "min=%g max=%g sum=%g" % (min(vals), max(vals), sum(vals))

    print("\nduration_s:", stats([r["duration_s"] for r in ok]))
    print("fmv0_bytes:", stats([r["fmv0_bytes"] for r in rows]))
    print("extracted_bytes:", stats([r["extracted_bytes"] for r in ok]))
    multi = [r["asset_file"] for r in ok if int(r["audio_track_count"] or 0) > 1]
    bframes = [(r["asset_file"], r["has_b_frames"]) for r in ok if int(r["has_b_frames"] or 0) > 0]
    print("movies with >1 audio track:", len(multi), multi)
    print("movies with has_b_frames>0:", len(bframes), "of", len(ok))
    print("video stream count > 1:", [r["asset_file"] for r in ok if int(r["video_streams"] or 0) != 1])
    for r in rows:
        if r["error"]:
            print("FAILED:", r["asset_file"], r["error"])


if __name__ == "__main__":
    sys.exit(main())
