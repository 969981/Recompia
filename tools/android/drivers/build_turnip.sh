#!/usr/bin/env bash
# Builds Mesa's Turnip Vulkan driver (freedreno, KGSL kernel interface) for Android arm64 from an upstream Mesa release and
# packages it as a driver zip in the meta.json layout the Switch emulators use (package_driver.sh), so the same zip loads there.
#
# usage: build_turnip.sh [out_dir]          (default: tools/android/drivers/build, git-ignored)
# env:   MESA_VERSION (default below), MESA_SHA256 (checked when set; the default version's hash is built in), JOBS (default 6),
#        ANDROID_NDK_HOME (NDK r28c or newer), API (default 30: the app's minSdk),
#        ASSET_DIR (optional: also copy the zip as turnip.zip + NOTICE.txt there, e.g. the app's assets/gpu_drivers/)
# needs on the host: curl, tar, python3 with meson, ninja, mako and pyyaml (pip), flex, bison, patchelf, zip, glslangValidator (glslang-tools)
#
# The options follow the community build scripts (e.g. github.com/ilhan-athn7/freedreno_turnip-CI turnip_builder.sh) minus LTO:
# -Dplatforms=android -Dandroid-stub=true (link against stubs of the platform libraries, no AOSP tree needed),
# -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl (the Qualcomm kernel driver every stock Android phone runs), no gallium, no EGL.
# -Dandroid-strict=false: with the default (true) Mesa hides every KHR extension Android's CTS allows only from a newer API level
# than -Dplatform-sdk-version (src/vulkan/util/vk_extensions.py ALLOWED_ANDROID_VERSION): at API 30 that hides
# VK_KHR_pipeline_library (31), so VK_EXT_graphics_pipeline_library is unusable, and VK_KHR_maintenance5 (35). Measured on an
# Adreno 750 phone: the strict API 30 build made the runtime log "pipeline libraries off". A driver the player installs for this app is not
# the system driver CTS checks, and API 30 stays the link target so the library loads on every Android the app supports.
# Mesa is MIT licensed (docs/license.rst; per-file SPDX headers). The output is built from an unmodified upstream release.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "${1:-$here/build}" && cd "${1:-$here/build}" && pwd)
ver=${MESA_VERSION:-26.3.1}
[ "$ver" = 26.3.1 ] && : "${MESA_SHA256:=4e4e438eac7b840a9383988cbb7c08dad8041da544b9319fc26d319fa7348961}"
jobs=${JOBS:-6}
api=${API:-30}
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME (NDK r28c)}"
tc="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
for t in meson ninja flex bison patchelf zip curl glslangValidator; do command -v "$t" >/dev/null || { echo "missing host tool: $t" >&2; exit 1; }; done
python3 -c 'import mako, yaml' 2>/dev/null || { echo "python3 needs mako and pyyaml (pip install mako pyyaml)" >&2; exit 1; }

src="$out/mesa-$ver"
if [ ! -d "$src" ]; then
    tarball="$out/mesa-$ver.tar.xz"
    [ -f "$tarball" ] || curl -fL --retry 3 -o "$tarball" "https://archive.mesa3d.org/mesa-$ver.tar.xz"
    if [ -n "${MESA_SHA256:-}" ]; then echo "$MESA_SHA256  $tarball" | sha256sum -c -; else sha256sum "$tarball"; fi
    tar -C "$out" -xf "$tarball"
fi

cat > "$out/android-aarch64.ini" <<EOF
[binaries]
ar = '$tc/llvm-ar'
c = ['$tc/aarch64-linux-android$api-clang']
cpp = ['$tc/aarch64-linux-android$api-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$tc/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$out/no-pkgconfig', '/usr/bin/pkg-config']

[built-in options]
c_link_args = ['-Wl,-z,max-page-size=16384']
cpp_link_args = ['-Wl,-z,max-page-size=16384']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
mkdir -p "$out/no-pkgconfig"

