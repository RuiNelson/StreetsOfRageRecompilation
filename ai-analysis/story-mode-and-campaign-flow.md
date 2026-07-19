# Streets of Rage Story Mode and Campaign Flow

**Manuscript:** static analysis of the original ROM and the C++ recompilation

**Scope:** story opening, attract mode, campaign start, progression through all eight rounds, round-clear screen, Mr. X's offer, and ending selection

**Primary sources:** `output/sor.asm`, `generated/Sor.cpp`, `SorManualFunctions.cpp`, `code-analysis/addresses.csv`, `code-analysis/labels.csv`, and `rom/SOR.bin`

The names used below match the symbols added to the analysis CSVs. Meanings inferred from context rather than proved by unambiguous reads and writes are explicitly identified as such.

---

## 1. Main result

The game has no single class, structure, or function representing “story mode.” The campaign is composed of several state machines that communicate almost entirely through Mega Drive work RAM:

1. `game_state` (`$FFFF00`) selects the global mode.
2. Each mode normally has an initialization handler and an update handler.
3. Complex modes use a secondary state index and their own jump table.
4. `level` (`$FFFF02`) is the persistent campaign counter, ranging from `0` through `7`.
5. After each round, the round-clear screen increments `level` and returns to the level-introduction state.
6. After round 8, `bad_ending_selected` (`$FFDE10`) selects the good or bad ending.

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

- `init_intro` and `game_mode_intro` implement only the narrative opening before the title.
- The playable campaign is the `level start -> in-game -> round clear` cycle governed by `game_state`, `level`, completion flags, and the final offer.

---

## 2. Global dispatcher

### 2.1 Assembly implementation

The core loop at `game_infinite_loop` (`$3A2`) reads `game_state`, doubles it to obtain a four-byte table offset, and looks up `game_state_handler_table` at `$3BA`:

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

Because every `game_state` value is even, each value selects one longword. The table contains eleven initialization/update pairs:

| `game_state` | Handler | Purpose |
|---:|---|---|
| `$00` / `$02` | `init_segascreen` / `game_mode_segascreen` | Sega logo |
| `$04` / `$06` | `init_intro` / `game_mode_intro` | story opening |
| `$08` / `$0A` | `init_titlescreen` / `game_mode_titlescreen` | title screen |
| `$0C` / `$0E` | `init_top10score` / `game_mode_top10score` | top-ten scores |
| `$10` / `$12` | `init_selectscreenmode` / `game_mode_selectscreenmode` | main menu and OPTIONS |
| `$14` / `$16` | `init_ingame` / `game_mode_ingame` | gameplay or attract mode |
| `$18` / `$1A` | `init_roundclear` / `game_mode_roundclear` | round bonuses and results |
| `$1C` / `$1E` | `init_ending_bad` / `game_mode_ending_bad` | bad ending |
| `$20` / `$22` | `init_characterselectscreen` / `game_mode_characterselectscreen` | character select |
| `$24` / `$26` | `init_ending_good` / `game_mode_ending_good` | good ending |
| `$28` / `$2A` | `init_levelstart` / `game_mode_levelstart` | round presentation |

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

`init_intro` (`$8FD0`) does more than draw the opening. It also resets the persistent state of a new session:

- `level = 0`;
- `wave = 0`;
- P1 and P2 scores to zero;
- player death and status flags to zero;
- score-based extra-life pointers to zero;
- `player_mode_copy` to zero;
- opening music to sound ID `$83`.

This makes the opening the logical boundary of a new campaign. The menu and character-select screens that follow operate on this freshly cleared session.

### 3.2 Skipping the opening

In `game_mode_intro` (`$904E`), Start can route execution to:

- the title (`game_state = $08`) while the scene is in an early phase;
- the main menu (`game_state = $10`) once the scene has passed its internal threshold.

Without input, the shared `story_scene_timeline_update` routine (`$B6DE`) creates narrative objects from a timed list. When the list ends, it writes the next state configured for that scene. `story_scene_select_script` (`$3F65E`) reads this configuration from `$3F680`: the opening configuration ends with state `$00`, which `game_mode_intro` converts into entry into attract mode.

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
- `init_ingame` internally calls `init_levelstart`, bypassing the interactive start flow;
- characters, lives, and demo duration are forced;
- Start sets bit 7 of `demo_mode`, starts a fade, and aborts the demonstration;
- after the fade, `game_mode_ingame` returns to the Sega logo (`$00`) or the top-ten screen (`$0C`) instead of entering round clear.

States `$14/$16` are therefore shared by the campaign and the demonstration, but `demo_mode` changes their input source, HUD setup, and exit route.

---

## 4. Campaign start and round introduction

After character confirmation, `initialize_player_continues` (`$17A2`) initializes continues and lives, then writes:

```asm
move.w #$0028,game_state
```

### 4.1 `init_levelstart` (`$106EA`)

The round initialization handler:

