"""Contact sheet of a folder of frames, for looking at a whole Eden capture session in one image.

  python contact_sheet.py work/eden_ref/explore1/frames sheet.png [--cols 6] [--thumb 320x180] [--first 0 --last 999999]
"""
import argparse
import glob
import os

from PIL import Image, ImageDraw


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("out")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--thumb", default="320x180")
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=10 ** 9)
    a = ap.parse_args(argv)
    tw, th = (int(x) for x in a.thumb.split("x"))
    files = [f for f in sorted(glob.glob(os.path.join(a.folder, "*.png"))) if a.first <= int(os.path.splitext(os.path.basename(f))[0]) <= a.last]
    rows = (len(files) + a.cols - 1) // a.cols
    sheet = Image.new("RGB", (a.cols * tw, rows * (th + 14)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, f in enumerate(files):
        im = Image.open(f).convert("RGB").resize((tw, th))
        x, y = (i % a.cols) * tw, (i // a.cols) * (th + 14)
        sheet.paste(im, (x, y + 14))
        d.text((x + 3, y + 1), os.path.splitext(os.path.basename(f))[0], fill=(255, 255, 0))
    sheet.save(a.out)
    print(a.out, len(files), "frames")


if __name__ == "__main__":
    main()
