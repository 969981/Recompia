#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""On-device shader setup: builds <data>/shader_cache from the dump on the phone, as the PC launcher's Set up does (launcher/src-tauri/src
shaders.rs + shader_archive.rs), with the same output: spirv.archive/NNNNNN.pack segments (NXSPACK1, one zstd frame per shader),
spirv/results.tsv and setup.json. The runtime writes spirv.preload.pack itself on the first start.

  1. extract: tools/shaders/ampere/extract_corpus.py finds every shader block in romfs and exefs/main -> shader_cache/corpus
  2. translate: libspirv_harness.so (NativeAOT build of tools/shaders/harness, --host desktop: the PC's output, byte for byte) in chunks;
     each chunk is packed into a new archive segment and its rows appended to results.tsv as soon as it is done, so a stopped run
     resumes where it was (a block with a results row is never translated again)
  3. the corpus is deleted, setup.json marks the cache complete

Progress on stdout, one event per line: @@STEP i/n name, @@PROGRESS done/total unit, @@LOG text, @@DONE path, @@ERROR text.
usage: python -I -u build_shaders.py --data /sdcard/Recompia --native-lib-dir <dir> --extractor <extract_corpus.py> [--jobs 6] [--chunk 2000]
"""
import argparse, json, os, shutil, signal, struct, subprocess, sys, threading, time

TRANSLATOR_REV = 2          # launcher/src-tauri/src/shaders.rs TRANSLATOR_REV: the cache a PC setup of this revision writes
ZSTD_LEVEL = 6              # shader_archive.rs LEVEL
MAGIC, VERSION, HEADER, ENTRY = b"NXSPACK1", 1, 32, 40


def say(kind, text):
    print(f"@@{kind} {text}", flush=True)


def fail(text):
    say("ERROR", text)
    sys.exit(1)


def read_tsv(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    return (lines[0], lines[1:]) if lines else ("", [])


def write_segment(arch_dir, items):
    """items: [(hash, spirv bytes)] -> the next NNNNNN.pack, written to .tmp, synced, renamed (shader_archive.rs write_segment)."""
    from compression import zstd
    os.makedirs(arch_dir, exist_ok=True)
    nums = [int(n[:6]) for n in os.listdir(arch_dir) if len(n) == 11 and n.endswith(".pack") and n[:6].isdigit()]
    fin = os.path.join(arch_dir, "%06d.pack" % (max(nums, default=0) + 1))
    tmp = fin + ".tmp"
    index, off = bytearray(), HEADER
    with open(tmp, "wb") as f:
        f.write(b"\0" * HEADER)
        for h, raw in items:
            frame = zstd.compress(raw, ZSTD_LEVEL)
            f.write(frame)
            index += struct.pack("<16sQQII", h.encode()[:16], 0, off, len(frame), len(raw))
            off += len(frame)
        f.write(index)
        f.seek(0)
        f.write(struct.pack("<8sIIQQ", MAGIC, VERSION, len(items), off, 0))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, fin)


def free_gib(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / (1 << 30)


# The tool that runs right now (extractor or translator). A stop (SIGTERM from the app's Pause) ends it too: otherwise the translator
# kept running on its own after the driver exited, a phone full of busy cores for a job nobody waits for.
CHILD = None


def on_term(signum, frame):
    if CHILD is not None and CHILD.poll() is None:
        CHILD.terminate()
        try:
            CHILD.wait(timeout=10)
        except subprocess.TimeoutExpired:
            CHILD.kill()
    sys.exit(2)


def run_tool(cmd):
    """subprocess.run with the child remembered for on_term."""
    global CHILD
    CHILD = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = CHILD.communicate()[0]
    rc, CHILD = CHILD.returncode, None
    return subprocess.CompletedProcess(cmd, rc, out)


def main():
    signal.signal(signal.SIGTERM, on_term)
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, help="data root: dump/romfs, dump/exefs/main, shader_cache/")
    ap.add_argument("--native-lib-dir", required=True)
    ap.add_argument("--extractor", required=True, help="extract_corpus.py")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--chunk", type=int, default=2000)
    # a dump the app reads in place (pkp.ini [fs]) instead of <data>/dump
    ap.add_argument("--romfs", help="the romfs folder (default <data>/dump/romfs)")
    ap.add_argument("--romfs-base", help="the base game's romfs, layered under --romfs (an update dumped without the base's files)")
    ap.add_argument("--main", help="exefs/main, or the exefs folder holding it (default <data>/dump/exefs/main)")
    ap.add_argument("--min-free-gib", type=float, default=12.0,
                    help="free space demanded at the start (default 12, the full game's peak; lower only for a test on a partial romfs)")
    a = ap.parse_args()

    data = os.path.abspath(a.data)
    romfs = os.path.abspath(a.romfs) if a.romfs else os.path.join(data, "dump", "romfs")
    main_nso = os.path.abspath(a.main) if a.main else os.path.join(data, "dump", "exefs", "main")
    if os.path.isdir(main_nso):
        main_nso = os.path.join(main_nso, "main")
    romfs_base = os.path.abspath(a.romfs_base) if a.romfs_base else None
    cache = os.path.join(data, "shader_cache")
    corpus, spirv, stage = os.path.join(cache, "corpus"), os.path.join(cache, "spirv"), os.path.join(cache, "stage")
    arch, results = os.path.join(cache, "spirv.archive"), os.path.join(cache, "spirv", "results.tsv")
    harness = os.path.join(a.native_lib_dir, "libspirv_harness.so")
    all_tsv = os.path.join(corpus, "unique_all.tsv")
    try:
        with open(os.path.join(cache, "setup.json"), encoding="utf-8") as f:
            m = json.load(f)
        if m.get("complete") and m.get("translator", 0) >= TRANSLATOR_REV and os.path.exists(results):
            say("DONE", cache)          # a finished cache (made here or imported from a PC)
            return
    except (OSError, ValueError):
        pass
    for p, what in ((romfs, "romfs"), (main_nso, "exefs/main"), (harness, "translator"), (a.extractor, "extractor"),
                    *(((romfs_base, "base romfs"),) if romfs_base else ())):
        if not os.path.exists(p):
            fail(f"missing {what}: {p}")
    try:
        import compression.zstd  # noqa: F401  (Python 3.14 stdlib; the archive needs it)
    except ImportError:
        fail("this Python has no compression.zstd")
    os.makedirs(spirv, exist_ok=True)
    # the peak: corpus (4.8 GB for 2.0.1) + archive (6.2 GB) + one chunk of loose SPIR-V; checked again before each chunk
    if not os.path.exists(all_tsv) and free_gib(cache) < a.min_free_gib:
        fail(f"{free_gib(cache):.1f} GiB free on the data storage; the shader setup needs {a.min_free_gib:g} GiB at its peak (6.6 GiB when done)")

    # 1. extract
    say("STEP", "1/3 extract")
    if not os.path.exists(all_tsv):
        cmd = [sys.executable, "-I", "-B", a.extractor, "--romfs", romfs, "--main", main_nso, "--out", corpus,
               "--jobs", str(min(a.jobs, 2)), "--progress"] + (["--romfs-base", romfs_base] if romfs_base else [])   # 2.5 GB resident at 4 jobs on the PC; the phone has about 3 GB free
        global CHILD
        p = CHILD = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in p.stdout:
            line = line.rstrip()
            if line.startswith("scan: "):
                d, t = line[6:].split()[:2]
                say("PROGRESS", f"{d}/{t} files")
            elif line:
                say("LOG", line)
        if p.wait() != 0:
            fail(f"the shader extractor failed (exit {p.returncode})")
        os.replace(os.path.join(corpus, "unique.tsv"), all_tsv)

    # 2. translate what results.tsv does not list yet
    say("STEP", "2/3 translate")
    uheader, urows = read_tsv(all_tsv)
    done = set()
    rheader = ""
    if os.path.exists(results):
        rheader, rrows = read_tsv(results)
        done = {r.split("\t", 1)[0] for r in rrows}
    todo = [r for r in urows if r.split("\t", 1)[0] not in done]
    total, finished = len(urows), len(urows) - len(todo)
    say("LOG", f"{total} shader blocks, {finished} already translated")
    for k in range(0, len(todo), a.chunk):
        if free_gib(cache) < 1.0:
            fail(f"only {free_gib(cache):.1f} GiB free on the data storage; free some space and start again (the work so far is kept)")
        part = todo[k:k + a.chunk]
        shutil.rmtree(stage, ignore_errors=True)
        with open(os.path.join(corpus, "unique.tsv"), "w", encoding="utf-8") as f:
            f.write(uheader + "\n" + "\n".join(part) + "\n")
        cmd = [harness, "--batch", corpus, "--out", stage, "--jobs", str(a.jobs), "--host", "desktop", "--no-val"]
        stop = threading.Event()

        def watch(base=finished):   # the harness writes "<t> s  <done>/<total>" lines to harness_progress.txt
            prog = os.path.join(stage, "harness_progress.txt")
            while not stop.wait(2.0):
                try:
                    with open(prog, encoding="utf-8") as f:
                        last = [l for l in f.read().splitlines() if "/" in l and l.split()[-1].count("/") == 1]
                    if last:
                        say("PROGRESS", f"{base + int(last[-1].split()[-1].split('/')[0])}/{total} shaders")
                except (OSError, ValueError):
                    pass
        t = threading.Thread(target=watch, daemon=True)
        t.start()
        p = run_tool(cmd)
        stop.set()
        t.join()
        if p.returncode != 0:
            fail(f"the shader translator failed (exit {p.returncode}): {p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ''}")
        sheader, srows = read_tsv(os.path.join(stage, "results.tsv"))
        items = []
        for r in srows:
            h = r.split("\t", 1)[0]
            f = os.path.join(stage, h + ".spv")
            if os.path.exists(f):
                with open(f, "rb") as s:
                    items.append((h, s.read()))
        if items:
            write_segment(arch, items)
        # rows after their shaders are in the archive: a stop between the two translates the chunk again, never loses a shader
        new = not os.path.exists(results)
        with open(results, "a", encoding="utf-8") as f:
            if new:
                f.write(sheader + "\n")
            f.write("".join(r + "\n" for r in srows))
            f.flush()
            os.fsync(f.fileno())
        finished += len(part)
        say("PROGRESS", f"{finished}/{total} shaders")
    shutil.rmtree(stage, ignore_errors=True)

    # 3. finish: the marker the launcher and runtime read (shaders.rs Marker), corpus removed
    say("STEP", "3/3 finish")
    with open(os.path.join(cache, "setup.json"), "w", encoding="utf-8") as f:
        json.dump({"romfs": romfs, "extracted": True, "blocks": total, "complete": True, "translator": TRANSLATOR_REV,
                   "storage": "compact"}, f, indent=2)
    shutil.rmtree(corpus, ignore_errors=True)
    say("DONE", cache)


if __name__ == "__main__":
    main()
