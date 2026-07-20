# Streets of Rage Story Mode and Campaign Flow

**Manuscript:** static analysis of the original ROM and the C++ recompilation

**Scope:** story opening, attract mode, campaign start, progression through all eight rounds, round-clear screen, Mr. X's offer, and ending selection

**Primary sources:** `output/sor.asm`, `generated/Sor.cpp`, `SorManualFunctions.cpp`, `code-analysis/addresses.csv`, `code-analysis/labels.csv`, and `rom/SOR.bin`

The names used below match the symbols added to the analysis CSVs. Meanings inferred from context rather than proved by unambiguous reads and writes are explicitly identified as such.

---

## 1. Main result

The game has no single class, structure, or function representing “story mode.” The campaign is composed of several state machines that communicate almost entirely through Mega Drive work RAM:

1. `$FFFF00 (game_state)` selects the global mode.
2. Each mode normally has an initialization handler and an update handler.
3. Complex modes use a secondary state index and their own jump table.
4. `$FFFF02 (level)` is the persistent campaign counter, ranging from `0` through `7`.
5. After each round, the round-clear screen increments `$FFFF02 (level)` and returns to the level-introduction state.
6. After round 8, `$FFDE10 (bad_ending_selected)` selects the good or bad ending.

Normal campaign flow:

```text
Sega ($00/$02)
  -> story opening ($04/$06)
  -> title ($08/$0A)
  -> mode menu ($10/$12)
  -> character select ($20/$22)
  -> round introduction ($28/$2A)
  -> gameplay ($14/$16)
  -> round clear ($18/$1A)
       | level < 7: level++ and return to $28
       ` level = 7: $24 good ending or $1C bad ending
```

There is an important distinction between two uses of the word “story”:

- `$8FD0 (init_intro)` and `$904E (game_mode_intro)` implement only the narrative opening before the title.
- The playable campaign is the `level start -> in-game -> round clear` cycle governed by `$FFFF00 (game_state)`, `$FFFF02 (level)`, completion flags, and the final offer.

---

## 2. Global dispatcher

### 2.1 Assembly implementation

The core loop at `$3A2 (game_infinite_loop)` reads `$FFFF00 (game_state)`, doubles it to obtain a four-byte table offset, and looks up `$3BA (game_state_handler_table)`:

```asm
moveq  #0,d0
move.w game_state,d0
add.w  d0,d0
move.l $3BA(pc,d0.w),d0
movea.l d0,a0
jsr    (a0)
jsr    sync_z80_1
bra.s  game_infinite_loop
```

Because every `$FFFF00 (game_state)` value is even, each value selects one longword. The table contains eleven initialization/update pairs:

| `$FFFF00 (game_state)` | Handler | Purpose |
|---:|---|---|
| `$00` / `$02` | `$8EAC (init_segascreen)` / `$8F44 (game_mode_segascreen)` | Sega logo |
| `$04` / `$06` | `$8FD0 (init_intro)` / `$904E (game_mode_intro)` | story opening |
| `$08` / `$0A` | `$90C8 (init_titlescreen)` / `$9102 (game_mode_titlescreen)` | title screen |
| `$0C` / `$0E` | `$9128 (init_top10score)` / `$9152 (game_mode_top10score)` | top-ten scores |
| `$10` / `$12` | `$9170 (init_selectscreenmode)` / `$919A (game_mode_selectscreenmode)` | main menu and OPTIONS |
| `$14` / `$16` | `$10840 (init_ingame)` / `$1087A (game_mode_ingame)` | gameplay or attract mode |
| `$18` / `$1A` | `$91A0 (init_roundclear)` / `$91CE (game_mode_roundclear)` | round bonuses and results |
| `$1C` / `$1E` | `$9250 (init_ending_bad)` / `$9278 (game_mode_ending_bad)` | bad ending |
| `$20` / `$22` | `$927C (init_characterselectscreen)` / `$92A8 (game_mode_characterselectscreen)` | character select |
| `$24` / `$26` | `$91D4 (init_ending_good)` / `$9214 (game_mode_ending_good)` | good ending |
| `$28` / `$2A` | `$106EA (init_levelstart)` / `$1077C (game_mode_levelstart)` | round presentation |

The pattern is consistent: an initialization handler normally ends with `addq.w #2,game_state`, after which the global loop calls the corresponding update handler every frame.

