#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the on-device toolchain for the APK (run build-python.sh, build-llvm.sh and make-sysroot.sh first):
#   $D/out/jniLibs/arm64-v8a/libpython_exec.so   the Python launcher
#   $D/out/jniLibs/arm64-v8a/libllvm_exec.so     clang + lld + llvm-ar (one multicall executable)
#   $D/out/assets/recompia-toolchain.tar.gz      unpacked by the app to <filesDir>/toolchain (INTERFACE.md)
#   $D/out/assets/recompia-toolchain.version     sha256 of the archive
# The archive holds python/ (PYTHONHOME), sysroot/, lib/clang/19/, driver/recompia_build.py and the build kit
# kit/tools/build/{pkp_build.py, guest_layout.py, dump_hashes.json, recomp/, sdk/} laid out as tools/android/build-game/build-game.sh does.
# sdk/ comes from the Android runtime build (A_OUT/sdk: pkp_stub.cpp, pkp_stub_android.cpp, pkpgame_android.ver,
# pkp_rt_api.h, guest_base.txt); libpkp_rt.so is NOT packed: the driver links the APK's own copy into sdk/.
#   D=<scratch dir> A_OUT=<runtime build output> ./package.sh
set -euo pipefail
: "${D:?set D}"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
S=$D/stage
T=$S/toolchain
O=$D/out
for f in "$S/jni/libpython_exec.so" "$S/python/lib/libpython3.14.so" "$D/llvm/bin/llvm" "$T/sysroot/usr/include/stdio.h"; do
  [ -e "$f" ] || { echo "missing $f: run the build scripts first" >&2; exit 1; }
done
rm -rf "$O" "$T/python" "$T/driver" "$T/kit"
mkdir -p "$O/jniLibs/arm64-v8a" "$O/assets" "$T/driver" "$T/kit/tools/build/recomp" "$T/kit/tools/build/sdk"
cp "$S/jni/libpython_exec.so" "$O/jniLibs/arm64-v8a/"
cp "$D/llvm/bin/llvm" "$O/jniLibs/arm64-v8a/libllvm_exec.so"
cp -a "$S/python" "$T/python"
cp "$HERE/driver/recompia_build.py" "$T/driver/"
K=$T/kit/tools/build
cp -p "$REPO/game-build/release_b/pkp_build.py" "$REPO/game-build/release_b/guest_layout.py" "$REPO/game-build/release_b/dump_hashes.json" "$K/"
for d in nxrecomp include games native; do cp -Rp "$REPO/recomp/$d" "$K/recomp/"; done
find "$K" -name __pycache__ -prune -exec rm -rf {} +
if [ -n "${A_OUT:-}" ]; then
  for f in pkp_stub.cpp pkp_stub_android.cpp pkpgame_android.ver pkp_rt_api.h guest_base.txt; do cp -p "$A_OUT/sdk/$f" "$K/sdk/"; done
else
  echo "warning: A_OUT not set, the kit has no sdk/ files (the link step will fail on the phone)" >&2
fi
# byte-compile once here, so the phone does not write __pycache__ on first run (and -I runs never miss). -f: the stage's own .pyc
# (build-python.sh) carry this machine's stage path; -s/-p: the source path recorded in every .pyc is "toolchain/...", not a path
# of this PC. Checked below: no .pyc or other file of the archive may contain the stage path.
python3 -c 'import sys; print(sys.version_info[:2])' | grep -q '(3, 14)' && python3 -m compileall -q -f --invalidation-mode unchecked-hash -s "$T" -p toolchain "$T/python/lib/python3.14" "$K" || true
if grep -rlaF "$S" "$T" | head -5 | grep .; then echo "error: the stage path is in the files above" >&2; exit 1; fi
if grep -rlaE "$HOME/" "$T" | head -5 | grep .; then echo "error: a home path is in the files above" >&2; exit 1; fi
# reproducible: hash-based .pyc (no timestamps), fixed entry times, gzip without name/time, so the same inputs give the same
# version line (the app treats a new kit version as new compile inputs and asks for a rebuild)
tar -C "$T" --format=ustar --owner=0 --group=0 --numeric-owner --sort=name --mtime=@1700000000 -cf - . | gzip -9 -n > "$O/assets/recompia-toolchain.tar.gz"
sha256sum "$O/assets/recompia-toolchain.tar.gz" | cut -d' ' -f1 > "$O/assets/recompia-toolchain.version"
echo "jniLibs:"; ls -l "$O/jniLibs/arm64-v8a"
echo "assets:"; ls -l "$O/assets"
echo "unpacked toolchain: $(du -sh "$T" | cut -f1)"
