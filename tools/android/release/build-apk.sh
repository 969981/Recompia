#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The Android APK from a clean checkout: every part the app carries is built here, placed where the Gradle project reads it, and
# the release APK is assembled and checked. Nothing of the game goes in: the game library is built on the phone from the player's own
# dump (tools/android/ondevice), and check_no_game_data below stops the build if a game file is found in the APK or in the kit.
#
#   D=<scratch dir> ANDROID_NDK_HOME=<ndk r28c> DOTNET=<dotnet 10> RYUBING=<Ryujinx checkout> VERSION=<x.y.z> tools/android/release/build-apk.sh
#
# D                 downloads and build trees (outside the repo is better); an LLVM already built there (build-llvm.sh) is reused
# VERSION           the release version (0.0.0 or unset: the app keeps the version of its build file)
# NXRT_DEPS_DIR     optional: the runtime's third-party source cache
# RECOMPIA_SIGNING_DIR  the folder with release.jks and passwords.txt (app/build.gradle.kts). Unset: ALLOW_DEBUG_SIGNED=1 signs
#                   with the debug key on purpose (a build that is not published); otherwise the build stops.
# Result: $D/out/Recompia-<version>-android-arm64.apk
#
# The parts, and where the app expects them (game-build/android/app/src/main, all git-ignored):
#   jniLibs/arm64-v8a  libpkp_rt.so libSDL3.so libc++_shared.so  the runtime, SDL and the NDK's C++ library
#                      libadrenotools.so + its four hook libraries  loaded only for a player-installed GPU driver
#                      libpython_exec.so libllvm_exec.so          the on-device build's Python and clang/lld
#                      libspirv_harness.so                        the shader translator
#   assets             recompia-toolchain.tar.gz + .version       the on-device build kit
#                      gpu/turnip.zip + gpu/NOTICE.txt            Mesa Turnip
#                      shaders/build_shaders.py extract_corpus.py the on-device shader setup
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../../.." && pwd)
: "${D:?set D to the scratch folder}" "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME (NDK r28c)}" "${DOTNET:?dotnet executable (SDK 10)}" "${RYUBING:?Ryujinx checkout}"
VERSION=${VERSION:-0.0.0}
JOBS=${JOBS:-$(nproc)}
export D ANDROID_NDK_HOME JOBS
mkdir -p "$D"
D=$(cd "$D" && pwd)
ndkbin=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
app=$repo/game-build/android
main=$app/app/src/main
abi=arm64-v8a

step() { printf '\n==== %s ====\n' "$*"; }

# A game file in what we publish stops the build: the game's own archives and executables by extension, a dump's folders, the
# game library, and the exefs module names. Takes a listing (one path per line) on stdin.
check_no_game_data() {
    local what=$1 bad
    bad=$(grep -iE '(^|/)(exefs|romfs)(/|$)|\.(nso|nca|nsp|xci|nup|npdm|bfres|bntx|sarc|szs|bwav|bfstm|bars)$|(^|/)libpkpgame\.so$|(^|/)(main|rtld|sdk|subsdk[0-9])$' || true)
    if [ -n "$bad" ]; then
        echo "error: $what holds game files:" >&2
        printf '%s\n' "$bad" | head -20 >&2
        exit 1
    fi
    echo "$what: no game files"
}

step "on-device toolchain: clang + lld"
if [ -x "$D/llvm/bin/llvm" ] && [ -d "$D/llvm-android/lib/clang/19/include" ]; then
    echo "reusing $D/llvm/bin/llvm"
else
    bash "$repo/tools/android/ondevice/build-llvm.sh"
fi

step "on-device toolchain: Python, sysroot"
bash "$repo/tools/android/ondevice/build-python.sh"
bash "$repo/tools/android/ondevice/make-sysroot.sh"

step "runtime: libpkp_rt.so, libSDL3.so, driver loader"
rt=$D/rt
(cd "$repo/game-build" && cmake --preset android-arm64-rt -B "$rt" "-DPKP_VERSION=$VERSION" \
    ${NXRT_DEPS_DIR:+"-DNXRT_DEPS_DIR=$NXRT_DEPS_DIR"} ${PYTHON:+"-DPython3_EXECUTABLE=$PYTHON"} \
    ${CMAKE_LAUNCHER:+"-DCMAKE_C_COMPILER_LAUNCHER=$CMAKE_LAUNCHER" "-DCMAKE_CXX_COMPILER_LAUNCHER=$CMAKE_LAUNCHER"})
cmake --build "$rt" --target pkp_rt SDL3-jar -j "$JOBS"
one() {   # the one file of that name under the runtime build
    local f
    f=$(find "$rt" -name "$1" -type f -not -path '*/CMakeFiles/*' | head -n 1)
    [ -n "$f" ] || { echo "error: the runtime build made no $1" >&2; exit 1; }
    printf '%s' "$f"
}
sdl_java=$(find "$rt" -type d -path '*/android-project/app/src/main/java' | head -n 1)
[ -f "$sdl_java/org/libsdl/app/SDLActivity.java" ] || { echo "error: no SDL Java sources under $rt" >&2; exit 1; }

