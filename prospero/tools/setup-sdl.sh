#!/usr/bin/env bash
# Streets of Rage for the PlayStation 5 - SDL3 at a pinned release, patched for the console.
#
#   setup-sdl.sh DIR       fetch it into DIR (once a release), print the path
#
# patches/sdl.patch registers the console's video, audio and joystick drivers, which
# live in sdl/, and builds them when the toolchain says PS5. It is griches' for this release,
# so the console's SDL stays here while the desktop keeps MegaDriveEnvironment's pin.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdl=${1:?usage: setup-sdl.sh DIR}
tag=release-3.4.10
patch="$ps5/patches/sdl.patch"
stamp="$tag $(sha256sum "$patch" | cut -d' ' -f1)"

if [[ ! -f $sdl/.ps5-sdl || $(<"$sdl/.ps5-sdl") != "$stamp" ]]; then
    {
        if [[ ! -d $sdl/.git ]]; then
            git clone -q --depth 1 -b "$tag" https://github.com/libsdl-org/SDL.git "$sdl"
        fi
        git -C "$sdl" checkout -q -f HEAD -- .
        git -C "$sdl" apply --whitespace=nowarn "$patch"
    } >&2
    echo "$stamp" > "$sdl/.ps5-sdl"
fi
echo "$sdl"
