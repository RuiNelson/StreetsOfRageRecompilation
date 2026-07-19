# Streets of Rage Select Screen, Options Menu, and Character Select

- **Manuscript:** reverse-engineering notes from the recompilation project
- **Scope:** main mode-select screen (1 PLAYER / 2 PLAYERS / OPTIONS), the OPTIONS sub-menu (including the P2 button cheat), and the character-select flow that leads into level start
- **Primary sources:** `code-analysis/addresses.csv`, `code-analysis/labels.csv`, `output/sor.asm`, generated `Sor.cpp`, ROM `rom/SOR.bin`

Symbol names below match the analysis CSVs (100% confidence entries unless noted). Despite older disassembly comments that call `$FE8 (init_game_start_screen)` / `$108E (game_start_screen_update)` a “title screen”, those routines implement the **mode-select / start menu**, not the logo title (`$90C8 (init_titlescreen)` / `$9102 (game_mode_titlescreen)`).

---

## 1. Executive summary

After the Sega logo, story intro, title, and optional hi-score screens, the game enters:

| `$FFFF00 (game_state)` | Init handler | Update handler | Role |
|--------------|--------------|----------------|------|
| `$10` / `$12` | `$9170 (init_selectscreenmode)` → `$FE8 (init_game_start_screen)` | `$919A (game_mode_selectscreenmode)` → `$108E (game_start_screen_update)` | Mode menu + OPTIONS |
| `$20` / `$22` | `$927C (init_characterselectscreen)` → `$1634 (init_character_select_screen)` | `$92A8 (game_mode_characterselectscreen)` → `$170A (screen_state_dispatcher)` | Pick character (L→R: Adam, Axel, Blaze) |
| `$28` / `$2A` | `$106EA (init_levelstart)` | `$1077C (game_mode_levelstart)` | Level intro → in-game |

High-level path:

```
  Title / Top-10
        │
        ▼
  game_state $10/$12   SELECT SCREEN
        │
        ├─ 1 PLAYER / 2 PLAYERS ──► game_state $20/$22  CHARACTER SELECT
        │                                      │
        │                                      ▼
        │                             game_state $28/$2A  LEVEL START
        │                                      │
        │                                      ▼
        │                             game_state $14/$16  IN-GAME
        │
        └─ OPTIONS ──(exit)───────► game_state $10      (re-init select screen)
```

Both the select screen and character select are **sub-state machines** on top of the global `$FFFF00 (game_state)` dispatcher:

- Select / OPTIONS: sub-state word `$FFFB0E (select_screen_substate)`, jump table at `$10C4 (select_screen_jt)`
- Character select: sub-state word `$FFF904 (char_select_substate)`, jump table at `$171A (char_select_jt)`

---

## 2. Global entry: `$FFFF00 (game_state)` $10 / $12

### 2.1 Mode wrappers

```asm
; init_selectscreenmode  ($9170)  — game_state = $10
    ; program VDP, enable IRQs
    jsr  init_z80
    jsr  init_game_start_screen     ; $FE8
    jsr  clear_player_input
    ; restore VDP, disable IRQs
    addq.w #2, game_state           ; $10 → $12

; game_mode_selectscreenmode  ($919A)  — game_state = $12
    jmp  game_start_screen_update   ; $108E
```

---

## 3. Select-screen init — `$FE8 (init_game_start_screen)`

### 3.1 Sequence

| Step | Code effect |
|------|-------------|
| Clear `$FFFA7B (cheat_flag)` | OPTIONS cheat rows off |
| `$FF06 = 1`, `$E000 = 3` | Screen context flags |
| `load_nemesis_art_bundle(d0=2)` | Load menu art / palettes |
| Clear `$FFFB0E (select_screen_substate)` | Sub-state = 0 |
| Zero P1 object slot (`$FFB800 (p1_object)`, 128 bytes) | Cursor object workspace |
| Loop 3× over ROM `$1060 (select_menu_strings)` | Draw **1 PLAYER**, **2 PLAYERS**, **OPTIONS** via `$1290A (vdp_write_menu_string)` |
| `$FFFB0C (palette_fade_counter)` = `$40` | Fade length |
| Palette setup `$7267C` or `$72684` | Depends on `$FFFC0A (p2_joypad_present_negated)` |
| `$FFFA09 (select_menu_option_count)` | **3** if P2 pad present, **1** if not (hides 2 PLAYERS) |

