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
| `SoRManualFunctions.cpp` | Hand-written implementations of selected ROM routines |
| `SoRControls.cpp` | Hand-written controller sampling, input remap, OPTIONS controls, character-select input, and pause/Start routines |
| `code-analysis/manual_functions.txt` | Addresses dispatched to manual implementations |
| `code-analysis/labels.csv` | ROM code entry points and control-flow labels |
| `code-analysis/addresses.csv` | ROM data, RAM, hardware, table, and buffer symbols |
| `code-analysis/blocks.csv` | Known data/code block boundaries |
| `code-analysis/aux_addresses.txt` | Confirmed extra static entry points |
| `tools/call_map.py` | Converts runtime call logs into a deduplicated SQLite call map |
| `generated/SoR*` | Ignored local C++ generated from ROM and analysis inputs |
| `output/sor.asm` | Ignored local 68000 listing and primary code-analysis view |
| `ai-analysis/*.md` | English topic-based reverse-engineering manuscripts |
| `sync_ai_analysis.py` | Symbol-reference synchronization and validation |

Do not hand-edit `generated/SoR-XXX.cpp` or `output/sor.asm` to make a durable
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
Git. Once `generated/SoR.hpp`, `generated/SoR-common.hpp`, and the
`generated/SoR-XXX.cpp` files exist locally,
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

`--full` runs the sibling `RageDecompiler` and rewrites `generated/SoR*`.
Use `--full --discover` only inside the speculative discovery workflow; it can
include temporary candidates that do not belong in a normal build.

Review regenerated output carefully. A large unexpected change is evidence to
investigate, not a result to accept mechanically.

## Runtime call log and call map (game analysis)

Use runtime call recording plus `tools/call_map.py` to **analyse game control
flow**: observed caller→callee edges, per-callsite targets, which labelled
routines entered during a session, and how often each flow fired. Prefer this
over guessing from generated C++ alone. Pair results with `output/sor.asm`,
`code-analysis/labels.csv`, and `ai-analysis/*.md`. Agents that need to
explore an existing map should follow the workspace `explore-call-map` skill
and query SQLite directly (do not start the web viewer only for agent-side
analysis).

### Recording with `sor`

`--callLog PATH` optionally records emulated 68000 subroutine entries and
`bsr`/`jsr` calls as typed CSV with the columns
`event,source,callsite,target`. A `call` event contains the original ROM
source-subroutine entry, instruction address, and exact destination. An
`entry` event contains the entered address in `source` and empty `callsite` and
`target` fields. Addresses are uppercase, six-digit hexadecimal ROM addresses
without a prefix. The file is truncated at startup; without this option,
logging is disabled and the entry/call hooks only perform a null check.
Generated and hand-written subroutines must trace their true dynamic entry.

```bash
./build/sor --rom rom/SOR.bin --callLog ../calls.csv
```

Do not launch or leave the game unbounded for analysis unless asked; follow
**Run and observe safely** below.

### Collapsing logs with `tools/call_map.py`

`tools/call_map.py` consumes one or more call-log CSV files (typed
`event,source,callsite,target` or legacy `source,callsite,target`) and writes
a deduplicated SQLite call graph. It inserts every `code-analysis/labels.csv`
routine, even when it has zero observed activity, and preserves source
addresses that are already labelled. Unless
`--trust-recorded-source` is supplied, it approximates anonymous sources in
older grouped-owner logs from the closest label; that approximation cannot
reconstruct every non-contiguous dynamic entry, so regenerate important logs
with the current recompiler. Passing `--port PORT` starts its read-only
interactive web viewer after database generation. The viewer binds to
`127.0.0.1` unless `--host` is explicitly provided and exposes all labelled
routines, dynamic entry counts, flow counts, and per-flow callsites.

```bash
python3 tools/call_map.py ../calls.csv \
  --database call-map.sqlite \
  --labels code-analysis/labels.csv
```

Useful analysis views (addresses formatted as `$XXXXXX`):