### 2.2 Recompiled C++ implementation

`SorManualFunctions.cpp` preserves this mechanism in the manual implementation of `Sor::game_infinite_loop`. Its relevant constants are:

```cpp
constexpr m_long kGameState      = 0xFFFFFF00u;
constexpr m_long kStateJumpTable = 0x000003BAu;
```

The C++ reads the state word, calculates `state + state`, fetches a longword pointer from ROM, and invokes `dispatch(handler)`. The recompilation therefore does not replace campaign control with a modern abstraction; it deliberately preserves the cartridge's original execution model. The related routines in `generated/Sor.cpp` are nearly literal translations of the 68000 instructions and still expose the original RAM addresses.

---

## 3. Story opening, title, and attract mode

### 3.1 Starting a new session

`$8FD0 (init_intro)` does more than draw the opening. It also resets the persistent state of a new session:

- `level = 0`;
- `wave = 0`;
- P1 and P2 scores to zero;
- player death and status flags to zero;
- score-based extra-life pointers to zero;
- `$FFFF38 (player_mode_copy)` to zero;
- opening music to sound ID `$83`.

This makes the opening the logical boundary of a new campaign. The menu and character-select screens that follow operate on this freshly cleared session.

### 3.2 Skipping the opening

In `$904E (game_mode_intro)`, Start can route execution to:

- the title (`game_state = $08`) while the scene is in an early phase;
- the main menu (`game_state = $10`) once the scene has passed its internal threshold.

Without input, the shared `$B6DE (story_scene_timeline_update)` routine creates narrative objects from a timed list. When the list ends, it writes the next state configured for that scene. `$3F65E (story_scene_select_script)` reads this configuration from `$3F680 (story_scene_config_table)`: the opening configuration ends with state `$00`, which `$904E (game_mode_intro)` converts into entry into attract mode.

### 3.3 Attract mode is not a normal campaign

When the title times out, code at `$90AA` prepares attract mode:

```asm
move.w #$0014,game_state
move.b #1,demo_mode
move.l #$00FF7000,demo_ai_input_p1
move.l #$00FF8000,demo_ai_input_p2
```

While `demo_mode != 0`:

- joypad code at `$813C` merges bytes from scripted input streams;
- `$10840 (init_ingame)` internally calls `$106EA (init_levelstart)`, bypassing the interactive start flow;
- characters, lives, and demo duration are forced;
- Start sets bit 7 of `$FFFF2A (demo_mode)`, starts a fade, and aborts the demonstration;
- after the fade, `$1087A (game_mode_ingame)` returns to the Sega logo (`$00`) or the top-ten screen (`$0C`) instead of entering round clear.

States `$14/$16` are therefore shared by the campaign and the demonstration, but `$FFFF2A (demo_mode)` changes their input source, HUD setup, and exit route.

---

## 4. Campaign start and round introduction

After character confirmation, `$17A2 (initialize_player_continues)` initializes continues and lives, then writes:

```asm
move.w #$0028,game_state
```

### 4.1 `$106EA (init_levelstart)`

The round initialization handler:

- clears almost all work RAM;
- resets `wave = 0`;
- sets `level_intro_active = 1`;
- initializes the fade counter to `$40`;
- loads art, tilemaps, HUD data, and other resources selected by `$FFFF02 (level)`;
- calls `$E5C (start_round_setup)`;
- recreates the P1/P2 objects with the selected character IDs;
- advances `$FFFF00 (game_state)` from `$28` to `$2A`.

Level data is indexed by the current round number. For example, `$576 (load_level_data)` multiplies `$FFFF02 (level)` by six and selects a six-byte entry in the ROM table at `$1C378 (level_resource_table)`.

### 4.2 Round-presentation state machine

`$1077C (game_mode_levelstart)` enters `$11A50 (level_intro_dispatcher)`. `$FFFB48 (level_intro_substate)` selects one of six entries in `$11A5C (level_intro_jt)`:

