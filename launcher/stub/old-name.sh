#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The launcher's old file name after the release files were renamed: PokopiaLauncher.AppImage on Linux, PokopiaLauncher on macOS,
# shipped for good beside the new launcher (make-linux.sh, make-mac.sh). Steam shortcuts not yet pointed at the new file, shortcuts
# made by hand and `--play` entries start this; it becomes the launcher beside it with the same arguments, so Steam sees the
# launcher itself (exec, same process). The Windows package has the same as an exe (launcher/stub).
self=$(readlink -f "$0" 2>/dev/null || printf '%s' "$0")
here=$(cd "$(dirname "$self")" && pwd -P)
case "$(uname -s)" in
    Darwin) launcher="$here/Recompia" ;;
    *) launcher="$here/Recompia.AppImage" ;;
esac
[ -x "$launcher" ] || { echo "$launcher is missing: download the latest release from https://github.com/Dittobyte/Recompia/releases" >&2; exit 127; }
exec "$launcher" "$@"
