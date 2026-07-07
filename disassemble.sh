#!/bin/bash
# Run the Streets of Rage disassembler with auxiliary addresses.

set -e
cd "$(dirname "$0")"

ROM="${1:-rom/SOR.bin}"
AUX="code-analysis/aux_addresses.txt"
ADDR_CSV="code-analysis/addresses.csv"
BLOCKS_CSV="code-analysis/blocks.csv"
LABELS_CSV="code-analysis/labels.csv"
OUT_DIR="output"
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
"$PYTHON_BIN" -m tools disassemble "$ROM" \
    -o "$OUT_DIR/sor.asm" \
    -a "$AUX" \
    --addresses-csv "$ADDR_CSV" \
    --blocks-csv "$BLOCKS_CSV" \
    --labels-csv "$LABELS_CSV" \
    --map "$OUT_DIR/sor.map" \
    -v
