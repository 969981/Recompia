"""Extract every FMV0 movie from the RomFS paks and write <out>\\<asset-id>.mp4 plus <out>\\manifest.json.

Run:
  tools\\.venv\\Scripts\\python tools\\media\\fmv_manifest.py --romfs game\\romfs --out <dir>

Steps: read each pak's ADIR (directory only) to find FMV0 assets; extract just the paks that hold a
movie still missing or stale, with retrotool pak extract, into <out>\\_paks; pull each movie out with
retrotool fmv0 extract; probe the MP4 with ffprobe; write manifest.json. A movie whose MP4 exists and
whose size matches the previous manifest is reused without extracting or probing again.

Exit codes: 0 ok, 1 extraction or probe failure, 2 RomFS lacks the expected paks (or has no movies).
"""
import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import uuid

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RETROTOOL = os.path.join(ROOT, "toolchain", "retrotool", "retrotool.exe")
EXPECTED_PAK = os.path.join("UI", "MPR1", "UIMP1.pak")
EXPECTED_FRONTEND = os.path.join("Worlds", "FrontEnd", "*.pak")


def find_tool(name):
    # The Gyan 8.1.2 build (WinGet Links) is preferred; the PATH entry resolves to an ancient 2013 build.
    cands = [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Links", name + ".exe"),
        shutil.which(name) or "",
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def read_adir(path):
    """Return [(type, asset_id, decompressed_size)] from the PACK's ADIR chunk. Reads only the header and TOC."""
    with open(path, "rb") as f:
        hdr = f.read(64)
        if len(hdr) < 64 or hdr[:4] != b"RFRM" or hdr[20:24] != b"PACK" or hdr[32:36] != b"RFRM" or hdr[52:56] != b"TOCC":
            raise ValueError("not a PACK file")
        tocc_size = struct.unpack_from("<Q", hdr, 36)[0]
        toc = f.read(tocc_size)
    off = 0
    while off + 24 <= len(toc):
        cid = toc[off:off + 4]
        size = struct.unpack_from("<Q", toc, off + 4)[0]
        skip = struct.unpack_from("<Q", toc, off + 16)[0]
        body = toc[off + 24 + skip: off + 24 + skip + size]
        if cid == b"ADIR":
            n = struct.unpack_from("<I", body, 0)[0]
            entries = []
            for i in range(n):
                e = body[4 + i * 52: 4 + (i + 1) * 52]
                t = e[:4].decode("latin1")
                aid = str(uuid.UUID(bytes_le=e[4:20]))
                _offset, decomp, _stored = struct.unpack_from("<QQQ", e, 28)
                entries.append((t, aid, decomp))
            return entries
        off += 24 + skip + size
    return []


def missing_expected(romfs):
    miss = []
    if not os.path.isfile(os.path.join(romfs, EXPECTED_PAK)):
        miss.append(EXPECTED_PAK.replace(os.sep, "/"))
    if not glob.glob(os.path.join(romfs, EXPECTED_FRONTEND)):
        miss.append(EXPECTED_FRONTEND.replace(os.sep, "/"))
    return miss


def scan_fmv0(romfs):
    """Every FMV0 asset in every pak under romfs: [{id, pak, adir_bytes}]."""
    movies = []
    for dp, _dirs, files in os.walk(romfs):
        for fn in sorted(files):
            if not fn.endswith(".pak"):
                continue
            full = os.path.join(dp, fn)
            rel = os.path.relpath(full, romfs).replace(os.sep, "/")
            try:
                adir = read_adir(full)
            except (ValueError, OSError, struct.error) as e:
                print("WARN skipping %s: %s" % (rel, e))
                continue
            for t, aid, decomp in adir:
                if t == "FMV0":
                    movies.append({"id": aid, "pak": rel, "adir_bytes": decomp})
    return movies


def extract_pak(romfs, rel, pak_root):
    src = os.path.join(romfs, *rel.split("/"))
    dst = os.path.join(pak_root, *os.path.splitext(rel)[0].split("/"))
    os.makedirs(dst, exist_ok=True)
    r = subprocess.run([RETROTOOL, "pak", "extract", src, dst], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("retrotool pak extract %s rc=%d: %s" % (rel, r.returncode, r.stderr.strip()))
    return dst


def extract_movie(fmv_path, mp4_path):
    tmp = mp4_path + ".tmp"
    r = subprocess.run([RETROTOOL, "fmv0", "extract", fmv_path, tmp], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0 or not os.path.isfile(tmp) or os.path.getsize(tmp) == 0:
        if os.path.isfile(tmp):
            os.remove(tmp)
        raise RuntimeError("retrotool fmv0 extract rc=%d: %s %s" % (r.returncode, r.stdout.strip(), r.stderr.strip()))
    with open(tmp, "rb") as f:
        head = f.read(12)
    if head[4:8] != b"ftyp":
        os.remove(tmp)
        raise RuntimeError("%s is not an MP4 (no ftyp box)" % os.path.basename(fmv_path))
    os.replace(tmp, mp4_path)


def ffprobe_out(ffprobe, args, path):
    r = subprocess.run([ffprobe, "-v", "error"] + args + [path], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("ffprobe rc=%d: %s" % (r.returncode, r.stderr.strip()))
    return r.stdout


def _lines(out):
    # csv=p=0 output: one value per line, CRLF on Windows, and some packet rows carry a trailing comma.
    vals = []
    for ln in out.splitlines():
        fields = [x for x in ln.strip().split(",") if x.strip()]
        if fields:
            vals.append(fields[-1].strip())
    return vals


def probe_mp4(path, ffprobe=None):
    """Video and audio facts of one MP4, as the manifest stores them."""
    ffprobe = ffprobe or find_tool("ffprobe")
    if not ffprobe:
        raise RuntimeError("no ffprobe found")
    j = json.loads(ffprobe_out(ffprobe, ["-show_format", "-show_streams", "-of", "json"], path))
    streams = j.get("streams", [])
    vids = [s for s in streams if s.get("codec_type") == "video"]
    auds = [s for s in streams if s.get("codec_type") == "audio"]
    if not vids:
        raise RuntimeError("no video stream in %s" % path)
    v = vids[0]
    fps_num, fps_den = (int(x) for x in v["r_frame_rate"].split("/"))
    frames = int(_lines(ffprobe_out(ffprobe, ["-count_packets", "-select_streams", "v:0",
                                             "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0"], path))[0])
    pts = sorted(round(float(x) * 1e6) for x in
                 _lines(ffprobe_out(ffprobe, ["-select_streams", "v:0", "-show_entries", "packet=pts_time",
                                             "-of", "csv=p=0"], path)))
    vdur = v.get("duration") or j.get("format", {}).get("duration")
    video = {
        "width": int(v["width"]), "height": int(v["height"]),
        "fps_num": fps_num, "fps_den": fps_den,
        "frame_count": frames, "pts_us": pts,
        "duration_us": round(float(vdur) * 1e6),
    }
    audio = None
    if auds:
        a = auds[0]
        plines = _lines(ffprobe_out(ffprobe, ["-count_packets", "-select_streams", "a:0",
                                             "-show_entries", "packet=duration_time", "-of", "csv=p=0"], path))
        audio = {
            "channels": int(a["channels"]), "sample_rate": int(a["sample_rate"]),
            "codec": a["codec_name"], "duration_us": round(sum(float(x) for x in plines) * 1e6),
            "packet_count": len(plines),
        }
    return {"video": video, "audio": audio}


def build_movie_entry(mp4_path, file_name, pak, container_bytes, ffprobe=None, adir_bytes=None):
    """One manifest.json movie record for an MP4 already on disk.

    container_bytes is the size of the FMV0 file as retrotool extracts it. adir_bytes is the size the pak
    directory (ADIR) states for it; the two differ by a fixed 84 bytes, so only adir_bytes is used for reuse checks.
    """
    entry = {"file": file_name, "pak": pak, "container_bytes": container_bytes,
             "adir_bytes": adir_bytes, "mp4_bytes": os.path.getsize(mp4_path)}
    entry.update(probe_mp4(mp4_path, ffprobe))
    return entry


def load_prior(manifest_path):
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    return {m["file"]: m for m in data.get("movies", []) if isinstance(m, dict) and "file" in m}


def reusable(prior, mp4_path, movie):
    return (prior is not None and "video" in prior and os.path.isfile(mp4_path)
            and os.path.getsize(mp4_path) == prior.get("mp4_bytes")
            and prior.get("adir_bytes") == movie["adir_bytes"]
            and prior.get("pak") == movie["pak"])


def run(romfs, out):
    ffprobe = find_tool("ffprobe")
    if not ffprobe:
        raise RuntimeError("no ffprobe found")
    if not os.path.isfile(RETROTOOL):
        raise RuntimeError("retrotool not found at %s" % RETROTOOL)
    os.makedirs(out, exist_ok=True)

    movies = scan_fmv0(romfs)
    if not movies:
        raise LookupError("no FMV0 assets in any pak under %s" % romfs)
    seen = {}
    for m in movies:
        if m["id"] in seen:
            raise RuntimeError("movie %s is in both %s and %s" % (m["id"], seen[m["id"]], m["pak"]))
        seen[m["id"]] = m["pak"]
    movies.sort(key=lambda m: m["id"] + ".mp4")

    manifest_path = os.path.join(out, "manifest.json")
    prior = load_prior(manifest_path)
    entries = {}
    todo = []
    for m in movies:
        name = m["id"] + ".mp4"
        mp4 = os.path.join(out, name)
        if reusable(prior.get(name), mp4, m):
            entries[name] = dict(prior[name])
        else:
            todo.append(m)
    print("movies: %d in RomFS, %d up to date, %d to extract" % (len(movies), len(movies) - len(todo), len(todo)))

    if todo:
        pak_root = os.path.join(out, "_paks")
        try:
            if os.path.isdir(pak_root):
                shutil.rmtree(pak_root)
            for rel in sorted({m["pak"] for m in todo}):
                print("extracting pak %s" % rel)
                extract_pak(romfs, rel, pak_root)
            for m in todo:
                name = m["id"] + ".mp4"
                mp4 = os.path.join(out, name)
                fmv = os.path.join(pak_root, *os.path.splitext(m["pak"])[0].split("/"), m["id"] + ".FMV0")
                if not os.path.isfile(fmv):
                    raise RuntimeError("extracted pak has no %s" % fmv)
                extract_movie(fmv, mp4)
                entries[name] = build_movie_entry(mp4, name, m["pak"], os.path.getsize(fmv), ffprobe,
                                                  adir_bytes=m["adir_bytes"])
                print("  %s  %s" % (name, m["pak"]))
        finally:
            if os.path.isdir(pak_root):
                shutil.rmtree(pak_root)

    ordered = [entries[m["id"] + ".mp4"] for m in movies]
    manifest = {"count": len(ordered), "movies": ordered}
    tmp = manifest_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    os.replace(tmp, manifest_path)
    return manifest


def print_summary(manifest):
    movies = manifest["movies"]
    with_audio = [m for m in movies if m["audio"]]
    frames = sum(m["video"]["frame_count"] for m in movies)
    vdur = sum(m["video"]["duration_us"] for m in movies)
    print("\nmovies: %d, with audio: %d" % (len(movies), len(with_audio)))
    print("total video frames: %d" % frames)
    print("total video duration: %.3f s" % (vdur / 1e6))
    for m in movies:
        v, a = m["video"], m["audio"]
        aud = "none" if a is None else "%s %dch %dHz" % (a["codec"], a["channels"], a["sample_rate"])
        print("  %-42s %-28s %dx%d %d/%d frames=%-5d %8.3f s  audio=%s" % (
            m["file"], m["pak"], v["width"], v["height"], v["fps_num"], v["fps_den"],
            v["frame_count"], v["duration_us"] / 1e6, aud))
        if len(v["pts_us"]) != v["frame_count"]:
            print("    WARN pts count %d != frame_count %d" % (len(v["pts_us"]), v["frame_count"]))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract FMV0 movies from the RomFS and write manifest.json.")
    ap.add_argument("--romfs", required=True, help="RomFS root, e.g. game\\romfs")
    ap.add_argument("--out", required=True, help="output folder for <id>.mp4 and manifest.json")
    a = ap.parse_args(argv)
    romfs = os.path.abspath(a.romfs)
    out = os.path.abspath(a.out)
    t0 = time.time()
    miss = missing_expected(romfs)
    if miss:
        print("RomFS at %s lacks expected paks: %s" % (romfs, ", ".join(miss)), file=sys.stderr)
        return 2
    try:
        manifest = run(romfs, out)
    except LookupError as e:
        print(str(e), file=sys.stderr)
        return 2
    except (RuntimeError, OSError, ValueError, KeyError) as e:
        print("FAILED: %s" % e, file=sys.stderr)
        return 1
    print_summary(manifest)
    print("\nwrote %s in %.2f s" % (os.path.join(out, "manifest.json"), time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
