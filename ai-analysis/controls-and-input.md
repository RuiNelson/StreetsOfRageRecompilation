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

The native port's host option `--altControls` deliberately bypasses
`$FFFFC8 (control_scheme)`: the sampler reads the 6-button pad sequence and
pre-translates it into the same logical bits before `$568A
(remap_player_gameplay_input)` copies the word into each player object.

| Physical button under `--altControls` | Native-port role |
|---|---|
| A | Sets logical B+C together, producing the rear attack input. |
| B | Logical attack (`$10`). |
| C | Logical jump (`$20`). |
| X | Logical police special (`$40`), replacing original physical A. |
| Y | Pickup-only edge tracked outside the ROM input byte. |
| Z | No gameplay action. |
| Start | Start only; pause/demo-abort behavior is unchanged. |

Y is not written into `$FFFC04/$FFFC08` because the original active-high button
byte has no spare bit once D-pad, B, C, X-as-special, and Start are preserved.
The manual pickup hooks read a native per-player Y edge instead.

## Gameplay action priority

The player action code consumes the logical object input produced by
`$568A (remap_player_gameplay_input)`. On the idle/ground path around `$2CD2`
the priority chain is:

```text
resolve contact/throw state ($3266)
resolve attack+jump chord / rear attack ($322A)
resolve jump press, bit 5 ($2FCC)
resolve normal attack, bit 4 ($3028 / player_normal_attack_input)
otherwise directional movement from the low input nibble
```

`$3028 (player_normal_attack_input)` is the ordinary logical attack path. It
first calls `$3136 (find_close_interaction_target)`; if a nearby free weapon or
consumable pickup is found, the player enters the pickup action instead of
starting a normal punch. If no target is found, the same logical attack bit
starts the attack/combo transition.

This is why the original behavior gives pickup priority over attack when the
player presses the configured attack button near an item. With `--altControls`,
the manual pickup hooks invert that priority: B remains attack and will not be
converted into pickup, while Y alone requests
`$3136 (find_close_interaction_target)`. Start is no longer overloaded for
pickup and always reaches the normal Start path.

### Attack+jump chord versus jump-kick

Two different moves share the same face buttons. The order of presses decides
which one the ROM selects:

| Input pattern | Result | Action family |
|---|---|---|
| **B and C on the same decision** (held+edge either order) | Rear / escape attack | `$20+` via `$322A (player_attack_jump_chord)` |
| **C only** to leave the ground, then **B later** while airborne | Jump-kick | `$10 → $12 → $16` |

`$322A (player_attack_jump_chord)` runs **before** `$2FCC` in the ground
priority chain, so a simultaneous B+C chord never becomes a jump. Jump-kick is
strictly sequential: jump edge first, attack edge only after free flight.

#### Measured chord timing

The chord is not instant, and its timeline is not shared between characters.
Measured live: lockstep host at one frame per step, the B+C edge issued on
frame 0, then object `+$30` (action), `+$0A` (animation frame), and `+$34`
(outgoing damage) sampled every frame until the player returns to idle.

| Character | Action | Startup | Damaging frames | Damage | Recovers |
|---|---|---:|---|---:|---:|
| Axel | `$20` | 3 | 3 – 12 (10 frames) | 3 | idle `$02` at 17 |
| Blaze | `$20` | 7 | 7 – 23 (17 frames) | 2 | idle `$02` at 30 |
| Adam | `$22` → `$24` | 23 | 23 – 43 (21 frames) | 3 | lands `$14` at 44 |

Startup is the frame the first nonzero `+$34` appears; the move connects only
with a body inside the attack box during the damaging span.

Three consequences for anything that issues this chord:

- **The wind-up is long and uneven.** Adam's is nearly eight times Axel's. A
  caller that presses B+C once the target is already in range arms the hit
  after the target has walked through the box — early for Axel, hopelessly
  early for Adam.
- **Adam's chord is a different move.** `$322A (player_attack_jump_chord)` selects `$20 + facing`, but
  Adam's action table routes it through `$22` into `$24` and ends in the
  landing state `$14`: a hop, not the standing backfist Axel and Blaze get.
  Code that pattern-matches the `$20` family alone misses it entirely.
- **Damage is per character** — 3 / 2 / 3. The commonly quoted "back attack
  does 3" is Axel's number.

The attack box `+$70` is player X −7..+3 by Y ±8: a contact move centred on
the player's own body, not a reaching one. Compare the jump-kick boxes below,
which are offset well in front of the player.

## Jump and jump-kick (ROM physics)