### 3.2 Menu string table (`$1060 (select_menu_strings)`)

Each of three rows: VDP command longword + tile bytes (font index `1` = `A`, …) + base tile word stream from `$108A (select_menu_tile_bases)`.

Decoded text:

- `1 PLAYER`
- `2 PLAYERS`
- `OPTIONS`

---

## 4. Select-screen update — `$108E (game_start_screen_update)`

### 4.1 START shortcut (OPTIONS only)

If `$FFFB0E (select_screen_substate)` is in **`[$10, $28)`** (inside OPTIONS) and P1 Start is pressed (`$FFFC05 (p1_button_press)` bit 7):

1. Queue UI sound (`$1069E (queue_sound_id)`, ID `$E1`)
2. Force `select_screen_substate = $28` (OPTIONS exit fade)

### 4.2 Sub-state dispatch

```asm
lea    select_screen_jt($10C4), a1
move.w select_screen_substate, d0
move.w 0(a1,d0.w), d0
jmp    0(a1,d0.w)
```

Sub-state advances by **+2** (word index into the table).

### 4.3 Sub-state map (`$FFFB0E (select_screen_substate)` → handler)

| `$FFFB0E (select_screen_substate)` | Handler | Role |
|---------|---------|------|
| `$00` | `$10F2 (select_menu_wait_fade_in)` | Cursor object + wait palette fade-in |
| `$02` | `$1104 (select_menu_input)` | Wait face/Start; run cursor |
| `$04` | `$1130 (select_menu_begin_fade_out)` | Reset fade counter, advance |
| `$06` | `$113C (select_menu_fade_out)` | Palette fade-out step |
| `$08` | `$114A (select_menu_resolve_choice)` | 1P/2P → char select, or OPTIONS |
| `$0A` | `$117C (options_menu_build)` | Build OPTIONS screen |
| `$0C` | `$1124 (select_menu_alt_palette)` | Alternate palette path (no P2) |
| `$0E` | `$10F6 (select_menu_wait_fade)` | Wait fade helper |
| `$10` / `$12` | highlight / `$1218 (options_input_sound_test)` | Sound-test row (`$FFFFC4 (sound_test_index)`) |
| `$14` / `$16` | highlight / `$131A (options_input_difficulty)` | Difficulty row |
| `$18` / `$1A` | highlight / `$1390 (options_input_controls)` | Control-scheme row |
| `$1C` / `$1E` | highlight / `$1404 (options_input_lives)` | Lives row (**cheat only**) |
| `$20` / `$22` | highlight / `$1476 (options_input_level)` | Level row (**cheat only**) |
| `$24` / `$26` | highlight / `$14CA (options_input_exit)` | Exit row |
| `$28` / `$2A` | fade-out | Leaving OPTIONS |
| `$2C` | `$14EA (options_menu_return)` | `game_state = $10` |

---

## 5. Main menu (1P / 2P / OPTIONS)

### 5.1 Cursor object

While sub-state is `$00` / `$02`, update calls `$1564 (player_state_dispatcher)` on `$FFB800 (p1_object)`. In this screen that object is a **menu cursor**, not an in-game fighter:

| Field | Address | Meaning |
|-------|---------|---------|
| Cursor index | `$FFB840 (select_menu_cursor)` (`$FFB840 (select_menu_cursor)` = `$40` of P1 object) | `0` = 1P, `1` = 2P, `2` = OPTIONS |
| Previous index | `$FFB842 (select_menu_cursor_prev)` | Used when skipping 2P with one pad |

Up/Down on `$FFFC05 (p1_button_press)` bits 0–1 wrap the cursor `0…2`, play a UI beep, and place the cursor sprite in the `$FFDA00 (sprite_mappings_buffer)` buffer.

If `select_menu_option_count == 1` (no P2 pad), landing on index `1` (2 PLAYERS) is remapped via table `$1630 (select_menu_skip_2p_remap)` so the player only sees **1 PLAYER** and **OPTIONS**.

