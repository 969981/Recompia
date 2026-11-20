#!/bin/sh
# Builds the Linux release: the AppImage, the runtime and the build kit, staged from this checkout. Unlike the Windows
# zip, nothing here comes from another platform's build: the Linux package is what Set up needs on a Linux machine
# (the lifter, the SDK sources, the builder, the shader translator), and pkp is compiled by the player from their dump.
#
#   launcher/scripts/make-linux.sh <out dir> [version] [arch]
#
# Needs a production launcher build (npm run tauri build -- --bundles appimage), the runtime (PKP_RT_DLL=ON, see
# docs/linux-port.md) and the linux shader harness (tools/shaders, dotnet publish -r linux-<arch>) in build/.
#
# Two archives of the same tree (release_names.rs in the launcher), both left unpacked in <out dir> as well:
#   Recompia-<v>-linux-<x86_64|aarch64>.tar.gz   Recompia-<v>/ with Recompia.AppImage and the starter under the old name,
#                                                PokopiaLauncher.AppImage (launcher/stub/old-name.sh)
#   PokopiaRecomp-<v>-linux-<x64|arm64>.tar.gz   the old-name copy: PokopiaRecomp-<v>/ with the full AppImage as
#                                                PokopiaLauncher.AppImage and no starter, the only one a launcher from before the
#                                                rename can match and install. It ships for three releases after the rename.
set -eu
out=${1:?usage: make-linux.sh <out dir> [version] [arch]}
repo=$(cd "$(dirname "$0")/../.." && pwd)
version=${2:-$(sed -nE 's/^[[:space:]]*"version": "([^"]+)".*/\1/p' "$repo/launcher/src-tauri/tauri.conf.json" | head -1)}
arch=${3:-}
if [ -z "$arch" ]; then
    case "$(uname -m)" in
        aarch64|arm64) arch="arm64" ;;
        *) arch="x64" ;;
    esac
fi
case "$arch" in
    arm64) token="aarch64" ;;
    x64) token="x86_64" ;;
    *) echo "unknown arch $arch (x64 or arm64)" >&2; exit 1 ;;
esac
name="Recompia-$version"
old_name="PokopiaRecomp-$version"
pkg="$out/$name"

