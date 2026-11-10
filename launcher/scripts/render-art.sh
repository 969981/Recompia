#!/usr/bin/env bash
# Regenerate the launcher art (sharp and blurred, for the Dusk, Sunrise and Meadow themes) using the build container's cairo/rsvg/webp libraries.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
art="$root/src/art"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

webp_flags=$(pkg-config --cflags --libs libwebp 2>/dev/null || printf '%s\n' '-l:libwebp.so.7')
# Word splitting is intentional for pkg-config's compiler and linker flags.
cc -O3 -std=c11 "$root/scripts/render-art.c" -o "$tmp/render-art" \
  $(pkg-config --cflags --libs librsvg-2.0 cairo) $webp_flags -lm
"$tmp/render-art" "$art/island.svg" "$art/island-sharp.webp" "$art/island-soft.webp" \
  "$art/island-sharp-sunrise.webp" "$art/island-soft-sunrise.webp"
"$tmp/render-art" "$art/meadow.svg" "$art/meadow-sharp.webp" "$art/meadow-soft.webp"