Hot-plug: `$1546 (select_menu_sync_pad_count)` raises/lowers `$FFFA09 (select_menu_option_count)` when P2 appears or disappears while waiting for confirm.

### 5.2 Confirm (`$1104 (select_menu_input)`)

- No face/Start bits in `$FFFC05 (p1_button_press)` (`& $F0 == 0`): keep running cursor + pad sync
- Any face/Start: queue confirm sound, `select_screen_substate += 2`

### 5.3 Resolve (`$114A (select_menu_resolve_choice)`)

```asm
cmpi.w  #2, select_menu_cursor
bcc.s   go_options

; 1P or 2P:
move.b  select_menu_player_mode_tbl($117A)[cursor], player_mode   ; 0→1, 1→3
move.w  #$20, game_state    ; character select init
rts

go_options:
; Cheat: P2 held buttons XOR $78 == 0  (A+B+C+Start held)
eori.b  #$78, p2_button_held
bne.s   no_cheat
move.b  #1, cheat_flag
no_cheat:
addq.w  #2, select_screen_substate
```

| Cursor | Result |
|--------|--------|
| 0 | `$FFFF18 (player_mode)` = **1** → `game_state = $20` |
| 1 | `$FFFF18 (player_mode)` = **3** → `game_state = $20` |
| 2 | Enter OPTIONS; optional `cheat_flag = 1` |

---

## 6. OPTIONS menu

### 6.1 Build (`$117C (options_menu_build)`)

1. Clear sprite mapping buffer
2. Draw static labels (`sub_A8B8` / palette-remapped tilemaps)
3. Draw current values:
   - **Sound list** — `$FFFFC4 (sound_test_index)`, tile rows at `$1C684 (options_sound_name_table)`
   - **Difficulty** — `$FFFFC6 (difficulty)`, strings at `$12FE (options_difficulty_strings)`
   - **Controls** — `$FFFFC8 (control_scheme)`, strings at `$1C9FC (options_control_strings)`
4. If `$FFFA7B (cheat_flag)`:
   - Draw **lives** digit from `$FFFFCA (lives_setting)`
   - Draw **level** digit from `$FFFF02 (level)`
   - Extra labels (`sub_A8B8` IDs `$0F` / `$0E`)

### 6.2 Vertical navigation (`$14F2 (options_row_nav)`)

Up/Down move `$FFFB0E (select_screen_substate)` between option **rows**. Without the cheat, lives/level rows are skipped:

```
normal:  $10/$12 → $14/$16 → $18/$1A → $24/$26
cheat:   … also $1C/$1E (lives) and $20/$22 (level)
```

### 6.3 Sound test row (`$FFFFC4 (sound_test_index)` / `$FFFFC4 (sound_test_index)`)

Left/Right wrap the index **`0 … $48`** (73 entries). Face buttons play `options_sound_id_table[$128A + index]` via `$1069E (queue_sound_id)`.

This is **not** the starting level. The list is a BGM / voice / SFX browser. ROM text rows at `$1C684 (options_sound_name_table)` (12 bytes each) decode roughly as:

| Index | Label |
|-------|--------|
| `$00`–`$07` | ROUND 1 … ROUND 8 |
| `$08` | BOSS |
| `$09` | TITLE |
| `$0A` | GAME OVER |
| `$0B` | SELECT |
| `$0C` | NAME ENTRY |
| `$0D` | ROUND CLEAR |
| `$0E` | BAD ENDING |
| `$0F` | LAST BOSS |
| `$10` | GOOD ENDING |
| `$11`–`$1A` | VOICE … |
| `$1B`–`$48` | SE 1 … SE 46 |

`$FFFFC4 (sound_test_index)` is only read/written inside this menu.

### 6.4 Difficulty (`$FFFFC6 (difficulty)` / `$FFFFC6 (difficulty)`)

| Value | On-screen (`$12FE (options_difficulty_strings)`, 7 chars) |
|-------|------------------------------|
| 0 | EASY |
| 1 | NORMAL |
| 2 | HARD |
| 3 | Hardest (4th entry) |

Wraps `0 ↔ 3`. Copied to `$FFFB5A (difficulty_copy)` on level load; used for enemy spawn filtering.

