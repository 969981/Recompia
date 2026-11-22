#!/usr/bin/env bash
# Packages a Vulkan driver .so as a driver zip: <libraryName> + meta.json, the layout Eden, yuzu forks and the AdrenoTools
# driver repositories use, so the same zip installs in the emulators and in this app.
#
# usage: package_driver.sh <driver.so> <out.zip> [--name N] [--vendor V] [--driver-version X] [--author A] [--description D]
#                          [--min-api N] [--library soname] [--notice NOTICE.txt]
# The library's soname is set to --library (default vulkan.custom.so): the hook loads the driver by that file name.
set -euo pipefail
so=$1 zip_out=$(realpath -m "$2"); shift 2
name="Custom Vulkan driver" vendor="" dver="" author="" desc="" minapi=30 libname=vulkan.custom.so notice=""
while [ $# -gt 0 ]; do
    case "$1" in
        --name) name=$2;; --vendor) vendor=$2;; --driver-version) dver=$2;; --author) author=$2;;
        --description) desc=$2;; --min-api) minapi=$2;; --library) libname=$2;; --notice) notice=$2;;
        *) echo "unknown option $1" >&2; exit 1;;
    esac
    shift 2
done
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cp "$so" "$tmp/$libname"
patchelf --set-soname "$libname" "$tmp/$libname"
python3 -I - "$tmp/meta.json" "$name" "$desc" "$author" "$vendor" "$dver" "$minapi" "$libname" <<'PY'
import json, sys
p, name, desc, author, vendor, dver, minapi, lib = sys.argv[1:]
json.dump({"schemaVersion": 1, "name": name, "description": desc, "author": author, "packageVersion": "1", "vendor": vendor,
           "driverVersion": dver, "minApi": int(minapi), "libraryName": lib}, open(p, "w"), indent=2)
PY
rm -f "$zip_out"
[ -n "$notice" ] && cp "$notice" "$tmp/NOTICE.txt"
(cd "$tmp" && zip -q -9 "$zip_out" "$libname" meta.json $( [ -n "$notice" ] && echo NOTICE.txt ))
sha256sum "$zip_out"
