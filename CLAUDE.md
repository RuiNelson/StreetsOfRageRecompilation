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
| `SoRControls.cpp` | Hand-written controller sampling, input remap, OPTIONS controls and character-select input |
| `SoRInput.*` | Keyboard and gamepad input read from SDL by the game, as actions (see **Controls**) |
| `SoRControlsConfig.*` | `~/.config/StreetsOfRageProject/controls.yaml`: schemes, bindings, gamepad choice, deadzone |
| `SoRControlsMenu.*` | OPTIONS > CONTROLS, the in-game controls menu (see **Controls**) |
| `SoRPause.*` | The pause menu, and `$10D2E (handle_pause_start_input)` that runs it (see **The pause menu**) |
| `SoRVideoConfig.*` | `video.yaml`, next to `controls.yaml`: frame sync, widescreen, fullscreen, integer scaling |
| `SoRVideoMenu.*` | OPTIONS > VIDEO, the in-game video menu (see **OPTIONS > VIDEO**) |
| `SoRMenuCanvas.hpp` | What those two menus share: the OPTIONS font, drawing to plane A, held directions |
| `SoRRemoteInput.*` | Remote (autoplay) input as actions through the execution data, and each player's status in the game data (see **Controls**) |
| `SoRSoundtrack.*` | Pre-rendered soundtrack playback (SDL3_mixer) driven by the ROM's own sound engine |
| `tools/soundtrack/` | Separate CMake project: the Genesis Plus GX renderer that produces that soundtrack |
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
- SDL3 (optional: CMake fetches `release-3.4.8` when `find_package(SDL3)` fails);
- sibling `MegaDriveEnvironment`;
- Git/network access for CMake `FetchContent`;
- Python 3 and sibling `RageDecompiler` for regeneration and analysis;
- `make` and network access for the first build of the bundled CPython (macOS, Linux).

`CMakeLists.txt` builds `sor`, fetches CLI11 and SDL3_mixer (`release-3.2.4`,
FLAC through its bundled dr_flac, every other codec off), and adds
`MegaDriveEnvironment`. `tools/` is excluded from the `sor` source glob. It deliberately requests shared yaml-cpp, zlib, and
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
Windows uses llvm-mingw (Clang, LLD, libc++, UCRT) with Ninja: pass
`-G Ninja -DCMAKE_C_COMPILER=<arch>-w64-mingw32-clang
-DCMAKE_CXX_COMPILER=<arch>-w64-mingw32-clang++` (the scripts do), and
`sor` raises its stack with `-Wl,--stack,8388608`. `MegaDriveEnvironment` is
static there and the C++ runtime is linked in (`-static-libstdc++`), so only
`SDL3.dll`/`SDL3_mixer.dll` sit next to `sor.exe`. The meta-repository README
has the complete command. Built and run on a real Windows machine (user, 2026-10-30).

## The Python AI in the process (`SoRPython`, `--ai-path`)

For the **1 PLAYER AI ASSIST** mode, `sor` runs the same Python AI the external
`autoplay` does (`../autoplay/ai-code/sor_ai`). User decisions: our own CPython as a
shared library next to the binary, not the machine's, called through the **C API
directly** (no pybind11), and part of every build, with no variant without it. Runs on
macOS and on Windows (user, 2026-10-30: `generate_cpp_and_build.py` compiled and the mode
works, llvm-mingw with the NuGet package). Linux is written and not yet tried.

- `cmake/CPythonRuntime.cmake` builds the pinned version (`SOR_CPYTHON_VERSION`, checked
  by SHA-256) at configure time into `.cpython/<version>-<arch>/`: gitignored and outside
  `build/`, so `--clean` and `--release` keep it. About 2 minutes, once, needing `make`
  and network access. To rebuild it (say, after changing its options), delete `.cpython/`
  and `python/` in the build folder. On Windows upstream has no llvm-mingw build, and a
  Python installed there is no use (its DLL is in its own folder, not on the search path,
  and is named by version), so CMake downloads the official build of the same version from
  NuGet (`python`, or `pythonarm64`; pinned by SHA-256): it has the DLL, stdlib, headers and
  import library in one file, and `py` plays no part. `python/Lib` and `python/DLLs` hold the
  stdlib there, and `vcruntime140*.dll` go next to `sor.exe` with the Python DLLs.
- It is PGO-optimized even for a Debug `sor`: `tools/tick_bench.py`'s mean tick was 1.28
  ms plain and 1.19 ms with PGO. Modules that link other libraries of the machine
  (`_ssl`, `_decimal`, `_zstd`, ...) are disabled, so the runtime depends only on the
  system: check with `otool -L` when changing the version. `_decimal` and
  `_remote_debugging` got in that way, tied to Homebrew.
- After linking, the library goes next to `sor` and the stdlib into `python/lib/python3.X/`
  beside it. `.cpython`'s own Python is usable for measuring (`DYLD_LIBRARY_PATH=.../lib`),
  and it lacks Tk, so it cannot import `sor_autoplay.app` or the tests that do.
- `scripts/run.py` passes `--ai-path autoplay/ai-code` unless told otherwise; `play.py` has the
  field. So the Builder's shortcut, which opens `play.py`, needs nothing of its own.
