#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The Android game library libpkpgame.so from a player's dump, on a PC: lift + compile + link, incremental (a second run with
# nothing changed only checks; a runtime-only change only links). A wrapper around game-build/release_b/pkp_build.py
# --target android-arm64: it lays out the kit that script expects (tools/build/ with recomp/ and sdk/) in $WORK and runs it.
#
#   DUMP=<dump folder> ANDROID_NDK_HOME=<ndk r28c> A_OUT=<runtime build output> sh tools/android/build-game/build-game.sh
#
# DUMP      the dump: its exefs/ or the last updates/*/exefs/ is used (or set EXEFS directly)
# A_OUT     the Android runtime build's output: sdk/ (pkp_stub.cpp, pkp_stub_android.cpp, pkpgame_android.ver, pkp_rt_api.h,
#           guest_base.txt) and libpkp_rt.so (in sdk/ or out/arm64-v8a/)
# WORK      where the kit, the game folder and the object cache live (default: ./android-game, outside the repo is better)
# PYTHON    a Python 3.13 with capstone 5.0.9 and lz4 (recomp/requirements.txt), default python3
# JOBS      compile jobs (default: cores - 1), MEM_MB memory per compile job (default 1024), KEEP=1 keeps lift/ and obj/
# Result: $WORK/game/libpkpgame.so (and $WORK/game/build/done.json). Never inside the repo: it is game code.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
: "${A_OUT:?set A_OUT to the Android runtime build output}"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
WORK=${WORK:-$PWD/android-game}
PYTHON=${PYTHON:-python3}
JOBS=${JOBS:-$(( $(nproc) - 1 ))}
MEM_MB=${MEM_MB:-1024}
if [ -z "${EXEFS:-}" ]; then
    : "${DUMP:?set DUMP to the dump folder (or EXEFS to its exefs)}"
    if [ -d "$DUMP/exefs" ]; then EXEFS=$DUMP/exefs; else EXEFS=$(ls -d "$DUMP"/updates/*/exefs | sort -V | tail -n 1); fi
fi
rt=$A_OUT/sdk/libpkp_rt.so
[ -f "$rt" ] || rt=$A_OUT/out/arm64-v8a/libpkp_rt.so
[ -f "$rt" ] || { echo "no libpkp_rt.so in $A_OUT/sdk or $A_OUT/out/arm64-v8a" >&2; exit 1; }

# the kit, as the release scripts lay it out: tools/build/{pkp_build.py, guest_layout.py, dump_hashes.json, recomp/, sdk/}
kit=$WORK/kit/tools/build
mkdir -p "$kit/recomp" "$kit/sdk" "$WORK/game"
cp -p "$repo/game-build/release_b/pkp_build.py" "$repo/game-build/release_b/guest_layout.py" "$repo/game-build/release_b/dump_hashes.json" "$kit/"
for d in nxrecomp include games native; do
    rm -rf "$kit/recomp/$d.tmp" && cp -Rp "$repo/recomp/$d" "$kit/recomp/$d.tmp"
    find "$kit/recomp/$d.tmp" -name __pycache__ -prune -exec rm -rf {} +
    rm -rf "$kit/recomp/$d" && mv "$kit/recomp/$d.tmp" "$kit/recomp/$d"
done
for f in pkp_stub.cpp pkp_stub_android.cpp pkpgame_android.ver pkp_rt_api.h; do cp -p "$A_OUT/sdk/$f" "$kit/sdk/"; done
cp -p "$rt" "$kit/sdk/libpkp_rt.so"
if [ -f "$A_OUT/sdk/guest_base.txt" ]; then cp -p "$A_OUT/sdk/guest_base.txt" "$kit/sdk/"; else rm -f "$kit/sdk/guest_base.txt"; fi

set -- --exefs "$EXEFS" --game "$WORK/game" --jobs "$JOBS" --mem-per-job-mb "$MEM_MB" --target android-arm64 --ndk "$ANDROID_NDK_HOME"
[ "${KEEP:-0}" = 1 ] && set -- "$@" --keep
exec "$PYTHON" -I "$kit/pkp_build.py" "$@"