1. wait for fade-in;
2. move the two halves of the “ROUND n” banner toward the center;
3. wait `$60` frames;
4. move the banner off-screen;
5. wait `$30` frames;
6. execute `$11B04 (level_intro_finish)`, which writes `game_state = $14`.

Once state `$14` is reached, `$10840 (init_ingame)` configures the fade and timing counters and advances immediately to `$16`.

---

## 5. Gameplay and within-round progression

### 5.1 Per-frame update

On the normal path through `$1087A (game_mode_ingame)`, while neither fading nor paused, the game updates:

- the clock and scene boundaries;
- pause handling and second-player joining;
- Mr. X's offer state machine;
- HUD and objects;
- wave logic;
- pending art transfers;
- the secondary level-flow dispatcher.

`$464 (level_flow_handler)` uses `$FFFA72 (level_flow_flags)` to prevent loading, music, and setup phases from being repeated. `$FFFF04 (wave)` selects enemy groups within the round, while `$FFFF02 (level)` selects the main round table.

### 5.2 Completing a round

`$FFFA73 (end_of_level_flag)` explicitly marks the playable portion of a round as complete. It can be set by round-dependent paths, including wave exhaustion and the special completion condition for level index 6, which is round 7.

While this flag is set, `$502C (end_level_player_exit_update)` forces the players through their stage-exit animation and position. When the exit completes, it writes:

```asm
move.b #1,fade_out_flag
move.w #$40,palette_fade_counter
```

Once the fade completes, the normal campaign path in `$108CC (ingame_finish_fade)` selects:

```asm
move.w #$0018,game_state
```

The physical end of a level therefore does not increment `$FFFF02 (level)` directly. It transfers control to the round-clear screen.

The same routing point also has alternate paths:

- attract mode returns to the presentation loop;
- game-over and continue flags can enter the top-ten screen or another recovery sequence;
- a special two-player story branch forces `level = 5` and re-enters state `$28`.

That final branch is visible in the code, but its controlling flag has not been given a permanent CSV name because its complete meaning depends on every possible outcome of the player-versus-player confrontation.

---

## 6. Round clear is the campaign manager

### 6.1 Initialization

`$91A0 (init_roundclear)` calls `$181EA (round_clear_sequence_init)`. This routine:

- normalizes dead or inactive player objects;
- loads the results screen;
- prepares time, difficulty, life, and special-attack bonuses;
- includes additional remaining-life values on the final round;
- clears transient state, including `$FFDE00 (mr_x_offer_flag)`;
- initializes `round_clear_substate = 0`.

### 6.2 Score tally and campaign advancement

`$1833C (round_clear_sequence_update)` uses `$FFFB4C (round_clear_substate)` as an offset into `$18350 (round_clear_jt)`. The table controls animations, bonus-to-score conversion, delays, and fading.

The decisive routine is `$183B0 (round_clear_advance_campaign)`:

```asm
cmpi.w #7,level
beq.s   final_round
addq.w  #1,level
move.w  #$28,game_state
rts
```

The eight rounds are therefore represented by `level = 0..7`. For levels `0..6`, round clear is the only observed normal-campaign routine that increments the level counter and starts the next presentation.

### 6.3 Ending selection

On round 8 (`level == 7`), execution falls through to `$183C4 (round_clear_select_ending)`:

```asm
moveq #$24,d0
tst.b  bad_ending_selected
beq.s  set_state
moveq #$1C,d0
set_state:
move.w d0,game_state
```

This directly proves the meaning of `$FFDE10 (bad_ending_selected)`:

- zero selects `game_state = $24`, the good ending;
- nonzero selects `game_state = $1C`, the bad ending.

---

## 7. Mr. X's offer and the narrative branch

### 7.1 Activation

In the final section of round 8, player-object logic at `$50A6` detects that the active players have entered the scene area. Once all active players are in position, it executes:

```asm
move.b #1,mr_x_offer_flag
move.b #1,stop_clock
```

