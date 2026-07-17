# Streets of Rage Recompilation

C++ recompilation of *Streets of Rage* for the Sega Mega Drive. The cartridge is
translated from the ROM into C++; the host runtime lives in the sibling
[MegaDriveEnvironment](../MegaDriveEnvironment) repository, and disassembly /
recompile tooling in [RageDecompiler](../RageDecompiler).

This project expects those two repositories as siblings:

```text
MegaDriveEnvironment/
RageDecompiler/
StreetsOfRageRecompilation/   ← this repo
```

You need CMake ≥ 3.24, a C++23 compiler, and Python 3. The original ROM is not
versioned — put a local copy at `rom/SOR.bin`, or set `SOR_ROM` to override the
path.

## Build

`./build.sh` configures when needed and builds the `sor` binary (Debug by
default). Useful variants:

```bash
./build.sh                 # configure if needed, build Debug
./build.sh --clean         # wipe build/ and reconfigure
./build.sh -t Release      # Release build
./build.sh --full          # recompile ROM → generated/, then build
```

`--full` regenerates `generated/` from the ROM via RageDecompiler
(`python -m tools recompile`, with `PYTHONPATH=../RageDecompiler` and
`code-analysis/manual_functions.txt`). Use `--full --discover` only for the
speculative discovery loop; that build includes temporary stubs.

## Run

```bash
./build.sh -r -- --runSor --debug --fast --rom rom/SOR.bin
```

`--runSor` boots the recompiled cartridge. `--rom` defaults to `rom/SOR.bin`.
`--debug` logs CPU/VDP state once per second; `--fast` disables CPU pacing.
`--vsync` chooses frame sync (`0` = internal timer from `--hz`, `1`–`3` =
display VSync). Console pins are `--lang jp|en` and `--hz 50|60`; `--silent`
drops all audio chip writes. For jump-table discovery, `--auxAddrFile` appends
unknown dispatch targets and exits 42 instead of aborting.

The same binary also hosts environment tests that do not need a recompiled
cartridge:

```bash
./build.sh -r -- --testVDP
./build.sh -r -- --testControllers
./build.sh -r -- --testSound
./build.sh -r -- --configControls
```

Default controls are in `controls.yaml` (P1: arrows + Z/X/C/V). While playing,
keyboard hotkeys add a P1 life (`L`), special (`S`), toggle punch power ×12
(`P`), or jump to levels 1–8 (`1`–`8`).

## Disassembly and discovery

All scripts assume the ROM at `rom/SOR.bin` (or `SOR_ROM`) and put
RageDecompiler on `PYTHONPATH` themselves:

```bash
./disassemble.sh              # labels + addresses/blocks CSVs → output/
./disassemble_nolabels.sh     # same without labels CSV
./disassemble_iterative.sh    # iterative pass vs reference map
./discover_aux_fast.sh        # preferred: speculative stubs + runtime loop
./discover_aux.sh             # conservative: rebuild on every new address
```

The discovery scripts record missing jump-table entry points in
`code-analysis/aux_addresses.txt`. Exit **42** means a new address was written;
**43** means a seeded address still has no handler — usually a state bug, not a
missing entry.

## Layout

Most of the recompiled game lives under `generated/` (`Sor.cpp` / `Sor.hpp`),
produced by the recompiler. Hand-written host integration stays outside that
tree so `--full` does not wipe it:

- `main.cpp` — CLI for tests and `--runSor`
- `CPU68K.hpp` / `RecompilationEnvironment.*` — 68000 register file and host
  environment that owns it
- `SorRuntime.*` — runtime hooks (hotkeys)
- `SorManualFunctions.cpp` — bodies listed in `manual_functions.txt`
- `SorCheats.*` — P1 punch-power cheat
- `build.sh` — configure, build, optional full recompile and run

Analysis data sits in `code-analysis/`: `aux_addresses.txt` feeds extra entry
points the static disassembler cannot resolve; `manual_functions.txt` names the
functions with hand-written C++ bodies; the CSV files hold labels, blocks, and
address metadata. Disassembler output goes to `output/` (local).

Three cartridge functions are currently manual: `$0041EA` (attack strength, also
the punch-power hook) and `$010502` / `$010514` (Z80 sync via the VBlank
mailbox, using host-friendly waits instead of a busy loop).
