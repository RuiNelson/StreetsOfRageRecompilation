# Streets of Rage Select Screen, Options Menu, and Character Select

**Manuscript:** reverse-engineering notes from the recompilation project  
**Scope:** main mode-select screen (1 PLAYER / 2 PLAYERS / OPTIONS), the OPTIONS sub-menu (including the P2 button cheat), and the character-select flow that leads into level start  
**Primary sources:** `code-analysis/addresses.csv`, `code-analysis/labels.csv`, `output/sor.asm`, generated `Sor.cpp`, ROM `rom/SOR.bin`

Symbol names below match the analysis CSVs (100% confidence entries unless noted). Despite older disassembly comments that call `$FE8` / `$108E` a “title screen”, those routines implement the **mode-select / start menu**, not the logo title (`init_titlescreen` / `game_mode_titlescreen` at `$90C8` / `$9102`).

---

## 1. Executive summary

After the Sega logo, story intro, title, and optional hi-score screens, the game enters:

| `game_state` | Init handler | Update handler | Role |
|--------------|--------------|----------------|------|
| `$10` / `$12` | `init_selectscreenmode` → `init_game_start_screen` | `game_mode_selectscreenmode` → `game_start_screen_update` | Mode menu + OPTIONS |
| `$20` / `$22` | `init_characterselectscreen` → `init_character_select_screen` | `game_mode_characterselectscreen` → `screen_state_dispatcher` | Pick Axel / Adam / Blaze |
| `$28` / `$2A` | `init_levelstart` | `game_mode_levelstart` | Level intro → in-game |

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

Both the select screen and character select are **sub-state machines** on top of the global `game_state` dispatcher:

- Select / OPTIONS: sub-state word `select_screen_substate` (`$FB0E`), jump table at `$10C4`
- Character select: sub-state word `char_select_substate` (`$F904`), jump table at `$171A`

---

## 2. Global entry: `game_state` $10 / $12

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

## 3. Select-screen init — `init_game_start_screen` ($FE8)

### 3.1 Sequence

| Step | Code effect |
|------|-------------|
| Clear `cheat_flag` (`$FA7B`) | OPTIONS cheat rows off |
| `$FF06 = 1`, `$E000 = 3` | Screen context flags |
| `sub_A63A(d0=2)` | Load menu art / palettes |
| Clear `select_screen_substate` (`$FB0E`) | Sub-state = 0 |
| Zero P1 object slot (`$B800`, 128 bytes) | Cursor object workspace |
| Loop 3× over ROM `$1060` | Draw **1 PLAYER**, **2 PLAYERS**, **OPTIONS** via `vdp_write_menu_string` (`$1290A`) |
| `palette_fade_counter` (`$FB0C`) = `$40` | Fade length |
| Palette setup `$7267C` or `$72684` | Depends on `p2_joypad_present_negated` (`$FC0A`) |
| `select_menu_option_count` (`$FA09`) | **3** if P2 pad present, **1** if not (hides 2 PLAYERS) |

### 3.2 Menu string table (`select_menu_strings` @ `$1060`)

Each of three rows: VDP command longword + tile bytes (font index `1` = `A`, …) + base tile word stream from `$108A`.

Decoded text:

- `1 PLAYER`
- `2 PLAYERS`
- `OPTIONS`

---

## 4. Select-screen update — `game_start_screen_update` ($108E)

### 4.1 START shortcut (OPTIONS only)

If `select_screen_substate` is in **`[$10, $28)`** (inside OPTIONS) and P1 Start is pressed (`p1_button_press` bit 7):

1. Queue UI sound (`queue_sound_id`, ID `$E1`)
2. Force `select_screen_substate = $28` (OPTIONS exit fade)

### 4.2 Sub-state dispatch

```asm
lea    select_screen_jt($10C4), a1
move.w select_screen_substate, d0
move.w 0(a1,d0.w), d0
jmp    0(a1,d0.w)
```

Sub-state advances by **+2** (word index into the table).

### 4.3 Sub-state map (`select_screen_substate` → handler)