| View | Use |
| --- | --- |
| `subroutine_activity` | Entry counts plus incoming/outgoing flow totals per routine |
| `subroutine_flow` | Deduplicated source→target edges with `observed_count` |
| `callsite_flow` | Exact source/callsite/target rows with labels |

`observed_count` is how many runtime events collapsed into that row, not a
static reachability weight. A routine present only via `labels.csv` is known
but was not entered in the captured run.

The generated `call-map.sqlite` artifact may be kept locally in this repository
alongside the tool, but must remain ignored and unversioned. Regenerate it from
the workspace-root `calls.csv` with the command documented in the root README.
Do not stage call logs or the database unless the user explicitly requests
versioning that exact artefact.

## Manual subroutines

A manual subroutine replaces the generated body of a known ROM routine while
keeping the generated declaration, dispatcher, and call sites intact.

- Record manual addresses in `code-analysis/manual_functions.txt`.
- Implement native bodies in the established hand-written source, normally
  `SoRManualFunctions.cpp`.
- Sound helpers live in `SoRSound.cpp`. `$073298 (sound_ym2612_acquire)`
  replaces the generated BUSREQ / DAC-busy / YM-status spins with
  `waitForByteValue` and `yield()`. Do not use `waitForInterrupt()` there:
  the routine runs from `$19D16 (vblank_handler)` via `$72914 (sound_engine)`,
  so the IPL is already raised and an IRQ wait would deadlock. Do not sleep
  on the DAC-busy retry: voices pulse `$A01FFD (z80_dac_busy)` briefly, and
  a host sleep after release lets the Z80 thread start a long slice that
  stalls the next BUSREQ (frame hitch). Retry immediately, like the ROM's
  three NOPs.
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

### Debug cheats run on the wrong thread, and must not touch a spawning slot

Two rules, both paid for by lost measurement runs rather than reasoned out.

**Nothing in `handleOptionHotkey` may write emulated RAM.** That handler runs
on the **main** thread and `run()` on the **CPU** thread
(`MegaDriveEnvironment/README.md`, "The important threading rules"), so a
write from it races the game's own object update. `SoRCheats`'
`requestFreePoliceCall`/`consumeFreePoliceCall` pair already had the right
shape -- record on the main thread, consume on the CPU thread inside a manual
function -- and every other hotkey now follows it: the handler records into
`SoRCheats::requestCheats`/`requestLevelJump`, and `applyPendingSoRCheats`
drains them from the vblank waits in `SoRManualFunctions.cpp`, which is the
game's own frame boundary.

**A slot whose primary state is still `$0000` is mid-spawn and must be left
alone** (`isStillSpawning`). A wave's object slots are populated before
`$937A` runs, so for one frame a slot holds a complete, *visible*,
uninitialised entity: the type byte is already written while nothing else is.
`killOrdinaryEnemiesMatching` selects on that type byte, so without the guard
the family sweep can write a death (`$0300`, health `$FFFF`, `+$37` flag)
into an object the spawn code is still building.

That second one **resets the console**, and it is the bug: measured at four
runs in sixteen with `autoplay`'s `debug_scenario` sweep running twice a
second through round 2. A full per-tick trace
(`autoplay/tools/round2_death_diag.py`) caught the transition -- the actor
walking normally at full health with four lives, reaching x=2268, which is
exactly where round 2's wave 2 spawns, and the very next poll reading level 0,
`'Sega logo'`, lives 0. No death, no damage, no dispatch dump: a boot.

`autoplay`'s own observer had to learn the identical lesson from the read
side (`world_map._is_dormant_combatant`, which records five such slots
appearing for a single tick at state `$00` and the AI punching at the
nearest), so treat "the type byte is set" as *not* meaning "this object
exists" anywhere in this codebase.

The threading fix came first, is correct on its own terms, and did **not**
fix the reset -- do not read it as the cure.

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

Never rename symbols directly in `output/sor.asm` or generated `SoR-XXX.cpp`.

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
