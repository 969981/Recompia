#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-builds clang + lld for aarch64-linux-android (they run ON the phone) as ONE static multicall binary (llvm-driver: clang, lld, llvm-ar):
# LLVM 19.1.7 (the NDK r28c clang is 19.0.1), AArch64 backend only, no zlib/zstd/terminfo/libxml2, libc++ linked statically.
# Two stages: host tablegen tools, then the cross build with the NDK. Output: $D/llvm/bin/llvm (stripped).
#   D=<scratch dir> JOBS=6 ./build-llvm.sh
# Needs ANDROID_NDK_HOME (r28c), cmake, ninja, curl, a host C++ compiler.
set -euo pipefail
: "${D:?set D to the scratch folder (downloads and build trees go there)}" "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
JOBS=${JOBS:-6}
VER=19.1.7
SRC=$D/src/llvm-project-$VER.src
mkdir -p "$D/dl" "$D/src"
if [ ! -d "$SRC" ]; then
  f=$D/dl/llvm-project-$VER.src.tar.xz
  [ -f "$f" ] || curl -fsSL -o "$f" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$VER/llvm-project-$VER.src.tar.xz"
  tar -xJf "$f" -C "$D/src"
fi

# stage 1: host tablegen tools only
H=$D/llvm-host
if [ ! -x "$H/bin/clang-tblgen" ]; then
  cmake -S "$SRC/llvm" -B "$H" -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
  ninja -C "$H" -j "$JOBS" llvm-tblgen llvm-min-tblgen clang-tblgen
fi

# stage 2: the phone's toolchain. Release (-O3), not MinSizeRel: the compiler's own speed is the on-device build's
# bottleneck; the size difference is a few MiB after strip.
X=$D/llvm-android
cmake -S "$SRC/llvm" -B "$X" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$D/llvm" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-android30 -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-unknown-linux-android30 \
  -DLLVM_TABLEGEN="$H/bin/llvm-tblgen" -DLLVM_MIN_TABLEGEN="$H/bin/llvm-min-tblgen" -DCLANG_TABLEGEN="$H/bin/clang-tblgen" \
  -DLLVM_NATIVE_TOOL_DIR="$H/bin" \
  -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON -DLLVM_DISTRIBUTION_COMPONENTS="clang;lld;llvm-ar" -DLLVM_BUILD_TOOLS=ON -DLLVM_BUILD_UTILS=OFF -DLLVM_INCLUDE_UTILS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
  -DCLANG_PLUGIN_SUPPORT=OFF -DCLANG_BUILD_TOOLS=OFF -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_BACKTRACES=OFF \
  -DCLANG_DEFAULT_LINKER=lld -DCLANG_DEFAULT_CXX_STDLIB=libc++ -DCLANG_DEFAULT_RTLIB=compiler-rt \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
ninja -C "$X" -j "$JOBS" llvm-driver clang-resource-headers
mkdir -p "$D/llvm/bin"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" -o "$D/llvm/bin/llvm" "$X/bin/llvm"
ls -l "$D/llvm/bin/llvm"
