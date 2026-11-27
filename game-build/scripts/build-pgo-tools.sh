#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The optimized build's profile tools for the desktop packages (tools/build/pgo/tools, pkp_build.py profile_tools()), for the Zig
# the packages bundle (0.17.0: clang 22.1.8, raw profile version 10, indexed version 13):
#   <out>/<target>/libclang_rt.profile.a   LLVM's profile runtime, built here with zig cc from the pinned compiler-rt 22.1.8 sources
#                                          (compiler-rt/lib/profile, what Zig does not ship); the test version links it
#   <out>/<target>/llvm-profdata[.exe]     from the pinned official LLVM 22.1.8 release package (merges the session's raw counts)
#   <target>: windows-x64 (x86_64-windows-gnu, the mingw objects of the Windows package), linux-x64 (x86_64-linux-gnu)
# Every download is checked against its sha256. LLVM is Apache-2.0 WITH LLVM-exception (tools/release/licenses).
#   game-build/scripts/build-pgo-tools.sh <out> [<zig>]     (no <zig>: the pinned Zig is fetched)
# The downloads (about 3 GB) go to $PGO_DL (default <out>/.dl), so a cache can keep <out> without them.
set -euo pipefail
out=${1:?usage: build-pgo-tools.sh <out> [<zig>]}
mkdir -p "$out"
out=$(cd "$out" && pwd)
dl=${PGO_DL:-$out/.dl}
mkdir -p "$dl"
dl=$(cd "$dl" && pwd)
LLVM=22.1.8
REL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM

fetch() {   # <url> <sha256> <file>: the download, kept in .dl while its hash matches
  local url=$1 sum=$2 f=$dl/$3
  if [ ! -f "$f" ] || ! echo "$sum  $f" | sha256sum -c --status; then
    curl -fsSL --retry 3 -o "$f.part" "$url"
    echo "$sum  $f.part" | sha256sum -c --quiet
    mv "$f.part" "$f"
  fi
  echo "$f"
}

zig=${2:-}
if [ -z "$zig" ]; then   # the same pin as the Linux launcher's (launcher/src-tauri/src/host_tools.rs ZIG)
  z=$(fetch https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz \
      1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026 zig-x86_64-linux-0.17.0.tar.xz)
  [ -x "$dl/zig-x86_64-linux-0.17.0/zig" ] || tar -xJf "$z" -C "$dl"
  zig=$dl/zig-x86_64-linux-0.17.0/zig
fi
export ZIG_GLOBAL_CACHE_DIR=$dl/zig-cache ZIG_LOCAL_CACHE_DIR=$dl/zig-cache
"$zig" cc --version | grep -q "clang version $LLVM" || { echo "error: $zig is not clang $LLVM" >&2; exit 1; }

# the profile runtime: compiler-rt/lib/profile and its headers from the source package (only those are unpacked)
src=$(fetch "$REL/llvm-project-$LLVM.src.tar.xz" 922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888 llvm-project-$LLVM.src.tar.xz)
crt=$dl/llvm-project-$LLVM.src/compiler-rt
[ -d "$crt/lib/profile" ] || tar -xJf "$src" -C "$dl" "llvm-project-$LLVM.src/compiler-rt/lib/profile" "llvm-project-$LLVM.src/compiler-rt/include/profile"
for t in windows-x64:x86_64-windows-gnu linux-x64:x86_64-linux-gnu; do
  name=${t%%:*}; triple=${t#*:}
  o=$dl/obj-$name
  rm -rf "$o" && mkdir -p "$o" "$out/$name"
  # every source, as compiler-rt's CMake builds it: each platform file guards itself (InstrProfilingPlatformLinux.c, ...Windows.c)
  for f in "$crt"/lib/profile/*.c "$crt"/lib/profile/*.cpp; do
    lang=c; [ "${f##*.}" = cpp ] && lang=c++
    "$zig" cc -target "$triple" -O2 -g0 -w -fno-builtin -I "$crt/include" -DCOMPILER_RT_HAS_ATOMICS=1 \
      $([ "$name" = linux-x64 ] && echo -DCOMPILER_RT_HAS_FCNTL_LCK=1 -DCOMPILER_RT_HAS_UNAME=1) -x $lang -c "$f" -o "$o/$(basename "$f").o"
  done
  rm -f "$out/$name/libclang_rt.profile.a"
  "$zig" ar rcs "$out/$name/libclang_rt.profile.a" "$o"/*.o
done

# llvm-profdata from the official packages (only that file is unpacked)
w=$(fetch "$REL/clang+llvm-$LLVM-x86_64-pc-windows-msvc.tar.xz" d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234 clang+llvm-$LLVM-x86_64-pc-windows-msvc.tar.xz)
tar -xJf "$w" -O --wildcards '*/bin/llvm-profdata.exe' > "$out/windows-x64/llvm-profdata.exe"
l=$(fetch "$REL/LLVM-$LLVM-Linux-X64.tar.xz" df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384 LLVM-$LLVM-Linux-X64.tar.xz)
tar -xJf "$l" -O --wildcards '*/bin/llvm-profdata' > "$out/linux-x64/llvm-profdata"
chmod 755 "$out/linux-x64/llvm-profdata"
# a self-contained tool or nothing: an llvm-profdata that needs libLLVM.so would fail on the player's PC, not here
if readelf -d "$out/linux-x64/llvm-profdata" | grep -q 'NEEDED.*libLLVM'; then
  echo "error: the official linux llvm-profdata needs libLLVM" >&2; exit 1
fi
"$out/linux-x64/llvm-profdata" --version | grep -q "LLVM version $LLVM"
ls -l "$out"/*/
