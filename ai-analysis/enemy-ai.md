# Enemy Artificial Intelligence

## Scope, correction, and evidence

This document covers the ordinary-enemy engine centered on `$00937A-$00A43D`. A cross-check against decoded enemy-load-cue (ELC) placement data establishes that object types `$55-$58` are **boss families**, not ordinary enemies:

| Type | Boss family | Confirming rounds | Handler |
|---:|---|---|---:|
| `$55` | Souther | 2, 6, 8 | `$0158C4` |
| `$56` | Antonio | 1, 8 | `$015E70` |
| `$57` | Bongo | 4, 6, 8 | `$016CE4` |
| `$58` | Onihime/Yasha | 5, 8 | `$0174E0` |
| `$30` | Abadede | 3, 8 | `$0143D0` |

Those handlers are discussed only at the boundary of shared infrastructure. Earlier inference from their sophisticated target selection was insufficient to classify them as ordinary enemies; ELC placement is decisive.

The ordinary roster is the contiguous type range `$20-$2A`. `$009350` subtracts `$20` and accepts exactly eleven values, and the palette/metadata pass at `$000810` applies the same range. The ordinary subsystem is more data-driven than the later boss handlers: type and variant select statistic, palette, animation and behavior tables around `$026FCE-$027032`.

## Ordinary-enemy lifecycle

```text
spawn type $20-$2A
    -> wait until active/on screen ($937A / $A59C)
    -> initialize type+variant tables ($938C)
    -> choose active player target ($96EC)
    -> primary state $0100: normal behavior
    -> collision may enter $0300 hit/airborne, $0500 held, or $0700 blocked
    -> health/death enters $0600
    -> remove object, release palette/active-enemy accounting
```

`$00937A` is the activation entry. Once the object is eligible, `$00938C` derives `type_index=type-$20`, initializes combat/animation metadata, selects a target, and enters the normal state.

Primary state is a **word** at object offset `$30`, normally encoded in `$0100` increments. This differs from the byte-sized state conventions in several boss families.

## Object layout used by ordinary enemies

Objects are 128 bytes in the table beginning at `$FFB900`.

| Offset | Width | Meaning | Evidence |
|---:|---:|---|---|
| `$00` | B | Type `$20-$2A` | `$9350`, `$938C` |
| `$01` | B | Visibility, collision and airborne flags | hit/death paths |
| `$04` | L | Animation-set pointer | selected from `$27032` |
| `$08` | W | Animation/action index | `$969E/$96C0` |
| `$09` | B | Facing flags; bit 1 is left/right | `$96C0`, `$9E4C` |
| `$10/$14/$18` | L each | X, lane/depth, and vertical position | movement helpers |
| `$1C/$20/$24` | L each | X, lane, and vertical velocity | `$973E`, `$9F96`, `$A00E` |
| `$30` | W | Primary state (`$0100`, `$0300`...) | reaction paths |
| `$31` | B | Fine-grained reaction/physics flags | `$991A-$9D16` |
| `$32` | W | Health/energy | `$93CE`, `$9BC6`, `$A13A` |
| `$33` | B | Type/variant combat statistic; mirrored at `$38` | `$93CE` |
| `$34` | B | Contact/attack damage | `$93CE`, damage consumers |
| `$37` | B | Hit/throw/death flags | shared collision paths |
| `$39` | B | Score/palette/accounting selector | `$93B4`, `$9E26` |
| `$3E` | W | Current collision/attacker object pointer | `$969E`, `$9BC6` |
| `$40-$41` | B/B | Spawn/variant metadata | `$945A`, visibility gates |
| `$42` | W | Current target player pointer | `$96EC` |
| `$48-$4B` | B each | status, damage/reaction and substate scratch | reaction dispatchers |
| `$50-$51` | B/B | General action timer/substate scratch | many states |
| `$60/$62` | W/W | Desired X/lane point for scripted approach | `$9604-$9682` |
| `$66/$68` | W/W | Type-derived approach offsets | `$945A` |

Several offsets are polymorphic by state and type; the table lists only uses demonstrated across the common subsystem.

## Type and variant data

`$00938C` performs four data-driven steps:

1. `$00945A` splits spawn byte `$41` into two nibbles and maps each through `$9484`, producing signed/unsigned approach offsets at `$68/$66`.
2. `$0093CE` indexes six-byte records at `$26FCE` by `type_index*6 + variant`. It loads `$33`, copies it to `$38`, and loads attack damage `$34`. On highest difficulty both receive `+4`.
3. `$0093B4` indexes `$27010` to choose `$39`, used by score/palette accounting.
4. `$009406` selects palette/tile base, while `$27032[type_index]` supplies the animation/behavior resource pointer.

Thus archetype differences are not eleven completely separate top-level functions. The shared state machinery is parameterized by type records, animation command streams, approach offsets, attack damage, palette and per-animation callbacks.