| `$FB0E` | Handler | Role |
|---------|---------|------|
| `$00` | `select_menu_wait_fade_in` (`$10F2`) | Cursor object + wait palette fade-in |
| `$02` | `select_menu_input` (`$1104`) | Wait face/Start; run cursor |
| `$04` | `select_menu_begin_fade_out` (`$1130`) | Reset fade counter, advance |
| `$06` | `select_menu_fade_out` (`$113C`) | Palette fade-out step |
| `$08` | `select_menu_resolve_choice` (`$114A`) | 1P/2P → char select, or OPTIONS |
| `$0A` | `options_menu_build` (`$117C`) | Build OPTIONS screen |
| `$0C` | `select_menu_alt_palette` (`$1124`) | Alternate palette path (no P2) |
| `$0E` | `select_menu_wait_fade` (`$10F6`) | Wait fade helper |
| `$10` / `$12` | highlight / `options_input_sound_test` | Sound-test row (`sound_test_index`) |
| `$14` / `$16` | highlight / `options_input_difficulty` | Difficulty row |
| `$18` / `$1A` | highlight / `options_input_controls` | Control-scheme row |
| `$1C` / `$1E` | highlight / `options_input_lives` | Lives row (**cheat only**) |
| `$20` / `$22` | highlight / `options_input_level` | Level row (**cheat only**) |
| `$24` / `$26` | highlight / `options_input_exit` | Exit row |
| `$28` / `$2A` | fade-out | Leaving OPTIONS |
| `$2C` | `options_menu_return` (`$14EA`) | `game_state = $10` |

---

## 5. Main menu (1P / 2P / OPTIONS)

### 5.1 Cursor object

While sub-state is `$00` / `$02`, update calls `player_state_dispatcher` (`$1564`) on `p1_object` (`$B800`). In this screen that object is a **menu cursor**, not an in-game fighter:

| Field | Address | Meaning |
|-------|---------|---------|
| Cursor index | `select_menu_cursor` (`$B840` = `$40` of P1 object) | `0` = 1P, `1` = 2P, `2` = OPTIONS |
| Previous index | `$B842` | Used when skipping 2P with one pad |

Up/Down on `p1_button_press` bits 0–1 wrap the cursor `0…2`, play a UI beep, and place the cursor sprite in the `$DA00` buffer.

If `select_menu_option_count == 1` (no P2 pad), landing on index `1` (2 PLAYERS) is remapped via table `$1630` so the player only sees **1 PLAYER** and **OPTIONS**.

Hot-plug: `select_menu_sync_pad_count` (`$1546`) raises/lowers `select_menu_option_count` when P2 appears or disappears while waiting for confirm.

### 5.2 Confirm (`select_menu_input` @ `$1104`)

- No face/Start bits in `p1_button_press` (`& $F0 == 0`): keep running cursor + pad sync  
- Any face/Start: queue confirm sound, `select_screen_substate += 2`

### 5.3 Resolve (`select_menu_resolve_choice` @ `$114A`)

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
| 0 | `player_mode` (`$FF18`) = **1** → `game_state = $20` |
| 1 | `player_mode` = **3** → `game_state = $20` |
| 2 | Enter OPTIONS; optional `cheat_flag = 1` |

---

## 6. OPTIONS menu

### 6.1 Build (`options_menu_build` @ `$117C`)

1. Clear sprite mapping buffer  
2. Draw static labels (`sub_A8B8` / palette-remapped tilemaps)  
3. Draw current values:
   - **Sound list** — `sound_test_index` (`$FFC4`), tile rows at `$1C684`
   - **Difficulty** — `difficulty` (`$FFC6`), strings at `$12FE`
   - **Controls** — `control_scheme` (`$FFC8`), strings at `$1C9FC`
4. If `cheat_flag`:
   - Draw **lives** digit from `lives_setting` (`$FFCA`)
   - Draw **level** digit from `level` (`$FF02`)
   - Extra labels (`sub_A8B8` IDs `$0F` / `$0E`)

### 6.2 Vertical navigation (`options_row_nav` @ `$14F2`)

Up/Down move `select_screen_substate` between option **rows**. Without the cheat, lives/level rows are skipped:

```
normal:  $10/$12 → $14/$16 → $18/$1A → $24/$26
cheat:   … also $1C/$1E (lives) and $20/$22 (level)
```

