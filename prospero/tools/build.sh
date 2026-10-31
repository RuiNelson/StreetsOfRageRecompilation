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

bash "$ps5/tools/build-cpython.sh" /build/cpython "$jobs"

if [[ -f $build/CMakeCache.txt ]]; then
    cmake -S "$root" -B "$build" -DSOR_CPYTHON_PS5_DIR=/build/cpython
else
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_PROJECT_StreetsOfRageRecompilation_INCLUDE="$ps5/cmake/ps5-sor.cmake" \
        -DPS5_VULKAN_DIR="$PS5_VULKAN_DIR" -DSDL_DIR="$sdl" -DSOR_CPYTHON_PS5_DIR=/build/cpython
fi
ninja -C "$build" -j "$jobs" sor

# What goes to /data/sor beside the player's ROM and soundtrack: the stdlib and the AI, sources
# only, since Python writes its .pyc files there on the console.
data="$PS5_DIST_DIR/data-sor"
source_tree=$(ls -d /build/cpython/Python-*/ | head -1)
version=$(basename "$source_tree"); version=${version#Python-}
mkdir -p "$data/python/lib/python${version%.*}" "$data/ai-code/sor_ai"
rsync -a --delete --exclude=__pycache__ --exclude=/test --exclude=/idlelib --exclude=/tkinter --exclude=/turtledemo \
    --exclude=/ensurepip --exclude=/venv --exclude=/pydoc_data --exclude=/lib2to3 \
    "$source_tree/Lib/" "$data/python/lib/python${version%.*}/"
rsync -a --delete --exclude=__pycache__ "$root/../autoplay/ai-code/sor_ai/" "$data/ai-code/sor_ai/"
echo "==> /data/sor: $data ($(du -sh "$data" | cut -f1))"
