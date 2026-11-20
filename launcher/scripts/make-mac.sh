#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the macOS release: a zip whose root is Recompia-<version>/ holding the launcher binary Recompia, the payload
# make-payload.sh staged and the runtime dylib. No .app and no signature: the player unpacks the zip anywhere and starts the
# launcher from there, as on Windows and Linux, and the note in the zip walks them through the one-time Open Anyway macOS asks
# for. The package ships no game code: Set up lifts and compiles pkp from the player's own dump.
#
# Two zips of the same tree (release_names.rs in the launcher), in <play folder>:
#   Recompia-<v>-macos-aarch64.zip     Recompia-<v>/ with Recompia and the starter under the old name, PokopiaLauncher
#                                      (launcher/stub/old-name.sh)
#   PokopiaRecomp-<v>-macos-arm64.zip  the old-name copy: PokopiaRecomp-<v>/ with the full binary as PokopiaLauncher and no
#                                      starter, the only one a launcher from before the rename can match and install. It ships for
#                                      three releases after the rename.
#
#   launcher/scripts/make-mac.sh <play folder with Recompia> <payload dir> [version]
#
# Needs a production launcher build (npm run tauri build -- --no-bundle) and the runtime dylib (PKP_RT_DLL=ON, docs/macos-port.md).
set -eu
here=$(cd "${1:?usage: make-mac.sh <play folder> <payload dir> [version]}" && pwd)
payload=$(cd "${2:?usage: make-mac.sh <play folder> <payload dir> [version]}" && pwd)
repo=$(cd "$(dirname "$0")/../.." && pwd)
# the release version: the launcher's manifest, unless the caller names one (the release workflow passes the tag's)
version=${3:-$(sed -nE 's/^[[:space:]]*"version": "([^"]+)".*/\1/p' "$repo/launcher/src-tauri/tauri.conf.json" | head -1)}
name="Recompia-$version"
old_name="PokopiaRecomp-$version"

[ -x "$here/Recompia" ] || { echo "no launcher binary at $here/Recompia (npm run tauri build -- --no-bundle)" >&2; exit 1; }
[ -d "$payload/tools/build" ] || { echo "no payload at $payload (launcher/scripts/make-payload.sh)" >&2; exit 1; }
[ -f "$repo/work/build/rtb/libpkp_rt.dylib" ] || { echo "no runtime dylib at work/build/rtb (cmake --build --target pkp_rt)" >&2; exit 1; }

# The layout the launcher expects (paths::exe_dir): tools/ next to the binary, and the runtime dylib twice — beside the binary,
# as pkp_rt.dll sits beside Recompia.exe on Windows, and in tools/build/sdk, which is where Set up copies it from into the
# game folder.
stage=$(mktemp -d)
root="$stage/$name"
mkdir -p "$root/tools/build/sdk"
cp "$here/Recompia" "$root/Recompia"
install -m 755 "$repo/launcher/stub/old-name.sh" "$root/PokopiaLauncher"
cp -R "$payload/." "$root/"
# The launcher runs the apphost itself (host.rs: apphost.is_file() -> command(&apphost)), and a payload that came back through
# an artifact has no mode: without this the shipped zip carries tools/harness/spirv_harness as 0644 and Set up fails EACCES.
chmod +x "$root/tools/harness/spirv_harness"
cp "$repo/work/build/rtb/libpkp_rt.dylib" "$root/libpkp_rt.dylib"
cp "$repo/work/build/rtb/libpkp_rt.dylib" "$root/tools/build/sdk/libpkp_rt.dylib"
# The fetched per-machine tools (python-build-standalone, lz4, capstone) belong to a game folder: Set up fetches them there. A
# payload that came back from a build machine that ran the launcher carries one under tools/macos; in the release it is 78 MB
# that nothing reads (tools_dir_for() is the game folder). Bytecode caches are build leftovers as well.
rm -rf "$root/tools/macos"
find "$root" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true
# the licence texts and notices the README promises, with the root licence
sh "$repo/launcher/scripts/stage-licenses.sh" "$root/licenses" "$repo/work/licenses/THIRD_PARTY_CRATES.txt"
cp "$repo/LICENSE" "$root/LICENSE.txt"
# An artifact round trip leaves com.apple.provenance on the files; harmless, but no reason to ship it.
xattr -cr "$root" 2>/dev/null || true

# the note names the launcher file and folder of its zip
note() {
    cat > "$1/How to open Recompia.txt" <<TXT
Recompia is not notarized by Apple, so macOS refuses to open it the first time. This is expected.

1. Unpack the zip anywhere you like (the folder can stay where it is; nothing is installed).
2. Start $2 once and let macOS say it cannot be opened. Close that dialog.
3. Open System Settings, Privacy and Security, and scroll down to the Security section.
   There is a line about $2 being blocked: press "Open Anyway", then "Open".
4. From then on it opens like any other program.

On macOS 14 and older you can also right-click $2, choose Open, and confirm.

If System Settings shows no such line, remove the download flag in Terminal instead:

   xattr -dr com.apple.quarantine /path/to/$3

Licences: LICENSE.txt and the licenses folder.

The folder is the whole release: the launcher, tools/ (the lifter, the SDK and the shader translator), and libpkp_rt.dylib. A
game folder is made by the launcher's Set up from your own dump; no game code is shipped here.
TXT
}
note "$root" Recompia "$name"

# each launcher file is the one built for it: the binary under the new name, the starter script under the old one
cmp -s "$here/Recompia" "$root/Recompia" || { echo "Recompia is not the launcher just built" >&2; exit 1; }
cmp -s "$repo/launcher/stub/old-name.sh" "$root/PokopiaLauncher" || { echo "PokopiaLauncher is not the starter" >&2; exit 1; }
zip="$here/$name-macos-aarch64.zip"
rm -f "$zip"
ditto -c -k --sequesterRsrc --keepParent "$root" "$zip"
echo "built $zip"

# the old-name copy: the same tree under the old folder, the full binary under the old name, no starter
old_root="$stage/$old_name"
mv "$root" "$old_root"
rm -f "$old_root/Recompia"
cp "$here/Recompia" "$old_root/PokopiaLauncher"
xattr -c "$old_root/PokopiaLauncher" 2>/dev/null || true
note "$old_root" PokopiaLauncher "$old_name"
cmp -s "$here/Recompia" "$old_root/PokopiaLauncher" || { echo "the old-name copy's PokopiaLauncher is not the launcher just built" >&2; exit 1; }
zip="$here/$old_name-macos-arm64.zip"
rm -f "$zip"
ditto -c -k --sequesterRsrc --keepParent "$old_root" "$zip"
rm -rf "$stage"
echo "built $zip"
