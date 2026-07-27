# Streets of Rage Controls and Input Pipeline

## Scope and sources

This manuscript describes how the game reads the Mega Drive controller ports,
stores per-frame button state, routes that state into menus and gameplay
objects, and implements the configurable control layouts exposed in OPTIONS.
It is based primarily on `output/sor.asm`, with symbol names from
`code-analysis/labels.csv` and `code-analysis/addresses.csv`.

The input pipeline has three layers:

1. Hardware sampling during VBlank fills global P1/P2 button buffers.
2. Screen-specific code consumes those global buffers directly for menus and
   pause.
3. Gameplay copies the owning global buffer into each live player object and
   optionally remaps the face buttons according to OPTIONS.

## Button byte layout

The ROM stores controller input as an active-high byte: a set bit means the
button is currently down, or was newly pressed for an edge buffer.

| Bit | Mask | Physical button | Logical gameplay role after remap |
|---:|---:|---|---|
| 0 | `$01` | Up | Up |
| 1 | `$02` | Down | Down |
| 2 | `$04` | Left | Left |
| 3 | `$08` | Right | Right |
| 4 | `$10` | B | Attack |
| 5 | `$20` | C | Jump |
| 6 | `$40` | A | Police special |
| 7 | `$80` | Start | Start / pause |

The last column is the fixed logical layout used by gameplay after
`$568A (remap_player_gameplay_input)` has applied `$FFFFC8 (control_scheme)`.
Menus generally read the physical face-button group as `$F0` and do not care
which face button was pressed.

## Hardware setup and VBlank sampling

`$7F50 (init_joypad)` initializes the I/O control ports for the player and
modem ports, then writes `$C0` to `$A10003 (io_player1_data_port)`. Normal
frame-by-frame sampling happens inside `$19D16 (vblank_handler)`: after the
mode-specific VBlank work reaches the common tail, it calls
`$810C (sample_all_joypads)` and then resumes incremental art decoding with
`$8510 (continue_incremental_nemesis_decode)`.

`$810C (sample_all_joypads)` requests the Z80 bus with `$A11100 (z80_busreq)`,
samples P1 through `$813C (sample_one_joypad)`, advances the output buffer by
two bytes, samples P2, and releases the bus. The bus request keeps the 68000's
joypad reads synchronized with the rest of the hardware work in VBlank.

`$813C (sample_one_joypad)` performs the actual 3-button read:

1. Write `$00` to the data port, wait two NOPs, and read the port.
2. Shift the first read left by two bits and keep its high button bits.
3. Write `$40` to the data port, wait two NOPs, and read the port again.
4. Keep the low six bits from the second read, OR the two reads together, and
   invert the byte so the rest of the engine sees active-high buttons.
5. Compare the two raw reads. If they are identical, write `1` to the
   present-negated flag; otherwise write `0`.

The same routine also computes the edge-triggered press byte. It loads the old
held byte from the destination buffer, XORs it with the new held byte, and then
ANDs with the new held byte. The result is "newly pressed this sample", not
"changed state".

## Global input buffers

The sampler writes compact three-byte records in low work RAM:

| Reference | Meaning |
|---|---|
| `$FFFC04 (p1_button_held)` | P1 held byte followed by P1 press byte. |
| `$FFFC05 (p1_button_press)` | P1 newly pressed buttons for the current sample. |
| `$FFFC06 (p1_joypad_present_negated)` | P1 pad detection result: `0` present, `1` not detected. |
| `$FFFC08 (p2_button_held)` | P2 held byte followed by P2 press byte. |
| `$FFFC09 (p2_button_press)` | P2 newly pressed buttons for the current sample. |
| `$FFFC0A (p2_joypad_present_negated)` | P2 pad detection result: `0` present, `1` not detected. |

Because `$FFFC04 (p1_button_held)` and `$FFFC08 (p2_button_held)` are word
symbols, a word read gives held in the high byte and press in the low byte. That
is why gameplay can copy both held and pressed-edge input with one word move.

`$10526 (clear_player_input)` writes `0xFF00` to both word buffers. On the next
sample, this suppresses stale edges while leaving the detection byte separate.
The select and character-select init wrappers call it after changing major
screen state.

## Demo input injection

When `$FFFF2A (demo_mode)` is nonzero, `$813C (sample_one_joypad)` still reads
the physical controller but replaces the low seven bits of both the held byte
and press byte from the scripted streams at `$FFFF2C (demo_ai_input_p1)` and
`$FFFF30 (demo_ai_input_p2)`. Bit 7, Start, remains the real hardware input.
Each sample consumes two script bytes per player.

This explains the attract-mode behavior: scripted direction and face-button
input can drive the demo, while a real Start press is still visible to the
pause/demo-abort path.

## Menu and character-select usage

The mode-select and OPTIONS screens consume the global buffers directly. The
main menu handler `$1104 (select_menu_input)` checks
`$FFFC05 (p1_button_press) & $F0`; any face button or Start confirms the
current row. Cursor movement uses the P1 edge byte's direction bits. When
entering OPTIONS, `$114A (select_menu_resolve_choice)` checks
`$FFFC08 (p2_button_held)` against the P2 A+B+C+Start cheat chord to enable the
extra lives and level rows.

