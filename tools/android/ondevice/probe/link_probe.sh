#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Measurement helper (not shipped): archives the objects of each module and links libpkpgame.so exactly as pkp_build.py's
# archive() and link() do for --target android-arm64, with a given clang++/llvm-ar (the PC's NDK or the phone's own).
#   CXX=<clang++> AR=<llvm-ar> SYSROOT=<sysroot or empty> link_probe.sh <obj dir with main/ sdk/ subsdk0/ rtld/> <sdk dir> <gen dir> <out dir>
set -eu
obj=$1 sdk=$2 gen=$3 out=$4
mkdir -p "$out"
s=$(date +%s)
for m in main sdk subsdk0 rtld; do
  rm -f "$out/pkp_$m.a"
  ls "$obj/$m"/*.o | sort | sed 's/.*/"&"/' > "$out/$m.rsp"
  "$AR" rcs "$out/pkp_$m.a" @"$out/$m.rsp"
done
a=$(date +%s)
"$CXX" ${SYSROOT:+--sysroot=$SYSROOT} --target=aarch64-linux-android30 -std=c++20 -O1 -w -fno-exceptions -fno-rtti \
  -fno-strict-aliasing -ffp-contract=off -fno-threadsafe-statics -g0 -fPIC -Dmain=pkp_stub_main -I "$sdk" -I "$gen" \
  "$sdk/pkp_stub.cpp" "$sdk/pkp_stub_android.cpp" "$out/pkp_main.a" "$out/pkp_sdk.a" "$out/pkp_subsdk0.a" "$out/pkp_rtld.a" \
  "$sdk/libpkp_rt.so" -shared -o "$out/libpkpgame.so" -Wl,-soname,libpkpgame.so -Wl,--version-script,"$sdk/pkpgame_android.ver" \
  -Wl,-z,max-page-size=16384 -Wl,--no-undefined -Wl,--strip-debug
e=$(date +%s)
echo "archive $((a - s)) s, link $((e - a)) s"
ls -l "$out/libpkpgame.so"
