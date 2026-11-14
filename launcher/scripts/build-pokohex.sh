#!/bin/sh
# Builds the Save Editor's editor: PokoHex (GPL-3.0, github.com/berichan/PokoHex) at the commit in pokohex/PIN, with
# pokohex/launcher.patch, which adds
#   - its host mode: "Load Game save" / "Write to Game" through window.pokohexHost (src/pages/saveedit.js);
#   - a build without the game's names, tables and pictures (-p:PokoHexGameData=false), which loads them instead from a folder;
#   - pokohex-extract, which writes that folder from the player's own game files (src-tauri/src/saveedit_data.rs runs it).
# PokoHex.Web is published into public/pokohex/, which Vite copies into the launcher; nothing in it comes from the game. Run it before
# `npm run tauri build`; a launcher built without it shows a note on the Save Editor page instead.
#
#   launcher/scripts/build-pokohex.sh                          the editor only; needs git and the .NET 10 SDK
#   launcher/scripts/build-pokohex.sh --extractor RID DIR      also pokohex-extract for RID (win-x64, linux-x64, linux-arm64,
#                                                              osx-arm64) as one self-contained file in DIR: the release's
#                                                              tools/pokohex/
set -eu
launcher=$(cd "$(dirname "$0")/.." && pwd)
rid= ; extractor_dir=
if [ "${1:-}" = "--extractor" ]; then
    [ $# -eq 3 ] || { echo "usage: $0 [--extractor RID DIR]" >&2; exit 2; }
    rid=$2
    mkdir -p "$3"
    extractor_dir=$(cd "$3" && pwd)
elif [ $# -ne 0 ]; then
    echo "usage: $0 [--extractor RID DIR]" >&2
    exit 2
fi
pin=$(tr -d ' \r\n' < "$launcher/pokohex/PIN")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
nuget="--source https://api.nuget.org/v3/index.json"

git -c core.autocrlf=false clone -q https://github.com/berichan/PokoHex "$work/src"
git -C "$work/src" -c advice.detachedHead=false checkout -q "$pin"
git -C "$work/src" apply --whitespace=nowarn "$launcher/pokohex/launcher.patch"
# shellcheck disable=SC2086
dotnet publish "$work/src/PokoHex.Web/PokoHex.Web.csproj" -c Release -o "$work/out" -p:CompressionEnabled=false \
    -p:PokoHexGameData=false $nuget

rm -rf "$launcher/public/pokohex"
mkdir -p "$launcher/public"
cp -R "$work/out/wwwroot" "$launcher/public/pokohex"
cp "$work/src/LICENSE.txt" "$launcher/public/pokohex/LICENSE.txt"
echo "built $launcher/public/pokohex (PokoHex $pin)"

if [ -n "$rid" ]; then
    # shellcheck disable=SC2086
    dotnet publish "$work/src/PokoHex.Extract/PokoHex.Extract.csproj" -c Release -r "$rid" --self-contained \
        -p:PublishSingleFile=true -p:PublishTrimmed=true -p:DebugType=none -o "$extractor_dir" $nuget
    cp "$work/src/LICENSE.txt" "$extractor_dir/LICENSE.txt"
    echo "built $extractor_dir (pokohex-extract for $rid)"
fi