### 6.3 Sound test row (`sound_test_index` / `$FFC4`)

Left/Right wrap the index **`0 … $48`** (73 entries). Face buttons play `options_sound_id_table[$128A + index]` via `queue_sound_id`.

This is **not** the starting level. The list is a BGM / voice / SFX browser. ROM text rows at `$1C684` (12 bytes each) decode roughly as:

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

`$FFC4` is only read/written inside this menu.

### 6.4 Difficulty (`difficulty` / `$FFC6`)

| Value | On-screen (`$12FE`, 7 chars) |
|-------|------------------------------|
| 0 | EASY |
| 1 | NORMAL |
| 2 | HARD |
| 3 | Hardest (4th entry) |

Wraps `0 ↔ 3`. Copied to `difficulty_copy` (`$FB5A`) on level load; used for enemy spawn filtering.

### 6.5 Control scheme (`control_scheme` / `$FFC8`)

Three layouts; on-screen copy at `$1C9FC` (blocks of `$50` bytes) describes A/B/C as Special / Attack / Jump variants.

In-game remap is applied in the object input path (`$568A`):

- `0` — default bit layout  
- `1` — shift A/B/C one way  
- `2` — shift the other way  

D-pad and Start are not remapped by that path.

### 6.6 Lives (cheat) (`lives_setting` / `$FFCA`)

Input wraps **`0 … 3`**. Applied when leaving character select (and on some respawn paths):

```asm
move.w  lives_setting, d0
add.w   d0, d0
addq.w  #1, d0          ; lives = 2*n + 1  →  1, 3, 5, or 7
move.b  d0, p1_lives
move.b  d0, p2_lives
```

Continues are always set to **3** in `initialize_player_continues`. Older comments called this a “9-lives” cheat; the formula observed here yields **1 / 3 / 5 / 7**.

### 6.7 Level (cheat) (`level` / `$FF02`)

Left/Right adjust the global `level` word (0 = first round). That value is what `init_levelstart` uses when play begins.

### 6.8 Leaving OPTIONS

1. Exit row + face button, **or** Start anywhere in OPTIONS → sub-state `$28`  
2. Fade `$28`–`$2A`  
3. `options_menu_return` (`$14EA`): **`game_state = $10`**

`init_game_start_screen` clears `cheat_flag` again on re-entry. Difficulty, controls, lives setting, and level **persist** in high work RAM (outside the large clear used later by character select).

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

### 7.2 Init — `init_character_select_screen` ($1634)

1. **Clear low work RAM** — `$1FA` × 128-byte fills from `$FF0000` → ends near `$FFFD00`. High variables (`game_state`, `player_mode`, `difficulty`, `control_scheme`, `lives_setting`, `level`, character IDs) live above that range and survive.  
2. `char_select_idle_timer` (`$F900`) = `$12C` (~300 frames)  
3. Fade setup; palette `$71F30`  
4. Spawn **player cursors** (object type **6**):

| Object | Base | Type | Char slot (`+$58`) | X (`+$10`) |
|--------|------|------|--------------------|------------|
| P1 | `p1_object` `$B800` | 6 | **0** (left) | `$20` |
| P2 (if `player_mode == 3`) | `p2_object` `$B880` | 6 | **2** (right) | `$E0` |

5. Spawn **portraits** (object type **7**):

| Slot | Base | Type | ID at `+$50` | Character |
|------|------|------|--------------|-----------|
| Left | `$B900` | 7 | **1** | Adam |
| Center | `$B980` | 7 | **0** | Axel |
| Right | `$BA00` | 7 | **2** | Blaze |

6. Load character art (`sub_A63A`, Kosinski `$71C6C`, UI tilemaps via `sub_A8B8`).

### 7.3 Update — `screen_state_dispatcher` ($170A)

Indexed by `char_select_substate` (`$F904`), table `$171A`:

| `$F904` | Handler | Role |
|---------|---------|------|
| `$00` | `char_select_play_music` (`$1726`) | Queue select BGM; run one object pass |
| `$02` | `char_select_fade_in` (`$1738`) | Wait fade-in; enable portrait anim flags |
| `$04` | `char_select_interactive` (`$175A`) | Main loop: objects + wait for confirms |
| `$06` | `char_select_exit_delay` (`$1788`) | Count down `char_select_exit_delay` (`$F90A`) |
| `$08` | `char_select_fade_out` (`$1794`) | Palette fade-out |
| `$0A` | `initialize_player_continues` (`$17A2`) | Lives/continues → `game_state = $28` |