This section is the mathematical model of the unarmed jump-kick. Addresses are
ROM code/data unless prefixed with `FF` (work RAM). Player objects live at
`$FFB800 (p1_object)` and `$FFB880 (p2_object)`. Action state is object `+$30`
(bit 0 = facing left). Velocities and positions are 16.16 fixed-point longs at
`+$1C` (X), `+$20` (lane Y), `+$24` (height Z) and `+$10` / `+$14` / `+$18`.

### Action state machine

`$1C44 (update_player_object)` dispatches `+$30` through the word table at
`$1CE2`:

| Action | Name | Handler | Role |
|---:|---|---:|---|
| `$10/$11` | Jump start | `$1FC0` | Crouch timer, then launch |
| `$12/$13` | Free flight | `$1FDC` | Gravity, air steer, attack edge |
| `$16/$17` | Jump attack (kick) | `$2000` | Lighter fall gravity + damage |
| `$14/$15` | Jump land | `$1FE8` | Recovery → ground `$02` |

Held-weapon jumps use the parallel family `$3C–$43` (`$2158` / `$2174` /
`$2198` / `$2180`) with the same physics helpers.

```text
ground ──C ($2FCC)──► $10 JUMP_START ──(timer)──► $12 FREE FLIGHT
                              │                         │
                              │                    B ($3914)
                              │                         ▼
                              │                    $16 JUMP_ATK
                              │                         │
                              └──────── land ($3E78) ───┴──► $14 LAND ──► $02
```

### Jump press and crouch

`$2FCC` on a new jump edge (object `+$55` bit 5):

1. queues sound id `$A0` through `$35D6`;
2. sets action `$10` (plus facing) via `$2EE8`.

`$2EE8` / `$2EF2` always call `$3614`, which loads character walk speeds from
the tables at `$3670` / `$3706` / `$379C`. **Jump rows of those tables are
zero**, so entering `$10` clears horizontal and lane velocity. Walk momentum
does not carry into a jump.

Jump-start handler `$1FC0` only decrements frame timer `+$0D`. All three
characters use jump anim bank `c = $04` with frame-0 duration **5**, so crouch
lasts **5 frames** before launch. Animation does not advance during crouch.

### Launch (end of crouch)

When the crouch timer expires, `$1FC0`:

1. advances action by 2 (`$10 → $12`);
2. `$2EF2` re-enters free-flight anim and again zeros X/Y via `$3614`;
3. `$3832` writes initial Z velocity from the character table at `$3842`;
4. `$384E` sets X velocity to **±`$00030000` (±3.0 px/frame)** if Left or
   Right is held, and updates facing; if neither is held, **X stays 0**.

On that same transition frame, gravity is **not** applied yet; `$442C`
integrates position once with the launch velocities.

| Character | ID | `$3842` long | Launch \(v_z\) |
|---|---:|---:|---:|
| Axel | 0 | `$FFF88000` | **−7.5** px/frame |
| Adam | 1 | `$FFF78000` | **−8.5** px/frame |
| Blaze | 2 | `$FFF68000` | **−9.5** px/frame |

Negative Z velocity raises the character (object `+$18` decreases). Positive Z
velocity falls toward the floor sample from `$AD2A`.

### Free flight

Each free-flight frame (`$1FDC`):