### 6.5 Control scheme (`$FFFFC8 (control_scheme)` / `$FFFFC8 (control_scheme)`)

Three layouts; on-screen copy at `$1C9FC (options_control_strings)` (blocks of `$50` bytes) describes A/B/C as Special / Attack / Jump variants.

In-game remap is applied in the object input path (`$568A (remap_player_gameplay_input)`):

- `0` — default bit layout
- `1` — shift A/B/C one way
- `2` — shift the other way

D-pad and Start are not remapped by that path.

### 6.6 Lives (cheat) (`$FFFFCA (lives_setting)` / `$FFFFCA (lives_setting)`)

Input wraps **`0 … 3`**. Applied when leaving character select (and on some respawn paths):

```asm
move.w  lives_setting, d0
add.w   d0, d0
addq.w  #1, d0          ; lives = 2*n + 1  →  1, 3, 5, or 7
move.b  d0, p1_lives
move.b  d0, p2_lives
```

Continues are always set to **3** in `$17A2 (initialize_player_continues)`. Older comments called this a “9-lives” cheat; the formula observed here yields **1 / 3 / 5 / 7**.

### 6.7 Level (cheat) (`$FFFF02 (level)` / `$FFFF02 (level)`)

Left/Right adjust the global `$FFFF02 (level)` word (0 = first round). That value is what `$106EA (init_levelstart)` uses when play begins.

### 6.8 Leaving OPTIONS

1. Exit row + face button, **or** Start anywhere in OPTIONS → sub-state `$28`
2. Fade `$28`–`$2A`
3. `$14EA (options_menu_return)`: **`game_state = $10`**

`$FE8 (init_game_start_screen)` clears `$FFFA7B (cheat_flag)` again on re-entry. Difficulty, controls, lives setting, and level **persist** in high work RAM (outside the large clear used later by character select).

---

## 7. Character select

### 7.1 Mode wrappers ($20 / $22)

```asm
; init_characterselectscreen  ($927C)
    jsr  init_z80
    jsr  init_character_select_screen   ; $1634
    jsr  load_z80_dac_driver
    jsr  clear_player_input
    addq.w #2, game_state               ; $20 → $22

; game_mode_characterselectscreen  ($92A8)
    jmp  screen_state_dispatcher        ; $170A  (uses char_select_substate)
```

### 7.2 Init — `$1634 (init_character_select_screen)`

1. **Clear low work RAM** — `$1FA` × 128-byte fills from `$FF0000` → ends near `$FFFD00`. High variables (`$FFFF00 (game_state)`, `$FFFF18 (player_mode)`, `$FFFFC6 (difficulty)`, `$FFFFC8 (control_scheme)`, `$FFFFCA (lives_setting)`, `$FFFF02 (level)`, character IDs) live above that range and survive.
2. `$FFF900 (char_select_idle_timer)` = `$12C` (~300 frames)
3. Fade setup; palette `$71F30`
4. Spawn **player cursors** (object type **6**):

| Object | Base | Type | Char slot (`+$58`) | X (`+$10`) | Default character |
|--------|------|------|--------------------|------------|-------------------|
| P1 | `$FFB800 (p1_object)` `$FFB800 (p1_object)` | 6 | **0** (left) | `$20` | Adam |
| P2 (if `player_mode == 3`) | `$FFB880 (p2_object)` `$FFB880 (p2_object)` | 6 | **2** (right) | `$E0` | Blaze |

5. Spawn **animated full-body character previews** (object type **7**), left → right on screen. The large static face portraits above them are separate screen art, not these objects:

| Screen order | Slot | Base | Type | ID at `+$50` | Character |
|--------------|------|------|------|--------------|-----------|
| 1st (left) | 0 | `$FFB900 (object_table)` | 7 | **1** | **Adam** |
| 2nd (center) | 1 | `$B980` | 7 | **0** | **Axel** |
| 3rd (right) | 2 | `$BA00` | 7 | **2** | **Blaze** |

Confirmed layout: **Adam · Axel · Blaze** (left to right). Note that the stored **character ID** is not the same as the screen slot index (Axel is ID `0` but stands in the middle).

