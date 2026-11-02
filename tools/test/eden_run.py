"""Run Metroid Prime Remastered in the portable Eden under work/eden unattended and collect frames (work area S5).

Frames come from the LunarG screenshot layer (VK_LAYER_LUNARG_screenshot, in the Vulkan SDK): it writes the swapchain image of chosen present numbers as PPM,
independent of any desktop, so it works in a disconnected session where screen capture is black. A converter thread turns each PPM into a PNG and deletes it.
Input is posted to the SDL window by edeninput.py. State can be sampled through Eden's GDB stub (edengdb.py).

  python eden_run.py --out work/eden_ref/session1 --every 120 --seconds 240
  python eden_run.py --out work/eden_ref/s2 --every 60 --timeline tools/test/timelines/title_to_frigate.json --seconds 600

Timeline file: JSON list of events, executed in list order (an event waits for the one before it), each either timed with "t" or present-based with
"at_present": N (fires once a captured frame with present number >= N has been converted; use a small --every, e.g. 30). Present numbers follow the game, not the wall
clock, so they are the robust choice. Events are each {"t": <seconds after the window appeared>, "press": "A" | "A+B", "hold": 0.15} or
{"t": 30, "hold_keys": "LSTICK_UP", "seconds": 2.0} or {"t": 90, "state": "name"} (a GDB state read written to <out>/state_<name>.json) or {"t": 300, "trace": 600, "name": "frigate"} (a per-frame
GDB trace of 600 game frames, <out>/trace_<name>.jsonl, same JSONL as the runtime's trace).

Output: <out>/frames/<present number>.png, <out>/index.json (frame list with wall times, events done), <out>/eden.log copy.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import edengdb  # noqa: E402
import edeninput  # noqa: E402

DEFAULT_NSP = os.environ.get("MPR_DUMP", "")   # your own .nsp; or pass --nsp
VK_SDK = r"C:\VulkanSDK\1.4.363.0\Bin"


def convert_loop(frames_dir, out_dir, stop, index, scale=None):
    from PIL import Image
    seen = {}
    while not stop.is_set() or glob.glob(os.path.join(frames_dir, "*.ppm")):
        for p in sorted(glob.glob(os.path.join(frames_dir, "*.ppm"))):
            try:
                sz = os.path.getsize(p)
            except OSError:
                continue
            if seen.get(p) != sz:
                seen[p] = sz
                continue                       # wait until the size is stable between two polls
            try:
                n = int(os.path.splitext(os.path.basename(p))[0])
                im = Image.open(p)
                im.load()
                if scale:
                    im = im.resize(scale)
                png = os.path.join(out_dir, "%06d.png" % n)
                im.save(png)
                os.remove(p)
                index.append({"present": n, "png": os.path.basename(png), "wall_s": round(time.time() - index.t0, 2)})
            except Exception as e:  # noqa: BLE001
                print("convert", p, e, file=sys.stderr)
        time.sleep(0.5)


class Index(list):
    t0 = 0.0


def run(args):
    out = os.path.abspath(args.out)
    frames_dir = os.path.join(out, "_ppm")
    png_dir = os.path.join(out, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    os.makedirs(png_dir, exist_ok=True)
    eden_dir = os.path.join(ROOT, "work", "eden")
    env = dict(os.environ)
    env["VK_INSTANCE_LAYERS"] = "VK_LAYER_LUNARG_screenshot"
    env["VK_LAYER_PATH"] = VK_SDK
    env["VK_SCREENSHOT_DIR"] = frames_dir
    env["VK_SCREENSHOT_FRAMES"] = args.frames if args.frames else ",".join(str(n) for n in range(args.every, args.max_present, args.every))
    events_peek = []
    if args.timeline:
        with open(args.timeline) as f:
            events_peek = json.load(f)
    want_gdb = args.gdb or any("state" in e or "trace" in e for e in events_peek)
    # With the GDB stub on Eden waits at start until a client connects (and a connection pauses the game until a continue): only switch it on when needed.
    subprocess.run([sys.executable, os.path.join(HERE, "eden_config.py"), "set", "Debugging.use_gdbstub=" + ("true" if want_gdb else "false")] + (args.set or []), check=True)
    exe = os.path.join(eden_dir, "eden-cli.exe")
    cmd = [exe, args.nsp]
    log = open(os.path.join(out, "eden_stdout.txt"), "wb")
    proc = subprocess.Popen(cmd, cwd=eden_dir, env=env, stdout=log, stderr=subprocess.STDOUT)
    index = Index()
    stop = threading.Event()
    hwnd = None
    t_start = time.time()
    while time.time() - t_start < 60 and not hwnd:
        hwnd = edeninput.find_window("Eden", proc.pid)
        time.sleep(0.3)
    if not hwnd:
        proc.kill()
        raise SystemExit("eden window did not appear")
    base = 0
    if want_gdb:
        # release the start-up wait: connect, look up the modules, continue
        for _ in range(60):
            try:
                g0 = edengdb.Gdb(port=args.port)
                break
            except OSError:
                time.sleep(0.5)
        else:
            proc.kill()
            raise SystemExit("gdb stub did not come up")
        base = edengdb.main_base(g0)
        g0.cont()
        g0.close()
    index.t0 = time.time()
    th = threading.Thread(target=convert_loop, args=(frames_dir, png_dir, stop, index, args.scale), daemon=True)
    th.start()
    events = []
    if args.timeline:
        with open(args.timeline) as f:
            events = json.load(f)
        for e in events:
            e.setdefault("t", 1e9 if "at_present" in e else 0)
    done = []
    gdb = None
    mgr = 0
    try:
        while time.time() - index.t0 < args.seconds and proc.poll() is None:
            now = time.time() - index.t0
            newest = max((e["present"] for e in index), default=0)
            while events and ("at_present" in events[0] and newest >= events[0]["at_present"] or "at_present" not in events[0] and events[0]["t"] <= now):
                ev = events.pop(0)
                if "press" in ev:
                    edeninput.press(hwnd, ev["press"].split("+"), ev.get("hold", 0.15))
                elif "hold_keys" in ev:
                    edeninput.press(hwnd, ev["hold_keys"].split("+"), ev.get("seconds", 1.0))
                elif "trace" in ev:
                    try:
                        if not mgr:
                            gdb = edengdb.Gdb(port=args.port)
                            mgr = edengdb.capture_manager(gdb, base, 30.0)
                            gdb.close()
                            gdb = None
                        g = edengdb.Gdb(port=args.port)
                        ev["frames_written"] = edengdb.trace(g, base, os.path.join(out, "trace_%s.jsonl" % ev.get("name", "eden")), int(ev["trace"]), mgr)
                        g.close()
                    except Exception as e:  # noqa: BLE001
                        ev["error"] = str(e)
                elif "raw" in ev:
                    try:
                        dump = {"main_base": hex(base)}
                        for attempt in range(int(ev.get("attempts", 6))):
                            gdb = edengdb.Gdb(port=args.port)
                            mgr = mgr or edengdb.capture_manager(gdb, base, 30.0)
                            gdb.close()
                            if mgr:
                                break
                        dump["mgr"] = hex(mgr)
                        if mgr:
                            g = edengdb.Gdb(port=args.port)
                            dump["regions"] = edengdb.raw_dump(g, base, mgr)
                            g.cont()
                            g.close()
                        with open(os.path.join(out, "raw_%s.json" % ev["raw"]), "w") as f:
                            json.dump(dump, f, indent=1)
                    except Exception as e:  # noqa: BLE001
                        ev["error"] = str(e)
                elif "state" in ev:
                    try:
                        if not mgr:
                            gdb = edengdb.Gdb(port=args.port)
                            mgr = edengdb.capture_manager(gdb, base, 20.0)
                            gdb.close()
                            gdb = None
                        g = edengdb.Gdb(port=args.port)
                        st = edengdb.read_state(g, base, mgr, debug=True)
                        g.cont()
                        g.close()
                        st["_wall_s"] = round(now, 2)
                        with open(os.path.join(out, "state_%s.json" % ev["state"]), "w") as f:
                            json.dump(st, f, indent=1)
                        ev["result"] = st
                    except Exception as e:  # noqa: BLE001
                        ev["error"] = str(e)
                ev["done_at"] = round(now, 2)
                done.append(ev)
            time.sleep(0.1)
    finally:
        stop.set()
        time.sleep(1.5)
        proc.kill()
        th.join(timeout=20)
        index.sort(key=lambda e: e["present"])
        with open(os.path.join(out, "index.json"), "w") as f:
            json.dump({"frames": list(index), "events": done, "seconds": args.seconds, "every": args.every}, f, indent=1)
        for p in glob.glob(os.path.join(eden_dir, "user", "log", "eden_log.txt")):
            try:
                with open(p, "rb") as a, open(os.path.join(out, "eden.log"), "wb") as b:
                    b.write(a.read())
            except OSError:
                pass
    print("frames:", len(index), "->", png_dir)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--nsp", default=DEFAULT_NSP)
    ap.add_argument("--every", type=int, default=120, help="capture every N-th present")
    ap.add_argument("--frames", help="explicit VK_SCREENSHOT_FRAMES value (overrides --every)")
    ap.add_argument("--seconds", type=float, default=180)
    ap.add_argument("--max-present", type=int, default=60000, help="highest present number a --every capture list goes to")
    ap.add_argument("--timeline")
    ap.add_argument("--gdb", action="store_true", help="enable the GDB stub for the run even without state events")
    ap.add_argument("--port", type=int, default=6543)
    ap.add_argument("--set", nargs="*", help="eden_config KEY=VALUE pairs applied before the start")
    ap.add_argument("--scale", type=lambda s: tuple(int(x) for x in s.split("x")), help="WxH to resize frames to")
    run(ap.parse_args(argv))
    return 0


if __name__ == "__main__":
    sys.exit(main())
