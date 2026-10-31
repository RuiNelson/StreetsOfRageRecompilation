#!/usr/bin/env bash
# Streets of Rage for the PlayStation 5 - CPython as a static library, inside the image.
#
#   build-cpython.sh DIR [JOBS]
#
# Leaves DIR/target-build/libpythonX.Y.a and pyconfig.h, and the sources in DIR/Python-<version>.
# A title cannot load a module, so the extension modules the AI imports are compiled into the
# library and the others are off. Same version and hash as cmake/CPythonRuntime.cmake. There is
# no PGO: its training run cannot execute on this machine.
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dir=${1:?usage: build-cpython.sh DIR [JOBS]}
jobs=${2:-$(nproc)}
version=$(sed -n 's/^set(SOR_CPYTHON_VERSION \(.*\))$/\1/p' "$ps5/../cmake/CPythonRuntime.cmake")
hash=$(sed -n 's/^set(SOR_CPYTHON_SHA256 \(.*\))$/\1/p' "$ps5/../cmake/CPythonRuntime.cmake")
abi=${version%.*}
source_tree="$dir/Python-$version"
stamp="$dir/target-build/.stamp"
recipe="$version $(cat "${BASH_SOURCE[0]}" "$ps5/tools/cpython-config.site" | sha256sum | cut -d' ' -f1)"

[[ -f $stamp && $(<"$stamp") == "$recipe" ]] && { echo "==> CPython $version is built"; exit 0; }

mkdir -p "$dir"
archive="$dir/Python-$version.tar.xz"
[[ -f $archive ]] || wget -q "https://www.python.org/ftp/python/$version/Python-$version.tar.xz" -O "$archive"
echo "$hash  $archive" | sha256sum --check --strict --quiet
if [[ ! -f $source_tree/.ps5 ]]; then
    rm -rf "$source_tree"
    tar -xf "$archive" -C "$dir"
    # configure only cross-builds for the systems it names. Threads, time and files are FreeBSD's here.
    sed -i 's|^\t\*-\*-linux-android\*)$|\t*-*-freebsd*)\n\t\tac_sys_system=FreeBSD\n\t\t;;\n&|' "$source_tree/configure"
    python3 - "$source_tree/configure" <<'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
marker = "\t*-*-cygwin*)\n\t\t_host_ident=\n"
assert marker in text
text = text.replace(marker, "\t*-*-freebsd*)\n\t\t_host_ident=$host_cpu\n\t\t;;\n" + marker, 1)
# Without _XOPEN_SOURCE: under it, these FreeBSD headers hide C11's static_assert from Python's headers.
assert "define_xopen_source=yes" in text
open(path, "w").write(text.replace("define_xopen_source=yes", "define_xopen_source=no"))
EOF
    touch "$source_tree/.ps5"
fi

# The same version for this machine: a cross build runs it to freeze the core modules.
host="$dir/host-build"
if [[ ! -x $host/python ]]; then
    mkdir -p "$host"
    (cd "$host" && "$source_tree/configure" --without-ensurepip --disable-test-modules > configure.log 2>&1 &&
        make -j "$jobs" python > make.log 2>&1) || { cat "$host/configure.log" "$host/make.log" | tail -n 40 >&2; exit 1; }
fi

# What sor_ai imports (run its import under the desktop's CPython to list it again), and what
# they need. posix, time, _thread and the rest of the core are always in.
keep=" math _math_integer _struct _bisect _heapq _random _contextvars "
disabled=()
while read -r name; do
    [[ $keep == *" $name "* ]] || disabled+=("py_cv_module_$name=n/a")
done < <(grep -o 'py_cv_module_[A-Za-z0-9_]*' "$source_tree/configure" | sed 's/py_cv_module_//' | sort -u)

target="$dir/target-build"
rm -rf "$target"
mkdir -p "$target"
(
    cd "$target"
    CONFIG_SITE="$ps5/tools/cpython-config.site" MODULE_BUILDTYPE=static \
    CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" \
    RANLIB="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" READELF=llvm-readelf-21 \
    CFLAGS="-march=znver2 -fPIC -fno-omit-frame-pointer -ffunction-sections -fdata-sections -w" \
    "$source_tree/configure" --build="$("$source_tree/config.guess")" --host=x86_64-unknown-freebsd9 \
        --with-build-python="$host/python" --disable-shared --without-ensurepip --disable-test-modules \
        --disable-ipv6 --without-mimalloc --without-doc-strings --without-readline --with-suffix= --with-libm= \
        "${disabled[@]}" > configure.log 2>&1 || { tail -n 40 configure.log >&2; tail -n 60 config.log >&2; exit 1; }
    make -k -j "$jobs" "libpython$abi.a" > make.log 2>&1 || { grep -E 'error:|Error ' make.log | head -30 >&2; tail -n 15 make.log >&2; exit 1; }
)
echo "$recipe" > "$stamp"
echo "==> CPython $version: $target/libpython$abi.a"
