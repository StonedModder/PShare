#!/usr/bin/env bash
# PShare - Build the host unit tests (sanitizers on).
# Copyright (C) 2026 PShare contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds build/tests/pshare_tests from tests/pshare_tests.cpp, the shared core
# (src/pshare/core) and the thumbnail code. Needs clang and libcurl's
# development files (the core links curl, the tests never use the network).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
build="$root/build/tests"
ninja_begin "$build/build.ninja"
read -r -a flags <<< "${HOST_TEST_CXXFLAGS:--std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-sanitize-recover=all}"

sources=("$root/tests/pshare_tests.cpp" "$root/src/app/thumbs.cpp" "$root/src/gfx/gl_batch.cpp"
    "$root/src/gfx/font.cpp" "$root/src/gfx/gl_program.cpp" "$root/src/gfx/draw_list.cpp"
    "$root/src/gfx/triangulate.cpp" "$root/host/platform_host.cpp")
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/src/pshare/core" -name '*.cpp' -print0 | sort -z)
c_sources=("$root/src/third_party/stb_image/stb_image.c" "$root/src/third_party/stb_image/stb_image_write.c")
while IFS= read -r -d '' source; do
    c_sources+=("$source")
done < <(find "$root/src/third_party/jxrlib" -name '*.c' -print0 | sort -z)
# jxrlib's own headers, for its wrappers (src/third_party/jxrlib).
jxr_includes=(-I"$root/third_party/jxrlib" -I"$root/third_party/jxrlib/common/include"
    -I"$root/third_party/jxrlib/image/sys" -I"$root/third_party/jxrlib/jxrgluelib")

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    ninja_inputs=("$source" "$cxx")
    ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" "${flags[@]}" -pthread \
        -DGL_GLEXT_PROTOTYPES=1 -I"$root/src" -MD -MF "$object.d" -c "$source" -o "$object"
    objects+=("$object")
done
for source in "${c_sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    ninja_inputs=("$source" "$cc")
    ninja_edge CC "$object" "${compiler_cache[@]}" "$cc" -std=c11 -O2 -w \
        -I"$root/src" "${jxr_includes[@]}" -MD -MF "$object.d" -c "$source" -o "$object"
    objects+=("$object")
done
ninja_inputs=("${objects[@]}" "$cxx")
# thumbs.cpp uploads through the kit's GL batch; the tests never reach it, so
# the GL library is linked only to resolve the names. Vendored C (stb) builds
# without sanitizers: it shifts signed values on purpose.
ninja_edge LINK "$build/pshare_tests" "$cxx" "${flags[@]}" -pthread "${objects[@]}" \
    -lcurl -lGL -o "$build/pshare_tests"
ninja_run