bld="$out/build-$ver-api$api"
[ -f "$bld/build.ninja" ] || meson setup "$bld" "$src" --cross-file "$out/android-aarch64.ini" \
    -Dbuildtype=release -Dstrip=true \
    -Dplatforms=android -Dplatform-sdk-version="$api" -Dandroid-stub=true -Dandroid-strict=false -Dandroid-libbacktrace=disabled \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dvideo-codecs= -Dllvm=disabled -Dzstd=disabled
ninja -C "$bld" -j "$jobs" src/freedreno/vulkan/libvulkan_freedreno.so

lib="$bld/src/freedreno/vulkan/libvulkan_freedreno.so"
# NOTICE: the licence texts and every copyright line of the sources linked into the library (ninja's inputs of the target).
# Turnip and the Mesa code it uses are MIT; it also links Berkeley SoftFloat 3e (BSD-3-Clause, src/util/softfloat.c) and BLAKE3
# 1.8.2 (CC0-1.0 OR Apache-2.0, src/util/blake3).
notice="$out/turnip-$ver.NOTICE.txt"
ninja -C "$bld" -t commands src/freedreno/vulkan/libvulkan_freedreno.so | grep -o ' -c [^ ]*' | sed 's/^ -c //' | sort -u > "$out/sources.txt"
python3 -I - "$bld" "$out/sources.txt" "$src" "$ver" > "$notice" <<'PY'
import os, re, sys
bld, lst, src, ver = sys.argv[1:]
mit = open(os.path.join(src, "licenses/MIT")).read().split("License-Text:", 1)[1].strip()
sf = open(os.path.join(src, "src/util/softfloat.c")).read()
sf = "\n".join(l.lstrip(" *") for l in sf[sf.index("License for Berkeley"):sf.index("*/")].splitlines()).strip()
cr = set()
for f in open(lst).read().split():
    p = os.path.normpath(os.path.join(bld, f))
    if os.path.exists(p):
        for m in re.finditer(r"Copyright[^\n]*", open(p, errors="ignore").read(8000)):
            line = m.group(0).strip().rstrip("*/ ").strip()
            if re.search(r"\d{4}", line): cr.add(line)
print(f"Turnip Vulkan driver, built from the unmodified Mesa {ver} release (https://archive.mesa3d.org/mesa-{ver}.tar.xz)")
print("with tools/android/drivers/build_turnip.sh. Source: https://gitlab.freedesktop.org/mesa/mesa\n")
print("== MIT License (Mesa, Turnip, ir3, NIR, Vulkan runtime and utilities) ==\n" + mit + "\n")
print("Copyright notices of the sources in this library:\n" + "\n".join(sorted(cr)) + "\n")
print("== Berkeley SoftFloat Release 3e (src/util/softfloat.c, modified), BSD-3-Clause ==\n" + sf + "\n")
print("== BLAKE3 1.8.2 (src/util/blake3), used under CC0-1.0 (also offered under Apache-2.0) ==")
print("https://github.com/BLAKE3-team/BLAKE3")
PY
"$here/package_driver.sh" "$lib" "$out/turnip-$ver.zip" \
    --name "Turnip (Mesa $ver)" --vendor Mesa --driver-version "$ver" --author "Mesa contributors" \
    --description "Mesa $ver Turnip, unmodified upstream release, built with build_turnip.sh (API $api)" --min-api "$api" \
    --library vulkan.turnip.so --notice "$notice"
echo "built: $out/turnip-$ver.zip"
if [ -n "${ASSET_DIR:-}" ]; then
    mkdir -p "$ASSET_DIR" && cp "$out/turnip-$ver.zip" "$ASSET_DIR/turnip.zip" && cp "$notice" "$ASSET_DIR/NOTICE.txt"
    echo "copied to $ASSET_DIR/turnip.zip and NOTICE.txt"
fi
