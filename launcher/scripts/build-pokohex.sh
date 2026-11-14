#!/bin/sh
# Builds the Save Editor page's editor: PokoHex.Web (GPL-3.0, github.com/berichan/PokoHex) at the commit in pokohex/PIN, with
# pokohex/launcher-host.patch (its host mode: "Load Game save" / "Write to Game" through window.pokohexHost, src/pages/saveedit.js),
# published into public/pokohex/, which Vite copies into the launcher. Run it before `npm run tauri build`; a launcher built without it
# shows a note on the Save Editor page instead.
#
#   launcher/scripts/build-pokohex.sh          needs git and the .NET 10 SDK
set -eu
launcher=$(cd "$(dirname "$0")/.." && pwd)
pin=$(tr -d ' \r\n' < "$launcher/pokohex/PIN")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git -c core.autocrlf=false clone -q https://github.com/berichan/PokoHex "$work/src"
git -C "$work/src" -c advice.detachedHead=false checkout -q "$pin"
git -C "$work/src" apply --whitespace=nowarn "$launcher/pokohex/launcher-host.patch"
dotnet publish "$work/src/PokoHex.Web/PokoHex.Web.csproj" -c Release -o "$work/out" -p:CompressionEnabled=false \
    --source https://api.nuget.org/v3/index.json

rm -rf "$launcher/public/pokohex"
mkdir -p "$launcher/public"
cp -R "$work/out/wwwroot" "$launcher/public/pokohex"
cp "$work/src/LICENSE.txt" "$launcher/public/pokohex/LICENSE.txt"
echo "built $launcher/public/pokohex (PokoHex $pin)"