`$1A44 (char_select_character_preview_setup)` is the type-7 state-zero handler,
not a gameplay player-position or static-portrait routine. The table at `$1A20`
dispatches preview state zero to `$1A44 (char_select_character_preview_setup)`;
the routine selects the character-specific animation pointer, writes
X/lane/height, advances `+$30`, and starts the idle animation. A live remote run
reached character select by normal input and observed the three type-7 objects
after this handler:
`$FFB900 (object_table)` ID 1 at `(48,32,192)`, `$B980` ID 0 at `(176,32,192)`, and `$BA00`
ID 2 at `(280,32,192)`. A framebuffer capture shows these coordinates belong
to the three animated, full-body fighters in the lower grid, while the face
portraits occupy the separate upper panels. This disconfirms both the former
`set_player_position` name and the intermediate `char_select_portrait_setup`
name.

6. Load character art (`$A63A (load_nemesis_art_bundle)`, Kosinski `$71C6C`, UI tilemaps via `sub_A8B8`).

### 7.3 Update — `$170A (screen_state_dispatcher)`

Indexed by `$FFF904 (char_select_substate)`, table `$171A (char_select_jt)`:

| `$FFF904 (char_select_substate)` | Handler | Role |
|---------|---------|------|
| `$00` | `$1726 (char_select_play_music)` | Queue select BGM; run one object pass |
| `$02` | `$1738 (char_select_fade_in)` | Wait fade-in; enable character-preview animation flags |
| `$04` | `$175A (char_select_interactive)` | Main loop: objects + wait for confirms |
| `$06` | `$1788 (char_select_exit_delay)` | Count down `$FFF90A (char_select_exit_delay)` |
| `$08` | `$1794 (char_select_fade_out)` | Palette fade-out |
| `$0A` | `$17A2 (initialize_player_continues)` | Lives/continues → `game_state = $28` |

### 7.4 Object pass — `$AD8E (update_select_objects)`

Each frame during interactive select:

1. For P1 and P2 slots: if type ≠ 0, dispatch via object-type jump table `$B236`
2. Copy held/press input into object fields
3. Process animated character-preview objects in the object table
4. `$10514 (sync_z80_2)` (VBlank wait)

Type **6** = cursor selection logic (`$1916 (char_select_player_input)`).
Type **7** = animated full-body character preview / DMA frames.

### 7.5 Cursor input — `$1916 (char_select_player_input)`

Important object fields on the type-6 cursor:

| Offset | Use |
|--------|-----|
| `$55` | Button *press* copy |
| `$58` | Character **slot** (0 left / 1 center / 2 right) |
| `$5A` | Locked-in flag |
| `$10` | Cursor X |

**Left / Right** use wrap tables:

```
right ($19E4):  0→1, 1→2, 2→0
left  ($19EA):  0→2, 2→1, 1→0
```

In 2P mode, if the destination slot equals the other player’s `$58`, the code steps **again** so both cannot claim the same character.

On move: clear the old preview selection (`+$5C`), select the new preview, update X from `$19FC (char_select_cursor_x)` (`$20` / `$80` / `$E0`), refresh name palettes, play UI sound.

**Confirm** (face/Start, `$55 & $F0`):

```asm
move.b  #1, $5a(a0)                    ; lock
addq.w  #1, char_select_confirm_count  ; $F908
; write character ID:
;   P1 → p1_character_id ($FF1E)
;   P2 → p2_character_id ($FF1F)
move.b  char_id_from_slot($1A0E)[slot], (a2)
```

Slot → ID table `$1A0E (char_id_from_slot)`:

| Screen (L→R) | Slot | Cursor X | Character ID | Character |
|--------------|------|----------|--------------|-----------|
| 1st | 0 | `$20` | **1** | **Adam** |
| 2nd | 1 | `$80` | **0** | **Axel** |
| 3rd | 2 | `$E0` | **2** | **Blaze** |

Defaults: P1 starts on slot 0 (**Adam**); P2 starts on slot 2 (**Blaze**).

After lock, movement input is ignored for that player.

### 7.6 Confirm count (`$FFF908 (char_select_confirm_count)` / `$FFF908 (char_select_confirm_count)`)