- `SoRPython` runs one thread (SDL, 16 MiB of stack) that owns the interpreter's whole
  lifetime: nothing else touches Python or waits for the GIL, so the AI can never slow
  the game. **It keeps its own time and is not woken by the game** (user, 2026-10-31): on its timer
  (33 ms, or 32 ms / `--turbo`: autoplay's poll, `sor_host.tick_seconds`) it asks for the memory,
  waits for it, ticks and sleeps what is left. **It lives from choosing the mode until the mode
  menu is back**, whose init stops it (a fresh interpreter next time; `--ai-assist` keeps it
  running through the menu). The stop blocks the CPU thread only on that screen. It is isolated:
  search paths are `--ai-path` (default `ai-code` in the CWD)
  and the bundled stdlib; the system Python and `PYTHON*` play no part. Code from that
  folder runs only once `SoRPython::start` is called: never at launch, except with the
  debug flag `--ai-assist`.
- It calls `sor_ai.embedded.run(sor_host)`; the module `sor_host` is the AI's view of the
  game. A failing AI (missing folder, exception, `sys.exit`) is reported on stderr as
  `[ai] ...` and the game goes on. One that does not end is abandoned 3 seconds into quitting.
- With AI ASSIST on (`SoRRemoteInput::setAiAssist`, the debug flag `--ai-assist`; the menu
  comes next), `sample_all_joypads` calls `SoRPython::serveFrame`: when the AI has asked, the
  CPU thread copies the 64 KiB of work RAM with no lock and answers; otherwise the call costs one
  atomic read, and a slow tick delays nothing. The AI
  answers with the same `SORX` documents as a remote client (`sor_host.set_input`), applied by
  `SoRRemoteInput::update` to a P2 of its own. P2 then plays from those alone: a client's P2
  records and physical P2 devices are ignored, while P1 and the observer work as ever.
  `HOST_MODE` in `SORG` tells clients. The flag is host state, not game state, so it is not
  in 68000 RAM, and the game sees an ordinary 2-player game.
- **Why no image pushed by the game on a frame count** (user, watching `--turbo 6`: P2 kept falling into round
  4's holes while P1, on the external autoplay, never did). When a frame overruns its time the frame count advances by
  2 between samples (about half the samples at x6), so it can stay on odd numbers: 121 samples in a row were measured,
  and `frame % 2` published nothing, the AI saw nothing and its last stick stayed held. Round 4, `--turbo 6`,
  external AI on P1: with the modulo 2,613-6,362 images skipped, P2 falling 0, 2, 6 and 3 times (P1 none), and a higher
  thread priority changed nothing; tick cost was never the cause. On the AI's own timer, three runs each, P2 embedded
  against P2 on the socket (P1 on the socket in both):

  | P2 | round 4 cleared | lives lost P1 / P2 | P2 falls | P2 score |
  | --- | --- | --- | --- | --- |
  | embedded | 3 of 3 | 0/0, 0/0, 0/0 | 0 | 40,800, 46,100, 47,100 |
  | socket | 2 of 3 | 0/0, 2/2, 1/1 | 0 | 42,800, 12,000 (cut at 75 s), 45,900 |

  Embedded mean tick 1.8-2.2 ms, 363-458 frame-gaps skipped of about 10,000 ticks, 0 failed. Four mode-to-menu cycles at x4:
  a thread each, ended each time, the game healthy. Normal speed not measured.
  The user, playing it afterwards: better; before, the AI sometimes stayed stuck holding an enemy (2026-10-31; that
  stall was not traced, so its cause is unconfirmed).
- **The mode's screens** (user: original look, the row after 2 PLAYERS, the player picks
  their character and then the computer's, "COMPUTER" for "2 PLAYER"). `select_menu_cursor`
  is 0 1 PLAYER, 1 2 PLAYERS, 2 AI ASSIST, 3 OPTIONS (it was 2); the row takes the third row's
  place and OPTIONS moves two cells down. Without the AI (`SoRPython::available()`: no
  `sor_ai/embedded.py` in `--ai-path`, or the Windows stub) the screen is the ROM's: three rows,
  index 2 skipped. Choosing the row sets `player_mode` to 3 and the flag and starts Python, so
  the mode screen's init clears the flag and each choice sets it; `--ai-assist` forces it through.
  The whole menu sits 5 tiles left and 2 up of the ROM's place (user), rows and cursor sprite.
  **QUIT** (user, 2026-10-31) is the row after OPTIONS, with or without the AI: cursor 4, the
  wrap's maximum. Choosing it pushes `SDL_EVENT_QUIT` in `select_menu_resolve_choice`, so it ends
  the game the way closing the window does.
  On character select the P2 cursor and Blaze's preview do not exist until P1 locks; then P1's
  buttons move a cursor spawned on a free character. They come from P1's `+$54`, not the sampled
  word: the select-screen pass runs every other frame, so a press can be gone from `$FFFC04`
  while it lives on in the copy `$AD8E` took after sampling (half the P2 taps were lost before).
  The label is ROM sprite art, 2 + 4 + 2 tiles with the middle shared with "1 PLAYER", so eight
  tiles at VRAM tile `$D1` (free on this screen) are drawn from the screen's small font
  (`$7C0`+letter, its opaque colour 2 kept only as an outline) and `screen_state_dispatcher`
  repoints the frame's sprite-table entries. VDP data-port reads lag a word here (no prefetch
  on address set): `stateSnapshot().vram` reads VRAM instead.
- `tools/ai_assist_fight.py` (autoplay) runs it live with an external AI on P1. Measured at
  `--turbo 2`: round 1 cleared with 34,900 points on P2 and no life lost, 6,944 ticks, mean
  1.12 ms, worst 7.0 ms, 2 skipped, 0 failed. The user called a full A/B against the socket
  unnecessary (2026-10-30); three runs each had finished by then, every one clearing round 1
  with no life lost: P2's score 34,500-37,100 embedded, 35,300-40,300 over the socket, a small
  sample. Frames from the join do not compare, because the two join at different times.

## OPTIONS > VIDEO

The user's decisions are in the workspace `CLAUDE.md`, **OPTIONS > VIDEO and `video.yaml`**.

- The row is host UI like CONTROLS: label at plane A row 15, free in the ROM's layout (CONTROLS' block ends at 13,
  EXIT is at 18, the cheat rows at 21 and 23), with a summary at column 18. It adds OPTIONS sub-states `$2E`
  (highlight) and `$30` (input), past the end of the ROM's table `$10C4`, so `game_start_screen_update` sends them
  to `options_highlight_controls` and `options_input_controls` before dispatching, and those routines tell the two
  rows apart. No new `StreetsOfRage` member: the class is generated. `options_row_nav` puts it between CONTROLS and
  the next row, cheat rows or not.
