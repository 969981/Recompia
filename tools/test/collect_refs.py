"""Collect named reference captures from Eden sessions into work/eden_ref/ref (game-derived: never in git).

  python collect_refs.py spec.json [--out work/eden_ref/ref]
spec.json: [{"name": "title", "src": "work/eden_ref/explore1/frames/001500.png", "note": "..."}, ...]
Copies each PNG to <out>/<name>.png and writes <out>/index.json (name, source, size, note, mean colour) so that run_suite `ref_shots` entries can point at them.
"""
import argparse
import json
import os
import shutil
import sys

from PIL import Image, ImageStat

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("spec")
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "eden_ref", "ref"))
    a = ap.parse_args(argv)
    spec = json.load(open(a.spec))
    os.makedirs(a.out, exist_ok=True)
    index = []
    for e in spec:
        src = e["src"] if os.path.isabs(e["src"]) else os.path.join(ROOT, e["src"])
        dst = os.path.join(a.out, e["name"] + ".png")
        shutil.copyfile(src, dst)
        im = Image.open(dst).convert("RGB")
        index.append({"name": e["name"], "source": os.path.relpath(src, ROOT), "size": list(im.size), "note": e.get("note", ""),
                      "mean_rgb": [round(v, 1) for v in ImageStat.Stat(im).mean]})
    with open(os.path.join(a.out, "index.json"), "w") as f:
        json.dump(index, f, indent=1)
    print("collected", len(index), "->", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
