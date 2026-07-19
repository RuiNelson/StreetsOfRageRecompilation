# CLAUDE.md

Guidance for LLM agents working in `StreetsOfRageRecompilation`.

## Scope

This repository contains the Streets of Rage-specific C++ recompilation,
analysis data, ROM-local scripts, generated C++ output, and the executable entry
point. It expects sibling checkouts of:

- `../MegaDriveEnvironment`
- `../RageDecompiler`

Do not make changes in `../Genesis-Plus-GX`; it is an upstream dependency not
owned by this project.

## Important Files

- `main.cpp` - CLI entry point and runtime/test mode selection.
- `CMakeLists.txt` - builds the `sor` executable and links shared
  `MegaDriveEnvironment` (`CMAKE_LINK_DEPENDS_NO_SHARED` skips re-link when only
  the dylib changes). Sets `MEGADRIVE_ENVIRONMENT_SHARED_DEPS=ON` so
  yaml-cpp/zlib/libpng are built and linked as shared libraries for this
  consumer only.
- `CPU68K.hpp` - 68000 register file for recompiled cartridge code.
- `RecompilationEnvironment.hpp` - MegaDriveEnvironment plus CPU68K ownership.
- `build.sh` - preferred build wrapper.
- `generated/` - generated C++ recompilation output.
- `code-analysis/aux_addresses.txt` - extra entry points for static
  disassembly.
- `code-analysis/*.csv` - labels, comments, and memory-region metadata.
- `rom/.gitignore` - keeps local ROM files out of git.

## Build

Preferred build:

```bash
./build.sh
```

Clean build:

```bash
./build.sh --clean
```

Release build:

```bash
./build.sh --type Release
```

Regenerate `generated/` from the local ROM before building:

```bash
./build.sh --full
```

The ROM is not versioned. Put it at:

```bash
rom/SOR.bin
```

or set `SOR_ROM` when running full builds.

## Running

Always wrap runs in `timeout -k`. Boot bugs can hang, and SDL may not exit on a
plain timeout signal.

```bash
timeout -k 3 20 ./build.sh -r -- --runSor --debug --fast --rom rom/SOR.bin
```

Runtime test examples:

```bash
timeout -k 3 20 ./build.sh -r -- --testVDP
timeout -k 3 20 ./build.sh -r -- --testControllers
```

After a run, confirm no `build/sor` process is still alive.

## Disassembly Workflows

Use the checked-in scripts from this repository:

```bash
./disassemble.sh
./disassemble_nolabels.sh
./disassemble_iterative.sh
./discover_aux_smart.sh
```

Equivalent direct tool calls use the sibling `RageDecompiler` on `PYTHONPATH`:

```bash
PYTHONPATH=../RageDecompiler python3 -m tools disassemble rom/SOR.bin -o output/sor.asm -v
PYTHONPATH=../RageDecompiler python3 -m tools iterative-disasm output/sor.asm output/sor.map etc/sor-exodus.asm code-analysis/aux_addresses.txt rom/SOR.bin
```

## Conventions

- Do not commit `rom/SOR.bin`, build directories, CMake fetch content, or local
  output captures.
- Generated recompilation code belongs under `generated/`.
- Keep every manuscript in `ai-analysis/` synchronized with
  `code-analysis/labels.csv` and `code-analysis/addresses.csv`. When a symbol is
  added, renamed, or removed, update the corresponding prose and evidence tables
  in the same change; do not retain a generated `sub_...` or `loc_...` name where
  a semantic label exists.
- Keep `MegaDriveEnvironment` changes in the sibling submodule and update the
  parent gitlink afterward.
