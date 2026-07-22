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
- `output/sor.asm` - generated 68000 assembly listing and primary source for
  reverse-engineering manuscripts; regenerate it, do not edit it by hand.
- `ai-analysis/*.md` - English reverse-engineering manuscripts, organized by
  gameplay/system topic.
- `sync_ai_analysis.py` - synchronizes manuscript references from the analysis
  CSVs and checks the canonical address/label format.
- `code-analysis/aux_addresses.txt` - extra entry points for static
  disassembly.
- `code-analysis/labels.csv` - ROM code entry points and control-flow labels.
- `code-analysis/addresses.csv` - named ROM data, work RAM, hardware registers,
  tables, buffers, and other non-code addresses.
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
timeout -k 3 20 ./build.sh -r -- --runSor --debug --rom rom/SOR.bin
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
./sync_ai_analysis.py
./sync_ai_analysis.py --check
```

Equivalent direct tool calls use the sibling `RageDecompiler` on `PYTHONPATH`:

```bash
PYTHONPATH=../RageDecompiler python3 -m tools disassemble rom/SOR.bin -o output/sor.asm -v
PYTHONPATH=../RageDecompiler python3 -m tools iterative-disasm output/sor.asm output/sor.map etc/sor-exodus.asm code-analysis/aux_addresses.txt rom/SOR.bin
```

## Analysis Manuscripts and Symbol Synchronization

Treat `code-analysis/labels.csv` and `code-analysis/addresses.csv` as the source
of truth for names and locations. Manuscripts explain behavior; they must not
maintain an independent symbol vocabulary. Use `output/sor.asm` as the primary
code source and generated C++ only as a secondary navigation aid.

Manuscripts live in `ai-analysis/`, are written in English, and use kebab-case
filenames. Keep one document per system/topic. Boss behavior belongs in
`ai-analysis/enemy-ai.md`; do not recreate a separate `bosses.md`.

After adding, renaming, moving, or removing an entry in either CSV, run:

```bash
./sync_ai_analysis.py
./sync_ai_analysis.py --check
```

The first command updates every `ai-analysis/*.md` file. The second is
read-only and exits nonzero if another synchronization pass would change a
manuscript. The address is the stable identity, so canonical references are
updated even when their labels were renamed. The script also merges compatible
Address/Symbol table columns and deliberately leaves fenced assembly,
pseudocode, and other code samples unchanged. It is idempotent: a second update
must report that all manuscripts are synchronized.

When changing a code label, also regenerate the derived views before testing:

```bash
./disassemble.sh
./build.sh --full
```

Do not edit `output/sor.asm` or `generated/Sor.cpp` to rename a symbol; both are
regenerated from the CSV analysis data.

### Address and Location Conventions

- Known references in manuscript prose and evidence tables use exactly
  `` `$ADDRESS (label)` ``, for example
  `` `$4D60 (update_score_hud_and_check_extra_life)` ``.
- Write hexadecimal addresses with `$` and uppercase digits. Manuscripts omit
  redundant leading zeroes for ROM offsets; CSV files retain their established
  fixed-width form.
- Use the full 24-bit work-RAM address in canonical references, such as
  `` `$FFFF00 (game_state)` `` and `` `$FFB800 (p1_object)` ``. The synchronizer
  expands mapped 16-bit shorthand such as `$FF00` or `$B800`.
- Keep address spaces distinct. A Z80-local address and its 68000 mapping are
  not interchangeable; for example Z80 `$1FFF` maps to
  `` `$A01FFF (z80_dac_command)` ``.
- Object-structure fields are relative offsets and remain `+$NN`, for example
  player health at `+$32`. Numeric constants, state values, address ranges, and
  genuinely unnamed locations remain plain hexadecimal and do not require a
  fabricated label.
- If a manuscript needs a new semantic code/data name, add it to the appropriate
  CSV with evidence and confidence first, then run the synchronizer. Do not use
  a generated `sub_...` or `loc_...` name when that address already has a
  semantic CSV label.

## Conventions

- Do not commit `rom/SOR.bin`, build directories, CMake fetch content, or local
  output captures.
- Generated recompilation code belongs under `generated/`.
- Keep CSV changes and their synchronized manuscript updates in the same
  commit. Run `./sync_ai_analysis.py --check` before committing analysis work.
- Keep `MegaDriveEnvironment` changes in the sibling submodule and update the
  parent gitlink afterward.