| Type range | Classification | Proven differentiation |
|---|---|---|
| `$20-$2A` | Ordinary enemies | Eleven type records; type/variant stats, palette and animation-set pointer |
| `$30` | Abadede boss | Separate byte-state handler `$143D0` |
| `$55-$58` | Souther/Antonio/Bongo/Onihime-Yasha bosses | Separate boss-family handlers and target selectors |

Mapping each `$20-$2A` value to a retail name requires correlating ELC type, art/palette and framebuffer output; names should not be guessed from code order.

## Target selection

`$0096EC` is the common ordinary-enemy selector. It writes the player object pointer to `$42(a0)`:

- no active player: enter state `$0400` and set the relevant state flag;
- 1P: select the active player's object;
- 2P: compare absolute X distance and select the closer player.

```text
function ordinary_enemy_select_target(enemy):
    if no players active:
        enemy.state = $0400
        return none
    if only P1 active: target = P1
    else if only P2 active: target = P2
    else target = argmin(abs(P1.x-enemy.x), abs(P2.x-enemy.x))
    enemy.target_ptr = target
```

There is no global threat table. Targeting is nearest-X and can be recalculated by behavior states. Boss selectors at `$129F8`, `$15946`, `$16294`, `$16D40`, and `$1753A` are separate and often add pair-role, facing or lane biases.

## Navigation and spacing

`$009604-$009682` moves toward desired X/lane coordinates stored at `$60/$62`. `$009648/$009654` derive those coordinates from the target and type-derived offsets `$66/$68`, reflecting offsets at lane boundaries. `$00982C` converts a vector to fixed-point X/lane velocity using the direction table at `$2705E`; on Easy, high speed values are reduced.

`$0098E8` computes an inexpensive distance metric:

```text
major = max(abs(dx), abs(dlane))
minor = min(abs(dx), abs(dlane))
distance ~= 3/8 * major + minor
```

Movement is constrained by collision and arena helpers:

- `$009F96` advances X but rejects stage bounds (with special bounds for rounds 7/8);
- `$00A00E` advances lane and constrains it normally to `$02-$70`, with a wider round-7 special case;
- `$009E68` probes ground/obstacles, resolves small side steps and transitions to state `$0700` when blocked;
- `$009F22/$009F56/$009F6A/$009FE6` perform forward/side obstruction checks before allowing movement.

This is steering plus collision probes, not graph search. Enemies approach a target-relative point, stop or sidestep when probes fail, and let animation/state callbacks decide when to attack or retreat.

## Attack decisions, difficulty, and randomness

The common engine divides responsibility:

- distance/target helpers decide whether the enemy can reach its desired point;
- animation command streams and type-selected callbacks decide exact attack phases;
- contact routine `$AA22` reports interaction kind in `d7`;
- reaction dispatch uses flags/substate at `$31/$4A/$4B/$50`.

Randomness is explicit rather than ambient. `$0104D8` is used in reaction/landing paths, for example choosing a short recovery animation delay (`(rng & 3)*2+3` at `$9A32`). The code observed here does not support a claim that every attack is probability-driven.

Proven difficulty effects are:

- spawn filtering by low two bits of ELC metadata in `object_manager_loop`;
- `$93CE` adds four to ordinary-enemy combat values `$33/$34` on the highest difficulty;
- `$982C` reduces large movement speed on Easy;
- type/variant tables may already encode different baseline health/damage.

## Collision, reactions, grabs, and death

`$00991A` initializes a generic hit/knockback reaction, clears attack damage, selects facing from the attacker, and dispatches by reaction subtype `$4A`. `$0099A2` advances airborne physics and landing, using `$973E` for vertical motion and `$9F22` for obstacle response.

`$009B88` is a common contact-damage/stun path. It obtains the attacker through `$3E`, subtracts attacker damage `$34` from health `$32`, and chooses:

- continue timed stun;
- `$0300` for damaging/airborne reaction or lethal transition;
- `$0500` when the collision result indicates a held/grabbed condition;
- `$0400` for scripted removal/control cases.

`$009C50` handles another airborne/grab reaction, including vertical launch and collision tests. `$00A04A` dispatches responses from the interacting player's `$7D` state. `$00A0C2` positions an enemy relative to a holding/throwing player and selects facing/animation.

Death accounting is centralized:

- `$00950E` and `$009566` can force all ordinary enemies into scripted death/removal states;
- `$0097E6/$00997E` detect offscreen/fall deaths and select sounds;
- `$009DC0` decrements palette/enemy counters;
- `$009E26` awards score to P1 or P2 using `$39`;
- `$009E3E` clears all 128 bytes of the object.

## Group behavior

The ordinary subsystem has no proven formation controller. Its group-level behavior comes from:

