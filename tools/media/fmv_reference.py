#!/usr/bin/env python3
"""Reference values for the FMV decoder tests, produced by ffmpeg.

For every <id>.mp4 in --dir (described by its manifest.json) writes:
  <id>.md5      one lowercase md5 per video frame: raw NV12, 1600x900, 2160000 bytes/frame,
                in presentation order.
  <id>.ref.pcm  raw interleaved signed 16-bit little-endian PCM at the audio track's native
                rate and channel count (no resampling, no remapping). Only for movies whose
                manifest entry has an audio track.

Idempotent: an output newer than its mp4 is skipped, but still validated against the manifest.
Outputs are written to a .tmp file and renamed only after validation, so a failed run never
leaves a bad output that a later run would skip.

Usage: fmv_reference.py --dir <movie dir> [--jobs 8] [--only <id-prefix>]
Exit 0 only when every movie matched its manifest. Exit 1 otherwise, 2 on setup errors.
"""
import argparse
import concurrent.futures as cf
import json
import os
import shutil
import subprocess
import threading
import time

FRAME_W = 1600
FRAME_H = 900
FRAME_BYTES = 2160000  # NV12: Y plane W*H, then interleaved UV plane W*H/2

_print_lock = threading.Lock()


def say(msg):
    with _print_lock:
        print(msg, flush=True)