- clears almost all work RAM;
- resets `wave = 0`;
- sets `level_intro_active = 1`;
- initializes the fade counter to `$40`;
- loads art, tilemaps, HUD data, and other resources selected by `level`;
- calls `start_round_setup`;
- recreates the P1/P2 objects with the selected character IDs;
- advances `game_state` from `$28` to `$2A`.

Level data is indexed by the current round number. For example, `load_level_data` (`$576`) multiplies `level` by six and selects a six-byte entry in the ROM table at `$1C378`.

### 4.2 Round-presentation state machine

`game_mode_levelstart` enters `level_intro_dispatcher` (`$11A50`). `level_intro_substate` (`$FB48`) selects one of six entries in `level_intro_jt` (`$11A5C`):

1. wait for fade-in;
2. move the two halves of the “ROUND n” banner toward the center;
3. wait `$60` frames;
4. move the banner off-screen;
5. wait `$30` frames;
6. execute `level_intro_finish`, which writes `game_state = $14`.

Once state `$14` is reached, `init_ingame` configures the fade and timing counters and advances immediately to `$16`.

---

## 5. Gameplay and within-round progression

### 5.1 Per-frame update

On the normal path through `game_mode_ingame` (`$1087A`), while neither fading nor paused, the game updates:

- the clock and scene boundaries;
- pause handling and second-player joining;
- Mr. X's offer state machine;
- HUD and objects;
- wave logic;
- pending art transfers;
- the secondary level-flow dispatcher.

`level_flow_handler` (`$464`) uses `level_flow_flags` (`$FFFA72`) to prevent loading, music, and setup phases from being repeated. `wave` (`$FFFF04`) selects enemy groups within the round, while `level` selects the main round table.

### 5.2 Completing a round

`end_of_level_flag` (`$FFFA73`) explicitly marks the playable portion of a round as complete. It can be set by round-dependent paths, including wave exhaustion and the special completion condition for level index 6, which is round 7.

While this flag is set, `end_level_player_exit_update` (`$502C`) forces the players through their stage-exit animation and position. When the exit completes, it writes:

```asm
move.b #1,fade_out_flag
move.w #$40,palette_fade_counter
```

Once the fade completes, the normal campaign path in `ingame_finish_fade` (`$108CC`) selects:

```asm
move.w #$0018,game_state
```

The physical end of a level therefore does not increment `level` directly. It transfers control to the round-clear screen.

The same routing point also has alternate paths:

- attract mode returns to the presentation loop;
- game-over and continue flags can enter the top-ten screen or another recovery sequence;
- a special two-player story branch forces `level = 5` and re-enters state `$28`.

That final branch is visible in the code, but its controlling flag has not been given a permanent CSV name because its complete meaning depends on every possible outcome of the player-versus-player confrontation.

---

## 6. Round clear is the campaign manager

### 6.1 Initialization

`init_roundclear` (`$91A0`) calls `round_clear_sequence_init` (`$181EA`). This routine:

- normalizes dead or inactive player objects;
- loads the results screen;
- prepares time, difficulty, life, and special-attack bonuses;
- includes additional remaining-life values on the final round;
- clears transient state, including `mr_x_offer_flag`;
- initializes `round_clear_substate = 0`.

### 6.2 Score tally and campaign advancement

`round_clear_sequence_update` (`$1833C`) uses `round_clear_substate` (`$FB4C`) as an offset into `round_clear_jt` (`$18350`). The table controls animations, bonus-to-score conversion, delays, and fading.

The decisive routine is `round_clear_advance_campaign` (`$183B0`):

```asm
cmpi.w #7,level
beq.s   final_round
addq.w  #1,level
move.w  #$28,game_state
rts
```

The eight rounds are therefore represented by `level = 0..7`. For levels `0..6`, round clear is the only observed normal-campaign routine that increments the level counter and starts the next presentation.

### 6.3 Ending selection

On round 8 (`level == 7`), execution falls through to `round_clear_select_ending` (`$183C4`):

```asm
moveq #$24,d0
tst.b  bad_ending_selected
beq.s  set_state
moveq #$1C,d0
set_state:
move.w d0,game_state
```

This directly proves the meaning of `$FFDE10`:

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

From that point onward, `mr_x_offer_update` (`$11B4C`), which runs every gameplay frame, stops returning immediately and begins processing `mr_x_offer_state` (`$FFDE04`).

### 7.2 State-machine structure

The offer state is used in two ways:

- as a byte index into `mr_x_offer_control_table` (`$120AA`), which determines whether control is blocked, enabled, or waiting for a choice;
- doubled to index `mr_x_offer_jt` (`$11B94`).

The observable phases include:

- stopping the players and the game clock;
- loading art and text;
- opening and closing the visible scene area through VDP register `$92xx`;
- drawing dialogue one character at a time;
- allowing left/right selection and confirmation;
- comparing both players' choices in 2P mode;
- enabling `half_damage` during a P1-versus-P2 duel;
- returning to normal combat or marking the narrative outcome.

The choices themselves are stored in player-object state, particularly bits in `object+$59`. `$FFDE0E` does not store the answers: it is `mr_x_dialogue_clear_flags`. Routine `$12576` consumes bit 0 to clear the main dialogue area and bit 1 to clear both player-choice tile areas. The connection between accepting the offer and the bad ending is unambiguous.