From that point onward, `$11B4C (mr_x_offer_update)`, which runs every gameplay frame, stops returning immediately and begins processing `$FFDE04 (mr_x_offer_state)`.

### 7.2 State-machine structure

The offer state is used in two ways:

- as a byte index into `$120AA (mr_x_offer_control_table)`, which determines whether control is blocked, enabled, or waiting for a choice;
- doubled to index `$11B94 (mr_x_offer_jt)`.

The control-table interpretation is now 100% confirmed by its two independent
consumers. `$11B54` distinguishes values `0`, `1`, and `2` to release control,
set the player-lock bits, or zero player input respectively. `$120F6` tests bit
7 of the same indexed byte before accepting the left/right/face-button choice.
The ROM bytes contain ordinary `$01/$02/$00` phases plus `$81` choice-enabled
phases, so `$120AA (mr_x_offer_control_table)` is a per-state control/choice
policy table rather than code or dialogue data.

The observable phases include:

- stopping the players and the game clock;
- loading art and text;
- opening and closing the visible scene area through VDP register `$92xx`;
- drawing dialogue one character at a time;
- allowing left/right selection and confirmation;
- comparing both players' choices in 2P mode;
- enabling `$FFFA43 (duel_damage_modifier)` during a P1-versus-P2 duel;
- returning to normal combat or marking the narrative outcome.

The choices themselves are stored in player-object state, particularly bits in `object+$59`. `$FFDE0E (mr_x_dialogue_clear_flags)` does not store the answers. Routine `$12576` consumes bit 0 to clear the main dialogue area and bit 1 to clear both player-choice tile areas. The connection between accepting the offer and the bad ending is unambiguous.

### 7.3 One-player path

`$11CCA (mr_x_offer_choice_init)` begins by clearing `$FFDE10 (bad_ending_selected)`. The player's response then selects one of two branches:

- refuse: the scene ends, control is restored, and the fight against Mr. X can finish normally;
- accept: `$12074 (mr_x_offer_mark_bad_ending)` writes `1` to `$FFDE10 (bad_ending_selected)` and advances the dialogue state.

Combat and stage exit still finish through the normal round-clear mechanism. The result byte is consumed only later at `$183C4 (round_clear_select_ending)`, after the score tally.

### 7.4 Two-player path

The same state machine supports additional cases in 2P mode:

- matching answers proceed directly to the corresponding branch;
- conflicting raw selection bits activate a P1-versus-P2 confrontation;
- `$FFFA43 (duel_damage_modifier)` triples the low damage nibble (modulo 16)
  and selects alternate player-reaction values during this fight;
- the `$FFFF18 (player_mode)` mask can be modified temporarily while the machine determines which player continues;
- one branch returns to round 6 by setting `level = 5` before re-entering the normal cycle.

The ROM jump and control tables establish the raw route matrix completely.
`$11F74 (mr_x_offer_compare_2p_choices)` performs the equal/different test, and
`$120EC (mr_x_offer_player_choice_input)` owns per-player selection and confirm.
`object+$59` bit 3 is the selected side in each choice UI:

| Phase | P1 bit 3 | P2 bit 3 | Static route |
|---|---:|---:|---|
| Initial 2P choice | 0 | 0 | state `$0B`; set `$FF34/$FF36`, fade, then `$108F8` restarts at `level=5` / `game_state=$28` |
| Initial 2P choice | 1 | 1 | state `$12`, then `$18/$41`; release the clock and continue the final fight |
| Initial 2P choice | 0 | 1 | state `$22`; enable the player duel |
| Initial 2P choice | 1 | 0 | state `$22`; enable the player duel |
| Post-duel choice | 0 | — | state `$34->$0B`; Round 6 restart route |
| Post-duel choice | 1 | — | state `$3B->$3C`; `$12074 (mr_x_offer_mark_bad_ending)` sets `$FFDE10 (bad_ending_selected)` |

The remaining dynamic/visual task is only to attach the exact localized answer
text to raw bit 3 in each prompt; route destinations no longer depend on a
runtime matrix.

