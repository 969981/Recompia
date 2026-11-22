#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The target files the phone's clang compiles and links against, from the NDK r28c (API 30, arm64 only):
#   $D/stage/toolchain/sysroot/usr/include           bionic + libc++ headers (other ABIs' folders dropped)
#   $D/stage/toolchain/sysroot/usr/lib/aarch64-linux-android/{libc++_shared.so,30/...}   crt objects and the stub libraries
#   $D/stage/toolchain/lib/clang/19/include          clang's own headers (arm_neon.h, ...) from OUR clang build (same version)
#   $D/stage/toolchain/lib/clang/19/lib/linux/       compiler-rt builtins + libunwind from the NDK
# Run after build-llvm.sh (it needs $D/llvm-android/lib/clang/19/include).
set -euo pipefail
: "${D:?set D}" "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
API=30
P=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
T=$D/stage/toolchain
rm -rf "$T/sysroot" "$T/lib/clang"
mkdir -p "$T/sysroot/usr/lib/aarch64-linux-android/$API" "$T/lib/clang/19/lib/linux/aarch64"
(cd "$P/sysroot/usr" && tar -cf - --exclude=include/arm-linux-androideabi --exclude=include/i686-linux-android \
   --exclude=include/x86_64-linux-android --exclude=include/riscv64-linux-android include) | tar -xf - -C "$T/sysroot/usr"
L=$P/sysroot/usr/lib/aarch64-linux-android
cp "$L/libc++_shared.so" "$T/sysroot/usr/lib/aarch64-linux-android/"
for f in crtbegin_so.o crtend_so.o crtbegin_dynamic.o crtend_android.o libc.so libm.so libdl.so liblog.so libc++.so; do
  cp "$L/$API/$f" "$T/sysroot/usr/lib/aarch64-linux-android/$API/"
done
cp -a "$D/llvm-android/lib/clang/19/include" "$T/lib/clang/19/"
cp "$P/lib/clang/19/lib/linux/libclang_rt.builtins-aarch64-android.a" "$T/lib/clang/19/lib/linux/"
cp "$P/lib/clang/19/lib/linux/aarch64/libunwind.a" "$T/lib/clang/19/lib/linux/aarch64/"
du -sh "$T/sysroot" "$T/lib/clang"