- 1P (`player_mode == 1`): need count **1**
- 2P (`player_mode == 3`): need count **2**

Then set `$FFF90A (char_select_exit_delay)` = `$12` and advance sub-state.

### 7.7 Exit — `$17A2 (initialize_player_continues)`

```asm
moveq   #3, d0
move.w  d0, p1_continues
move.w  d0, p2_continues

move.w  lives_setting, d0     ; $FFCA
add.w   d0, d0
addq.w  #1, d0                ; 1,3,5,7
move.b  d0, p1_lives
move.b  d0, p2_lives

clr special-attack / attack flags
move.w  #$28, game_state      ; level start
queue_sound_id
```

Downstream consumers:

| Symbol | Use |
|--------|-----|
| `$FFFF1E (p1_character_id)` / `$FFFF1F (p2_character_id)` | Copied into object `$50` / `$FFB850 (p1_character_id_ingame)` / `$FFB8D0 (p2_character_id_ingame)` for combat |
| `$FFFFC6 (difficulty)` | → `$FFFB5A (difficulty_copy)` on level load |
| `$FFFFC8 (control_scheme)` | Button remap in object input |
| `$FFFF02 (level)` | Starting round |
| `$FFFF18 (player_mode)` | Which players are active |

---

## 8. Memory map (this feature area)

### 8.1 Work RAM

| Reference | Size | Notes |
| --- | ------ | ------- |
| `$FFFF00 (game_state)` | W | Global mode |
| `$FFFF02 (level)` | W | Starting / current level; OPTIONS cheat editable |
| `$FFFF18 (player_mode)` | B | `1` = 1P, `3` = 2P |
| `$FFFF1E (p1_character_id)` | B | ID: 0=Axel, 1=Adam, 2=Blaze (screen L→R is Adam, Axel, Blaze) |
| `$FFFF1F (p2_character_id)` | B | Same encoding |
| `$FFFFC4 (sound_test_index)` | W | OPTIONS sound browser only |
| `$FFFFC6 (difficulty)` | W | 0…3 |
| `$FFFFC8 (control_scheme)` | W | 0…2 |
| `$FFFFCA (lives_setting)` | W | 0…3 → lives `2n+1` |
| `$FFFA09 (select_menu_option_count)` | B | 1 or 3 visible main-menu entries |
| `$FFFA7B (cheat_flag)` | B | OPTIONS lives/level rows |
| `$FFFB0C (palette_fade_counter)` | W | Shared fade timing |
| `$FFFB0E (select_screen_substate)` | W | Select / OPTIONS sub-state |
| `$FFF900 (char_select_idle_timer)` | W | Idle / blink timer on char select |
| `$FFF904 (char_select_substate)` | W | Character-select sub-state |
| `$FFF908 (char_select_confirm_count)` | W | Players locked in |
| `$FFF90A (char_select_exit_delay)` | W | Frames before fade-out |
| `$FFB800 (p1_object)` | — | Cursor (select/char select) or fighter |
| `$FFB840 (select_menu_cursor)` | W | Main menu index (object +$40) |
| `$FFB858 (p1_char_slot)` | W | Char-select slot (object +$58) |
| `$FFB880 (p2_object)` | — | P2 cursor / fighter |
| `$FFB8D8 (p2_char_slot)` | W | P2 char-select slot |
| `$FFFC05 (p1_button_press)` | B | Edge-triggered buttons |
| `$FFFC08 (p2_button_held)` | W | Level for OPTIONS cheat |
| `$FFFC0A (p2_joypad_present_negated)` | B | 0 = P2 present |

### 8.2 ROM data

