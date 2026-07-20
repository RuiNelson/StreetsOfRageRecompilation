#!/bin/bash
# Refresh speculative entry points, then regenerate generated/Sor.{hpp,cpp}.

set -euo pipefail
cd "$(dirname "$0")"

AUX="code-analysis/aux_addresses.txt"
SPEC="code-analysis/speculative_addresses.txt"
ROM="${SOR_ROM:-rom/SOR.bin}"
RAGE_DECOMPILER_DIR="$PWD/../RageDecompiler"
if command -v python3.14 >/dev/null 2>&1; then
    PYTHON_BIN="python3.14"
elif command -v python3.13 >/dev/null 2>&1; then
    PYTHON_BIN="python3.13"
else
    PYTHON_BIN="${PYTHON:-python3}"
fi
SCAN_ASM="$(mktemp -t sor_smart_disasm.XXXXXX)"
SCAN_MAP="$(mktemp -t sor_smart_map.XXXXXX)"
trap 'rm -f "$SCAN_ASM" "$SCAN_MAP"' EXIT

[ -f "$ROM" ] || { echo "ROM not found at '$ROM' (set \$SOR_ROM to override)." >&2; exit 1; }

export PYTHONPATH="$RAGE_DECOMPILER_DIR${PYTHONPATH:+:$PYTHONPATH}"
echo "==> Refreshing coverage map"
"$PYTHON_BIN" -m tools disassemble "$ROM" -o "$SCAN_ASM" -a "$AUX" --map "$SCAN_MAP"

echo "==> Refreshing speculative entry points"
"$PYTHON_BIN" -m tools speculative-scan "$SCAN_MAP" "$ROM" "$AUX" -o "$SPEC"

echo "==> Regenerating $ROM -> generated (smart)"
"$PYTHON_BIN" -m tools recompile "$ROM" -o generated \
    --aux "$AUX" \
    --speculative "$SPEC" \
    --manual-functions code-analysis/manual_functions.txt