# the SDK the kit links the game library with (package.sh, as make-release.ps1 lays out tools/build/sdk on the desktop)
sdk=$D/a_out/sdk
rm -rf "$D/a_out" && mkdir -p "$sdk"
cp -p "$repo/game-build/src/pkp_stub.cpp" "$repo/game-build/src/pkp_stub_android.cpp" "$repo/game-build/src/pkpgame_android.ver" \
      "$repo/game-build/src/pkp_rt_api.h" "$sdk/"
cp -p "$(one guest_base.txt)" "$sdk/"

step "on-device build kit"
# package.sh refuses a kit that names this machine's home folder. HOME is given as a folder under $D for that check: python.org's
# own Android build was made under /home/runner, which is also a CI runner's home, and $D (checked as the stage path) is where
# everything of this build lives.
mkdir -p "$D/home"
HOME=$D/home A_OUT=$D/a_out bash "$repo/tools/android/ondevice/package.sh"

step "Turnip"
MESA_OUT=$D/turnip
bash "$repo/tools/android/drivers/build_turnip.sh" "$MESA_OUT"

step "shader translator"
DOTNET=$DOTNET RYUBING=$RYUBING bash "$repo/tools/android/shaders/package.sh" "$D/shaders-out" "$D/shaders-work"

step "app inputs"
rm -rf "$main/jniLibs" "$main/assets/gpu" "$main/assets/shaders" "$main"/assets/recompia-toolchain*
mkdir -p "$main/jniLibs/$abi" "$main/assets/gpu" "$main/assets/shaders"
for l in libpkp_rt.so libSDL3.so libadrenotools.so libhook_impl.so libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so; do
    "$ndkbin/llvm-strip" --strip-unneeded -o "$main/jniLibs/$abi/$l" "$(one "$l")"
done
cp "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$main/jniLibs/$abi/"
cp "$D/out/jniLibs/$abi/libpython_exec.so" "$D/out/jniLibs/$abi/libllvm_exec.so" "$D/shaders-out/jniLibs/$abi/libspirv_harness.so" "$main/jniLibs/$abi/"
cp "$D/out/assets/recompia-toolchain.tar.gz" "$D/out/assets/recompia-toolchain.version" "$main/assets/"
cp "$D/shaders-out/assets/shaders/build_shaders.py" "$D/shaders-out/assets/shaders/extract_corpus.py" "$main/assets/shaders/"
turnip_zip=$(ls "$MESA_OUT"/turnip-*.zip | head -n 1)
cp "$turnip_zip" "$main/assets/gpu/turnip.zip"
cp "${turnip_zip%.zip}.NOTICE.txt" "$main/assets/gpu/NOTICE.txt"
ls -l "$main/jniLibs/$abi" "$main/assets" "$main/assets/gpu" "$main/assets/shaders"
# every library the APK loads or runs is built for 16 KiB pages
for f in "$main/jniLibs/$abi"/*.so; do
    al=$("$ndkbin/llvm-readelf" -lW "$f" | awk '$1 == "LOAD" { print $NF }' | sort -u | tr '\n' ' ')
    case " $al" in *" 0x1000 "*) echo "error: $(basename "$f") has a 4 KiB segment alignment ($al)" >&2; exit 1 ;; esac
done
tar -tzf "$main/assets/recompia-toolchain.tar.gz" | check_no_game_data "the on-device kit"

step "APK"
set -- assembleRelease --no-daemon --console=plain "-Precompia.sdlJavaDir=$sdl_java"
case "$VERSION" in 0.0.0) ;; [0-9]*.[0-9]*.[0-9]*) set -- "$@" "-Precompia.versionName=${VERSION%%-*}" ;; esac
if [ -z "${RECOMPIA_SIGNING_DIR:-}" ]; then
    [ "${ALLOW_DEBUG_SIGNED:-0}" = 1 ] || { echo "error: no RECOMPIA_SIGNING_DIR (set ALLOW_DEBUG_SIGNED=1 for a build that is not published)" >&2; exit 1; }
    set -- "$@" -PallowDebugSignedRelease=true
fi
(cd "$app" && ANDROID_NDK_HOME=$ANDROID_NDK_HOME ./gradlew "$@")
apk=$(ls "$app"/app/build/outputs/apk/release/*.apk | head -n 1)
unzip -Z1 "$apk" | check_no_game_data "the APK"
for need in lib/$abi/libpkp_rt.so lib/$abi/libSDL3.so lib/$abi/libc++_shared.so lib/$abi/libpython_exec.so lib/$abi/libllvm_exec.so \
            lib/$abi/libspirv_harness.so lib/$abi/libadrenotools.so assets/recompia-toolchain.version assets/gpu/turnip.zip \
            assets/shaders/build_shaders.py assets/prompts/prompt_icons.pak assets/dump_hashes.json; do
    unzip -Z1 "$apk" | grep -qxF "$need" || { echo "error: the APK has no $need" >&2; exit 1; }
done
# the kit: the packager stores a .gz asset under its name without .gz (the app looks for both names)
unzip -Z1 "$apk" | grep -qE '^assets/recompia-toolchain\.tar(\.gz)?$' || { echo "error: the APK has no on-device kit archive" >&2; exit 1; }
mkdir -p "$D/out"
out=$D/out/Recompia-$VERSION-android-arm64.apk
cp "$apk" "$out"
ls -l "$out"
sha256sum "$out"
