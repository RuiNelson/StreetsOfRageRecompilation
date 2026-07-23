# Agent guide

Instructions for automated contributors working in
`StreetsOfRageRecompilation`.

## Purpose and boundaries

This repository contains the Streets of Rage-specific native recompilation:
ROM analysis data, generated C++, hand-written runtime/native overrides,
reverse-engineering manuscripts, discovery scripts, tests, and the `sor`
desktop executable.

It expects these owned sibling repositories:

- `../MegaDriveEnvironment`: C++23 host runtime;
- `../RageDecompiler`: Python disassembly and recompilation tools.

`../Genesis-Plus-GX` is an upstream emulator used only as a read-only
behavioral reference. Never edit, patch, reformat, commit, or update it.

Before changing files:

1. inspect this repository's status and preserve unrelated work;
2. determine whether the source of truth is hand-written C++, analysis CSV,
   generated C++, assembly output, or an analysis manuscript;
3. read the sibling repository's local instructions before changing it;
4. choose a bounded validation method because game boot can hang indefinitely.

## Source-of-truth map

| Path | Role |
| --- | --- |
| `main.cpp` | CLI and runtime/test-mode selection |
| `CPU68K.hpp` | 68000 register file used by recompiled code |
| `RecompilationEnvironment.*` | Runtime integration and CPU ownership |
| `SorRuntime.*` | Host orchestration around the recompiled cartridge |
| `SorManualFunctions.cpp` | Hand-written implementations of selected ROM routines |
| `code-analysis/manual_functions.txt` | Addresses dispatched to manual implementations |
| `code-analysis/labels.csv` | ROM code entry points and control-flow labels |
| `code-analysis/addresses.csv` | ROM data, RAM, hardware, table, and buffer symbols |
| `code-analysis/blocks.csv` | Known data/code block boundaries |
| `code-analysis/aux_addresses.txt` | Confirmed extra static entry points |
| `generated/Sor.*` | Ignored local C++ generated from ROM and analysis inputs |
| `output/sor.asm` | Ignored local 68000 listing and primary code-analysis view |
| `ai-analysis/*.md` | English topic-based reverse-engineering manuscripts |
| `sync_ai_analysis.py` | Symbol-reference synchronization and validation |

Do not hand-edit `generated/Sor.cpp` or `output/sor.asm` to make a durable
semantic change. Update the appropriate analysis input, generator, or manual
implementation, then regenerate the derived files.

## Build dependencies and CMake

Requirements:

- CMake 3.24 or newer;
- a C++23 compiler;
- SDL3 development files;
- sibling `MegaDriveEnvironment`;
- Git/network access for CMake `FetchContent`;
- Python 3 and sibling `RageDecompiler` for regeneration and analysis.

`CMakeLists.txt` builds `sor`, fetches CLI11, and adds
`MegaDriveEnvironment`. It deliberately requests shared yaml-cpp, zlib, and
libpng dependencies and avoids relinking `sor` for implementation-only runtime
library rebuilds.

Use the centralized meta-repository scripts:

```bash
../scripts/generate_cpp_and_build
../scripts/build --clean
../scripts/build --release
```

`--full` is mandatory after a fresh clone because `generated/` is ignored by
Git. Once `generated/Sor.cpp` and `generated/Sor.hpp` exist locally,
subsequent builds may omit it until their inputs change.

`../scripts/build` reconfigures only when necessary. Use `--clean` when
changing an existing single-configuration build directory between Debug and
Release.

Portable direct CMake build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

On multi-config generators, pass `--config Debug` or `--config Release`.
Windows builds should put runtime DLL targets in a common output directory;
the meta-repository README contains a complete CMake/Ninja/vcpkg command.

## ROM and regeneration

The original ROM is copyrighted, local-only, and never versioned. The default
path is:

```text
rom/SOR.bin
```

Set `SOR_ROM` to use another path with scripts. `generated/` is ignored by Git,
so generate it after a fresh clone and regenerate whenever code-generation
inputs change:

```bash
../scripts/generate_cpp
```

`--full` runs the sibling `RageDecompiler` and rewrites `generated/Sor.*`.
Use `--full --discover` only inside the speculative discovery workflow; it can
include temporary candidates that do not belong in a normal build.

Review regenerated output carefully. A large unexpected change is evidence to
investigate, not a result to accept mechanically.

## Manual subroutines

A manual subroutine replaces the generated body of a known ROM routine while
keeping the generated declaration, dispatcher, and call sites intact.

- Record manual addresses in `code-analysis/manual_functions.txt`.
- Implement native bodies in the established hand-written source, normally
  `SorManualFunctions.cpp`.
- Preserve 68000-visible register, memory, flag, stack, and control-flow
  effects expected by callers.
