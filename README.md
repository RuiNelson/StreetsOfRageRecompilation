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

The generated C++ directory is ignored by Git and is not present in a fresh
clone. The first build must regenerate it from your local ROM:

```bash
./build.sh --full
```

`--full` creates `generated/` from the ROM via RageDecompiler
(`python -m tools recompile`, with `PYTHONPATH=../RageDecompiler` and
`code-analysis/manual_functions.txt`). Use `--full --discover` only for the
speculative discovery loop; that build includes temporary stubs.

Once `generated/Sor.cpp` and `generated/Sor.hpp` exist locally, ordinary
incremental builds do not need to run the recompiler again:

```bash
./build.sh                 # configure if needed, build Debug
./build.sh --clean         # wipe build/ and reconfigure
./build.sh --clean -t Release
./build.sh --full          # regenerate local C++, then build
```

## Run

Build and launch the recompiled cartridge in one step:

```bash
./build.sh -r -- --runSor --debug --debugUtils --rom rom/SOR.bin
```

Everything after `-r` / `--run` is passed to the `sor` binary. You can also
invoke `build/sor` directly once it exists. Default controls live in
`controls.yaml` (P1: arrows + Z/X/C/V; P2: WASD + J/K/L/O). Rebind them with
`--configControls` or by editing that file.

### Jump directly to gameplay

With a `--runSor --debugUtils` process already running, use the reusable
remote-control script to skip the boot/menu/character-select presentation
while preserving the real level initialization:

```bash
python3 tools/reach_gameplay.py axel
python3 tools/reach_gameplay.py adam
python3 tools/reach_gameplay.py blaze --host 127.0.0.1 --port 6969
```

The script cold-restarts, observes RAM to synchronize each real screen, and
uses only one-frame P1 joypad presses to skip the story/title, choose 1P, and
select the requested character. It does not write RAM or skip the level intro,
so normal campaign, player, sound, and music initialization still runs. It
returns only after observing the spawn-complete sound command `$A1`, then
verifying gameplay state `$16`, the in-game character ID, active player object,
full health, and an initialized music voice bank. The
Python client is discovered from the sibling `MegaDriveEnvironment` checkout,
so no package installation or `PYTHONPATH` setup is needed.

## Command-line arguments

The `sor` executable hosts the recompiled game and the controls configurator.
MegaDriveEnvironment runtime diagnostics live in the sibling
`MegaDriveEnvironmentSampleGame` executable. Flags are processed by CLI11;
`-V` / `--version` prints `0.1.0`.

### Cartridge (`--runSor`)

| Flag | Meaning |
|------|---------|
| `--runSor` | Boot the recompiled Streets of Rage cartridge |
| `--rom PATH` | ROM image (default: `rom/SOR.bin`) |
| `--debug` | Log CPU/VDP state once per second |
| `--debugUtils` | Enable debug hotkeys (except Ctrl+Q, which is always active), host cheats, and remote access |
| `--fullScreen` | Start the game in fullscreen |
| `--vsync N` | Frame sync: `0` = internal timer from `--hz` or `--turbo` (default); `1` = display VSync; `2` / `3` = half / third rate |
| `--turbo N` | With internal timing, run the VDP at `60 × N` Hz (`2` = 120 Hz, `10` = 600 Hz) |
| `--port PORT` | With `--debugUtils`, remote access TCP port (default: `6969`; `0` disables remote access) |
| `--auxAddrFile PATH` | Discovery mode: on an unknown indirect dispatch, append the address and exit **42** instead of aborting |

### Console pins

| Flag | Meaning |
|------|---------|
| `--lang jp\|en` | Language pin (default `jp` = Japanese / domestic; `en` = overseas) |
| `--hz 50\|60` | Video standard pin (default `60` = NTSC; `50` = PAL); turbo overrides only the host frame rate |
| `--silent` | Drop all audio chip writes (no sound output) |

### Host utility

| Flag | Meaning |
|------|---------|
| `--configControls` | Open the controller configuration UI |

Examples:

```bash
./build.sh -r -- --configControls
./build.sh -r -- --runSor --lang en --hz 60 --vsync 1 --debugUtils --port 6970
./build.sh -r -- --runSor --fullScreen
./build.sh -r -- --runSor --turbo 10 --silent
```

## Cheats

Host-side cheats are available only with `--debugUtils` and are wired through keyboard **option hotkeys** in
`SorRuntime` (not the in-game pad bindings). Hold **Alt** (Windows/Linux) or
**Option** (macOS) and press the key — the bare key alone does nothing, so
cheats do not clash with typing or with pad-mapped letters. Only the keyboard
source is handled here; gamepad option chords are ignored. Each activation
logs a short `[cheat] …` line via the async host `Logger`.