rt="$repo/build/rtdll/libpkp_rt.so"
[ -f "$rt" ] || { echo "no runtime at $rt (cmake -DPKP_RT_DLL=ON, target pkp_rt)" >&2; exit 1; }
appimage=$(ls "$repo"/launcher/src-tauri/target/release/bundle/appimage/*.AppImage 2>/dev/null | head -1)
[ -n "$appimage" ] || { echo "no AppImage (npm run tauri build -- --bundles appimage)" >&2; exit 1; }
harness="$repo/build/harness"
# Existence, not the mode: an uploaded artifact comes back 0644 (modes are not part of the transport), so the copy below is
# made executable instead. A win-x64 publish has spirv_harness.exe and no spirv_harness, which this still catches.
[ -f "$harness/spirv_harness" ] || { echo "no shader harness at $harness (dotnet publish -r linux-$arch)" >&2; exit 1; }

rm -rf "$pkg" "$out/$old_name"
mkdir -p "$pkg/tools/build/sdk" "$pkg/tools/shaders" "$pkg/assets" "$pkg/licenses"

# the programs
install -m 755 "$appimage" "$pkg/Recompia.AppImage"
install -m 755 "$repo/launcher/stub/old-name.sh" "$pkg/PokopiaLauncher.AppImage"
install -m 755 "$rt" "$pkg/libpkp_rt.so"
install -m 755 "$rt" "$pkg/tools/build/sdk/libpkp_rt.so"

# the build kit: what pkp_build.py runs from tools/build
for f in pkp_build.py dump_hashes.json fetch_dlss.py; do
    cp "$repo/game-build/release_b/$f" "$pkg/tools/build/$f"
done
cp -R "$repo/recomp" "$pkg/tools/build/recomp"
find "$pkg/tools/build/recomp" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true
# what the package ships under tools/build/recomp: pkp_build.py stamps a build by these files only, so a file an older release
# left in an updated install (an update never removes one) does not change the stamp
(cd "$pkg/tools/build/recomp" && find . -type f ! -name '*.pyc' | sed 's|^\./||' | LC_ALL=C sort) > "$pkg/tools/build/recomp.files"
cp "$repo/game-build/src/pkp_stub.cpp" "$repo/game-build/src/pkp_rt_api.h" "$pkg/tools/build/sdk/"

# the shader pipeline: the corpus extractor and a self-contained publish of the harness (its own .NET runtime)
cp "$repo/tools/shaders/ampere/extract_corpus.py" "$pkg/tools/shaders/"
cp -R "$harness" "$pkg/tools/harness"
chmod +x "$pkg/tools/harness/spirv_harness"   # the launcher runs the apphost itself, so it needs the mode
# the Save Editor's data tool (build-pokohex.sh --extractor linux-<arch> build/pokohex), when it was built
if [ -f "$repo/build/pokohex/pokohex-extract" ]; then
    cp -R "$repo/build/pokohex" "$pkg/tools/pokohex"
    chmod +x "$pkg/tools/pokohex/pokohex-extract"
fi

# what the game reads next to pkp: the keyboard and mouse prompt icons and the shared font (pkp_build.py copies both)
mkdir -p "$pkg/tools/build/sdk/kbm_icons" "$pkg/tools/build/sdk/fonts"
python3 "$repo/tools/kbm_icons.py" >/dev/null
cp "$repo/work/assets/kenney_input_prompts/Keyboard & Mouse/Default/"*.png "$pkg/tools/build/sdk/kbm_icons/"
cp "$repo/runtime/assets/fonts/DejaVuSans.ttf" "$pkg/tools/build/sdk/fonts/"
cp -R "$repo/runtime/assets/prompts" "$pkg/assets/prompts"

# licences and documents
sh "$repo/launcher/scripts/stage-licenses.sh" "$pkg/licenses" "$repo/build/licenses/THIRD_PARTY_CRATES.txt"
cp "$repo/game-build/release_b/BUNDLED_TOOLS.txt" "$pkg/licenses/"
# the notices of the libraries inside the AppImage: made on the runner that built it (appimage-notices.sh), where dpkg knows them
notices="$repo/build/appimage-notices"
[ -f "$notices/SOURCES.txt" ] || { echo "no AppImage library notices at $notices (launcher/scripts/appimage-notices.sh, see release.yml)" >&2; exit 1; }
mkdir -p "$pkg/licenses/appimage"
cp -R "$notices/." "$pkg/licenses/appimage/"
cp "$repo/LICENSE" "$pkg/LICENSE.txt"
cp "$repo/CHANGELOG.md" "$pkg/CHANGELOG.txt"
# the READMEs name the launcher file of their archive
readmes() {
    sed -e "s/@VERSION@/$version/g" -e "s/@LAUNCHER@/$2/g" "$repo/game-build/release_b/README.linux.txt" > "$1/README.txt"
    for t in "$repo"/game-build/release_b/README.linux.*.txt; do
        [ -f "$t" ] || continue
        lang=${t##*/README.linux.}
        sed -e "s/@VERSION@/$version/g" -e "s/@LAUNCHER@/$2/g" "$t" > "$1/README.${lang%.txt}.txt"
    done
}
readmes "$pkg" Recompia.AppImage

# each launcher file is the one built for it: the AppImage under the new name, the starter script under the old one
cmp -s "$appimage" "$pkg/Recompia.AppImage" || { echo "Recompia.AppImage is not the AppImage just built" >&2; exit 1; }
cmp -s "$repo/launcher/stub/old-name.sh" "$pkg/PokopiaLauncher.AppImage" || { echo "PokopiaLauncher.AppImage is not the starter" >&2; exit 1; }
tar -C "$out" -czf "$out/$name-linux-$token.tar.gz" "$name"
echo "built $out/$name-linux-$token.tar.gz"

# the old-name copy: the same tree under the old folder, the full AppImage under the old name, no starter
cp -a "$pkg" "$out/$old_name"
rm -f "$out/$old_name/Recompia.AppImage"
install -m 755 "$appimage" "$out/$old_name/PokopiaLauncher.AppImage"
readmes "$out/$old_name" PokopiaLauncher.AppImage
cmp -s "$appimage" "$out/$old_name/PokopiaLauncher.AppImage" || { echo "the old-name copy's PokopiaLauncher.AppImage is not the AppImage just built" >&2; exit 1; }
tar -C "$out" -czf "$out/$old_name-linux-$arch.tar.gz" "$old_name"
echo "built $out/$old_name-linux-$arch.tar.gz"
