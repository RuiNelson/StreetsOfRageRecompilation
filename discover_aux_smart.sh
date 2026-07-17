#!/bin/bash
# Smart aux discovery: speculate → build → loop.
#
# 1. Scan ROM regions still unknown after the recompiler's static table-discovery
#    fixpoint for 68000 entry point candidates (speculative_scan.py), and write
#    them to code-analysis/speculative_addresses.txt.
# 2. Build once with all speculative stubs compiled in (--full --discover).
# 3. Run the game in a loop:
#      - Confirmed speculative addresses are appended to aux_addresses.txt on-the-fly
#        (no restart needed for those).
#      - Unknown non-speculative addresses exit 42: add to aux, rebuild, run again.
#      - exit 43 (stuck address): genuine state bug, stop.
#      - Ctrl+C or any other exit: stop.
#
# Compared to discover_aux_conservative.sh: fewer rebuild iterations because
# speculative stubs cover many jump-table targets that would otherwise each
# require a rebuild.
#
# Usage:
#   ./discover_aux_smart.sh
# Env overrides:
#   SOR_ROM=rom/SOR.bin
#   MAX_ITERS=500

set -uo pipefail
cd "$(dirname "$0")"

AUX="code-analysis/aux_addresses.txt"
SPEC="code-analysis/speculative_addresses.txt"
ROM="${SOR_ROM:-rom/SOR.bin}"
BIN="build/sor"
RAGE_DECOMPILER_DIR="$PWD/../RageDecompiler"
if command -v python3.14 >/dev/null 2>&1; then
    PYTHON_BIN="python3.14"
elif command -v python3.13 >/dev/null 2>&1; then
    PYTHON_BIN="python3.13"
else
    PYTHON_BIN="${PYTHON:-python3}"
fi
MAX_ITERS="${MAX_ITERS:-500}"
SCAN_ASM="$(mktemp -t sor_smart_disasm.XXXXXX)"
SCAN_MAP="$(mktemp -t sor_smart_map.XXXXXX)"
trap 'rm -f "$SCAN_ASM" "$SCAN_MAP"' EXIT

sort_aux_addresses() {
    local sorted
    sorted="$(mktemp -t aux_addresses.XXXXXX)"
    {
        # Preserve the descriptive header (and any future comments), then
        # normalize the fixed-width hexadecimal addresses deterministically.
        grep -Ev '^[[:space:]]*[[:xdigit:]]{6}[[:space:]]*$' "$AUX" || true
        (grep -E '^[[:space:]]*[[:xdigit:]]{6}[[:space:]]*$' "$AUX" || true) | LC_ALL=C sort -u
    } >"$sorted"
    chmod 0644 "$sorted"
    mv "$sorted" "$AUX"
}

sort_aux_addresses
start_count=$(grep -cE '^[0-9a-fA-F]+' "$AUX" 2>/dev/null || echo 0)

echo "==> [smart] Refreshing coverage map from current aux addresses..."
export PYTHONPATH="$RAGE_DECOMPILER_DIR${PYTHONPATH:+:$PYTHONPATH}"
if ! "$PYTHON_BIN" -m tools disassemble "$ROM" -o "$SCAN_ASM" -a "$AUX" --map "$SCAN_MAP"; then
    echo "Coverage-map refresh failed." >&2
    exit 1
fi

echo "==> [smart] Scanning for speculative entry points (after static fixpoint)..."
if ! "$PYTHON_BIN" -m tools speculative-scan "$SCAN_MAP" "$ROM" "$AUX" -o "$SPEC"; then
    echo "Speculative scan failed." >&2
    exit 1
fi
spec_count=$(grep -cE '^[0-9a-fA-F]+' "$SPEC" 2>/dev/null || echo 0)
echo "    $spec_count speculative candidates written to $SPEC"

do_build() {
    echo "==> [fast $1] build (--full --discover)"
    if ! ./build.sh --full --discover; then
        echo "Build failed." >&2
        exit 1
    fi
}

do_build "init"

for ((i = 1; i <= MAX_ITERS; i++)); do
    pkill -9 -f "$BIN" 2>/dev/null

    echo "==> [fast $i] run"
    "$BIN" --lang en --runSor --rom "$ROM" --auxAddrFile "$AUX"
    code=$?
    sort_aux_addresses

    case "$code" in
        42)
            echo "==> [fast $i] new unknown address recorded; rebuilding"
            do_build "$i"
            ;;
        43)
            echo "==> [fast $i] STUCK: a seeded address still has no handler —" >&2
            echo "    likely a bad jump-table index (state bug), not a missing entry." >&2
            exit 1
            ;;
        *)
            now=$(grep -cE '^[0-9a-fA-F]+' "$AUX" 2>/dev/null || echo 0)
            echo "==> [fast $i] exited $code. Added $((now - start_count)) address(es) over $i iteration(s)."
            exit 0
            ;;
    esac
done

echo "Reached MAX_ITERS=$MAX_ITERS without converging." >&2
exit 1