### 7.4 Object pass — `update_select_objects` ($AD8E)

Each frame during interactive select:

1. For P1 and P2 slots: if type ≠ 0, dispatch via object-type jump table `$B236`  
2. Copy held/press input into object fields  
3. Process portrait objects in the object table  
4. `sync_z80_2` (VBlank wait)

Type **6** = cursor selection logic (`char_select_player_input` @ `$1916`).  
Type **7** = portrait animation / DMA frames.

### 7.5 Cursor input — `char_select_player_input` ($1916)

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

On move: clear old portrait highlight (`+$5C`), set new highlight, update X from `$19FC` (`$20` / `$80` / `$E0`), refresh name palettes, play UI sound.

**Confirm** (face/Start, `$55 & $F0`):

```asm
move.b  #1, $5a(a0)                    ; lock
addq.w  #1, char_select_confirm_count  ; $F908
; write character ID:
;   P1 → p1_character_id ($FF1E)
;   P2 → p2_character_id ($FF1F)
move.b  char_id_from_slot($1A0E)[slot], (a2)
```

Slot → ID table `$1A0E`:

| Slot (screen position) | ID | Character |
|------------------------|----|-----------|
| 0 left (`X=$20`) | **1** | Adam |
| 1 center (`X=$80`) | **0** | Axel |
| 2 right (`X=$E0`) | **2** | Blaze |

After lock, movement input is ignored for that player.

### 7.6 Confirm count (`char_select_confirm_count` / `$F908`)

- 1P (`player_mode == 1`): need count **1**  
- 2P (`player_mode == 3`): need count **2**  

Then set `char_select_exit_delay` (`$F90A`) = `$12` and advance sub-state.

### 7.7 Exit — `initialize_player_continues` ($17A2)

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
| `p1_character_id` / `p2_character_id` | Copied into object `$50` / `$B850` / `$B8D0` for combat |
| `difficulty` | → `difficulty_copy` on level load |
| `control_scheme` | Button remap in object input |
| `level` | Starting round |
| `player_mode` | Which players are active |

---

## 8. Memory map (this feature area)

### 8.1 Work RAM

| Address | Symbol | Size | Notes |
|---------|--------|------|-------|
| `$FF00` | `game_state` | W | Global mode |
| `$FF02` | `level` | W | Starting / current level; OPTIONS cheat editable |
| `$FF18` | `player_mode` | B | `1` = 1P, `3` = 2P |
| `$FF1E` | `p1_character_id` | B | 0=Axel, 1=Adam, 2=Blaze |
| `$FF1F` | `p2_character_id` | B | Same encoding |
| `$FFC4` | `sound_test_index` | W | OPTIONS sound browser only |
| `$FFC6` | `difficulty` | W | 0…3 |
| `$FFC8` | `control_scheme` | W | 0…2 |
| `$FFCA` | `lives_setting` | W | 0…3 → lives `2n+1` |
| `$FA09` | `select_menu_option_count` | B | 1 or 3 visible main-menu entries |
| `$FA7B` | `cheat_flag` | B | OPTIONS lives/level rows |
| `$FB0C` | `palette_fade_counter` | W | Shared fade timing |
| `$FB0E` | `select_screen_substate` | W | Select / OPTIONS sub-state |
| `$F900` | `char_select_idle_timer` | W | Idle / blink timer on char select |
| `$F904` | `char_select_substate` | W | Character-select sub-state |
| `$F908` | `char_select_confirm_count` | W | Players locked in |
| `$F90A` | `char_select_exit_delay` | W | Frames before fade-out |
| `$B800` | `p1_object` | — | Cursor (select/char select) or fighter |
| `$B840` | `select_menu_cursor` | W | Main menu index (object +$40) |
| `$B858` | `p1_char_slot` | W | Char-select slot (object +$58) |
| `$B880` | `p2_object` | — | P2 cursor / fighter |
| `$B8D8` | `p2_char_slot` | W | P2 char-select slot |
| `$FC05` | `p1_button_press` | B | Edge-triggered buttons |
| `$FC08` | `p2_button_held` | W | Level for OPTIONS cheat |
| `$FC0A` | `p2_joypad_present_negated` | B | 0 = P2 present |

