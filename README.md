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

You need CMake ≥ 3.24, a C++23 compiler, and Python 3.

## ROM

The original cartridge image is **not** shipped with this repository. You must
supply your own dump before a full recompile or a `--runSor` session.

Place it at:

```text
rom/SOR.bin
```

That path is what `build.sh`, the disassembly scripts, and the default
`--rom` flag expect. To use another location, set `SOR_ROM` (for the scripts)
or pass `--rom PATH` when running the binary.

The dump this project is developed against is the 512 KiB *Streets of Rage*
Mega Drive / Genesis cartridge (*Bare Knuckle* / serial `MK 00001019-01`,
region `JUE`). Verify yours matches:

| | |
|---|---|
| **Size** | 524 288 bytes |
| **MD5** | `59a3b22a1899461dceba50d1ade88d3a` |
| **SHA-256** | `95d7efb98e97f4ffffe68257aef9a855034a36a41b86cf9d332d129f30cb2d4b` |

On macOS / Linux:

```bash
md5 rom/SOR.bin          # or: md5sum rom/SOR.bin
shasum -a 256 rom/SOR.bin
```

### Legal considerations

*Streets of Rage* / *Bare Knuckle* is copyrighted by Sega (and related
rightsholders). This project does **not** distribute the ROM, and it does not
grant any licence to obtain or use one.

Only use a ROM image you are legally entitled to — for example a personal dump
of a cartridge you own, where that is allowed under local law. Do not download
or share commercial ROM dumps unless you have a clear right to do so. Nothing
here is legal advice; if you are unsure, do not use a ROM with this software.

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
RageDecompiler on `PYTHONPATH` themselves.

### Disassembly

```bash
./disassemble.sh              # labels + addresses/blocks CSVs → output/
./disassemble_nolabels.sh     # same without labels CSV
./disassemble_iterative.sh    # iterative pass vs reference map
```

These produce `output/sor.asm` and a coverage map. The static tools follow known
control flow from the vector table and from every address listed in
`code-analysis/aux_addresses.txt`.

### Discovery

A large part of Streets of Rage is reached only through **indirect** control
flow: jump tables, function pointers in object state, and similar patterns that
a pure recursive-descent pass cannot resolve. Without those targets, the
recompiler never emits handlers for the code that actually runs, and the
binary aborts on the first unknown dispatch.

The goal of discovery is to collect **every live entry point** the game
reaches in practice — the full set of code that executes during real play —
and feed it into `aux_addresses.txt` so the next recompile includes it. It is
**not** the goal to treat the whole ROM as code: graphics, sound banks, maps,
and other data blobs can byte-for-byte look like 68000 opcodes. Blindly
recompiling those regions produces false functions, bloated output, and
misleading coverage. Dead code that is never called is equally uninteresting;
we only want what the running game can touch.

That is why discovery is **runtime-driven**, not a full-ROM scan that trusts
instruction-shaped bytes:

1. Build and run the recompiled cartridge with `--auxAddrFile` pointing at
   `code-analysis/aux_addresses.txt`.
2. When the game performs an indirect jump or call to an address that has no
   generated handler, the runtime appends that address to the aux file and
   exits **42** (or, for addresses already seeded as speculative stubs,
   records the confirmation without restarting).
3. Regenerate and rebuild with the new entry points, then run again.
4. Exit **43** means a seeded address still has no handler — usually a bad
   jump-table index or state bug, not “another missing function.” Any other
   exit stops the loop (clean quit, crash, Ctrl+C).

Two scripts automate that loop:

```bash
./discover_aux_smart.sh           # recommended: advanced speculative loop
./discover_aux_conservative.sh    # simpler fallback: leaner per-build compiles
```

**Smart** is the more advanced path and the one you should use by default. It
first refreshes the coverage map, runs a speculative scan over regions still
unmarked after the static fixpoint, and builds once with `--full --discover` so
candidate stubs are compiled in. Confirmed speculative hits are appended to
`aux_addresses.txt` on the fly; only truly unexpected addresses force a
rebuild. That cuts many iterations while still requiring a **live hit** before
an address becomes a permanent entry point — candidates alone never define
“code” for a normal build. The trade-off is a bulkier `generated/` tree while
discovery is running, so each compile is heavier.

**Conservative** is the simpler fallback. It never uses speculation: every new
unknown dispatch is one rebuild cycle (`build.sh --full` without `--discover`).
You pay for more iterations, but each compile stays lean — only confirmed entry
points are recompiled, with no intermediate stubs — so individual builds are
faster.

Normal day-to-day builds should use `./build.sh` or `./build.sh --full`
**without** `--discover`, so `generated/` contains only handlers for the
vector table plus confirmed aux addresses — live entry points, not speculative
guesses or data misread as instructions. Play (or script) enough of the game
during discovery that every mode, enemy, weapon, and cutscene path you care
about has been exercised; coverage is only as complete as the runs you make.

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

## License

This project’s own source and documentation are released under the [MIT
License](LICENSE) — Copyright (c) 2026 Rui Nelson. That does not cover the game
ROM or any Sega intellectual property; see [Legal considerations](#legal-considerations)
above.
