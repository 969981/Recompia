"""Renders src/art/zonith-sky.svg to the two pictures the launcher shows behind Zonith's pages:
zonith-sky-sharp.webp (its Home) and zonith-sky-soft.webp (a small pre-blurred copy for the other pages, like the other themes').

    python scripts/render-zonith-art.py

Needs Pillow and Microsoft Edge (or Chrome: set BROWSER) to rasterise the SVG headless."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageFilter

ART = Path(__file__).resolve().parent.parent / 'src' / 'art'
BROWSER = os.environ.get('BROWSER', r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')


def main():
    svg = ART / 'zonith-sky.svg'
    with tempfile.TemporaryDirectory() as tmp:
        png = Path(tmp) / 'sky.png'
        page = Path(tmp) / 'sky.html'
        page.write_text(f'<html><body style="margin:0">{svg.read_text(encoding="utf-8")}</body></html>', encoding='utf-8')
        subprocess.run([BROWSER, '--headless=new', '--disable-gpu', '--hide-scrollbars', f'--user-data-dir={tmp}/profile',
                        '--window-size=1920,1080', f'--screenshot={png}', page.as_uri()], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        im = Image.open(png).convert('RGB').crop((0, 0, 1920, 1080))
    im.save(ART / 'zonith-sky-sharp.webp', quality=82, method=6)
    soft = im.resize((96, 54), Image.LANCZOS).filter(ImageFilter.GaussianBlur(3))
    soft.save(ART / 'zonith-sky-soft.webp', quality=80)
    print('wrote zonith-sky-sharp.webp and zonith-sky-soft.webp')


if __name__ == '__main__':
    sys.exit(main())