### 8.2 ROM data

| Address | Symbol | Notes |
|---------|--------|-------|
| `$1060` | `select_menu_strings` | 1P / 2P / OPTIONS VDP strings |
| `$10C4` | `select_screen_jt` | Sub-state jump table |
| `$117A` | `select_menu_player_mode_tbl` | Cursor → `player_mode` (`01`, `03`) |
| `$128A` | `options_sound_id_table` | Sound IDs for `$FFC4` indices |
| `$12FE` | `options_difficulty_strings` | EASY / NORMAL / HARD / … |
| `$171A` | `char_select_jt` | Character-select sub-state table |
| `$19E4` / `$19EA` | `char_select_nav_right` / `_left` | Slot wrap tables |
| `$19FC` | `char_select_cursor_x` | X = `$20/$80/$E0` |
| `$1A0E` | `char_id_from_slot` | Slot → character ID |
| `$1A02` | `char_portrait_object_ptrs` | → `$B900/$B980/$BA00` |
| `$1C684` | `options_sound_name_table` | 12-byte name rows for sound test |
| `$1C9FC` | `options_control_strings` | Control-scheme descriptions |

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
 $04  interactive (type 6 cursors + type 7 portraits)
        │ all players confirmed ($F908)
 $06  short delay
 $08  fade out
 $0A  lives/continues → game_state $28 (level start)
```

---

## 10. Related labels (code)

| Address | Symbol |
|---------|--------|
| `$0FE8` | `init_game_start_screen` |
| `$108E` | `game_start_screen_update` |
| `$10F2` | `select_menu_wait_fade_in` |
| `$1104` | `select_menu_input` |
| `$114A` | `select_menu_resolve_choice` |
| `$117C` | `options_menu_build` |
| `$1218` | `options_input_sound_test` |
| `$131A` | `options_input_difficulty` |
| `$1390` | `options_input_controls` |
| `$1404` | `options_input_lives` |
| `$1476` | `options_input_level` |
| `$14CA` | `options_input_exit` |
| `$14EA` | `options_menu_return` |
| `$14F2` | `options_row_nav` |
| `$1546` | `select_menu_sync_pad_count` |
| `$1564` | `player_state_dispatcher` |
| `$1634` | `init_character_select_screen` |
| `$170A` | `screen_state_dispatcher` |
| `$1726` | `char_select_play_music` |
| `$175A` | `char_select_interactive` |
| `$17A2` | `initialize_player_continues` |
| `$1916` | `char_select_player_input` |
| `$0AD8E` | `update_select_objects` |
| `$9170` / `$919A` | `init_selectscreenmode` / `game_mode_selectscreenmode` |
| `$927C` / `$92A8` | `init_characterselectscreen` / `game_mode_characterselectscreen` |

---

## 11. Implementation notes for recompilation

- Prefer the CSV symbols above when renaming generated `sub_*` helpers that match these entry points.  
- `game_start_screen_*` names are historical; behaviour is the **mode-select** screen under `game_state` `$10`/`$12`.  
- OPTIONS sound rows must not be confused with `level` (`$FF02`).  
- Character IDs are **not** the same as screen slot indices (slot 0 → Adam ID 1, slot 1 → Axel ID 0, slot 2 → Blaze ID 2).  
- 2P anti-collision on character slots is easy to break if a native reimplementation allows double-picks.  
- Lives formula is `2 * lives_setting + 1`, not a fixed “9 lives”.

---

## 12. Open follow-ups

- Trace `init_levelstart` / `game_mode_levelstart` (`$28`/`$2A`) end-to-end: how `level`, character IDs, and difficulty are applied to the first wave spawn.  
- Name the full object-type jump table at `$B236` (types 6 and 7 handlers).  
- Map control-scheme `1` / `2` to exact A/B/C physical buttons with a live input log.  
- Confirm default power-on values of `lives_setting` / `control_scheme` before first OPTIONS visit.
