#!/bin/bash
# Regenerate generated/Sor.{hpp,cpp} from confirmed entry points only.

set -euo pipefail
cd "$(dirname "$0")"

ROM="${SOR_ROM:-rom/SOR.bin}"
RAGE_DECOMPILER_DIR="$PWD/../RageDecompiler"
if command -v python3.14 >/dev/null 2>&1; then
    PYTHON_BIN="python3.14"
elif command -v python3.13 >/dev/null 2>&1; then
    PYTHON_BIN="python3.13"
else
    PYTHON_BIN="${PYTHON:-python3}"
fi

[ -f "$ROM" ] || { echo "ROM not found at '$ROM' (set \$SOR_ROM to override)." >&2; exit 1; }

export PYTHONPATH="$RAGE_DECOMPILER_DIR${PYTHONPATH:+:$PYTHONPATH}"
echo "==> Regenerating $ROM -> generated (conservative)"
"$PYTHON_BIN" -m tools recompile "$ROM" -o generated \
    --aux code-analysis/aux_addresses.txt \
    --manual-functions code-analysis/manual_functions.txt