- independent nearest-X target choice in 2P;
- collision avoidance and obstacle probes;
- shared palette and active-enemy counters;
- player interaction bytes `$7C/$7D`, which prevent incompatible simultaneous grab/contact states;
- ELC timing and difficulty filters, which control when a group enters play.

The explicit same-type pair roles at boss helper `$17F2E` belong to types `$55-$58` and must not be generalized to ordinary enemies.

## Shared infrastructure and boss boundary

Ordinary enemies and bosses share the 128-byte object format, fixed-point position/velocity, animation engine, collision routine `$AA22`, RNG `$104D8`, player interaction bytes, and some generic physics helpers. They do **not** share one tactical dispatcher.

Boss type `$30` and types `$55-$58` use byte-sized primary/tactical states, bespoke target selectors, pair/link metadata and multi-object attack choreography. Their code is useful for understanding collision/grab conventions, but it is not evidence for ordinary-enemy archetypes.

## Confidence and open questions

High confidence (95-100%): ordinary type range `$20-$2A`; boss correction for `$30/$55-$58`; target pointer `$42`; primary word states; type/variant initialization; movement vector conversion; arena constraints; common damage/death flow.

Medium confidence (75-90%): precise semantic names for `$33/$38/$39`; exact division between animation-script decisions and native behavior callbacks; interpretation of every `$31` reaction bit.

Open questions:

1. Map ordinary types `$20-$2A` to retail names using ELC, art and framebuffer evidence.
2. Name every behavior callback reachable from the eleven pointers at `$27032`.
3. Fully enumerate collision result `d7` from `$AA22`.
4. Separate health from other type-specific combat values in the `$26FCE` records with runtime traces.
5. Determine how often active states recalculate `$42` in 2P and whether particular archetypes deliberately retain a farther target.

## Analysis-data update ledger

These duplicate-checked entries were integrated into the shared CSV files.

### `labels.csv`

```csv
00009350, is_nonordinary_enemy_type, "100% - Returns zero only for ordinary enemy object types $20-$2A accepted by the common enemy subsystem"
0000937A, ordinary_enemy_activate, "95% - Activates an on-screen ordinary enemy, initializes type/variant data, animation resources and common AI state"
0000938C, ordinary_enemy_init_type_data, "100% - Initializes type $20-$2A offsets, combat values, palette/tile base and animation-set pointer from ROM tables"
000093CE, ordinary_enemy_init_combat_values, "95% - Loads type/variant combat bytes into object+$33/+$38/+$34; highest difficulty adds four"
00009604, ordinary_enemy_approach_point, "100% - Moves toward desired X/lane words at object+$60/+$62 using type speed and vector conversion"
000096EC, ordinary_enemy_select_target, "100% - Selects nearest active player by X in 2P and stores target object pointer at +$42; handles no-player state"
0000982C, ordinary_enemy_vector_to_velocity, "100% - Converts target vector and speed d6 into fixed-point X/lane velocity using direction table $2705E; Easy reduces high speed"
000098E8, ordinary_enemy_distance_metric, "100% - Computes approximate target distance as 3/8 of the major axis plus the minor axis"
0000991A, ordinary_enemy_begin_hit_reaction, "95% - Initializes common hit/knockback state, clears attack damage and dispatches reaction subtype"
000099A2, ordinary_enemy_update_airborne_reaction, "95% - Updates knockback/airborne physics, landing, obstacle response and death transition"
00009B88, ordinary_enemy_apply_contact_damage, "95% - Applies attacker damage to ordinary-enemy health and selects stun, grab, lethal or scripted state"
00009DC0, ordinary_enemy_release_accounting, "95% - Releases active-enemy palette/variant counters when an ordinary enemy is removed"
00009E26, ordinary_enemy_award_score, "95% - Awards defeated-enemy score using object+$39 to the player indicated by bit7"
00009E3E, clear_object_128, "100% - Clears all 128 bytes of the current object"
00009E68, ordinary_enemy_move_with_collision, "95% - Integrates ordinary-enemy movement with ground/obstacle probes and blocked-state transition"
00009F96, ordinary_enemy_advance_x_bounded, "100% - Advances X velocity subject to level-specific horizontal bounds and reports blockage in d5 bit0"
0000A00E, ordinary_enemy_advance_lane_bounded, "100% - Advances lane velocity subject to normal or round-7 lane bounds and reports blockage in d5 bit1"
```

### `addresses.csv`

No new absolute RAM symbol is necessary. The important fields are offsets in each `$80`-byte object, and adding first-slot aliases would misleadingly imply that only `$FFB900` carries them. The existing `object_table` entry should instead be corrected to:

```csv
FFB900, object_table, "100% - Start of 32-slot, $80-byte gameplay object table; ordinary enemies use types $20-$2A, with type at +$00, primary state W at +$30, health W at +$32 and target pointer W at +$42"
```
