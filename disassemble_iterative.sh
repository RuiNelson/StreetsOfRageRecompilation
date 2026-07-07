#!/bin/bash
# Iterative disassembler loop for Streets of Rage.

set -e
cd "$(dirname "$0")"

ROM="${1:-rom/SOR.bin}"
OUT_DIR="output"
SOR_ASM="${OUT_DIR}/sor.asm"
SOR_MAP="${OUT_DIR}/sor.map"
EXODUS="etc/sor-exodus.asm"
AUX="code-analysis/aux_addresses.txt"
RAGE_DECOMPILER_DIR="$(cd "$(dirname "$0")/../RageDecompiler" && pwd)"
if command -v python3.14 >/dev/null 2>&1; then
    PYTHON_BIN="python3.14"
elif command -v python3.13 >/dev/null 2>&1; then
    PYTHON_BIN="python3.13"
else
    PYTHON_BIN="${PYTHON:-python3}"
fi

mkdir -p "$OUT_DIR"

export PYTHONPATH="$RAGE_DECOMPILER_DIR${PYTHONPATH:+:$PYTHONPATH}"
# First pass: disassemble with current aux_addresses
"$PYTHON_BIN" -m tools disassemble "$ROM" \
    -o "$SOR_ASM" \
    -a "$AUX" \
    --map "$SOR_MAP" \
    -v

# Iterative loop: add missing addresses one by one
"$PYTHON_BIN" -m tools iterative-disasm \
    "$SOR_ASM" \
    "$SOR_MAP" \
    "$EXODUS" \
    "$AUX" \
    "$ROM"