The transition from office scene to combat is also explicit. Object type `$33`
dispatches through `$12B5C (mr_x_office_controller_update)`. Once bit 3 of
`$FFFA72 (level_flow_flags)` is set,
`$12CE0 (mr_x_office_controller_spawn_boss)` allocates a type-`$35` object at
the controller position (height plus `$28`), clears the controller's linked
type-`$34` scene object, and deletes the controller. The new type-`$35` object
is the body handled by `$1306A (mr_x_boss_update)`.

---

## 8. Endings

### 8.1 Good ending: states `$24/$26`

`$91D4 (init_ending_good)`:

- clears object RAM;
- loads the ending assets through `$B3C6 (good_ending_sequence_init)`;
- queues music ID `$91`;
- advances to state `$26`.

`$9214 (game_mode_ending_good)` uses `$B6DE (story_scene_timeline_update)`, the same timed-scene infrastructure used by the opening. Once the scene is sufficiently advanced, Start can skip to the top-ten screen (`$0C`). Without input, configuration 1 in `$3F680 (story_scene_config_table)` also ends at `$0C` when its timeline index passes `$12`.

### 8.2 Bad ending: states `$1C/$1E`

`$87C6 (bad_ending_sequence_init)`:

- clears the object area;
- queues music ID `$8F`;
- selects portrait art based on the surviving player and character;
- initializes the state machine at `$FFF910 (bad_ending_substate)`;
- creates the first scene object.

`$8890 (bad_ending_sequence_update)` dispatches this machine through the relative table at `$88A0`, updates ending objects, and accepts Start during the final phases. The last fade writes `game_state = 0`, returning to the Sega-logo loop.

The bad ending therefore has a separate implementation from the generic timeline used by the opening and the good ending.

---

## 9. Reconstructed pseudocode

```cpp
for (;;) {
    dispatch(gameStateHandlerTable[game_state / 2]);
    waitForVBlank();
}

void finishGameplayFade() {
    if (demo_mode) {
        game_state = (demo_mode & 0x80) ? SEGA_INIT : TOP10_INIT;
        demo_mode = 0;
        return;
    }

    if (specialStoryRestart) {
        specialStoryRestart = 0;
        level = 5;
        game_state = LEVEL_INTRO_INIT;
        return;
    }

    game_state = ROUND_CLEAR_INIT;
}

void advanceCampaignAfterTally() {
    if (level != 7) {
        ++level;
        game_state = LEVEL_INTRO_INIT;
        return;
    }

    game_state = bad_ending_selected
        ? BAD_ENDING_INIT
        : GOOD_ENDING_INIT;
}
```

---

## 10. Essential data map

| Reference | Role |
|---|---|
| `$FFFF00 (game_state)` | global mode |
| `$FFFF02 (level)` | current round, `0..7` |
| `$FFFF04 (wave)` | current enemy group within the round |
| `$FFFF18 (player_mode)` | active-player mask |
| `$FFFF2A (demo_mode)` | distinguishes attract mode from campaign play |
| `$FFFA1F (level_intro_active)` | blocks gameplay during the initial fade |
| `$FFFA30 (story_scene_step)` / `$FFFA31 (story_scene_last_step)` / `$FFFA33 (story_scene_next_state)` | opening and good-ending timeline |
| `$FFFA71 (fade_out_flag)` | transition out of gameplay |
| `$FFFA72 (level_flow_flags)` | internal loading and flow gates |
| `$FFFA73 (end_of_level_flag)` | playable round completed |
| `$FFFB06 (story_scene_timer)` | delay between timeline entries |
| `$FFFB48 (level_intro_substate)` | “ROUND n” presentation |
| `$FFFB4A (level_intro_timer)` | round-presentation timing |
| `$FFFB4C (round_clear_substate)` | score tally and campaign advancement |
| `$FFFB4E (round_clear_timer)` | short tally delay |
| `$FFDE00 (mr_x_offer_flag)` | activates the final offer |
| `$FFDE04 (mr_x_offer_state)` | offer state-machine index |
| `$FFDE0E (mr_x_dialogue_clear_flags)` | requests clearing dialogue areas |
| `$FFDE10 (bad_ending_selected)` | final result consumed after round 8 |
| `$FFF910 (bad_ending_substate)` | bad-ending-specific state machine |

