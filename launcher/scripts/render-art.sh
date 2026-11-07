#!/usr/bin/env bash
# Regenerate the launcher art. Requires rsvg-convert and ImageMagick.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
art="$root/src/art"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

rsvg-convert --width 3840 --height 2400 --output "$tmp/sharp.png" "$art/island.svg"
rsvg-convert --width 960 --height 600 --output "$tmp/soft.png" "$art/island.svg"

convert "$tmp/sharp.png" -quality 90 "$art/island-sharp.webp"
# CSS blur(56px) has sigma 28. At the default 1120x720 window, the 180px bleed
# makes this 960px image display at 1.8x, so bake the blur at 28 / 1.8.
convert "$tmp/soft.png" -gaussian-blur 0x15.5556 -modulate 100,120,100 -quality 85 "$art/island-soft.webp"
