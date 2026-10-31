#!/usr/bin/env bash
# What the image must give a title's build (scripts/build_ps5.py --check). Links nothing yet:
# that takes the title's own recipe.
set -euo pipefail

vulkan=$PS5_VULKAN_DIR
sdk=$PS5_PAYLOAD_SDK

for file in \
    "$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" \
    "$vulkan/.deps/native/radv-release/include/vulkan/vulkan.h" \
    "$vulkan/build/host/ps5-native-tool" \
    "$vulkan/runtime/libc.prx" \
    "$vulkan/tools/radv-link.sh" \
    "$sdk/bin/prospero-clang++" \
    "$sdk/bin/prospero-lld" \
    "$sdk/target/lib/libps5platform.a" \
    "$("$PS5_CLANG" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; exit 1; }
done
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)

echo "host: $(uname -m)"
"$PS5_CLANG" --version | head -1
sed -n 's/^\(revision\|sdk\|assertions\): /radv \1: /p' "$vulkan/.deps/native/radv-release/PROVENANCE.txt"
echo "title sdk: $(cat "$sdk/.ps5-sdk-revision")"

# What sor's hand-written C++ uses of C++23 and the library.
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cat > "$work/check.cpp" <<'EOF'
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

static_assert(__cplusplus >= 202302L);

std::string check(const std::filesystem::path &path, std::span<const int> values) {
    std::thread thread([] {});
    thread.join();
    return std::format("{} {}", path.string(), std::ranges::max(values));
}
EOF
"$sdk/bin/prospero-clang++" -std=c++23 -march=znver2 -O2 -c "$work/check.cpp" -o "$work/check.o"
file "$work/check.o"
echo "toolchain ok"