- Base behavior on `output/sor.asm`, analysis data, and bounded runtime
  observations. Generated C++ is a navigation aid, not the primary evidence.
- Add focused instrumentation/tests where feasible and compare behavior before
  removing a generated implementation.
- Regenerate and compile after changing the manual-function list.

Do not turn unknown behavior into a host shortcut merely because it makes one
scenario pass.

## Run and observe safely

Boot defects can spin forever and SDL windows may outlive plain `SIGTERM`. On
systems with GNU `timeout`, always use a kill grace period:

```bash
timeout -k 3 20 ../scripts/run rom/SOR.bin --debug
```

MegaDriveEnvironment runtime diagnostics are owned by the sibling
`MegaDriveEnvironmentSampleGame` repository.

After automation, verify that no `build/sor` process remains. On platforms
without GNU `timeout`, use another bounded process runner. Do not leave a game
process or remote-access port active after a test.

For deterministic gameplay automation, prefer the checked-in remote client
scripts and the `megadrive_remote` API over imprecise sleeps or unbounded key
presses.

## Disassembly and discovery

Use the repository entry points:

```bash
../scripts/disassemble_to_asm
../scripts/discover_aux_smart
```

Equivalent direct tools require the sibling checkout:

```bash
PYTHONPATH=../RageDecompiler python3 -m tools --help
```

The static disassembler follows confirmed reachable code. Indirect dispatches
may require runtime active-disassembly evidence and confirmed additions to
`code-analysis/aux_addresses.txt`. Keep speculative candidates separate from
confirmed addresses and do not commit discovery stubs as production behavior.

## Analysis manuscripts and symbol synchronization

`code-analysis/labels.csv` and `code-analysis/addresses.csv` are authoritative
for symbol names and locations. Manuscripts explain behavior; they must not
invent a parallel symbol vocabulary. Use `output/sor.asm` as the primary code
evidence and generated C++ as a secondary navigation aid.

Manuscripts:

- are written in English;
- live under `ai-analysis/`;
- use kebab-case filenames;
- cover one coherent system/topic per file;
- keep boss behavior in `enemy-ai.md`, not a separate `bosses.md`.

After adding, renaming, moving, or removing a CSV symbol:

```bash
./sync_ai_analysis.py
./sync_ai_analysis.py --check
```

The update must be idempotent: a second synchronization should report no
changes. Keep CSV changes and synchronized manuscripts in the same logical
delivery. When a code label changes, also regenerate the derived views:

```bash
../scripts/disassemble_to_asm
../scripts/generate_cpp_and_build
```

Never rename symbols directly in `output/sor.asm` or `generated/Sor.cpp`.

## Address conventions

- Known prose and evidence references use `` `$ADDRESS (label)` ``.
- Hexadecimal addresses use `$` and uppercase digits.
- Manuscripts omit redundant leading zeroes for ROM offsets; CSV files retain
  their established fixed-width format.
- Work RAM references use the full 24-bit form, such as
  `` `$FFFF00 (game_state)` `` and `` `$FFB800 (p1_object)` ``.
- Keep address spaces distinct. Z80 `$1FFF`, for example, maps to the separate
  68000-visible `` `$A01FFF (z80_dac_command)` `` address.
- Object fields remain relative offsets such as `+$32`.
- Constants, state values, ranges, and genuinely unnamed locations remain
  plain hexadecimal; do not fabricate labels.
- Add a new semantic name to the appropriate CSV with evidence and confidence
  before using it in a manuscript.
- Prefer a semantic CSV label over generated `sub_...` or `loc_...` names.

## Tests and validation

Run Python tests from this repository:

```bash
python3 -m pytest
```

For C++ changes, build first and run the narrowest relevant bounded runtime
test. For cartridge behavior, record the exact ROM, flags, inputs, observed
state, timeout, and whether the result came from host automation or an
independent emulator/reference comparison.

Before finishing analysis work:

```bash
./sync_ai_analysis.py --check
```

Before finishing generated/manual-function work, regenerate, inspect the
output, compile `sor`, and execute an appropriate bounded observation.

## Generated files and delivery

Never commit:

- `rom/SOR.bin` or any commercial ROM dump;
- build directories or CMake fetch trees;
- caches, screenshots, logs, or temporary discovery output;
- local captures not explicitly requested as fixtures.

Generated source under `generated/` and assembly under `output/` are ignored
local artifacts. Do not add them to Git unless repository policy is explicitly
changed as part of the task.

After validation, commit and push this repository to `main` automatically
unless the user explicitly asks not to publish. When checked out as a
submodule, publish this repository first and then update the parent gitlink.
Preserve unrelated work and never force-push or rewrite history. Report all
validation and clearly state which platforms or runtime scenarios were not
tested.