---

## 11. Dynamic verification with `megadrive-remote`

The story-opening analysis was checked against a fresh, naturally booted game process through the typed `megadrive_remote` client. The test did not call `restart_game`, inject controller input, patch ROM, or write RAM. It used read-only state queries, VSync waits, VDP inspection, and framebuffer captures.

### 11.1 Observed timeline state

The first connection was made while the opening was already running:

| Field | Observed value | Static interpretation |
|---|---:|---|
| `$FFFF00 (game_state)` | `$0006` | `$904E (game_mode_intro)` |
| `$FFFA30 (story_scene_step)` | `$05` | current timeline entry |
| `$FFFA31 (story_scene_last_step)` | `$16` | last opening entry loaded from config 0 |
| `$FFFA33 (story_scene_next_state)` | `$00` | configured completion state |
| `$FFFB06 (story_scene_timer)` | `$01EA` | 490 frames remaining for the current object |
| `$FFFC24 (story_scene_script_ptr)` | `$0003F690` | opening timeline selected by `$3F680 (story_scene_config_table)` |

Samples were then taken every 60 VSyncs. During timeline step 5, `$FFFB06 (story_scene_timer)` decreased from 490 to 9. Once it expired, `$FFFA30 (story_scene_step)` advanced first to 6 and then to 7; the next long-duration object loaded a timer of 888. This behavior directly matches `$B6DE (story_scene_timeline_update)`: wait for `$FFFB06 (story_scene_timer)` to reach zero, increment `$FFFA30 (story_scene_step)`, read the next eight-byte script record, and install its timer and object parameters.

### 11.2 Visual and VDP evidence

The captured framebuffer was consistently `320x224`. The coherent VDP snapshot reported:

- active display: `320x224`;
- 64 CRAM palette entries;
- 80 decoded SAT entries;
- Plane A tilemap: `64x64` cells.

The captures showed three distinct phases:

1. Japanese narrative text over the night-time city panorama;
2. the horizontally moving city panorama after the text disappeared;
3. a black transition frame while the timeline advanced to the next scene object.

Every sampled framebuffer had a different SHA-256 digest, including while `$FFFA30 (story_scene_step)` remained at 5. The scene therefore continued animating during the per-object wait rather than remaining visually static until the next timeline entry.

### 11.3 Natural completion route

After the opening completed without input, a later read reported:

```text
game_state = $0016
demo_mode  = $01
level      = $0000
```

The runtime log also reached `$E5C (start_round_setup)`. This confirms the complete natural route described in section 3: opening state `$06` uses config 0 with next state `$00`; `$904E (game_mode_intro)` recognizes that completion value, enables `$FFFF2A (demo_mode)`, supplies the scripted input pointers, and enters attract-mode gameplay on level 0.

The dynamic run therefore confirms the manuscript's central claims about the narrative animation: its global state, timeline variables, timer behavior, script pointer, changing visual output, and natural transition into attract mode.

---

## 12. Conclusions

- The campaign's persistent unit of progress is `$FFFF02 (level)`; `$FFFF04 (wave)` describes only progress within a round.
- Normal advancement between rounds belongs to the round-clear screen, not to the logic that defeats the boss.
- The round introduction and score tally have independent state machines at `$FFFB48 (level_intro_substate)` and `$FFFB4C (round_clear_substate)`.
- Attract mode reuses gameplay but is isolated by `$FFFF2A (demo_mode)` and never follows normal campaign progression.
- Mr. X's offer runs inside `$1087A (game_mode_ingame)`; it is not a separate global `$FFFF00 (game_state)`.
- The final narrative choice is reduced to one byte, `$FFDE10 (bad_ending_selected)`, read at a single routing point after round 8.
- The recompiled C++ deliberately preserves this ROM/RAM architecture, so understanding story mode still requires tracing the original addresses and jump tables.

## 13. Future work

Capture both choice prompts in each ROM language/region and associate the
displayed left/right words with `object+$59` bit 3. The assembly and ROM tables
already determine every route destination and the exact Round 6 restart point.