| Step | Routine | Effect |
|---|---|---|
| Gravity | `$389A` | \(v_z \mathrel{+}= `$E800`\) (**+0.90625** px/f²) |
| Air steer | `$38C0` | Left/Right adds **±`$6000` (±0.375)** to \(v_x\); clamp **±`$38000` (±3.5)**; updates facing |
| Kick edge | `$3914` | If attack bit 4 is newly pressed: set `+$58` bit 2, action ← `(action & $FE) + 4` (`$12 → $16`), re-init anim, play kick sound |
| Fall clamp | `$3886` | \(v_z \le `$C0000` (**12.0**) |
| Integrate | `$442C` | \(x \mathrel{+}= v_x\), \(y \mathrel{+}= v_y\), \(z \mathrel{+}= v_z\) |

There is **no mid-air lane (Up/Down) control**. Lane Y is frozen at takeoff.
`$3914` sets `+$58` bit 2 before `$2EE8` so `$3614` **preserves** the current
X velocity when entering the kick (it does not re-zero it).

### Jump-kick gravity and damage

Kick handler `$2000` uses `$38AE`:

- while rising (\(v_z < 0\)): full gravity `$E800`;
- while falling (\(v_z \ge 0\)): lighter gravity **`$8800` (+0.53125)** so a
  kick mid-air lengthens hang time and horizontal range.

`$41EA (compute_player_attack_descriptor)` indexes anim `$14/$16` descriptors:

| Character | Kick anim behaviour | Damage (low nibble) | Reaction (high nibble) |
|---|---|---:|---:|
| Axel / Adam | Stay on frame 0 for the whole kick | **3** every kick frame | 1 |
| Blaze | Advances frames until frame 3 | **0** on f0–f1, **2** on f2–f3 | 1 |

Free flight (anim `$10`) has **0** damage on frame 0. A duel flag at
`$FFFA43 (duel_damage_modifier)` triples the damage nibble modulo 16.

### Kick hitboxes

`$4140` builds body (`+$64`) and attack (`+$70`) AABBs from anim frame box IDs
into tables `$1ABA8` / `$1AB8E`. Jump-kick attack boxes (relative to the
player origin; bit0 of action selects facing):

| Char | Facing right (bit0=0) | Facing left (bit0=1) |
|---|---|---|
| Axel | x[+6..+16] y[−8..+8] z[−52..−38] | x[−16..−6] y[−8..+8] z[−52..−38] |
| Adam | x[+10..+21] y[−8..+8] z[−49..−33] | x[−21..−10] y[−8..+8] z[−49..−33] |
| Blaze | x[−8..+3] y[−8..+8] z[−48..−28] | x[−3..+8] y[−8..+8] z[−48..−28] |

Lane half-width is about **8** pixels. The Z band sits high on the body, so a
kick connects best while the player is not at maximum apex against a grounded
foe—timing the B edge on the way down (or early at close range) matters.

On connect, `$21B4` can set `+$59` bit 7 (hit freeze); `$442C` then skips
position integration for the jump-attack anim group.

### Landing

When falling collision in `$3E78` finds floor height `d6`: snap `+$18` to the
floor, clear \(v_z\), play land sound, set action `$14` (or weapon `$40`). Land
handler `$1FE8` uses another **5-frame** timer, then returns to ground idle
`$02` via `(action & $FE) − $12`.

### Closed-form trajectory summary

With constant \(v_x = 3.0\) (direction held), crouch 5 frames, no early kick:

| Char | Free-flight frames to land | Approx. horizontal range | Apex (relative) |
|---|---:|---:|---:|
| Axel | 18 | **54** px | ≈ −35 |
| Adam | 20 | **60** px | ≈ −44 |
| Blaze | 22 | **66** px | ≈ −55 |

Kicking from the first free-flight frame (lighter fall gravity) extends that
to about **60 / 69 / 75** px. Air steer can push \(|v_x|\) to 3.5. Stationary
launches (\(v_x = 0\) at takeoff) only gain range from mid-air L/R.

Discrete recurrence (faithful agent solver form):

```text
// After 5 crouch frames at ground z_g:
vz ← vz0[char];  vx ← ±3.0 if dir held else 0
// First free-flight frame (no gravity yet):
x ← x + vx;  z ← z + vz
// Later free-flight / kick frames:
if kick and vz ≥ 0:  vz ← vz + 0.53125
else:                vz ← vz + 0.90625
vx ← clamp(vx + air_steer, −3.5, +3.5)
vz ← min(vz, 12)
x ← x + vx;  z ← z + vz
// Land when z ≥ z_g after a falling integrate (ROM uses collision probe)
```

Multi-enemy use: the kick remains active for the rest of the airtime after B.
Any foe whose body AABB intersects the moving attack box on some kick frame is
hit. Same-lane packs on the flight path are therefore one of the strongest
uses of jump-kick; a predictive solver should score plans by **how many live
hostiles the arc will damage**, not only primary-target distance bands.

### Partner vault high jump (related)

From a partner hold, `$2FE4` can enter higher jump actions (`$76` / `$80`
family). That path is a co-op boost, not the ordinary ground C→B kick, but the
airborne attack edge rule is the same once free flight is reached.

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
| `$322A (player_attack_jump_chord)` | Same-decision B+C rear/escape attack (blocks jump-kick). |
| `$2FCC` | Jump edge → action `$10`. |
| `$1CE2` | Player primary-state jump table (includes jump family). |
| `$1FC0` / `$1FDC` / `$2000` / `$1FE8` | Jump start / free flight / kick / land handlers. |
| `$3832` / `$3842` | Character launch Z-velocity table. |
| `$384E` / `$38C0` | Launch X velocity and air L/R steer. |
| `$389A` / `$38AE` | Full and kick-fall gravity. |
| `$3914` | Free-flight attack edge → jump-kick action `$16`. |
| `$41EA (compute_player_attack_descriptor)` | Per-frame kick damage / reaction nibbles. |
| `$4140` / `$1ABA8` | Body and attack AABB construction. |
| `$442C` / `$3E78` | Position integrate and ground landing → `$14`. |
| `$10D2E (handle_pause_start_input)` | Start buffering, pause toggle, and demo abort. |
| `$115CC (update_join_and_continue_hud)` | Inactive-player Start join and continue HUD path. |
