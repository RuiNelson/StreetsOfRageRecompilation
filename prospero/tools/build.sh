#!/usr/bin/env bash
# Streets of Rage for the PlayStation 5 - build the title, inside the image (scripts/build_ps5.py).
#
#   build.sh [JOBS]
#
# The build tree is on the /build volume; the title folder goes to dist-ps5/.
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
jobs=${1:-$(nproc)}
build=/build/sor
export PS5_DIST_DIR="$root/dist-ps5"

mkdir -p /build/deps "$build"
sdl=$(bash "$ps5/tools/setup-sdl.sh" /build/deps/SDL)

if [[ ! -f $build/build.ninja ]]; then
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_PROJECT_StreetsOfRageRecompilation_INCLUDE="$ps5/cmake/ps5-sor.cmake" \
        -DPS5_VULKAN_DIR="$PS5_VULKAN_DIR" -DSDL_DIR="$sdl"
fi
ninja -C "$build" -j "$jobs" sor