- The screen edits a copy of the live settings, which it reads from the VDP on opening, so it shows what Ctrl+F and
  the command line did. SAVE AND APPLY writes `video.yaml`; `options_input_controls` then calls `VDP::applyLive`, which
  waits for the main thread (a fullscreen change can hold the game a moment; nothing is running but this menu),
  sets the remote flag `GET_FLAGS` 0 and tells `SoRPython`. An attached autoplay re-reads that flag every second.
- Everything else that depends on widescreen already reads `vdp().widescreen()` live, and OPTIONS is only reachable
  from the mode menu, so no game is under way when it changes. A cold restart keeps it.
- Checked live on macOS at 120 Hz by driving it with remote buttons, with `HOME` pointed at a scratch folder: every
  row and help line, CANCEL, Start, RESET, SAVE, file contents, window and framebuffer size, the row order with and
  without cheat rows, leaving OPTIONS, a cold restart, a relaunch, every command-line override, a file with bad values
  (defaults), and speed: 119 fps on VSYNC, 61 on VSYNC HALF, 60 on AUTO, 120 at `--turbo 2` with `vsync` in the file.
  Fullscreen was applied and left through the menu. Not checked: Windows, Linux, a real gamepad or keyboard on
  this screen, Ctrl+F's effect on the screen's DISPLAY row.

## The pause menu

The user's rules are in the workspace `CLAUDE.md`, **The pause menu**.

- `$10D2E (handle_pause_start_input)` still sets `$FFFA46 (pause_text_flag)` to 3 and posts `$BD`, and
  `game_mode_ingame`'s paused path still draws the ROM's word PAUSE and freezes the game, so autoplay reads a pause as
  ever, and an AI lets go of the pad. While the flag is set it runs one menu frame on the players' presses (those not in
  `player_mode`: none; P2 under AI Assist: none). Start resumes, as on the ROM. In a question only the player asked
  answers, and anyone's Start is a NO, so a seat that cannot answer never holds the game.
- **Its state is host UI**, in C++ (`menu`, `forfeiting`); the only RAM it writes is what the ROM would: the flag,
  `play_se`, and the fields below. On resuming it clears `$FFFC05`/`$FFFC09`: the press that chose the entry would
  otherwise reach the players on that same frame. Seen live: RESUME on the Special button called the police, which
  also held off the KO for 9 s.
- **It draws over the finished frame** (`VDP::setFrameOverlay`), not in VDP memory: gameplay leaves no VRAM or sprites it
  can count on across eight rounds. The font is the mode menu's, copied from VRAM when that screen loads it (colour 1
  the letter, `$C` its shadow, 2 left see-through). The overlay reads the flag too, so a cold restart hides it.
- **Forfeiting** writes the continues word 0 and the life byte 1, then `adjust_player_health` with -`$50` on the object,
  as the time-over does (`$109DE`), and redraws the HUD. `$333E (resolve_player_hit_or_ko)` takes zero health as a KO on
  the next update, whatever the player was doing, so the rest is the ROM's: `$2B48`, the type `$0F` object, GAME OVER,
  then the attract sequence, or the other player plays on with `player_mode` updated. `forfeiting` then kills again
  anyone it sees back on their feet: the ROM's fall (`$358C`) takes no life at zero health and would respawn them.
