#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The on-device Python for the lifter: python.org's CPython 3.14 for Android (libpython + stdlib), a launcher executable,
# capstone 5.0.9 (its own ctypes binding, libcapstone.so built with the NDK, AArch64 only) and python-lz4 4.4.5 (the C
# extensions built with the NDK against that Python), plus the _multiprocessing stand-in the lifter's Pool needs.
# Output: $D/stage/python (the PYTHONHOME tree) and $D/stage/jni/libpython_exec.so.
#   D=<scratch dir> ./build-python.sh
set -euo pipefail
: "${D:?set D}" "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
HERE=$(cd "$(dirname "$0")" && pwd)
PYVER=3.14.10 PYMM=3.14 CSVER=5.0.9 LZ4VER=4.4.5
TC=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
CC="$TC/clang --target=aarch64-linux-android30"
STRIP=$TC/llvm-strip
LDF="-Wl,-z,max-page-size=16384"
mkdir -p "$D/dl" "$D/src"
get() { [ -f "$D/dl/$2" ] || curl -fsSL -o "$D/dl/$2" "$1"; }
get "https://www.python.org/ftp/python/$PYVER/python-$PYVER-aarch64-linux-android.tar.gz" python-$PYVER-aarch64-linux-android.tar.gz
get "https://files.pythonhosted.org/packages/source/c/capstone/capstone-$CSVER.tar.gz" capstone-$CSVER.tar.gz
get "https://files.pythonhosted.org/packages/source/l/lz4/lz4-$LZ4VER.tar.gz" lz4-$LZ4VER.tar.gz

S=$D/stage
rm -rf "$S/python" "$S/jni/libpython_exec.so" "$D/src/pyprefix"
mkdir -p "$S/python" "$S/jni" "$D/src/pyprefix"
tar -xzf "$D/dl/python-$PYVER-aarch64-linux-android.tar.gz" -C "$D/src/pyprefix" ./prefix
P=$D/src/pyprefix/prefix
INC=$P/include/python$PYMM

# the interpreter tree: libpython and the libraries its stdlib modules need, the stdlib without tests and GUI/dev parts
mkdir -p "$S/python/lib"
cp -a "$P"/lib/libpython$PYMM.so "$P"/lib/libpython3.so "$P"/lib/lib*_python.so "$S/python/lib/"
(cd "$P/lib" && tar -cf - --exclude=test --exclude=tests --exclude=idlelib --exclude=tkinter --exclude=turtledemo \
   --exclude=ensurepip --exclude=__pycache__ --exclude='_test*' --exclude='xxlimited*' --exclude=_xxtestfuzz* \
   --exclude=_ctypes_test* --exclude=lib2to3 --exclude=pydoc_data --exclude='config-*' python$PYMM) | tar -xf - -C "$S/python/lib"
SP=$S/python/lib/python$PYMM/site-packages
mkdir -p "$SP"

# launcher
$CC -O2 -fPIE -pie $LDF -I"$INC" "$HERE/python/launcher.c" -L"$P/lib" -lpython$PYMM -o "$S/jni/libpython_exec.so"

# capstone: C library (AArch64 only) + the pure-Python binding of the same release
rm -rf "$D/src/capstone-$CSVER" && tar -xzf "$D/dl/capstone-$CSVER.tar.gz" -C "$D/src"
CS=$D/src/capstone-$CSVER
cmake -S "$CS/src" -B "$CS/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DBUILD_STATIC_LIBS=OFF -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF \
  -DCAPSTONE_ARM64_SUPPORT=ON -DCMAKE_SHARED_LINKER_FLAGS="$LDF" >/dev/null
ninja -C "$CS/build" >/dev/null
cp -a "$CS/capstone" "$SP/capstone"
mkdir -p "$SP/capstone/lib"
$STRIP -o "$SP/capstone/lib/libcapstone.so" "$(readlink -f "$CS/build/libcapstone.so")"

# python-lz4: the two C extensions the lifter's import needs (lz4, lz4.block), with the bundled lz4 sources
rm -rf "$D/src/lz4-$LZ4VER" && tar -xzf "$D/dl/lz4-$LZ4VER.tar.gz" -C "$D/src"
L=$D/src/lz4-$LZ4VER
EXT=cpython-${PYMM/./}-aarch64-linux-android.so
mkdir -p "$SP/lz4/block"
cp "$L/lz4/__init__.py" "$L/lz4/version.py" "$SP/lz4/"
cp "$L/lz4/block/__init__.py" "$SP/lz4/block/"
for m in _version block/_block; do
  $CC -O2 -fPIC -shared $LDF -I"$INC" -I"$L/lz4libs" "$L/lz4/$m.c" "$L/lz4libs/lz4.c" "$L/lz4libs/lz4hc.c" \
    -L"$P/lib" -lpython$PYMM -o "$SP/lz4/$m.$EXT"
  $STRIP "$SP/lz4/$m.$EXT"
done

# Android stand-ins (see the files)
cp "$HERE/python/_multiprocessing.py" "$HERE/python/_posixshmem.py" "$HERE/python/sitecustomize.py" "$SP/"
du -sh "$S/python" "$S/jni/libpython_exec.so"
