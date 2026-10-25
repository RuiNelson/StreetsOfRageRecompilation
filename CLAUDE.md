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
../scripts/generate_cpp_and_build.py
../scripts/build.py --clean
../scripts/build.py --release
```

`--full` is mandatory after a fresh clone because `generated/` is ignored by
Git. Once `generated/SoR.hpp`, `generated/SoR-common.hpp`, and the
`generated/SoR-XXX.cpp` files exist locally,
subsequent builds may omit it until their inputs change.

`../scripts/build.py` reconfigures only when necessary. Use `--clean` when
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
../scripts/generate_cpp.py
```

`--full` runs the sibling `RageDecompiler` and rewrites `generated/SoR*`.
Use `--full --discover` only inside the speculative discovery workflow; it can
include temporary candidates that do not belong in a normal build.

Review regenerated output carefully. A large unexpected change is evidence to
investigate, not a result to accept mechanically.

The generated `StreetsOfRage` constructor (hardcoded in the decompiler's
`generator.py`) runs the VDP with `VDP::QuasiUnlimitedSprites`: excess sprites
per scanline are drawn instead of clipped at the hardware 20 (H40) / 16 (H32)
limit. SOVR is still set as on hardware, so game logic reading it is unaffected.

## Widescreen (`--widescreen`)

`sor --widescreen` calls `MegaDriveEnvironment::setWidescreen(true)` from
`main.cpp`, switching the VDP from Mode 40 to the host-only Mode 48 (48×27
tiles, 384×216; see `MegaDriveEnvironment/README.md`). Game logic, the work-RAM
state and the 320×224 timing are untouched, so `autoplay` addresses and
coordinates are unchanged; only the output grew. Sprites are shifted 32 px right
of Mode 40 relative to the backgrounds (sprite origin 96, background unshifted):
sprites keep their place relative to the screen centre. Remote snapshots
(`read_framebuffer`) are 384×216 in this mode.

The gameplay HUD is the window plane (rows 0-3 at VRAM `$D000`): the static
upload `$FF8000` (40×4 at window row 0) and the per-VBlank `$FF6000` block (40×3
at row 1), both written by `$8064 (vdp_copy_stride)`. Everything below lives in
the game (`SoRManualFunctions.cpp`) and only runs with `--widescreen`;
`MegaDriveEnvironment` is not involved beyond Mode 48 itself.

- The manual `vdp_copy_stride` drops the bottom edge, turns the frame tiles
  `$6D4`-`$6D7` into blank tiles and writes the other rows one window row lower
  (window row 0 is the hidden first tile row). Work RAM keeps the original
  block, so digit/BCD writers and `autoplay` are unaffected. It also reshapes
  the boss-bar bundle 9 (`$FFF510`, 40×2 at `$51800003`, window rows 3-4, loaded
  every VBlank while a boss is registered) the same way, so the boss bar lands
  on row 4 instead of overwriting the shifted HUD's last element row.
- The plate is also palette 0 colour 2 inside every glyph, digit and bar tile.
  The first per-VBlank block upload after the level-start upload (when the
  level's HUD art is in VRAM) rewrites those tiles through the VDP ports with
  colour 2 as 0. It covers tiles `$6BE`-`$6FF` and `$7C0`-`$7F3` only: tiles
  `$780`-`$7BF` are VRAM `$F000`-`$F7FF`, the sprite and horizontal-scroll
  tables. The art is read from `VDP::stateSnapshot()` because the emulated data
  port does not prefetch on an address set like the hardware.
- `$10F80 (vblank_update_gameplay_health_bars)` is a manual port (it inlines the
  `$11072`/`$1108E` bar uploads) so the bar tile rows, which the ROM rewrites
  on every damage event around a `$110A8` plate row, use a transparent plate
  row. Patching the ROM long instead fails the boot checksum and hangs the game.

Without `--widescreen` both routines copy exactly like the ROM (checked: the
plain gameplay frame is byte-identical, and a forced bar upload keeps the plate
rows).

### Widescreen arena (`SoRWidescreenArena.cpp`)

The game's own screen edges are 320-pixel assumptions. Hand-written ports (listed under "Widescreen arena" in
`code-analysis/manual_functions.txt`) widen each edge by `$20` per side under `--widescreen` and are the
ROM routine unchanged otherwise: the player clamp (`$43AA`), `spawn_single_object` (`$C60`), the on-screen tests
(`$97CE`, `$97E6`), the attack hit window (`$AAA0`, `$ABA4`), the locked-arena push-back (`$6A6`), the charging boss
(`$14C38`), an enemy's walk to the screen side (`$F4A6`), the scripted-object activation (`$A59C`), round 8's thrown table
(`$7546`) the police-special recovery (`$40A6`), the wave batch placement (`$CC0`: objects created beyond the old
screen edge are placed `$20` further out, or they would activate at once and pop into the widened screen) and the sprite-piece cull (`$B0FE`, whose left bound `$60` made sprites vanish
while still partly visible at the far left of the widescreen). `autoplay` is not touched until the user says the widescreen
feature is complete. Never patch the ROM (the boot checksum hangs the game) and
never edit `MegaDriveEnvironment` for game rules. A port must also leave the CCR flags the caller branches on
exactly as the ROM does at every exit: `update_player_object` skips the player's update on `bne` after
`$40A6 (police_special_player_update_gate)`, and that Z flag is the whole of the police special's freeze. The full table, the checks and what is left are in
`../docs/WidescreenLogBook.md`.

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
- **Keep the original memory layout.** Every piece of game state stays in
  68000 work RAM at its original address, width, and encoding (object slots,
  globals, BCD fields, flag bits), written on the same frame as the ROM
  writes it. Do not move state into C++ members, statics, or heap memory,
  and keep reading data tables from the ROM. `autoplay` reads the game by
  address (`../autoplay/src/sor_autoplay/memory_map.py`, `rom_data.py`) and
  its enemy models are frame-exact, so a relocated or retimed field breaks
  the AI. This is a project rule: reimplementation makes the code readable,
  never a different data model.

