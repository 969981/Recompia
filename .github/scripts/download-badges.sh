#!/usr/bin/env bash
# Turns the releases list (JSON, as GitHub's API returns it) into the small JSON files shields.io reads for the README badges.
# Usage: download-badges.sh releases.json out-dir
# Only the Windows zip and the Linux tarball are counted. SHA256SUMS.txt is left out, so the numbers are people, not files.
# Stable releases and Testing builds (prereleases) are counted together in win64/linux-x64/all, and apart in latest/testing.
set -euo pipefail
in=${1:?releases.json}
out=${2:?out-dir}
mkdir -p "$out"

num() {  # num JQ_FILTER -> a plain number
  jq -r "$1" "$in"
}

# Drafts are not public: leave them out. A platform file is a win64 zip or a linux-x64 tarball.
plat='[.assets[]? | select(.name | test("win64|linux-x64")) | .download_count] | add // 0'
win='[.assets[]? | select(.name | test("win64")) | .download_count] | add // 0'
lin='[.assets[]? | select(.name | test("linux-x64")) | .download_count] | add // 0'

win_all=$(num "[.[] | select(.draft | not) | $win] | add // 0")
lin_all=$(num "[.[] | select(.draft | not) | $lin] | add // 0")
all=$((win_all + lin_all))
latest=$(num "[.[] | select((.draft | not) and (.prerelease | not))] | first | if . == null then 0 else ($plat) end")
test_n=$(num "[.[] | select((.draft | not) and .prerelease) | $plat] | add // 0")
test_tag=$(num "[.[] | select((.draft | not) and .prerelease)] | first | .tag_name // empty")

fmt() { awk -v n="$1" 'BEGIN { if (n >= 1000) printf "%.1fk", n / 1000; else print n }'; }

badge() {  # badge FILE LABEL MESSAGE COLOR
  jq -n --arg l "$2" --arg m "$3" --arg c "$4" '{schemaVersion: 1, label: $l, message: $m, color: $c}' > "$out/$1"
}

badge win64.json downloads "$(fmt "$win_all")" 3fb950
badge linux-x64.json downloads "$(fmt "$lin_all")" 3fb950
badge all.json downloads "$(fmt "$all")" 3fb950
badge latest.json downloads "$(fmt "$latest")" 3fb950
if [ -n "$test_tag" ]; then
  badge testing.json testing "$test_tag, $(fmt "$test_n")" d29922
else
  badge testing.json testing "no builds yet" 6e7681
fi

echo "win64: $win_all  linux-x64: $lin_all  all: $all  latest stable: $latest  testing: ${test_tag:-none} $test_n"
