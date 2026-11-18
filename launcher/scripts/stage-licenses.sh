#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fills a release package's licenses/ folder: the licence text of every component (tools/release/licenses, tools/shaders/licenses),
# the project's own notices and the Rust crates' notices the release workflow generated (launcher/about.hbs). The Linux and macOS
# package scripts call it; game-build/scripts/make-release.ps1 does the same for the Windows zip.
#
#   launcher/scripts/stage-licenses.sh <licenses dir> <THIRD_PARTY_CRATES.txt>
set -eu
dest=${1:?usage: stage-licenses.sh <licenses dir> <THIRD_PARTY_CRATES.txt>}
crates=${2:?usage: stage-licenses.sh <licenses dir> <THIRD_PARTY_CRATES.txt>}
repo=$(cd "$(dirname "$0")/../.." && pwd)
[ -f "$crates" ] || { echo "no crate notices at $crates (cargo about generate, see release.yml)" >&2; exit 1; }
mkdir -p "$dest"
cp "$repo"/tools/release/licenses/* "$dest/"
cp "$repo"/tools/shaders/licenses/* "$dest/"
cp "$repo/runtime/THIRD_PARTY_NOTICES.md" "$dest/THIRD_PARTY_NOTICES.md"
cp "$repo/recomp/THIRD_PARTY.md" "$dest/THIRD_PARTY_RECOMP.md"
cp "$repo/CREDITS.md" "$dest/CREDITS.md"
cp "$crates" "$dest/THIRD_PARTY_CRATES.txt"
