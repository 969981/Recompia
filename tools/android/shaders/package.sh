#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packages the on-device shader setup for the APK:
#   <out>/jniLibs/arm64-v8a/libspirv_harness.so   tools/shaders/harness as a NativeAOT linux-bionic-arm64 executable (one file, ~5 MB)
#   <out>/assets/shaders/build_shaders.py         the driver (run by the on-device libpython_exec.so)
#   <out>/assets/shaders/extract_corpus.py        tools/shaders/ampere/extract_corpus.py
# A NativeAOT build, not a self-contained CoreCLR one: the APK can only execute files in nativeLibraryDir and the CoreCLR apphost wants
# its ~200 assemblies next to it; CoreCLR on Android also faults on bionic's tagged heap pointers (needs mallopt to turn tagging off).
# usage: package.sh <out dir> <work dir>   env: DOTNET (dotnet executable, SDK 10), RYUBING (Ryujinx checkout at release.yml's
#        RYUBING_COMMIT), ANDROID_NDK_HOME (r28c)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=$(mkdir -p "$2" && cd "$2" && pwd)
: "${DOTNET:?dotnet executable}" "${RYUBING:?Ryujinx checkout}" "${ANDROID_NDK_HOME:?NDK r28c}"
ndkbin=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin

# the harness sources, built out of the tree (the build writes obj/ and bin/ next to them)
rm -rf "$work/shaders" && mkdir -p "$work/shaders" "$work/bin"
(cd "$repo/tools/shaders" && tar --exclude=./build --exclude=./third_party -cf - .) | (cd "$work/shaders" && tar -xf -)
bash "$work/shaders/scripts/setup_ryujinx.sh" "$RYUBING"
# ILCompiler links with `clang` and strips with `llvm-objcopy`: the NDK's, aimed at Android API 30
printf '#!/bin/sh\nexec "%s/clang" --target=aarch64-linux-android30 "$@"\n' "$ndkbin" > "$work/bin/clang"
chmod +x "$work/bin/clang"
ln -sf "$ndkbin/llvm-objcopy" "$work/bin/llvm-objcopy"
# no path of this machine in the executable: the linker names the base version definition after its -o path unless a soname is
# given, and PathMap / deterministic builds keep source paths out of the managed metadata (checked below with strings)
cat > "$work/shaders/harness/Directory.Build.targets" <<'T'
<Project>
  <ItemGroup><LinkerArg Include="-Wl,-soname,libspirv_harness.so" /></ItemGroup>
</Project>
T
PATH="$work/bin:$PATH" DOTNET_CLI_TELEMETRY_OPTOUT=1 "$DOTNET" publish -c Release -r linux-bionic-arm64 \
    -p:Deterministic=true -p:ContinuousIntegrationBuild=true -p:PathMap="$work=." -p:StripSymbols=true \
    -p:PublishAot=true -p:DisableUnsupportedError=true -p:PublishAotUsingRuntimePack=true \
    -p:CppCompilerAndLinker=clang -p:ObjCopyName=llvm-objcopy \
    -p:SysRoot="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot" -m:6 \
    "$work/shaders/harness/SpirvHarness.csproj" -o "$work/publish"

mkdir -p "$out/jniLibs/arm64-v8a" "$out/assets/shaders"
cp "$work/publish/spirv_harness" "$out/jniLibs/arm64-v8a/libspirv_harness.so"
cp "$here/build_shaders.py" "$out/assets/shaders/"
cp "$repo/tools/shaders/ampere/extract_corpus.py" "$out/assets/shaders/"
ls -l "$out/jniLibs/arm64-v8a/libspirv_harness.so"
if strings -a "$out/jniLibs/arm64-v8a/libspirv_harness.so" | grep -E "$HOME|$work|/(home|Users)/"; then
    echo "error: a local path is in libspirv_harness.so" >&2; exit 1
fi