| Chord | Effect |
|-------|--------|
| Alt/Option + `L` | +1 P1 life (`$FFFFFF20`) |
| Alt/Option + `S` | +1 P1 special attack (`$FFFFFF21`) |
| Alt/Option + `P` | Toggle P1 punch power ×12 |
| Alt/Option + `K` | Kill all instantiated enemies, including bosses |
| Alt/Option + `W` | Call the police for the first active player without consuming a special |
| Alt/Option + `1`–`8` | Jump to level 1–8 (sets level/wave and forces the level-intro game state) |
| Alt/Option + `G` | Start good ending (`game_state` → `$0024` / `init_ending_good`) |
| Alt/Option + `B` | Start bad ending (`game_state` → `$001C` / `init_ending_bad`) |

Lives and specials simply increment a RAM byte (capped at `0xFF`). Level warp
writes the selected stage, clears the wave counter, and switches game state to
the level intro so the cart reloads that stage. Ending warps set the
corresponding init game-state word so the main loop runs `init_ending_*`.

Enemy kill scans the 32 instantiated-object slots and sends ordinary enemies,
bespoke bosses, and shared-framework bosses through their respective lethal
reaction states. It leaves players, pickups, weapons, scenery, and boss helper
objects untouched; normal enemy accounting and boss cleanup then advance the
encounter as usual.

The free police call uses the cartridge's normal activation gate and full
scripted sequence, but bypasses the special-counter requirement and decrement.
It still respects normal blockers such as the level intro, an active police
sequence, dead players, and Round 7 transitions.

Punch power is implemented in `SorCheats` and hooked from the hand-written
attack-strength routine (`$0041EA`). When enabled, only the player-1 object
(`$FFB800`) has its damage nibble multiplied by 12 and clamped to `0x0F`; other
objects and the upper nibble that drives hit reaction are left alone. Toggle
it off again with Alt/Option + `P` when you want normal combat.

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

### Analysis manuscript synchronization

Keep the reverse-engineering manuscripts synchronized after changing
`code-analysis/labels.csv` or `code-analysis/addresses.csv`:

```bash
./sync_ai_analysis.py          # update every ai-analysis/*.md manuscript
./sync_ai_analysis.py --check  # verify synchronization without writing
```

The stable address drives each reference, so a renamed CSV label is propagated
automatically. Known references use the canonical `` `$ADDRESS (label)` ``
form. The synchronizer also combines Address/Symbol table columns where both
refer to the same address and leaves fenced assembly or pseudocode unchanged.

### Offline asset decompression

`tools/decompress.py` extracts Nemesis, Enigma, and Kosinski blobs directly
from the ROM without starting the game or an emulator. Pass the format, ROM
offset, and destination file:

```bash
python3 tools/decompress.py nemesis 0x1B046 /tmp/enemy-cues.bin
python3 tools/decompress.py kosinski 0x71C6C /tmp/character-select.bin
python3 tools/decompress.py enigma 0x7228E /tmp/ui-map.bin \
  --plane-header --base-tile 0x0001
python3 tools/decompress.py nemesis 0x722A2 /tmp/art.png --png
```

The default input is `rom/SOR.bin`; select another dump with `--rom PATH`.
Offsets and tile bases accept decimal, `0x...`, or `$...` notation. The command
reports compressed bytes consumed and decoded bytes written; `--json` provides
the same metadata for scripts. `--png` renders complete 32-byte Mega Drive
4-bpp tiles as a diagnostic tile sheet; use `--columns N` to change its layout
and `--scale N` for nearest-neighbour enlargement. This is intended for art
streams—Enigma tilemaps still need their corresponding tileset to form a full
scene.

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

- `main.cpp` — CLI for `--runSor` and the controls configurator
- `CPU68K.hpp` / `RecompilationEnvironment.*` — 68000 register file and host
  environment that owns it
- `SorRuntime.*` — runtime hooks (hotkeys)
- `SorManualFunctions.cpp` — bodies listed in `manual_functions.txt`
- `SoRDecompress.cpp` — native Nemesis/Enigma/Kosinski decoders and direct VDP/Z80 delivery
- `SoRMainMenus.cpp` — native mode-select, OPTIONS, and character-select flow
- `SoRSound.cpp` — small native sound helpers
- `SorCheats.*` — thread-safe punch-power and free-police cheat state
- `build.sh` — configure, build, optional full recompile and run

Analysis data sits in `code-analysis/`: `aux_addresses.txt` feeds extra entry
points the static disassembler cannot resolve; `manual_functions.txt` names the
functions with hand-written C++ bodies; the CSV files hold labels, blocks, and
address metadata. Disassembler output goes to `output/` (local).

Forty-one cartridge functions are currently manual. Five provide the core
frame loop, cheat hooks, and host-friendly VBlank waits; seven implement native
decompression and asset delivery; two are small sound helpers; and the other
twenty-seven implement the mode-select, OPTIONS, and character-select flow in
`SoRMainMenus.cpp`.

## License

This project’s own source and documentation are released under the [MIT
License](LICENSE) — Copyright (c) 2026 Rui Nelson. That does not cover the game
ROM or any Sega intellectual property; see [Legal considerations](#legal-considerations)
above.