### 7.3 One-player path

`mr_x_offer_choice_init` (`$11CCA`) begins by clearing `bad_ending_selected`. The player's response then selects one of two branches:

- refuse: the scene ends, control is restored, and the fight against Mr. X can finish normally;
- accept: `mr_x_offer_mark_bad_ending` (`$12074`) writes `1` to `bad_ending_selected` and advances the dialogue state.

Combat and stage exit still finish through the normal round-clear mechanism. The result byte is consumed only later at `$183C4`, after the score tally.

### 7.4 Two-player path

The same state machine supports additional cases in 2P mode:

- matching answers can proceed directly to the corresponding branch;
- conflicting answers activate a P1-versus-P2 confrontation;
- `half_damage` changes applied strength during this fight;
- the `player_mode` mask can be modified temporarily while the machine determines which player continues;
- one branch returns to round 6 by setting `level = 5` before re-entering the normal cycle.

The code clearly establishes this topology. Assigning a definitive narrative name to every answer combination, however, requires a dynamic input matrix. The CSVs therefore name only states and outcomes confirmed by static evidence.

---

## 8. Endings

### 8.1 Good ending: states `$24/$26`

`init_ending_good`:

- clears object RAM;
- loads the ending assets through `good_ending_sequence_init` (`$B3C6`);
- queues music ID `$91`;
- advances to state `$26`.

`game_mode_ending_good` uses `story_scene_timeline_update`, the same timed-scene infrastructure used by the opening. Once the scene is sufficiently advanced, Start can skip to the top-ten screen (`$0C`). Without input, configuration 1 in `story_scene_config_table` also ends at `$0C` when its timeline index passes `$12`.

### 8.2 Bad ending: states `$1C/$1E`

`bad_ending_sequence_init` (`$87C6`):

- clears the object area;
- queues music ID `$8F`;
- selects portrait art based on the surviving player and character;
- initializes the state machine at `$F910`;
- creates the first scene object.

`bad_ending_sequence_update` (`$8890`) dispatches this machine through the relative table at `$88A0`, updates ending objects, and accepts Start during the final phases. The last fade writes `game_state = 0`, returning to the Sega-logo loop.

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

| Address | Symbol | Role |
|---:|---|---|
| `$FFFF00` | `game_state` | global mode |
| `$FFFF02` | `level` | current round, `0..7` |
| `$FFFF04` | `wave` | current enemy group within the round |
| `$FFFF18` | `player_mode` | active-player mask |
| `$FFFF2A` | `demo_mode` | distinguishes attract mode from campaign play |
| `$FFFA1F` | `level_intro_active` | blocks gameplay during the initial fade |
| `$FFFA30/$31/$33` | `story_scene_step/last_step/next_state` | opening and good-ending timeline |
| `$FFFA71` | `fade_out_flag` | transition out of gameplay |
| `$FFFA72` | `level_flow_flags` | internal loading and flow gates |
| `$FFFA73` | `end_of_level_flag` | playable round completed |
| `$FFFB06` | `story_scene_timer` | delay between timeline entries |
| `$FFFB48` | `level_intro_substate` | “ROUND n” presentation |
| `$FFFB4A` | `level_intro_timer` | round-presentation timing |
| `$FFFB4C` | `round_clear_substate` | score tally and campaign advancement |
| `$FFFB4E` | `round_clear_timer` | short tally delay |
| `$FFDE00` | `mr_x_offer_flag` | activates the final offer |
| `$FFDE04` | `mr_x_offer_state` | offer state-machine index |
| `$FFDE0E` | `mr_x_dialogue_clear_flags` | requests clearing dialogue areas |
| `$FFDE10` | `bad_ending_selected` | final result consumed after round 8 |
| `$FFF910` | `bad_ending_substate` | bad-ending-specific state machine |

---

## 11. Conclusions

- The campaign's persistent unit of progress is `level`; `wave` describes only progress within a round.
- Normal advancement between rounds belongs to the round-clear screen, not to the logic that defeats the boss.
- The round introduction and score tally have independent state machines at `$FB48` and `$FB4C`.
- Attract mode reuses gameplay but is isolated by `demo_mode` and never follows normal campaign progression.
- Mr. X's offer runs inside `game_mode_ingame`; it is not a separate global `game_state`.
- The final narrative choice is reduced to one byte, `bad_ending_selected`, read at a single routing point after round 8.
- The recompiled C++ deliberately preserves this ROM/RAM architecture, so understanding story mode still requires tracing the original addresses and jump tables.

## 12. Future work

A dynamic analysis can complete the 2P matrix for Mr. X's offer. The ideal test would record, for every combination of answers, the per-frame values of `$FFDE04`, `$FFDE10`, P1/P2 `object+$59`, `$FFFF18`, `$FFFF34`, `$FFFF36`, and `game_state`. This would make it possible to name the two remaining flags with 100% confidence and document exactly when the special route returns to round 6.