- Checked live with remote buttons on macOS: both screens, NO, Start, RESUME; 1 player through GAME OVER to state `$0E`;
  2 players with P1 only, P2 only, both and neither saying YES, and P2's input ignored on P1's question; AI Assist; a
  forfeit during a KO under way; widescreen; a cold restart while paused. Not checked: a forfeit while falling into a
  pit (so `forfeiting`'s second kill has not run live), rounds other than 1, a real gamepad or keyboard, the sounds by ear.

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

## Host flags (`GET_FLAGS`)

`main.cpp` publishes the launch options through `RemoteAccess::setFlag`: flag 0
is `--widescreen`. Flag 1 was `--altControls`, removed with the per-player
schemes (each player's scheme is in the game data, below). `autoplay`'s
`host_flags.py` mirrors the indices; change both together.

## Controls (`SoRInput`, `controls.yaml`)

User decision: the game reads SDL itself instead of MegaDriveEnvironment's
`Controllers` (`main.cpp` calls `controllers().setPhysicalInputEnabled(false)`,
which keeps only the remote buttons autoplay sends), and binds **actions**,
not Mega Drive buttons. `SoRInput` follows `Controllers`' pattern: an SDL event
watch on the main thread keeps every key, pad button and axis under a mutex;
`sample_all_joypads`/`sample_one_joypad` (CPU thread) turn each player's state
into actions through `SoRControlsConfig` and write the held byte themselves.
The ROM's port writes and reads still run, so `$FFFC06`/`$FFFC0A` (pad absent)
and every byte in `$FFFC04`-`$FFFC0A` keep their format and frame.

- Actions: Up, Down, Left, Right, Start, Special, Punch, Jump, Back Attack,
  Pick Item (`pickup`), Throw Knife (`throw`).
- Per player, a scheme: `original3` (Special `$40`, Punch `$10`, Jump `$20`,
  Start `$80`: the ROM game, Punch picks up, Punch+Jump back attack) or
  `enhanced6` (the former `--altControls` layout: Back Attack is `$30`, Pick
  Item and Throw Knife are native per-player presses in `SoRCheats`, Punch has
  a native edge too, and `findPickupTarget` widens the box). `SoRCheats::
  enhancedControls(player)` is what `SoRInteractions.cpp` tests; `SoRInput`
  publishes it.
- The ROM's A/B/C layouts (`$FFFFC8 control_scheme`) are gone:
  `remap_player_gameplay_input` never rotates and `$FFFFC8` stays 0. Its
  OPTIONS row (sub-state `$18/$1A`) is now **CONTROLS** (below).
- Keyboard bindings (per scheme): directions, the scheme's actions and Start,
  by SDL scancode (saved as `SDL_GetScancodeName`). Key-downs with Ctrl, Alt
  or Cmd held are ignored (host hotkeys); losing focus releases every key.
- A key or pad button pressed since a player's previous sample counts for
  that sample even if already released (`keyTapped`/`buttonTapped`), so a tap
  shorter than a frame is not lost. Found while checking the keyboard with
  posted macOS key events (a scratch CGEvent helper holding a key): a held
  posted key arrives as down/up pairs about every 35 ms, and without the latch
  the player barely moved. Real keyboards send one down plus repeats.
- Gamepad bindings (per scheme): the actions only, as positional buttons or a
  trigger past half travel. The D-pad and the left stick are always the
  directions (deadzone per axis, percent, default 24 = MDE's old 8000) and
  SDL Start is always Start.
- Gamepad choice: `auto`, `none`, or `device` (GUID, name, and which of the
  connected pads with that GUID). Explicit choices are resolved first, then
  `auto` hands out the remaining pads in player order (P1 first); a chosen pad
  that is not connected counts as `auto`.
- Defaults (user): P1 arrows, ORIGINAL 3 Special/Punch/Jump/Start = Z X C V,
  ENHANCED 6 Special/Punch/Jump/Back Attack/Pick/Throw/Start = Z X C V B N M;
  P2 W S A D, H J K L and H J K Y U I L; gamepad ORIGINAL 3 Special RB, Punch
  West, Jump South, ENHANCED 6 Special East, Punch West, Jump South, Back
  Attack LB, Pick Item North, Throw Knife RB -- the shoulder buttons (L1/R1 on
  a DualSense): the user's "LT/RT" meant the bumpers, found with the trace
  below. Default scheme ORIGINAL 3.
- `SOR_INPUT_TRACE=1` logs every gamepad button, axis and hot-plug event and
  each player's action mask when it changes (`[input]` lines on stderr), with
  the scheme, the pad and its trigger values -- what a controller really
  sends. Run from a terminal while playing: macOS only gives controller input
  to the app in front, so a separate command-line probe sees nothing.
- `controls.yaml` is created with the defaults when missing; a missing or bad
  entry keeps its default; a file that does not parse is left untouched. No
  migration from MegaDriveEnvironment's `./controls.yaml` (user).
- Remote (autoplay) input is actions too, through the remote-access
  execution data (`SoRRemoteInput`, user decision): MegaDriveEnvironment's
  gamepad commands no longer reach the game. Opposite directions cancel after
  physical and remote are combined. See **Remote input** below.
- Each player's scheme is theirs, from `controls.yaml`: a remote client
  queries it (`PLAYER_STATUS`) and adapts, never sets it (user: autoplay
  detects the scheme by querying the host). `--altControls` is gone.

### OPTIONS > CONTROLS (`SoRControlsMenu`)

User request: the in-game menu replaced `--configControls` (gone from `sor`
and `play.py`; MegaDriveEnvironment keeps its own UI for the sample game).

- The row: `$1360 (options_highlight_controls)` is now manual (it only adds 2
  to the sub-state and draws the row white); `options_menu_build` draws it grey
  instead of loading tilemap bundle `$0D` (the CONTROL label) and the layout
  strings; `options_row_nav` no longer skips it. It shows "CONTROLS" at row 11
  and "P1 <scheme>" / "P2 <scheme>" at rows 11 and 13, column 18 (where the
  ROM's label and layout block were). Punch, Jump or Special on that row opens
  the menu; Start still leaves OPTIONS.
- While open, `$1390 (options_input_controls)` runs one menu frame and
  returns; `game_start_screen_update` ignores Start (it goes back a screen).
  Its state is C++ (host UI): no work-RAM field changes. On closing, the menu
  clears what it drew and `options_input_controls` redraws the screen as
  `options_menu_build` does (bundles `$04 $0B $0C $10`, the sound name and
  difficulty grey, the cheat rows when unlocked) with CONTROLS selected.
- Screens: CONTROLS (PLAYER 1, PLAYER 2, BACK); PLAYER n (SCHEME, DEVICE,
  BIND GAMEPAD, BIND KEYBOARD, JOYSTICK DEADZONE, BACK; the gamepad entries
  are skipped and say NO GAMEPAD without one, BIND KEYBOARD without
  `SDL_HasKeyboard()`, for ports without a keyboard); DEVICE (AUTO, up to 7
  pads, NONE; `>` marks the saved choice, P1/P2 who reads each pad now); BIND
  (each action in turn; a key/button already given to an earlier action in
  this pass shows ALREADY USED; Esc or any gamepad's Start cancels and keeps
  the old bindings; finishing saves); JOYSTICK DEADZONE (left stick X/Y and
  tilt in percent, a 25-cell bar with `:` at the deadzone, INSIDE THE
  DEADZONE / MOVING; Left/Right change it by 1 with repeat, applied at once,
  saved on leaving; the stick does not move the menu there).
- Navigation from any device (`SoRInput::menuButtons`): arrows, Enter, Esc,
  both players' direction keys, their Punch/Jump/Special keys or pad inputs
  to confirm, their Start to go back; any pad's D-pad, left stick (half
  travel), South, Start; and the remote (autoplay) buttons (directions,
  A/B/C, Start), which is how it is tested. Directions repeat after 20 frames,
  every 6. The menu reads its own tap latch (`kMenuLatch`), separate from the
  players' samples. Sounds: `$B9` move, `$BA` confirm, written to
  `$FFF00A (play_se)` as `handle_pause_start_input` does.
- Drawing: the OPTIONS font (a text byte is the tile index: `$01`-`$1A`
  A-Z, `$1B`-`$24` 0-9, `$26` `-`, `$27` `>`, `$29` `.`, `$2A` `,`, `$2B` `'`,
  `$2C` `"`, `$2D` `?`, `$2E` `!`, `$2F` `:`; no `+ % /`) into plane A at `$C000`,
  palette line 0 (white) for the selected entry and line 1 (grey) for the
  others: the only two lines with a text colour on this screen. Each frame the
  screen is composed in a 64x28 buffer and only changed cells are written
  through the VDP ports. Text stays in columns 2-39 and rows 3-25 so it fits
  Mode 40 and widescreen.
- Checked (before remote input moved to the execution data) by driving it
  with remote buttons and posted macOS key events
  (`read_framebuffer` screenshots, plain and widescreen, with `HOME` pointed
  at a scratch folder so the user's `controls.yaml` was untouched): every
  screen, a full ORIGINAL 3 keyboard bind (duplicate refused, saved), Esc
  cancelling mid-bind, scheme toggling, closing and the OPTIONS redraw, and
  row navigation afterwards. Not checked here: a real gamepad (DEVICE list,
  BIND GAMEPAD, the deadzone readout).

### Remote input (`SoRRemoteInput`)

User decision: autoplay sends its input through the execution data as
actions, in a binary format built with Python's `struct` (not YAML), and
reads each player's state back from MegaDriveEnvironment's game-data buffer,
which only the game writes (`GET_GAME_DATA`). The layout is in
`SoRRemoteInput.hpp` and mirrored by `autoplay/ai-code/sor_ai/remote_input.py`
(change both together): a container (`magic:4, version:u16, records:u16,
serial:u32`) and tagged records, unknown tags skipped.

- `sample_all_joypads` calls `SoRRemoteInput::update` once a frame with
  `gameUptimeFrames()`: it re-reads the buffer (applying it only when the
  bytes change), advances both players' schedules and writes the status.
- `PLAYER_INPUT`: `held` is the steady state; a new `seq` starts its schedule
  (`actions, frames` entries) on the first frame sampled, in place of `held`;
  with the QUEUE flag a schedule arriving while another plays waits for it and
  one frame of `held` (a fresh edge), only the newest waiting one kept --
  MegaDriveEnvironment's `QUEUE_PRESS_BUTTONS`. Lockstep sends the tick's
  frames as one schedule, without the flag.
- `PLAYER_STATUS` (game data): each player's effective scheme, last `seq`
  applied and the frame its schedule ends.
- A client that disconnects (`RemoteAccess::clientConnected()`) or a restart
  (the frame count going back) leaves its document behind: it is dropped
  (actions cleared) until the buffer changes.
- The CONTROLS menu takes both players' remote actions as one more device
  (directions, Punch/Jump/Special confirm, Start back).

## 6-button pickup reach (ENHANCED 6)

User request: picking items up with Pick Item was frustrating for people,
because a missed press shows nothing (in ORIGINAL 3 the same press punched,
which was the feedback). `findPickupTarget` (`SoRInteractions.cpp`, the `$3136`
port) widens the box for a player on ENHANCED 6 from X ±`$14` / lane ±`$10` to
±`$1C` / ±`$18`; the height test, the slot scan, eligibility and the work-RAM
writes are the ROM's, and on ORIGINAL 3 the box is the ROM's. The user chose
only this: no press buffer, no miss sound. `autoplay` presses Pick Item inside
the ROM box, which the wider one contains, so it needs no change.

## Pre-rendered soundtrack (`--sound`)

User decision: the host's real-time YM2612/PSG/Z80 emulation is not good
enough for this game (the 68000 sequences the music and the BUSREQ/DAC-busy
handshakes cost host CPU that hardware never pays), so `sor` plays renders made
by Genesis Plus GX with the **Nuked OPN2 YM3438** core, **no low-pass filter**
(post-processing is for later), **48 kHz lossless (FLAC)**. Music, sound
effects and PCM voices are all pre-rendered. Music under `--turbo` plays at
normal speed (turbo is a development mode). A sound effect borrowing a music
channel is a hardware limitation the port does not reproduce: the music keeps
playing under it.

`--sound prerendered` is the default and falls back to `emulated` (with a
message) when `--soundtrack DIR` (default: `soundtrack/` next to the ROM, i.e.
`rom/soundtrack/`) has no `manifest.tsv`. `--silent` still disables all audio.
In prerendered mode `Sound` is disabled (chip writes dropped) and nothing else
changes: the 68000 engine runs every tick and keeps `$FFF000`-`$FFF36F` exactly
as on hardware, which game logic reads (`handle_pause_start_input` tests
`stop_music`). `SOR_SOUNDTRACK_TRACE=1` logs every engine event.

### Rendering: `tools/soundtrack`, `../scripts/render_soundtrack.py`

A standalone CMake project (so `sor` never fetches the emulator). It fetches
upstream Genesis Plus GX at a pinned commit (`SOR_GPGX_COMMIT`) into its own
build tree and compiles core + libretro glue **unmodified** (statically, no
CHD/Tremor/zlib), plus libFLAC 1.5.0. `gpgx_glue.c` reaches RAM, Z80 RAM, the
cartridge ROM and `audio_set_rate`, which libretro does not expose (libretro
pins 44.1 kHz). The script builds it, renders into `rom/soundtrack.rendering/`
and swaps it in (`$81`-`$91` as two stems each, `$A0`-`$CF`, `$D0`-`$DB`).
`--chip ym2612` selects Nuked's Model 1 mode. Diagnostics: `--verify-stems`
and `--measure-fade SECONDS` (with `--ids` to pick tracks).

How a render works (`render_soundtrack.cpp`):

1. Boot 600 frames, then park the main loop by writing `bra.s *` at `$3A2
   (game_infinite_loop)` in GPGX's ROM copy (the boot checksum already ran):
   VBlank keeps calling `$72914 (sound_engine)` and nothing else posts sounds.
   Post `$E1`, wait, check every channel block is idle, save a state.
2. Per ID: restore the state, write the ID to `$FFF00A (play_se)`, run frames.
   The machine is paced at **exactly 60.0 Hz** (the host's rate) so a frame is
   exactly 800 samples and loop points are exact sample indices.
3. Loop: the first frame N whose sequencer RAM (`$FFF040`-`$FFF1EF` for music,
   `$FFF1F0`-`$FFF36F` for effects) is byte-identical to an earlier frame M.
   `loop_start` is M + 5 s (release tails of the intro gone), `loop_end` N + 5 s
   (end of file). The RAM repeats but the chips' free-running phases (PSG tone
   counters, LFO) do not, so the raw seam jumped by ~1200; the last 25 ms
   before `loop_end` are cross-faded into the samples before `loop_start`, which
   makes the jump continuous.
4. Music is split into two stems that sum exactly to the full render (user
   request, so the `$E0` fade can be played as the ROM does it): `_fm`, the
   same frames rendered again with the PSG preamp at 0 and the DAC disabled,
   and `_dacpsg` = full - FM. The DAC is disabled by setting the operand of
   both DAC-enable writes to `$00` (Z80 `$004E`, the driver's `ld
   (hl),$80` after `ld (hl),$2B`; ROM `$72F91`, `$72F8A (sound_reset_all)`'s
   immediate), which keeps every instruction and its timing. Silencing the
   sample bytes does **not** work: the driver is 4-bit DPCM (delta table at
   Z80 `$01C8`, so `$80` fill is not silence and breaking the table is worse)
   and writes each delta into its own code, so the sample bytes change the
   Z80's timing, the 68000's DAC-busy waits and every FM write after them
   (measured: thousands of differing samples, peak ~13000). `--verify-stems`
   checks the split: full minus the DAC-off pass is the DAC alone, exactly
   flat for long runs between drum hits only if the FM did not move.
5. A non-looping sound ends when its channel blocks are idle (and, for PCM,
   `$A01FFD (z80_dac_busy)` is clear), plus up to 4 s of release; the settled
   tail is trimmed. Every render starts at its first non-zero sample (the base
   state is digital zero; the first chip write lands 20-400 samples into
   frame 1).

`manifest.tsv` is `# format 2` (the host refuses older ones and asks for a
re-render): `id kind file cut_file frames loop_start loop_end name` (hex ID;
`cut_file` is the DAC+PSG stem, `-` for effects and voices; `-1` for no loop;
names decoded from the sound test's `$1C684` rows). The FLAC files also carry
`LOOPSTART`/`LOOPLENGTH` tags.

Render facts: `$83` (title) does not loop; `$8D` (name entry) and `$8E` (round
clear) are the same data; `$BD`, `$CF`, `$D1` and `$DA` are not in the sound
test. Loop starts range from 6 s (`$8A`) to 147 s (`$84`).

### Playback: `SoRSoundtrack.cpp`

`StreetsOfRage::logEntry` (every traced entry) forwards three engine entries:

| Entry | Action |
| --- | --- |
| `$72D08 (sound_process_queue)` | Replicate the ROM's pick: each `play_se` slot with bit 7 and priority (`$72824 (sound_priority_table)`, ID - `$81`) at least the running one replaces `$FFF009 (sound_pending_id)` |
| `$72F8A (sound_reset_all)` | Stop music and effects; a PCM voice keeps playing, as the Z80 keeps streaming it. Reached by every music start, `$E1`, `$E3`, `$E4`+, and the end of a fade |
| `$73066 (sound_fade_music)` | Right after the queue each tick: start the pick (music: both stems streamed from disk and started under one mixer lock, sharing the loop; effect predecoded on one track per SFX channel block, header byte 3 through `$72E74`; voice on one PCM track), `$E2` stops the effects on `$FFF1F0`/`$FFF220`; when `stop_music` leaves 0 (`$7302E` armed the fade) stop the DAC+PSG stem at once; set the FM stem's gain from `stop_music` (15 steps of 0.75 dB, then the reset); take 6 dB off the FM stem while `$FFF007 (sound_engine_gate)` is positive |

`$BD` is the pause jingle (`handle_pause_start_input`): its sequence starts with
`$F9`, which sets the gate to `$01` (music keyed off and frozen) or, when set,
to `$80` (released on the next tick), so pausing and unpausing both post `$BD`.
**Paused, the renders do not stop** (user, 2026-10-31): the music plays on under the pause menu, with the FM stem 6 dB
down and the DAC+PSG stem as it is. The engine's channels still freeze as on hardware, since its fields are the game's,
and only the output differs; `--sound emulated` keeps the ROM's silence. Checked with an `SDL_AUDIO_DRIVER=disk` capture
of round 1 through a 5 s pause, fitted against the stems every 0.1 s: FM 1.0000 and DAC+PSG 1.0000 before, 0.5012 and
1.0000 during (residual ~1e-13, once the jingle's 0.5 s is over), 1.000 and 1.000 after, on the same stem offset
throughout, so the music never lost its place. Not heard by ear.
Left as a known difference: at the fade start the hardware lets a DPCM drum
sample that is already streaming play out (at most ~0.2 s); the DAC+PSG stem
stops at once.

Validated on macOS (arm64): `SDL_AUDIO_DRIVER=disk` capture of the attract
mode is sample-identical to `music_83_fm.flac` + `music_83_dacpsg.flac`; with
`$81` then `$E0` posted, the capture is exactly the stem sum until the fade and
then exactly the FM stem times a gain stepping 0.75 dB every 5 frames (fit
error ~1e-15), the DAC and PSG gone; `--verify-stems` finds the DAC alone
exactly flat for 2.6%-70% of every track (longest runs 0.1-41 s);
`--measure-fade 30` gives -11 to -13 dB at step 15 on every looping track.
Earlier, the single-file build: a 2-minute autoplay session plays
title, select, round 1, effects and voices with no clipping; `$BD` x2, `$E0`,
`$E1`, `$E2` posted through remote access behave as above; CPU 21% vs 29%
emulated over the same 45 s. Not tested: Windows and Linux builds, `--hz 50`.

## Widescreen (`--widescreen`)

`sor --widescreen` calls `MegaDriveEnvironment::setWidescreen(true)` from
`main.cpp`, switching the VDP from Mode 40 to the host-only Mode 48 (48×27
tiles, 384×216; see `MegaDriveEnvironment/README.md`). Game logic, the work-RAM
state and the 320×224 timing are untouched, so `autoplay` addresses and
coordinates are unchanged; only the output grew. Every layer is centred: planes
(from plane pixel -32), sprites (origin 96) and the window all sit 32 px right of
Mode 40, so sprites stay aligned with the backgrounds. Remote snapshots
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
(`$7546`) the police-special recovery (`$40A6`), the wave batch placement (`$CC0`: enemies and bosses created beyond the old
screen edge are placed `$20` further out, or they would activate at once and pop into the widened screen; scenery, props
and an enemy waiting behind a shop front -- whose first state `$DDE6` creates the shutter `$54` at its X -- keep their X,
or they leave their place in the background) and the sprite-piece cull (`$B0FE`, whose left bound `$60` made sprites vanish
while still partly visible at the far left of the widescreen), and the "ROUND n START" banner (`$11A68`, `$11A76`,
`$11AC4`: its halves wait and end just outside the 320-pixel sprite window, so they showed in the side margins; under
widescreen they start and end `$20` further out and slide the extra distance in the same frames, the step read from the
number sprite's Y, so the intro's timing and substates stay the ROM's). `autoplay` follows it (`GET_FLAGS` flag 0, `autoplay/CLAUDE.md` **Widescreen**). Never patch the ROM (the boot checksum hangs the game) and
never edit `MegaDriveEnvironment` for game rules. A port must also leave the CCR flags the caller branches on
exactly as the ROM does at every exit: `update_player_object` skips the player's update on `bne` after
`$40A6 (police_special_player_update_gate)`, and that Z flag is the whole of the police special's freeze. The full table, the checks and what is left are in
`../docs/WidescreenLogBook.md`.

**Round 2 reset in widescreen (fixed).** The console restarted ~110 s into round 2 (4 of 4 runs; plain never).
Found with `--call-log` (a second `entry,000200`), a backtrace at the second `init()` and dumps of the object
table. The ROM itself resets: `prepare_next_spawn_section` (`$810`) scans the first 32 object slots and
`jmp init`s (`$884`) when an ordinary enemy's `+$0E` tile word has no palette line 1 or 2, i.e. when it has
never run its first animation. Round 2's wave 3 spawns a Jack with `+$40` bit 7 set -- `$A59C` activates such an
object only once `cam_x` passes its X, and the camera locks at `$0DC0` -- and the `$CC0` port had moved him from
`$0DA8` to `$0DC8` (the `+$20` for a column beyond the screen edge): past the lock, never awake, caught by the next
section's check. `spawn_object_batch` now leaves a camera-gated object (`+$40` bit 7) where the ROM puts it.
Checked: widescreen round 2 reaches Souther (2 of 2, 0 of 6 before), plain round 2 and widescreen round 1
unchanged. `spawn_object_batch`'s other `init()` (no free slot) is the ROM's `$D66`.
Round 8 reset the same way in 2P: its gate is the camera's right edge (`cam+$144`, widened to `+$164`), and a Garcia
(probably a 2P-only spawn) at `cam+$160` of the lock never woke; at a lock (`max - min < 4`) `$A59C` now uses the ROM's edge
(`../docs/WidescreenLogBook.md`). Any widened gate that can only *delay* a wake must fall back to the ROM's at a lock. The plain-host reset
described above may be the same `$884` check (a sweep or a timing that leaves a just-spawned enemy at the
prepare); not verified.

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

`--call-log PATH` optionally records emulated 68000 subroutine entries and
`bsr`/`jsr` calls as typed CSV with the columns
`event,source,callsite,target`. A `call` event contains the original ROM
source-subroutine entry, instruction address, and exact destination. An
`entry` event contains the entered address in `source` and empty `callsite` and
`target` fields. Addresses are uppercase, six-digit hexadecimal ROM addresses
without a prefix. The file is truncated at startup; without this option,
logging is disabled and the entry/call hooks only perform a null check.
Generated and hand-written subroutines must trace their true dynamic entry.

```bash
./build/sor --rom rom/SOR.bin --call-log ../calls.csv
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
  three NOPs. When the chips are disabled (`--silent`, or the pre-rendered
  soundtrack) it skips the DAC-busy retry: the YM2612 drops every write, so
  there is no Z80 DAC stream to stay clear of.
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
  address (`../autoplay/ai-code/sor_ai/memory_map.py`, `rom_data.py`) and
  its enemy models are frame-exact, so a relocated or retimed field breaks
  the AI. This is a project rule: reimplementation makes the code readable,
  never a different data model.

Do not turn unknown behavior into a host shortcut merely because it makes one
scenario pass.

## Run and observe safely

Boot defects can spin forever and SDL windows may outlive plain `SIGTERM`. On
systems with GNU `timeout`, always use a kill grace period:

```bash
timeout -k 3 20 ../scripts/run.py rom/SOR.bin --debug-log
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
- `rom/soundtrack/` (renders derived from the ROM);
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
