#!/usr/bin/env bash
# ps5-native-app-boilerplate - List what PacBrew archives need that nothing provides.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PacBrew's archives are built for the payload SDK's libc; a native title links
# against the console's import stubs. The linker names missing symbols one at
# a time. This lists them all at once:
#
#   undefined in the archives
#   minus defined in the archives themselves (they call each other)
#   minus exported by the SDK's import stubs
#   minus defined by the app's own objects
#   = what the app has to provide (a stand-in, as in console_curl.c)
#
# Then it lists the symbols that do resolve, but only through
# libScePosixForWebKit: a module a native title does not load, so the call
# would jump to address 0 at run time. Those need a definition in the app too.
#
# usage: tools/find-missing-symbols.sh [ARCHIVE...]
#   ARCHIVE   names under the PacBrew lib folder; default: libcurl and what it
#             links (libcurl.a libssl.a libcrypto.a libz.a libzstd.a libpsl.a)
#   APP_OBJECTS  environment: the app's objects, default build/obj (after a build)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
nm_tool=${NM:-$(command -v llvm-nm-18 || command -v llvm-nm || true)}
[[ -n $nm_tool ]] || { echo "llvm-nm is required" >&2; exit 2; }

bash "$root/tools/setup-native-dependencies.sh" >/dev/null
stubs="$root/.deps/native/ps5-payload-sdk/target/lib"
lib="$(bash "$root/tools/setup-pacbrew-dependencies.sh" --all)/user/homebrew/lib"

names=("$@")
(( ${#names[@]} > 0 )) || names=(libcurl.a libssl.a libcrypto.a libz.a libzstd.a libpsl.a)
archives=()
for name in "${names[@]}"; do
    [[ $name =~ ^[A-Za-z0-9_.+-]+\.a$ && -f $lib/$name ]] || {
        echo "no such PacBrew archive: $name" >&2; exit 2;
    }
    archives+=("$lib/$name")
done

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# Names only, sorted and unique.
symbols() { awk 'NF { print $NF }' | LC_ALL=C sort -u; }
# A stub exports through its dynamic table.
exported() { "$nm_tool" --defined-only -D "$1" 2>/dev/null | awk 'NF >= 2 { print $NF }'; }

"$nm_tool" -u "${archives[@]}" 2>/dev/null | symbols > "$work/needed"
"$nm_tool" --defined-only "${archives[@]}" 2>/dev/null | symbols > "$work/archives"

: > "$work/webkit"
for stub in "$stubs"/*.so; do
    if [[ ${stub##*/} == libScePosixForWebKit.so ]]; then
        exported "$stub" >> "$work/webkit"
    else
        exported "$stub"
    fi
done | LC_ALL=C sort -u > "$work/stubs"
LC_ALL=C sort -u -o "$work/webkit" "$work/webkit"

objects=${APP_OBJECTS:-$root/build/obj}
mapfile -d '' app_objects < <(find "$objects" -type f -name '*.o' -print0 2>/dev/null)
if (( ${#app_objects[@]} > 0 )); then
    "$nm_tool" --defined-only "${app_objects[@]}" 2>/dev/null | symbols > "$work/app"
else
    : > "$work/app"
    echo "note: no app objects under $objects; nothing the app defines is subtracted" >&2
fi

LC_ALL=C comm -23 "$work/needed" "$work/archives" > "$work/outside"

echo "== provided by nothing (the link fails; write a stand-in):"
LC_ALL=C comm -23 "$work/outside" "$work/stubs" | LC_ALL=C comm -23 - "$work/webkit" |
    LC_ALL=C comm -23 - "$work/app"

echo
echo "== provided only by libScePosixForWebKit (links, then jumps to 0; define it in the app):"
LC_ALL=C comm -12 "$work/outside" "$work/webkit" | LC_ALL=C comm -23 - "$work/stubs" |
    LC_ALL=C comm -23 - "$work/app"