Do not turn unknown behavior into a host shortcut merely because it makes one
scenario pass.

## Run and observe safely

Boot defects can spin forever and SDL windows may outlive plain `SIGTERM`. On
systems with GNU `timeout`, always use a kill grace period:

```bash
timeout -k 3 20 ../scripts/run.py rom/SOR.bin --debug
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
../scripts/disassemble_to_asm.py
../scripts/discover_aux_smart.py
```

Equivalent direct tools require the sibling checkout:

```bash
PYTHONPATH=../RageDecompiler python3 -m tools --help
```

The static disassembler follows confirmed reachable code. Indirect dispatches
may require runtime active-disassembly evidence and confirmed additions to
`code-analysis/aux_addresses.txt`. Keep speculative candidates separate from
confirmed addresses and do not commit discovery stubs as production behavior.

### Catching an unhandled dispatch from a live session

`discover_aux_smart` only finds what an unattended run reaches. A target that
needs a specific game situation shows up as a `SIGABRT` in
`reportUnhandledDispatch` during some automated session instead, and the
target address is printed only on `sor`'s **stderr**
(`indirect dispatch to unknown address $XXXXXX (in fn $YYYYYY)`, then a CPU,
RAM, and trace-history dump). The macOS crash report does not contain it: the
Debug build keeps the address on the stack, and the registers it does show are
unrelated. So when a session aborts intermittently, loop it with `sor`'s
stderr redirected to a file until it aborts again, then:

1. read the dispatching code in `output/sor.asm` (`in fn`, the trace history,
   and the dumped registers name the path);
2. decode the jump table in the ROM and add **every** target of it, not only
   the one observed -- the others are the same path in a different situation;
3. regenerate (`../scripts/generate_cpp.py`), review the generated diff, rebuild,
   and drive the same situation again on purpose.

Example: round 5's clock running out with two players aborted on `$0109DC`.
The time-over (`$10976`) jumps through a `player_mode`-indexed table at
`$109D4` -- P1 only `$109DE`, P2 only `$109EA`, both `$109DC` (`bsr.s $109EA`,
then into `$109DE`) -- of which only `$109DE` had been seen before.

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
../scripts/disassemble_to_asm.py
../scripts/generate_cpp_and_build.py
```

Never rename symbols directly in `output/sor.asm` or generated `SoR-XXX.cpp`.

## Address conventions

- Known prose and evidence references use `` `$ADDRESS (label)` ``.
- Hexadecimal addresses use `$` and uppercase digits.
- Manuscripts omit redundant leading zeroes for ROM offsets; CSV files retain
  their established fixed-width format.
- Work RAM references use the full 24-bit form, such as
  `` `$FFFF00 (game_state)` `` and `` `$FFB800 (p1_object)` ``.
- `sync_ai_analysis.py` reads a bare `` `$B000` ``-`` `$FFFF` `` value as
  16-bit work-RAM shorthand whenever `$FFxxxx` has a symbol. Write a ROM
  offset in that range zero-padded to six digits (`` `$00FC1C` ``) so it is
  never linked to the RAM symbol, and write VRAM addresses that collide with
  RAM symbols as plain `E000h` / `F400h`, as `graphics-engine.md` does.
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
