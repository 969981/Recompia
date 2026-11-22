#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the differential test of the AArch64 FP / NEON fast paths for an arm64 Android device with the NDK, with the lifted
# code's own flags (-O1, -ffp-contract=off); the native build also gets the Android forced include (recomp/native). Run it on the device: adb push difftest /data/local/tmp && adb shell /data/local/tmp/difftest
#   ANDROID_NDK_HOME=<ndk> sh build.sh [output, default ./difftest]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
inc=${NX_INCLUDE:-$here/../../../recomp/include}
native=${NX_NATIVE:-$here/../../../recomp/native}
cxx=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++
out=${1:-$PWD/difftest}
fl="--target=aarch64-linux-android30 -std=c++20 -O1 -w -fno-exceptions -fno-rtti -fno-strict-aliasing -ffp-contract=off -I $inc"
tmp=$(mktemp -d)
$cxx $fl -DNX_PORTABLE_FP -DIMPL_NS=gen -c "$here/impl.cpp" -o "$tmp/gen.o"
$cxx $fl -DIMPL_NS=nat -include "$native/nx_native.h" -c "$here/impl.cpp" -o "$tmp/nat.o"
$cxx $fl -c "$here/difftest.cpp" -o "$tmp/main.o"
$cxx --target=aarch64-linux-android30 -static-libstdc++ "$tmp/main.o" "$tmp/gen.o" "$tmp/nat.o" -o "$out"
rm -rf "$tmp"
echo "$out"
