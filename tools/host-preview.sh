#!/usr/bin/env bash
# PShare - Render the app's screens on the PC (Mesa surfaceless EGL) to PNG.
# Copyright (C) 2026 PShare contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/host-preview.sh [output dir] [width height]
#
# Builds the platform-neutral part of PShare for the PC and runs
# host/preview_main.cpp: a pretend console, a scripted walk through every
# page, one PNG per named step. Needs clang, ninja, libEGL/libGL (Mesa) and
# libcurl's development files.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
build="$root/build/host-preview"
ninja_begin "$build/build.ninja"

# Everything under src/ except the console's platform layer, runtime, entry
# point, elevation client and console-only curl glue.
sources=("$root/host/preview_main.cpp" "$root/host/platform_host.cpp")
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/src" -type f \( -name '*.cpp' -o -name '*.c' \) \
    ! -path "$root/src/platform/*" ! -path "$root/src/runtime/*" \
    ! -path "$root/src/elevation/*" ! -path "$root/src/net/*" \
    ! -name main.cpp -print0 | sort -z)

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then
        ninja_inputs=("$source" "$cc")
        ninja_edge CC "$object" "${compiler_cache[@]}" "$cc" -std=c11 -O2 -w -I"$root/src" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    else
        ninja_inputs=("$source" "$cxx")
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 -Wall -Wextra \
            -DGL_GLEXT_PROTOTYPES=1 -I"$root/src" -I"$root/host" $(pkg-config --cflags libavformat) \
            -MD -MF "$object.d" \
            -c "$source" -o "$object"
    fi
    objects+=("$object")
done
ninja_inputs=("${objects[@]}")
ninja_edge LINK "$build/pshare_preview" "$cxx" "${objects[@]}" -lEGL -lGL -lcurl \
    $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -lpthread -lm \
    -o "$build/pshare_preview"
if ! ninja_run >"$build/build.log" 2>&1; then
    grep -E 'error|FAILED' -A6 "$build/build.log" | head -80 >&2
    exit 1
fi
grep -E 'warning' -A4 "$build/build.log" | head -40 >&2 || true

output=${1:-"$root/build/preview"}
mkdir -p "$output"
# A made-up clip (moving colour bars and a tone, VP9 + Opus like a PS5 recording).
sample="$build/sample-clip.webm"
if [[ ! -s $sample ]] && command -v ffmpeg >/dev/null; then
    ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=8" \
        -f lavfi -i "sine=frequency=440:beep_factor=4:duration=8" \
        -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1500k -c:a libopus -b:a 96k \
        "$sample" || rm -f "$sample"
fi
# PSHARE_SAMPLE_CLIP set by the caller wins.
[[ -n ${PSHARE_SAMPLE_CLIP:-} || ! -s $sample ]] || export PSHARE_SAMPLE_CLIP="$sample"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    "$build/pshare_preview" "$root/assets" "$output" "${@:2}"
