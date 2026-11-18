#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Licence notices for the libraries bundled inside the Linux AppImage (GTK, WebKitGTK, GLib, libsoup and the rest that
# linuxdeploy copies from the build machine). Run it on the runner that built the AppImage, after the build: the Debian/Ubuntu
# packages the libraries came from are installed there, so dpkg can name each one.
#
#   launcher/scripts/appimage-notices.sh <AppImage> <out dir>
#
# Writes into <out dir>: LIBRARIES.txt (every bundled ELF file, its package and version), SOURCES.txt (where the corresponding
# source is, and the written offer) and copyright/<package>.copyright (that package's /usr/share/doc/<package>/copyright).
# make-linux.sh puts the folder into the package as licenses/appimage/.
set -eu
ai=${1:?usage: appimage-notices.sh <AppImage> <out dir>}
out=${2:?usage: appimage-notices.sh <AppImage> <out dir>}
[ -f "$ai" ] || { echo "no AppImage at $ai" >&2; exit 1; }
command -v dpkg-query >/dev/null || { echo "dpkg-query is missing: this runs on the Debian/Ubuntu runner that built the AppImage" >&2; exit 1; }

ai=$(cd "$(dirname "$ai")" && pwd)/$(basename "$ai")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rm -rf "$out"
mkdir -p "$out/copyright"

# the AppDir, the way the AppImage itself holds it
(cd "$work" && APPIMAGE_EXTRACT_AND_RUN=1 "$ai" --appimage-extract >/dev/null)
appdir="$work/squashfs-root"
[ -d "$appdir/usr" ] || { echo "the AppImage has no usr/ after --appimage-extract" >&2; exit 1; }

# every regular file in the AppDir that is an ELF object (libraries, plugins, helper programs) except the launcher's own
# program in usr/bin
(cd "$appdir" && find usr lib lib64 -type f 2>/dev/null | LC_ALL=C sort) | while IFS= read -r rel; do
    case "$rel" in usr/bin/*) continue ;; esac
    if [ "$(head -c 4 "$appdir/$rel" | od -An -c | tr -d ' ')" = '177ELF' ]; then echo "$rel"; fi
done > "$work/elf.txt"
[ -s "$work/elf.txt" ] || { echo "no libraries found in the AppImage" >&2; exit 1; }

# the build machine's copy of a bundled file: the same path under /usr or /, else by the library name
host_path() {
    rel=$1
    sub=${rel#usr/}
    for c in "/usr/$sub" "/$sub"; do
        [ -e "$c" ] && { echo "$c"; return 0; }
    done
    b=$(basename "$rel")
    c=$(ldconfig -p 2>/dev/null | sed -n "s|^.* => \(.*/$b\)\$|\1|p" | head -1)
    [ -n "$c" ] && { echo "$c"; return 0; }
    find /usr/lib /lib -name "$b" -print -quit 2>/dev/null | grep . || return 1
}

# the package that owns a path (it may be listed under /lib or /usr/lib, and as the link or its target)
owner() {
    p=$1
    for c in "$p" "$(readlink -f "$p")" "/usr${p#/usr}" "${p#/usr}"; do
        pkg=$(dpkg-query -S "$c" 2>/dev/null | head -1 | sed 's/: .*//; s/,.*//') || true
        [ -n "$pkg" ] && { echo "$pkg"; return 0; }
    done
    return 1
}

: > "$work/rows.txt"
: > "$work/unmatched.txt"
while IFS= read -r rel; do
    if hp=$(host_path "$rel") && pkg=$(owner "$hp"); then
        ver=$(dpkg-query -W -f='${Version}' "$pkg")
        src=$(dpkg-query -W -f='${source:Package}' "$pkg")
        sver=$(dpkg-query -W -f='${source:Version}' "$pkg")
        printf '%s\t%s\t%s\t%s\t%s\n' "$rel" "$pkg" "$ver" "$src" "$sver" >> "$work/rows.txt"
    else
        echo "$rel" >> "$work/unmatched.txt"
    fi
done < "$work/elf.txt"

# an unmatched file is not an error here (the build can bundle a file no package owns); it is listed so it can be looked at
if [ -s "$work/unmatched.txt" ]; then
    echo "::warning::$(wc -l < "$work/unmatched.txt") bundled files belong to no installed package, see LIBRARIES.txt"
fi
[ -s "$work/rows.txt" ] || { echo "no bundled library could be matched to a package" >&2; exit 1; }

# each package's copyright file
cut -f2 "$work/rows.txt" | LC_ALL=C sort -u | while IFS= read -r pkg; do
    name=${pkg%%:*}
    f="/usr/share/doc/$name/copyright"
    [ -e "$f" ] || { echo "no copyright file for $pkg at $f" >&2; exit 1; }
    cp -L "$f" "$out/copyright/$name.copyright"
done

codename=$( . /etc/os-release && echo "${VERSION_CODENAME:-}")
distro=$( . /etc/os-release && echo "${NAME:-Ubuntu} ${VERSION_ID:-}")
arch=$(dpkg --print-architecture)
{
    echo "Libraries bundled in PokopiaLauncher.AppImage ($distro, $arch)"
    echo
    echo "AppImage path<TAB>package<TAB>package version<TAB>source package<TAB>source version"
    echo
    LC_ALL=C sort "$work/rows.txt"
    if [ -s "$work/unmatched.txt" ]; then
        echo
        echo "Files that no installed package owns (from the build, not from the distribution):"
        sed 's/^/  /' "$work/unmatched.txt"
    fi
} > "$out/LIBRARIES.txt"

{
    echo "Source of the libraries bundled in PokopiaLauncher.AppImage"
    echo "==========================================================="
    echo
    echo "PokopiaLauncher.AppImage carries the shared libraries listed in LIBRARIES.txt. They are unchanged copies of the"
    echo "$distro packages (architecture $arch) the AppImage was built on. Many of them (GTK, WebKitGTK, GLib, libsoup and others)"
    echo "are free software under the GNU Lesser General Public License, version 2, 2.1 or 3, or the GNU General Public License."
    echo "Each package's copyright and licence terms are in copyright/<package>.copyright; the LGPL and GPL texts are in the"
    echo "licenses folder above this one (LICENSE.AppImage-libraries.*). The libraries are separate files inside the AppImage:"
    echo "extract it with  ./PokopiaLauncher.AppImage --appimage-extract  and replace any of them with a version you built."
    echo
    echo "Corresponding source: the Ubuntu source packages below, at exactly these versions. Add the deb-src lines to your"
    echo "apt sources (or use https://packages.ubuntu.com and https://launchpad.net/ubuntu/+source/<name>), then run, for each:"
    echo
    cut -f4,5 "$work/rows.txt" | LC_ALL=C sort -u | while IFS="$(printf '\t')" read -r src sver; do
        echo "  apt-get source $src=$sver"
    done
    echo
    echo "Written offer: for three years from the date of the release this AppImage came with, Recompia will give anyone, on"
    echo "request, a copy of the complete corresponding source of any of these libraries, at no more than the cost of"
    echo "physically providing it. Write to legal.recompia@gmail.com and name the release and the library."
    echo
    echo "The source of Recompia itself (the launcher inside the AppImage) is the release tag in https://github.com/Dittobyte/Recompia."
    if [ -n "$codename" ]; then echo "Built on: $distro ($codename)."; fi
} > "$out/SOURCES.txt"
echo "wrote $out: $(wc -l < "$work/rows.txt") bundled files from $(cut -f2 "$work/rows.txt" | sort -u | wc -l) packages"