def find_tool(name):
    # Same lookup as fmv_manifest.py: the Gyan build in WinGet Links before the ancient PATH entry.
    cands = [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Links", name + ".exe"),
        shutil.which(name) or "",
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def run(cmd, stdout=subprocess.PIPE):
    r = subprocess.run(cmd, stdout=stdout, stderr=subprocess.PIPE)
    if r.returncode != 0:
        err = r.stderr.decode(errors="replace").strip()
        raise RuntimeError("%s rc=%d: %s" % (os.path.basename(cmd[0]), r.returncode, err))
    return r


def video_md5s(ffmpeg, mp4):
    cmd = [ffmpeg, "-v", "error", "-i", mp4, "-map", "0:v:0", "-fps_mode", "passthrough",
           "-pix_fmt", "nv12", "-f", "framemd5", "-"]
    md5s = []
    for line in run(cmd).stdout.decode("ascii").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = [x.strip() for x in line.split(",")]
        if len(f) != 6:
            raise RuntimeError("unexpected framemd5 line: %r" % line)
        if int(f[4]) != FRAME_BYTES:
            raise RuntimeError("frame size %s != %d: %r" % (f[4], FRAME_BYTES, line))
        md5s.append(f[5].lower())
    return md5s


def audio_info(ffprobe, mp4):
    r = run([ffprobe, "-v", "error", "-select_streams", "a:0", "-show_entries",
             "stream=codec_name,channels,channel_layout,sample_rate", "-of", "default=nw=1", mp4])
    return dict(l.split("=", 1) for l in r.stdout.decode("ascii").splitlines() if "=" in l)


def audio_pcm(ffmpeg, mp4, tmp):
    with open(tmp, "wb") as fh:
        run([ffmpeg, "-v", "error", "-i", mp4, "-map", "0:a:0", "-f", "s16le",
             "-acodec", "pcm_s16le", "-"], stdout=fh)


def fresh(out, src):
    return os.path.isfile(out) and os.path.getmtime(out) > os.path.getmtime(src)


def write_md5(path, md5s):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\n") as f:
        f.write("".join(m + "\n" for m in md5s))
    os.replace(tmp, path)


def check_pcm_size(path, channels):
    size = os.path.getsize(path)
    if size == 0 or size % (2 * channels):
        raise RuntimeError("%s: %d bytes is not a whole number of %d-channel s16 frames"
                           % (path, size, channels))
    return size


def process(movie, dirp, ffmpeg, ffprobe):
    name = movie["file"]
    mid = os.path.splitext(name)[0]
    mp4 = os.path.join(dirp, name)
    v = movie["video"]
    if (v["width"], v["height"]) != (FRAME_W, FRAME_H):
        raise RuntimeError("manifest size %dx%d, expected %dx%d"
                           % (v["width"], v["height"], FRAME_W, FRAME_H))
    want = v["frame_count"]

    # Video: md5 per frame.
    md5_path = os.path.join(dirp, mid + ".md5")
    if fresh(md5_path, mp4):
        with open(md5_path, encoding="ascii") as f:
            md5s = [l.strip() for l in f if l.strip()]
        vnote = "cached"
    else:
        md5s = video_md5s(ffmpeg, mp4)
        if len(md5s) != want:
            raise RuntimeError("%s: ffmpeg gave %d video frames, manifest frame_count is %d"
                               % (name, len(md5s), want))
        write_md5(md5_path, md5s)
        vnote = "written"
    if len(md5s) != want:
        raise RuntimeError("%s: %s has %d md5 lines, manifest frame_count is %d (delete it to regenerate)"
                           % (name, os.path.basename(md5_path), len(md5s), want))

    # Audio: native-layout s16le PCM.
    summary = "video %d frames, %d md5 (%s)" % (want, len(md5s), vnote)
    a = movie.get("audio")
    if a:
        ch, rate = int(a["channels"]), int(a["sample_rate"])
        info = audio_info(ffprobe, mp4)
        if int(info["channels"]) != ch or int(info["sample_rate"]) != rate:
            raise RuntimeError("%s: ffprobe says %s ch %s Hz, manifest says %d ch %d Hz"
                               % (name, info["channels"], info["sample_rate"], ch, rate))
        layout = info.get("channel_layout", "?")
        pcm_path = os.path.join(dirp, mid + ".ref.pcm")
        if fresh(pcm_path, mp4):
            anote = "cached"
            nbytes = check_pcm_size(pcm_path, ch)
        else:
            tmp = pcm_path + ".tmp"
            try:
                audio_pcm(ffmpeg, mp4, tmp)
                nbytes = check_pcm_size(tmp, ch)
            except Exception:
                if os.path.exists(tmp):
                    os.remove(tmp)
                raise
            os.replace(tmp, pcm_path)
            anote = "written"
        summary += "; audio %d bytes = %d sample frames, %dch %dHz layout %s (%s)" % (
            nbytes, nbytes // (2 * ch), ch, rate, layout, anote)
    else:
        summary += "; no audio"
    return summary


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", required=True, help="movie folder holding <id>.mp4 and manifest.json")
    ap.add_argument("--jobs", type=int, default=8, help="parallel movies (default 8)")
    ap.add_argument("--only", default="", help="only movies whose id starts with this prefix")
    args = ap.parse_args(argv)

    t0 = time.perf_counter()
    dirp = os.path.abspath(args.dir)
    ffmpeg = find_tool("ffmpeg")
    ffprobe = find_tool("ffprobe")
    if not ffmpeg or not ffprobe:
        say("error: ffmpeg/ffprobe not found (looked in WinGet Links and PATH)")
        return 2
    try:
        with open(os.path.join(dirp, "manifest.json"), encoding="utf-8") as f:
            manifest = json.load(f)
    except OSError as e:
        say("error: cannot read manifest.json: %s" % e)
        return 2
    by_file = {m["file"]: m for m in manifest["movies"]}

    mp4s = sorted(f for f in os.listdir(dirp)
                  if f.endswith(".mp4") and os.path.splitext(f)[0].startswith(args.only))
    if not mp4s:
        say("error: no .mp4 files matched in %s" % dirp)
        return 2
    if not args.only and len(mp4s) != len(by_file):
        say("error: %d mp4 files but manifest lists %d movies" % (len(mp4s), len(by_file)))
        return 1

    errors = []

    def job(name):
        try:
            if name not in by_file:
                raise RuntimeError("%s is not in manifest.json" % name)
            return process(by_file[name], dirp, ffmpeg, ffprobe)
        except Exception as e:  # keep the other movies going; reported below
            return "FAILED: %s" % e

    with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = {ex.submit(job, n): n for n in mp4s}
        for fut in cf.as_completed(futs):
            name = futs[fut]
            msg = fut.result()
            if msg.startswith("FAILED: "):
                errors.append("%s: %s" % (name, msg[len("FAILED: "):]))
                say("FAIL %s  %s" % (os.path.splitext(name)[0], msg))
            else:
                say("OK   %s  %s" % (os.path.splitext(name)[0], msg))

    wall = time.perf_counter() - t0
    say("")
    say("movies: %d, failed: %d, total wall time: %.1f s" % (len(mp4s), len(errors), wall))
    for e in sorted(errors):
        say("  - " + e)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