| Reference | Notes |
| --- | ------- |
| `$1060 (select_menu_strings)` | 1P / 2P / OPTIONS VDP strings |
| `$10C4 (select_screen_jt)` | Sub-state jump table |
| `$117A (select_menu_player_mode_tbl)` | Cursor → `$FFFF18 (player_mode)` (`01`, `03`) |
| `$128A (options_sound_id_table)` | Sound IDs for `$FFFFC4 (sound_test_index)` indices |
| `$12FE (options_difficulty_strings)` | EASY / NORMAL / HARD / … |
| `$171A (char_select_jt)` | Character-select sub-state table |
| `$19E4 (char_select_nav_right)` / `$19EA (char_select_nav_left)` | Slot wrap tables |
| `$19FC (char_select_cursor_x)` | X = `$20/$80/$E0` |
| `$1A0E (char_id_from_slot)` | Slot → ID: 0→Adam(1), 1→Axel(0), 2→Blaze(2) |
| `$1A02 (char_preview_object_ptrs)` | → `$B900/$B980/$BA00` animated full-body previews |
| `$1A44 (char_select_character_preview_setup)` | Initialize one type-7 character preview's position, animation, and first active state. |
| `$1C684 (options_sound_name_table)` | 12-byte name rows for sound test |
| `$1C9FC (options_control_strings)` | Control-scheme descriptions |

---

## 9. Diagrams

### 9.1 Select screen sub-states

```
 $00–$02  Main menu cursor (1P / 2P / OPTIONS)
     │ confirm
 $04–$06  Fade out
     │
     ├─ cursor < 2 ──► game_state $20  (character select)
     │
     └─ cursor ≥ 2 ──► $0A build OPTIONS
                           │
                      $10–$26 rows + input
                           │
                      $28–$2A fade
                           │
                      $2C game_state $10  (back to select init)
```

### 9.2 Character select sub-states

```
 $00  music + objects
 $02  fade in
 $04  interactive (type 6 cursors + type 7 character previews)
        │ all players confirmed ($F908)
 $06  short delay
 $08  fade out
 $0A  lives/continues → game_state $28 (level start)
```

---

## 10. Related labels (code)

| Reference |
| --- |
| `$FE8 (init_game_start_screen)` |
| `$108E (game_start_screen_update)` |
| `$10F2 (select_menu_wait_fade_in)` |
| `$1104 (select_menu_input)` |
| `$114A (select_menu_resolve_choice)` |
| `$117C (options_menu_build)` |
| `$1218 (options_input_sound_test)` |
| `$131A (options_input_difficulty)` |
| `$1390 (options_input_controls)` |
| `$1404 (options_input_lives)` |
| `$1476 (options_input_level)` |
| `$14CA (options_input_exit)` |
| `$14EA (options_menu_return)` |
| `$14F2 (options_row_nav)` |
| `$1546 (select_menu_sync_pad_count)` |
| `$1564 (player_state_dispatcher)` |
| `$1634 (init_character_select_screen)` |
| `$170A (screen_state_dispatcher)` |
| `$1726 (char_select_play_music)` |
| `$175A (char_select_interactive)` |
| `$17A2 (initialize_player_continues)` |
| `$1916 (char_select_player_input)` |
| `$AD8E (update_select_objects)` |
| `$9170 (init_selectscreenmode)` / `$919A (game_mode_selectscreenmode)` |
| `$927C (init_characterselectscreen)` / `$92A8 (game_mode_characterselectscreen)` |

---

## 11. Implementation notes for recompilation

- Prefer the CSV symbols above when renaming generated `sub_*` helpers that match these entry points.
- `game_start_screen_*` names are historical; behaviour is the **mode-select** screen under `$FFFF00 (game_state)` `$10`/`$12`.
- OPTIONS sound rows must not be confused with `$FFFF02 (level)`.
- Screen order is **Adam, Axel, Blaze** (left → right). Character IDs are **not** the same as slot indices (slot 0 → Adam ID 1, slot 1 → Axel ID 0, slot 2 → Blaze ID 2).
- 2P anti-collision on character slots is easy to break if a native reimplementation allows double-picks.
- Lives formula is `2 * lives_setting + 1`, not a fixed “9 lives”.

---

## 12. Open follow-ups

- Trace `$106EA (init_levelstart)` / `$1077C (game_mode_levelstart)` (`$28`/`$2A`) end-to-end: how `$FFFF02 (level)`, character IDs, and difficulty are applied to the first wave spawn.
- Name the full object-type jump table at `$B236` (types 6 and 7 handlers).
- Map control-scheme `1` / `2` to exact A/B/C physical buttons with a live input log.
- Confirm default power-on values of `$FFFFCA (lives_setting)` / `$FFFFC8 (control_scheme)` before first OPTIONS visit.