Inside OPTIONS, `$1390 (options_input_controls)` is the row that changes
`$FFFFC8 (control_scheme)`. Left/Right in `$FFFC05 (p1_button_press)` wrap the
value through `0..2`, and `$1370 (options_draw_controls)` redraws the
corresponding string from `$1C9FC (options_control_strings)`.

Character select also starts from the global sampled input, but copies it into
cursor objects. `$18D4 (char_select_cursor_dispatcher)` ORs the owning player's
word buffer into the type-6 cursor object's `+$54` input word. Then
`$1916 (char_select_player_input)` reads object `+$55` as the pressed-edge byte:
Left/Right changes the selected character slot, and `$F0` confirms.

## Gameplay object input and configurable controls

Gameplay does not keep reading `$FFFC04 (p1_button_held)` and
`$FFFC08 (p2_button_held)` throughout the player state machine. Instead, player
setup/update code calls `$568A (remap_player_gameplay_input)` for each active
player object.

That routine selects the source buffer by object address:

- `$FFB800 (p1_object)` uses `$FFFC04 (p1_button_held)`.
- Any other player object uses `$FFFC08 (p2_button_held)`.

It preserves the high face-button history already in object `+$54` with
`& $F0F0`, ORs in the current sampled word, and writes the result back to
object `+$54`. Object byte `+$54` is held input and object byte `+$55` is
pressed-edge input. Player action code therefore uses the object-local input
copy rather than global RAM.

The configurable controls are implemented only at this object-copy boundary.
If `$FFFF2A (demo_mode)` is active, the routine bypasses remapping. Otherwise,
when `$FFFFC8 (control_scheme)` is nonzero, it isolates the face-button bits in
both bytes with `$7070`, preserves D-pad and Start with `$8F0F`, and rotates
the A/B/C bits into the fixed logical slots:

| `$FFFFC8 (control_scheme)` | Physical A | Physical B | Physical C |
|---:|---|---|---|
| `0` | Special | Attack | Jump |
| `1` | Attack | Jump | Special |
| `2` | Jump | Special | Attack |

After this point, the player state machine can treat bit `$10` as attack, bit
`$20` as jump, and bit `$40` as police special regardless of the selected
OPTIONS layout. Directions and Start are never remapped.

## Gameplay action priority

The player action code consumes the logical object input produced by
`$568A (remap_player_gameplay_input)`. `$3028 (player_normal_attack_input)` is
the ordinary logical attack path. It first calls
`$3136 (find_close_interaction_target)`; if a nearby free weapon or consumable
pickup is found, the player enters the pickup action instead of starting a
normal punch. If no target is found, the same logical attack bit starts the
attack/combo transition.

This is why the original behavior gives pickup priority over attack when the
player presses the configured attack button near an item. The native port's
host option `--alternativePickupRoutine` changes that policy outside the
original ROM input pipeline: attack remains on the configured attack bit, while
the pickup attempt is moved to Start when a close item exists. Start should
still reach the normal pause handler when no pickup is being taken.

## Start, pause, and joining

Start is intentionally not part of the configurable face-button remap. In
gameplay, `$10D2E (handle_pause_start_input)` checks active players from
`$FFFF18 (player_mode)`, ORs `$FFFC05 (p1_button_press)` and
`$FFFC09 (p2_button_press)` into `$FFFA47 (p1_start_button_buffer)` and
`$FFFA48 (p2_start_button_buffer)`, and tests bit 7. In normal play it toggles
`$FFFA46 (pause_text_flag)` between running (`0`) and paused (`3`) and plays
the pause sound. In demo mode, Start sets bit 7 of `$FFFF2A (demo_mode)`,
starts a fade, and routes the attract sequence out.

Inactive-player joining is handled separately by
`$115CC (update_join_and_continue_hud)`. That path uses Start from the relevant
player's input buffer to activate a waiting player or continue display state,
while the main pause path only considers players whose bits are already set in
`$FFFF18 (player_mode)`.

## Evidence map

| Reference | Role |
|---|---|
| `$7F50 (init_joypad)` | Initial controller port setup. |
| `$19D16 (vblank_handler)` | Per-frame VBlank path that calls the sampler. |
| `$810C (sample_all_joypads)` | P1/P2 sampler wrapper and Z80 bus arbitration. |
| `$813C (sample_one_joypad)` | TH-toggle read, active-high conversion, edge detection, demo injection, and pad-present flag. |
| `$10526 (clear_player_input)` | Clears global held/press word buffers at screen transitions. |
| `$1104 (select_menu_input)` | Main menu confirm and cursor input from P1 press bits. |
| `$1390 (options_input_controls)` | OPTIONS control-layout row; writes `$FFFFC8 (control_scheme)`. |
| `$18D4 (char_select_cursor_dispatcher)` | Copies sampled player input into character-select cursor objects. |
| `$1916 (char_select_player_input)` | Character-select movement and confirm from object-local pressed input. |
| `$568A (remap_player_gameplay_input)` | Copies global input into player object `+$54` and applies configurable A/B/C remap. |
| `$3028 (player_normal_attack_input)` | Logical attack path that gives original pickup/search priority over a normal attack. |
| `$3136 (find_close_interaction_target)` | Close-range search for pickup and weapon acquisition targets. |
| `$10D2E (handle_pause_start_input)` | Start buffering, pause toggle, and demo abort. |
| `$115CC (update_join_and_continue_hud)` | Inactive-player Start join and continue HUD path. |
