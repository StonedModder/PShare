#!/usr/bin/env bash
# PShare - Build pshared.elf, the background uploader, with the PS5 payload SDK.
# Copyright (C) 2026 PShare contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/build-daemon.sh [output.elf]
#
# The daemon is a payload for the jailbreak's ELF loader, not a native title:
# it links the payload SDK's libc/libc++ and PacBrew's static libcurl
# (OpenSSL, zlib, zstd, libpsl), which are built for exactly that SDK.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-build/daemon/pshared.elf}
[[ $output == /* ]] || output="$root/$output"

eval "$(bash "$root/tools/setup-native-dependencies.sh" | grep '^SDK_ROOT=')"
sdk=${SDK_ROOT:?payload SDK missing; run make deps}
[[ -x $sdk/bin/prospero-clang++ ]] || { echo "the payload SDK has no prospero-clang++" >&2; exit 2; }

resolution=$(bash "$root/tools/setup-pacbrew-dependencies.sh" --resolve libcurl libsmb2)
mapfile -d '' -t curl_cflags < <(python3 -c \
    'import json,sys; sys.stdout.write("\0".join(json.loads(sys.argv[1])["cflags"]))' "$resolution")
mapfile -d '' -t curl_libs < <(python3 -c \
    'import json,sys; sys.stdout.write("\0".join(json.loads(sys.argv[1])["libs"]))' "$resolution")

build="$root/build/daemon"
mkdir -p "$build/obj" "$(dirname -- "$output")"

sources=("$root/daemon/main.cpp")
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/src/pshare/core" -name '*.cpp' -print0 | sort -z)

flags=(-std=c++20 -O2 -g0 -Wall -Wextra -Werror -Wno-unused-command-line-argument
    -DPSHARE_PAYLOAD=1 -DPSHARE_HAVE_SMB2=1 -I"$root/src" "${curl_cflags[@]}")

# Which build this is: the app compares it with the running pshared and
# replaces an older one (daemon/main.cpp, PSHARE_BUILD).
build_id=b$(cat "${sources[@]}" $(find "$root/src/pshare/core" -name '*.hpp' | sort) \
    $(find "$root/src/third_party/jxrlib" -type f | sort) |
    sha256sum | cut -c1-12)
main_object="$build/obj/daemon_main.cpp.o"
if [[ ! -f $build/build-id || $(<"$build/build-id") != "$build_id" ]]; then
    rm -f "$main_object"
fi

export PS5_PAYLOAD_SDK=$sdk
objects=()
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}
pids=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    objects+=("$object")
    if [[ ! -f $object || $source -nt $object || $0 -nt $object ]] ||
        find "$root/src/pshare/core" -name '*.hpp' -newer "$object" | grep -q .; then
        extra=()
        [[ $object == "$main_object" ]] && extra=(-DPSHARE_BUILD="$build_id")
        "$sdk/bin/prospero-clang++" "${flags[@]}" "${extra[@]}" -c "$source" -o "$object" &
        pids+=($!)
        if (( ${#pids[@]} >= jobs )); then
            wait "${pids[0]}"
            pids=("${pids[@]:1}")
        fi
    fi
done
# stb_image and jxrlib (core/picture.cpp) are vendored C; built as published, without warnings.
c_sources=("$root/src/third_party/stb_image/stb_image.c"
    "$root/src/third_party/stb_image/stb_image_write.c")
while IFS= read -r -d '' source; do
    c_sources+=("$source")
done < <(find "$root/src/third_party/jxrlib" -name '*.c' -print0 | sort -z)
for source in "${c_sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    objects+=("$object")
    if [[ ! -f $object || $source -nt $object ]]; then
        "$sdk/bin/prospero-clang" -std=c11 -O2 -g0 -w -I"$root/src" -I"$root/third_party/jxrlib" \
            -I"$root/third_party/jxrlib/common/include" -I"$root/third_party/jxrlib/image/sys" \
            -I"$root/third_party/jxrlib/jxrgluelib" -c "$source" -o "$object" &
        pids+=($!)
    fi
done
for pid in "${pids[@]}"; do wait "$pid"; done
printf '%s\n' "$build_id" > "$build/build-id"

"$sdk/bin/prospero-clang++" -pthread "${objects[@]}" "${curl_libs[@]}" -lSceNet -o "$output.tmp"
"$sdk/bin/prospero-strip" --strip-debug "$output.tmp" 2>/dev/null || true
mv -f "$output.tmp" "$output"
printf 'pshared: %s (%s bytes)\n' "$output" "$(stat -c %s "$output")"
