# Enemy and Boss Artificial Intelligence

## Scope, correction, and evidence

The first part of this document covers the ordinary-enemy engine centered on
`$00937A-$00A43D`; the second part covers boss architecture and every
round-specific encounter. A cross-check against decoded enemy-load-cue (ELC)
placement data establishes that object types `$55-$58` are **boss families**,
not ordinary enemies:

| Type | Boss family | Confirming rounds | Handler |
|---:|---|---|---:|
| `$55` | Souther | 2, 6, 8 | `$15E70 (souther_update)` |
| `$56` | Antonio | 1, 8 | `$16CE4 (antonio_update)` |
| `$57` | Bongo | 4, 6, 8 | `$174E0 (bongo_update)` |
| `$58` | Onihime/Yasha | 5, 8 | `$158C4 (onihime_yasha_update)` |
| `$30` | Abadede | 3, 8 | `$143D0 (abadede_update)` |

The ordinary-enemy part discusses those handlers only at the shared-infrastructure
boundary; the boss part later covers them directly. Earlier inference from their
sophisticated target selection was insufficient to classify them as ordinary
enemies; ELC placement is decisive.

The common ordinary-type dispatch range is the contiguous `$20-$2A`. `$9350 (is_nonordinary_enemy_type)` subtracts `$20` and accepts exactly eleven values, and the palette/metadata pass at `$810 (prepare_next_spawn_section)` applies the same bounds. Only `$20-$27` and `$2A` are complete combatants: `$28` is Jack's axe/torch projectile helper, while `$29` is the police-special enemy-sweep controller. Their zero health records and `$29`'s null animation pointer are therefore intentional. The ordinary subsystem is more data-driven than the later boss handlers: type and variant select health, damage, palette, animation, and behavior tables around `$026FCE-$027032`.

## Ordinary-enemy lifecycle

```text
spawn type $20-$2A
    -> wait until active/on screen ($937A / $A59C)
    -> initialize type+variant tables ($938C)
    -> choose active player target ($96EC)
    -> primary state $0100: normal behavior
    -> sufficient damage may enter $0300 knockdown/airborne; grabs use $0500
    -> blocked movement/contact may enter $0700
    -> health/death enters $0600
    -> remove object, release palette/active-enemy accounting
```

`$937A (ordinary_enemy_activate)` is the activation entry. Once the object is eligible, `$938C (ordinary_enemy_init_type_data)` derives `type_index=type-$20`, initializes combat/animation metadata, selects a target, and enters the normal state.

Primary state is a **word** at object offset `$30`, normally encoded in `$0100` increments. This differs from the byte-sized state conventions in several boss families.

## Object layout used by ordinary enemies

Objects are 128 bytes in the table beginning at `$FFB900 (object_table)`.

| Offset | Width | Meaning | Evidence |
|---:|---:|---|---|
| `$00` | B | Type `$20-$2A` | `$9350 (is_nonordinary_enemy_type)`, `$938C (ordinary_enemy_init_type_data)` |
| `$01` | B | Visibility, collision and airborne flags | hit/death paths |
| `$04` | L | Animation-set pointer | selected from `$27032 (ordinary_enemy_animation_set_pointer_table)` |
| `$08` | W | Animation/action index | `$969E/$96C0` |
| `$09` | B | Facing flags; bit 1 is left/right | `$96C0`, `$9E4C` |
| `$10/$14/$18` | L each | X, lane/depth, and vertical position | movement helpers |
| `$1C/$20/$24` | L each | X, lane, and vertical velocity | `$973E`, `$9F96 (ordinary_enemy_advance_x_bounded)`, `$A00E (ordinary_enemy_advance_lane_bounded)` |
| `$30` | W | Primary state (`$0100`, `$0300`...) | reaction paths |
| `$31` | B | Fine-grained reaction/physics flags | `$991A-$9D16` |
| `$32` | W | Health/energy | `$93CE (ordinary_enemy_init_combat_values)`, `$9BC6`, `$A13A` |
| `$33` | B | Low byte of current health word `$32`; initialized by type/variant and mirrored at `$38` | `$93CE (ordinary_enemy_init_combat_values)`, word damage subtraction at `$9BC6/$A13A` |
| `$34` | B | Contact/attack damage | `$93CE (ordinary_enemy_init_combat_values)`, damage consumers |
| `$37` | B | Hit/throw/death flags | shared collision paths |
| `$39` | B | Score/palette/accounting selector | `$93B4`, `$9E26 (ordinary_enemy_award_score)` |
| `$3E` | W | Current collision/attacker object pointer | `$969E`, `$9BC6` |
| `$40-$41` | B/B | Spawn/variant metadata | `$945A`, visibility gates |
| `$42` | W | Current target player pointer | `$96EC (ordinary_enemy_select_target)` |
| `$48-$4B` | B each | status, damage/reaction and substate scratch | reaction dispatchers |
| `$50-$51` | B/B | General action timer/substate scratch | many states |
| `$60/$62` | W/W | Desired X/lane point for scripted approach | `$9604-$9682` |
| `$66/$68` | W/W | Type-derived approach offsets | `$945A` |

Several offsets are polymorphic by state and type; the table lists only uses demonstrated across the common subsystem.

## Type and variant data

`$938C (ordinary_enemy_init_type_data)` performs four data-driven steps:

1. `$00945A` splits spawn byte `$41` into two nibbles and maps each through `$9484`, producing signed/unsigned approach offsets at `$68/$66`.
2. `$93CE (ordinary_enemy_init_combat_values)` indexes six-byte records at `$26FCE (ordinary_enemy_combat_value_table)` by `type_index*6 + variant`. Bytes 0-2 are initial health for variants 0-2; bytes 3-5 are their attack damage. It writes health to the low byte `$33` of word `$32`, mirrors it at `$38`, and writes damage to `$34`. On highest difficulty health and damage both receive `+4`.
3. `$0093B4` indexes `$27010 (ordinary_enemy_accounting_selector_table)` to choose `$39`, used by score/palette accounting.
4. `$009406` selects palette/tile base, while `$27032[type_index]` supplies the animation/behavior resource pointer.

Thus archetype differences are not eleven completely separate top-level functions. The shared state machinery is parameterized by type records, animation command streams, approach offsets, attack damage, palette and per-animation callbacks.

| Type range | Classification | Proven differentiation |
|---|---|---|
| `$20-$27`, `$2A` | Normal ordinary-enemy entries | Nine complete type records; type/variant health, damage, palette and animation-set pointer |
| `$28/$29` | Dedicated helper slots inside the accepted range | `$28` is Jack's thrown projectile; `$29` is the police-special ordinary-enemy sweep controller |
| `$30` | Abadede boss | Separate byte-state handler `$143D0 (abadede_update)` |
| `$55-$58` | Souther/Antonio/Bongo/Onihime-Yasha bosses | Separate boss-family handlers and target selectors |

The ROM reduces the eleven internal types to five visual families through the
byte table at `$A4E (ordinary_enemy_art_family_table)`. Each family selects one Nemesis stream; three art-cue
IDs then place that same stream in one of three resident VRAM slots. Rendering
those five streams with `tools/decompress.py` and visually classifying the
result gives this mapping:

| Internal type(s) | Art-family byte | Nemesis stream | Visual identity | Distinguishing behaviour |
|---|---:|---:|---|---|
| `$20-$23` | `$01` | `$20172 (garcia_nemesis_art)` | Garcia | Most common basic enemy |
| `$24` | `$02` | `$21708 (signal_nemesis_art)` | Signal | Sliding attacks; gets behind and throws the player |
| `$25`, `$2A` | `$03` | `$22BFE (haku_ro_nemesis_art)` | Haku-Ro | Highly mobile ninja |
| `$26` | `$04` | `$245E0 (nora_nemesis_art)` | Nora | Whip attacks; some variants feign injury before resuming combat |
| `$27`, helper `$28` | `$05` (`$85` for `$28`) | `$258F8 (jack_nemesis_art)` | Jack plus thrown axe/torch art | Jack juggles the projectile and may stop and throw it |

This is a **visual-family mapping**, not proof that every internal ID in a
shared row behaves identically. The type tables make the differences explicit:

| Type | Identity | Animation set | Variant health bytes | Variant damage bytes |
|---:|---|---:|---|---|
| `$20` | Garcia | `$1FC70 (garcia_animation_set)` | `$06,$09,$0B` | `$08,$08,$08` |
| `$21` | Garcia | `$1FC70 (garcia_animation_set)` | `$04,$07,$09` | `$04,$08,$08` |
| `$22` | Garcia | `$1FC70 (garcia_animation_set)` | `$04,$07,$09` | `$04,$08,$08` |
| `$23` | Garcia | `$1FC70 (garcia_animation_set)` | `$06,$09,$0B` | `$0C,$0C,$0C` |
| `$24` | Signal | `$22948 (signal_animation_set)` | `$04,$07,$09` | `$08,$08,$0C` |
| `$25` | Haku-Ro | `$2402C (haku_ro_animation_set)` | `$04,$07,$09` | `$0C,$0C,$10` |
| `$26` | Nora | `$242F8 (nora_animation_set)` | `$07,$0B,$0E` | `$08,$08,$08` |
| `$27` | Jack | `$2556C (jack_animation_set)` | `$09,$0E,$11` | `$0C,$10,$14` |
| `$28` | Jack axe/torch projectile helper | `$2556C (jack_animation_set)` | `$00,$00,$00` | `$0C,$10,$14` |
| `$29` | Police-special sweep controller | null | `$00,$00,$00` | `$00,$00,$00` |
| `$2A` | Haku-Ro | `$2402C (haku_ro_animation_set)` | `$07,$0B,$0E` | `$0C,$0C,$10` |

The exact duplicate animation pointers prove that `$20-$23` share the Garcia
animation resource, `$25/$2A` share Haku-Ro, and Jack's type `$27` plus its
type-`$28` projectile share the Jack resource. `$FC1C` creates type `$28` from
Jack, copies its target and damage, and the `$FC66-$FE3E` path launches,
collides, returns, or removes the axe/torch helper. Type `$29` is created by
`$9566 (prepare_ordinary_enemies_for_police_special)` and dispatches directly
to `$100B6 (police_special_enemy_sweep_update)`, so it needs neither the normal
animation pointer nor combat values. Neither helper belongs in regular ELC
waves.

The high bit on types `$28/$29` has engine meaning: `$810
(prepare_next_spawn_section)` excludes those entries from the ordinary
palette-family count, while `$9406` strips the bit when selecting the art
family and uses its fixed residency path.

Direct Nemesis decoding of all eight ELC streams gives a useful constraint on
that future mapping. The regular wave blocks contain these ordinary type IDs
after stripping the two-player qualifier bit:

| Round | Ordinary types present in regular blocks |
|---:|---|
| 1 | `$20,$21,$22,$23,$24,$25,$26` |
| 2 | `$20,$21,$22,$23,$24,$25,$26,$27` |
| 3 | `$20,$21,$22,$23,$24,$25,$26,$2A` |
| 4 | `$20,$21,$22,$23,$24,$25,$26,$27` |
| 5 | `$20,$21,$22,$23,$24,$25,$26,$27,$2A` |
| 6 | `$20,$21,$22,$23,$24,$25,$26,$27` |
| 7 | `$20,$21,$22,$23,$24,$25,$26` |
| 8 | `$20,$21,$22,$23,$24,$25,$26,$27,$2A` |

Types `$28/$29` do not occur in any regular ELC wave block in this ROM;
`$2A` occurs only in Rounds 3, 5, and 8. This is a ROM-distribution fact, not a
retail-name assignment, and it avoids treating all eleven table slots as
equally used by the level scripts.

## Target selection

`$96EC (ordinary_enemy_select_target)` is the common ordinary-enemy selector. It writes the player object pointer to `$42(a0)`:

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

There is no global threat table. Targeting is nearest-X and can be recalculated by behavior states. Boss selectors at `$129F8`, `$15946 (onihime_yasha_select_target)`, `$16294 (souther_select_target)`, `$16D40 (antonio_select_target)`, and `$1753A (bongo_select_target)` are separate and often add pair-role, facing or lane biases.

## Navigation and spacing

`$009604-$009682` moves toward desired X/lane coordinates stored at `$60/$62`. `$009648/$009654` derive those coordinates from the target and type-derived offsets `$66/$68`, reflecting offsets at lane boundaries. `$982C (ordinary_enemy_vector_to_velocity)` converts a vector to fixed-point X/lane velocity using the direction table at `$2705E`; on Easy, high speed values are reduced.

`$98E8 (ordinary_enemy_distance_metric)` computes an inexpensive distance metric:

```text
major = max(abs(dx), abs(dlane))
minor = min(abs(dx), abs(dlane))
distance ~= 3/8 * major + minor
```

Movement is constrained by collision and arena helpers:

- `$9F96 (ordinary_enemy_advance_x_bounded)` advances X but rejects stage bounds (with special bounds for rounds 7/8);
- `$A00E (ordinary_enemy_advance_lane_bounded)` advances lane and constrains it normally to `$02-$70`, with a wider round-7 special case;
- `$9E68 (ordinary_enemy_move_with_collision)` probes ground/obstacles, resolves small side steps and transitions to state `$0700` when blocked;
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

- spawn filtering by low two bits of ELC metadata in `$784 (process_timed_spawn_records)`;
- `$93CE (ordinary_enemy_init_combat_values)` adds four to initial health `$33` and attack damage `$34` on the highest difficulty;
- `$982C (ordinary_enemy_vector_to_velocity)` reduces large movement speed on Easy;
- type/variant tables may already encode different baseline health/damage.

## Collision, reactions, grabs, and death

`$991A (ordinary_enemy_begin_knockdown)` starts the knockdown/airborne fall after the enemy has taken sufficient damage; it is not the generic reaction to every hit. It clears attack damage, selects facing from the attacker, and dispatches by fall subtype `$4A`. `$99A2 (ordinary_enemy_update_airborne_reaction)` advances airborne physics and landing, using `$973E` for vertical motion and `$9F22` for obstacle response.

`$9B88 (ordinary_enemy_apply_contact_damage)` is a common contact-damage/stun path. It obtains the attacker through `$3E`, subtracts attacker damage `$34` from health `$32`, and chooses:

- continue timed stun;
- `$0300` for damaging/airborne reaction or lethal transition;
- `$0500` when the collision result indicates a held/grabbed condition;
- `$0400` for scripted removal/control cases.

`$009C50` handles another airborne/grab reaction, including vertical launch and collision tests. `$00A04A` dispatches responses from the interacting player's `$7D` state. `$00A0C2` positions an enemy relative to a holding/throwing player and selects facing/animation.

Death accounting is centralized:

- `$00950E` and `$9566 (prepare_ordinary_enemies_for_police_special)` can force all ordinary enemies into scripted death/removal states;
- `$0097E6/$00997E` detect offscreen/fall deaths and select sounds;
- `$9DC0 (ordinary_enemy_release_accounting)` decrements palette/enemy counters;
- `$9E26 (ordinary_enemy_award_score)` awards score to P1 or P2 using `$39`;
- `$9E3E (clear_object_128)` clears all 128 bytes of the object.

## Group behavior

The ordinary subsystem has no proven formation controller. Its group-level behavior comes from:

- independent nearest-X target choice in 2P;
- collision avoidance and obstacle probes;
- shared palette and active-enemy counters;
- player interaction bytes `$7C/$7D`, which prevent incompatible simultaneous grab/contact states;
- ELC timing and difficulty filters, which control when a group enters play.

The explicit same-type pair roles at boss helper `$17F2E (boss_link_same_type_pair)` belong to types `$55-$58` and must not be generalized to ordinary enemies.

## Shared infrastructure and boss boundary

Ordinary enemies and bosses share the 128-byte object format, fixed-point position/velocity, animation engine, collision routine `$AA22`, RNG `$104D8`, player interaction bytes, and some generic physics helpers. They do **not** share one tactical dispatcher.

Boss type `$30` and types `$55-$58` use byte-sized primary/tactical states, bespoke target selectors, pair/link metadata and multi-object attack choreography. Their code is useful for understanding collision/grab conventions, but it is not evidence for ordinary-enemy archetypes.

## Confidence and open questions

The code labels for the ordinary-enemy entries audited in this manuscript are
100% confirmed for their bounded contracts: activation and combat-table load,
hit/airborne/contact-damage transitions, palette/progression release, score
award, and collision-bounded movement. This follows the complete producer and
consumer chains for the stated fields and does not depend on assigning retail
names to types `$20-$2A`.

Medium confidence (75-90%): later purpose of the initial-health mirror `$38` and precise accounting meaning of `$39`; exact division between animation-script decisions and native behavior callbacks; interpretation of every `$31` reaction bit.

Open questions:

1. Separate Garcia types `$20-$23` and Haku-Ro types `$25/$2A` into exact behavioural variants.
2. Name every behavior callback reachable from the eleven pointers at `$27032 (ordinary_enemy_animation_set_pointer_table)`.
3. Fully enumerate collision result `d7` from `$AA22`.
4. Determine the later purpose, if any, of the initial-health mirror at object byte `$38`.
5. Determine how often active states recalculate `$42` in 2P and whether particular archetypes deliberately retain a farther target.

## Ordinary-enemy analysis-data update ledger

These duplicate-checked entries were integrated into the shared CSV files.

### `labels.csv`

```csv
00009350, is_nonordinary_enemy_type, "100% - Returns zero only for ordinary enemy object types $20-$2A accepted by the common enemy subsystem"
0000937A, ordinary_enemy_activate, "100% - Activates an on-screen ordinary enemy, initializes type/variant data, animation resources and common AI state"
0000938C, ordinary_enemy_init_type_data, "100% - Initializes type $20-$2A offsets, health/damage, palette/tile base and animation-set pointer from ROM tables"
000093CE, ordinary_enemy_init_combat_values, "100% - Loads type/variant initial health into object+$33/$38 and attack damage into +$34; highest difficulty adds four"
00009604, ordinary_enemy_approach_point, "100% - Moves toward desired X/lane words at object+$60/+$62 using type speed and vector conversion"
000096EC, ordinary_enemy_select_target, "100% - Selects nearest active player by X in 2P and stores target object pointer at +$42; handles no-player state"
0000982C, ordinary_enemy_vector_to_velocity, "100% - Converts target vector and speed d6 into fixed-point X/lane velocity using direction table $2705E; Easy reduces high speed"
000098E8, ordinary_enemy_distance_metric, "100% - Computes approximate target distance as 3/8 of the major axis plus the minor axis"
0000991A, ordinary_enemy_begin_knockdown, "100% - Starts the knockdown/airborne fall after sufficient damage, clears attack damage and dispatches the fall subtype"
000099A2, ordinary_enemy_update_airborne_reaction, "100% - Updates knockback/airborne physics, landing, obstacle response and death transition"
00009B88, ordinary_enemy_apply_contact_damage, "100% - Applies attacker damage to ordinary-enemy health and selects stun, grab, lethal or scripted state"
00009DC0, ordinary_enemy_release_accounting, "100% - Releases active-enemy palette/variant counters when an ordinary enemy is removed"
00009E26, ordinary_enemy_award_score, "100% - Awards defeated-enemy score using object+$39 to the player indicated by bit7"
00009E3E, clear_object_128, "100% - Clears all 128 bytes of the current object"
00009E68, ordinary_enemy_move_with_collision, "100% - Integrates ordinary-enemy movement with ground/obstacle probes and blocked-state transition"
00009F96, ordinary_enemy_advance_x_bounded, "100% - Advances X velocity subject to level-specific horizontal bounds and reports blockage in d5 bit0"
0000A00E, ordinary_enemy_advance_lane_bounded, "100% - Advances lane velocity subject to normal or round-7 lane bounds and reports blockage in d5 bit1"
```

### `addresses.csv`

No new absolute RAM symbol is necessary. The important fields are offsets in each `$80`-byte object, and adding first-slot aliases would misleadingly imply that only `$FFB900 (object_table)` carries them. The existing `$FFB900 (object_table)` entry should instead be corrected to:

```csv
FFB900, object_table, "100% - Start of 32-slot, $80-byte gameplay object table; ordinary enemies use types $20-$2A, with type at +$00, primary state W at +$30, health W at +$32 and target pointer W at +$42"
```

---

## Boss Architecture and Round-by-Round Encounters

### Scope and method

This document describes the Mega Drive game's boss implementation: how an
encounter is introduced by the level engine, which object types implement each
retail boss, how the state machines select targets and attacks, how damage and
death work, and how the result reaches stage-clear logic.

The primary evidence is `output/sor.asm`. The object-type mapping was checked
against the Nemesis-decoded enemy-load-cue (ELC) streams for all eight rounds,
not guessed from handler shape alone. In particular, the distinctive sections
at the end of the streams establish this sequence:

```text
round 1: type $56
round 2: type $55
round 3: type $30
round 4: type $57
round 5: type $58 pair
round 6: type $57 encounter, then type $55 pair
round 7: no terminal boss family
round 8: $56 -> $55 -> $30 -> $57 -> $58 -> Mr. X scene/fight
```

That sequence matches Antonio, Souther, Abadede, Bongo, and Onihime/Yasha.
Retail names are used only as identity labels; all implementation claims below
come from the ROM and the decoded ELC placement sequence.

### Executive summary

There is no single `Boss` class. There are three related implementation
strata:

1. **Abadede and Mr. X use older bespoke objects.** Abadede is type `$30`,
   dispatched to `$143D0 (abadede_update)`, and owns helper type `$31`. Mr. X uses the older
   `$1306A-$13EBC` subsystem (type `$35` by the dispatcher table); its terminal
   initialization at `$13E4C (mr_x_final_encounter_init)` explicitly registers the final encounter with
   the HUD/stage-clear system.
2. **Antonio, Souther, Bongo, and the twins share a later boss framework.**
   Types `$55-$58` have separate tactical state tables, but share target,
   movement, collision, damage, pairing, and death helpers in
   `$17924-$17F9C`.
3. **The level engine remains authoritative over progression.** Boss objects
   do not load the next round themselves. The ELC pipeline locks the arena,
   loads the required art, spawns the boss, and waits for encounter state to
   drain. Boss death updates counters or final-HUD pointers;
   `$117FC (stage_clear_monitor)` converts the resulting late-phase condition into
   `$FFFA73 (end_of_level_flag)`.

The types `$20-$2A` are ordinary/auxiliary objects from an earlier enemy
framework. Their common state tables and tracked-entity count matter to the
level pipeline, but they must not be confused with the retail bosses merely
because they use the same health offset and occur in late waves.

### Object dispatch and boss identity

The global object dispatcher at `$AD8E (update_objects_and_build_sprites)` indexes the word table at `$B236`.
Several entries are trampolines because the real handler lies outside the
signed 16-bit address range.

| Retail boss | Object type | Top-level update | Shared family | Important helper objects |
|---|---:|---:|---|---|
| Antonio | `$56` | `$16CE4 (antonio_update)` | later boss framework | `$96` linked boomerang/attack object |
| Souther | `$55` | `$15E70 (souther_update)` | later boss framework | `$98/$99` linked claw/afterimage attack objects |
| Abadede | `$30` | `$143D0 (abadede_update)` | bespoke older framework | `$31` linked body/attack component; `$39` conditional effect |
| Bongo | `$57` | `$174E0 (bongo_update)` | later boss framework | `$97` linked flame/attack object |
| Onihime/Yasha | `$58` | `$158C4 (onihime_yasha_update)` | later boss framework | pairing metadata in the two boss objects |
| Mr. X | `$35` | `$1306A (mr_x_boss_update)` | bespoke final-boss framework | attack/effect objects in the `$33-$38` family |

The type-to-name mapping is now 100% both as a sequence and for each individual
retail label. It is overdetermined by independent evidence: ELC round placement
and Round-8 repeat order; type `$56`'s linked thrown/caught object `$96`;
type `$55`'s claw/afterimage objects `$98/$99`; type `$57`'s linked flame object
`$97`; and type `$58`'s same-type paired grab/throw choreography. Abadede's
type `$30` is fixed by Round 3/8 placement, the charge state machine, and the
same-type scan at `$14486`. Mr. X's type `$35` is likewise 100%: the object-type
dispatcher selects `$1306A (mr_x_boss_update)`, its office/final-fight state
machine uses the Mr. X offer globals, and its terminal path registers itself in
the unique final-stage HUD/completion slots. A direct ELC body record is not
required for that identification.

### How a boss encounter starts

#### ELC records, resource residency, and the late phase

At round initialization `$E5C (start_round_setup)` Nemesis-decompresses the selected ELC stream to
`$FF6800 (elc_buffer)`. The level pipeline consumes its six-byte entity records, filters them
by difficulty and player count, and loads art before materializing an object.
Bosses therefore use the same data-driven entrance mechanism as other
encounters; they are not hard-coded at the end of every round.

The later-boss records are especially clear because one-player and two-player
variants are adjacent. For example, the Round 1 terminal section contains:

```text
$56 00 04 00 10 00    ; one-player Antonio record
$D6 01 05 00 10 01    ; type $56 with bit 7 set: 2P-qualified extra/variant
$99                   ; section terminator
```

The Round 8 stream repeats the same structural pattern for `$56`, `$55`, `$30`,
`$57`, and `$58`. Bit 7 is removed by the loader and acts only as a two-player
qualifier. Bytes copied to object `+$40`, `+$41`, and `+$49` choose partner
roles, palette/stat variants, and encounter-specific behavior.

When the pipeline reaches the late phase, `$FFFA05 (level_spawn_flow_flags)` bit 6 is set. This has
several effects:

- `$11B12 (play_level_music)` selects boss music (`$87`, or `$90` in Round 8);
- camera progression stops opening new corridors;
- boss initializers register HUD pointers through `$F502/$F508`;
- the level pipeline changes from spawning/scanning to waiting for completion;
- `$117FC (stage_clear_monitor)` begins considering the stage clear condition.

#### Arena and camera locking

The camera is constrained by the two X boundaries at `$FFE01A (camera_x_max)` (maximum) and
`$FFE01E (camera_x_min)` (minimum). Normal waves open one side of the corridor through
`$19570 (advance_wave_camera_boundary)`; the transition state at `$6A6 (update_camera_scroll_if_needed)` waits until the camera has reached the
new bound before normalizing active entities. A boss arena is therefore a
camera-boundary condition plus a spawn phase, not a rectangle owned by the
boss object.

This distinction explains why recurring bosses can appear mid-round. Round 6
can introduce Bongo and later the two Southers without either handler knowing
the round's map layout. Round 8 can run a whole boss-rush sequence in one fixed
office corridor by advancing ELC sections while retaining the late-phase arena.

### Shared later-boss framework (`$55-$58`)

#### Object layout

Types `$55-$58` are 128-byte objects in the common object table. The principal
fields are:

| Offset | Width | Meaning |
|---:|---:|---|
| `$00` | byte | object/boss type |
| `$08/$09` | word/byte | action/animation and facing |
| `$10/$14/$18` | long roots | X, lane/depth, and vertical position (16.16) |
| `$1C/$20/$24` | long | X, lane, and vertical velocity |
| `$30` | byte | primary state index |
| `$32` | word | current health |
| `$34` | byte | outgoing damage for the active contact |
| `$37` | byte | hit/reaction flags |
| `$40/$41` | byte | encounter variant and pairing metadata from ELC |
| `$4A` | byte | initialized base attack damage |
| `$4C` | long | ground/landing height |
| `$50/$52` | word | absolute X and lane distance to target |
| `$5D/$5E` | byte/word | pair role and partner object pointer |
| `$60/$61` | byte | signs of target X/lane deltas |
| `$64` | word | interaction/target-related object pointer |
| `$67` | byte | tactical substate |
| `$6C` | byte | pending received damage |
| `$70/$72` | word | attacker and selected-player pointers |
| `$77-$7B` | byte | target availability and family-specific counters |

#### Statistics and difficulty

`$17EDC (boss_init_combat_stats)` indexes four bytes by `type-$55`: one base-damage table at `$17F26`
and one health table at `$17F2A`. Difficulty transforms them as follows:

| Boss type | Base damage | Base health |
|---:|---:|---:|
| `$55` Souther | `$14` | `$20` |
| `$56` Antonio | `$14` | `$18` |
| `$57` Bongo | `$20` | `$1E` |
| `$58` Onihime/Yasha | `$20` | `$20` |

```text
Easy:       damage /= 2
Normal:     base values
Hard:       damage *= 2
Hardest:    damage *= 2, health += 5

if not in boss/late phase: damage += 2
if level == Round 8 index and not late phase: health += 2
```

The last two conditions show that these types can be used outside their
canonical terminal fights. Stats are encounter-sensitive, not properties of a
retail name alone.

#### Targeting, movement, and attack commitment

Each family owns a top-level primary-state table, but delegates geometry to the
same helpers:

- `$179F8` rejects unavailable players;
- `$17A94/$17AF6` measure absolute X distance and side;
- `$17B0C/$17B2C` face and measure lane distance;
- `$17924-$179AC` convert signed deltas into stepped velocities;
- `$17AB8` integrates all axes, clamps the lane to `$00-$70`, and clamps height
  against the ground plane;
- `$17A5C` dispatches the tactical byte at `+$67`.

No boss uses pathfinding. The characteristic behavior comes from distance
windows, facing tests, animation phase, small counters, and occasional RNG from
`$104D8`.

```text
function update_later_boss(boss):
    consume_global_forced_reaction_if_any()
    target = family_select_target(active_players, pair_role)
    measure_x_and_lane_distance(target)
    process_pending_damage_and_interaction()

    switch boss.primary_state:
        case approach:
            choose tactical substate from distance/facing windows
        case attack:
            advance animation-synchronized hit or linked object
        case recovery:
            wait, retreat, or select another player
        case airborne_or_hit:
            apply shared physics and landing logic
        case death:
            blink, award score, unlink partner, remove object
```

#### Damage, vulnerability, and death

`$17C36 (boss_apply_pending_damage)` is the shared received-damage path. Collision code leaves damage in
`+$6C` and the attacker pointer in `+$70`; the routine subtracts it from health
`+$32`, clears movement, and chooses hitstun, knockback, or lethal reaction.
Attack states can temporarily suppress or redirect this path through flags and
interaction reservations, which is why some visible moves appear invulnerable
or counter jump attacks.

The airborne/bounce path at `$16400` returns a living boss to active AI. A
defeated boss proceeds to `$16512`, which counts down, blinks/removes the
sprite, awards score via `$16542`, clears its partner relationship through
`$17F9C (boss_unlink_pair)`, and finally clears the object slot.

Pairing is significant for Round 5/6/8. `$17F2E (boss_link_same_type_pair)` scans for another object of
the same type and writes reciprocal roles (`+$5D=1/2`) and partner pointers
(`+$5E`). Target selectors use those roles to split attention across P1/P2.
Death unlinks the survivor so it can return to unpaired target selection.

#### Police-special damage

Police attacks do not reach bosses through the ordinary type-`$29` sweep.
Antonio, Souther, Bongo, and Onihime/Yasha call `$16AEC
(later_boss_enter_police_special_reaction)` before their primary-state
dispatch. On the one-frame `$FFFA1B (police_special_start_pulse)`, the helper
clears the per-boss latch; while the event is active, each living boss enters
shared state `$0A` exactly once, records the calling player at `+$70`, sets a
20-step effect counter, and starts a delay of 300 updates for P1 or 390 for P2.

The state-table target `$16A60 (later_boss_police_special_reaction)` was absent
from the earlier static-disassembly entry list. Adding it proves the result:
after the caller-specific delay it subtracts exactly 10 from health `+$32` and
enters the normal knockdown path, or the normal lethal path when health falls
to zero. The 20 at `+$76` is an initial effect/animation countdown, not damage.

Abadede implements the same fixed 10 damage independently in `$143D0
(abadede_update)`: he latches the active event, waits for it to end, subtracts
10 once, preserves P1/P2 attribution in his lethal flags, and forces state 6.
Mr. X has no police-special reaction because normal Round-8 initialization
sets both player special counters to zero.

### Antonio (`$56`, `$16CE4 (antonio_update)`)

Antonio uses the family-C table rooted near `$16CF4`. Initialization at
`$16D0A` selects a player, initializes stats through `$17EDC (boss_init_combat_stats)`, loads animations
from `$2E8B4`, and discovers a same-type partner if the ELC supplied one.

The tactical code keeps wider spacing than the close-range bosses and selects
an attack when X is roughly `$28-$78` and lane separation is small. `$17206`
creates/maintains linked object `$96`; `$16C6E` positions that object from the
parent's animation phase and facing. The link is the code-side implementation
of the visible boomerang choreography: the attack object follows Antonio during
wind-up/catch phases and becomes independently active during the throw.

The target selector at `$16D40 (antonio_select_target)` has explicit pair-role thresholds in 2P. This
is used by the optional extra/variant record as well as by repeated Round 8
encounters; it is not evidence for a story-level second Antonio in every mode.

### Souther (`$55`, `$15E70 (souther_update)`)

Souther's selector at `$16294 (souther_select_target)` is the most elaborate of the four shared
families. It considers both players' action states, X/lane distance, facing,
pair role, and a target-hold counter. This supports the characteristic response
to a player who commits to a jump or approaches from a vulnerable side.

The handler creates linked types `$98/$99` at `$16BC6/$16C2E`. They are
animation-synchronized attack/afterimage objects rather than separately
tracked enemies. The claw sequence can reserve the target's interaction state,
advance through several contact phases, and either continue the slash or fall
back to recovery depending on collision result.

Round 6 deliberately supplies two Souther records. Pair roles split targeting
and reduce both bosses choosing the same player in 2P. The same logic makes a
single surviving Souther behave normally after its partner dies.

### Abadede (`$30`, `$143D0 (abadede_update)`)

Abadede predates the `$55-$58` framework. His state byte still lives at `+$30`
and health at `+$32`, but he dispatches through the relative state table near
`$14466` and uses target pointer `+$5C` rather than `+$72`.

Initialization at `$144E0 (abadede_init)`:

- clears a global coordination bit;
- creates linked type `$31` and stores it at `+$50`;
- conditionally creates type `$39` for a variant;
- loads the `$34B94` animation set;
- calls `$1456A (abadede_init_combat_stats)` for difficulty/variant health and damage;
- selects a player through `$129F8`;
- seeds strong X/lane velocities and faces the target.

The base `(health, damage)` pairs at `$145BC` are Easy `($20,$10)`, Normal
`($20,$20)`, Hard `($20,$40)`, and Hardest `($34,$40)`. `$1456A (abadede_init_combat_stats)` can then add
variant bonuses from ELC fields, so a repeated Abadede need not have exactly
the canonical Round 3 values.

The core behavior is a charge/clothesline cycle. `$1401E` flips velocity signs
toward the selected player, while `$14048` updates facing. Collision dispatcher
`$13ED8 (bespoke_boss_collision_dispatch)` routes contact outcomes: a clean hit marks the player interaction,
received attacks subtract the attacker's `+$34` from `+$32`, and lethal damage
selects state `$0E`.

Abadede also has explicit multi-instance coordination. `$14486` scans all
object slots for another type `$30`; if one is active outside selected reaction
states, `$FFFA53 (boss_forced_reaction_flags)` is used to coordinate forced transitions. This explains why
Round 8 and two-player variants do not reduce to two completely independent
charge loops.

### Bongo (`$57`, `$174E0 (bongo_update)`)

Bongo's family-D state machine circles in the lane, corrects screen-edge
position, and then commits to a multi-stage acceleration/charge. The attack
chain around `$176B4-$177E2` uses distance and phase counters rather than a
single random decision.

Linked type `$97`, created at `$1781E`, is positioned from Bongo's animation
and facing and implements the flame/contact portion of the attack. Parent and
linked object exchange animation-phase information so the hit region appears
only during the appropriate breath/charge frames.

The target selector `$1753A (bongo_select_target)` alternates players more aggressively than
Antonio's and uses pair roles to avoid duplicate targets. Round 6 uses Bongo as
a mid-round boss-strength encounter; Round 7 reuses the family in the elevator
gauntlet; Round 8 repeats it as the fourth boss-rush family.

### Onihime and Yasha (`$58`, `$158C4 (onihime_yasha_update)`)

The twins are two objects of the same type rather than a controller with two
hard-coded child actors. ELC metadata causes `$17F2E (boss_link_same_type_pair)` to pair them and assign
reciprocal roles. Their family-A selector at `$15946 (onihime_yasha_select_target)` normally chooses the
nearest usable player but uses the pair role to bias the two bosses apart.

Their state table contains close-range approach, rapid jump/airborne attacks,
and explicit player-position synchronization for grabs and throws around
`$15B2A-$15BD8`. `$15ABA` starts a jumping reaction/attack with signed X
velocity and upward vertical velocity; later states wait for the stored ground
height before recovering.

The two-object design gives the desired phase change for free: after one twin
dies, `$17F9C (boss_unlink_pair)` clears the survivor's pair role and pointer, so its selector no
longer tries to maintain split targeting. There is no separate low-health
enrage variable in the inspected code; the apparent second phase is the
survivor operating without pair constraints.

### Mr. X and the final encounter

#### Offer scene before combat

Round 8 differs from every other round. The ELC boss rush first reintroduces
the five earlier families. The office controller then sets
`$FFDE00 (mr_x_offer_flag)` and `$FFFA79 (stop_clock)`.
`$11B4C (mr_x_offer_update)` runs every gameplay frame and dispatches the
dialogue/choice machine through `$11B94 (mr_x_offer_jt)`.

The offer can:

- freeze or restore player control;
- stream dialogue art;
- branch on one-player or two-player answers;
- enable modified friendly-fire damage for the P1-vs-P2 branch;
- return one branch to Round 6;
- eventually restore normal combat for the Mr. X fight.

This is narrative state layered over gameplay, not a separate global game
mode. The boss object and level pipeline continue to exist beneath it.

#### Mr. X body and attack state machine

The office-to-boss hand-off is explicit. The type-`$33` controller runs
`$12B5C (mr_x_office_controller_update)`. In its hand-off state,
`$12CE0 (mr_x_office_controller_spawn_boss)` waits for bit 3 of
`$FFFA72 (level_flow_flags)`, allocates an object, writes type `$35`, copies the
controller's X/lane/height and adds `$28` to height. It then clears the linked
type-`$34` object stored at controller `+$50` and deletes the type-`$33`
controller itself. The new type-`$35` object enters the dispatcher below on its
next object pass.

The final boss uses the bespoke handler at `$1306A (mr_x_boss_update)` (dispatcher type `$35`).
Its relative state table at `$130B8` reaches movement, charge, firing,
hit-reaction, and death states through `$130D6-$13E3E`. It uses:

- `+$5C` for the selected player;
- `+$32` for health and `+$34` for outgoing damage;
- `$129F8/$12A4E/$12A78` for target selection and range tests;
- `$1401E/$14048` for velocity steering and facing;
- `$13ED8 (bespoke_boss_collision_dispatch)` for collision-result dispatch;
- effect/projectile objects in the neighboring `$33-$38` type family for the
  machine-gun/impact choreography.

`$13EBC (mr_x_init_combat_stats)` selects health and damage from a difficulty table. The common
collision reaction at `$13F9A` subtracts the attacker's `+$34`; health at or
below zero selects the terminal state. Unlike the shared later-boss death
path, Mr. X's terminal initialization `$13E4C (mr_x_final_encounter_init)` explicitly:

| Difficulty | Health | Damage |
|---|---:|---:|
| Easy | `$28` | `$1E` |
| Normal | `$32` | `$22` |
| Hard | `$50` | `$28` |
| Hardest | `$50` | `$32` |

```text
$FFFA77 = 1             ; final encounter/stage-clear presentation active
$FFF50E = $000C         ; HUD/encounter display selector
$FFF502 = this object   ; primary health-bar object pointer
$FFFA56 = 0
initialize difficulty stats and final animation
```

This is why `$117FC (stage_clear_monitor)` has a special branch for `$FFFA77 (final_boss_presentation_active)`: final-stage
completion is coupled to the registered Mr. X object and presentation state,
not merely to the generic tracked-enemy count.

### Round-by-round behavior

| Round | Boss-side implementation | Important exception |
|---:|---|---|
| 1 | Antonio, type `$56` | ELC contains adjacent 1P/2P-qualified variant records. |
| 2 | Souther, type `$55` | Counter/target logic reacts to player action and facing, not only distance. |
| 3 | Abadede, type `$30` + linked `$31` | Bespoke charge framework; does not use `$17EDC (boss_init_combat_stats)`. |
| 4 | Bongo, type `$57` + linked `$97` | Flame/charge link is synchronized to parent animation. |
| 5 | Onihime/Yasha, two type `$58` objects | Same-type pairing splits targets; survivor becomes unpaired automatically. |
| 6 | Bongo encounter, then two Southers | Multiple boss families inside one round; only the final drain closes the stage. |
| 7 | No canonical terminal boss | Elevator/gauntlet progression uses special camera logic and pre-created controller objects `$50-$53`; completion is the level-index-6 special case in `$117FC (stage_clear_monitor)`. |
| 8 | `$56 -> $55 -> $30 -> $57 -> $58`, then Mr. X | Boss rush shares one late-phase pipeline; office offer interrupts control/clock; final completion uses `$FFFA77 (final_boss_presentation_active)` and Mr. X HUD pointers. |

### Progression counters and stage clear

Two completion mechanisms coexist.

#### Generic tracked entities

The ELC loader classifies the older `$20-$2A` objects through `$9350 (is_nonordinary_enemy_type)`. Tracked
objects increment `$FFFB1E (active_progression_entity_count)` when spawned, set an object flag, and decrement the
counter in the common death path at `$9D8C`. When the counter reaches zero,
level-flow flags can be cleared and the next ELC section or pipeline state can
run. This is especially important for adds around boss encounters: killing the
visual boss alone is not sufficient if a tracked add remains.

#### Registered boss health and final completion

The `$55-$58` initializer `$17F2E (boss_link_same_type_pair)` registers one or two boss pointers at
`$FFF502/$FFF508` while late-phase bit 6 is set. `$FFF50F (boss_pair_display_variant)` identifies the display
variant. `$117FC (stage_clear_monitor)` uses these pointers to render/observe health and
to decide when presentation state can advance.

For normal rounds, `$FFFA05 (level_spawn_flow_flags)` bit 6 is the gate that says a late/boss phase is
eligible to finish. Round 7 bypasses the normal boss expectation and directly
sets `$FFFA73 (end_of_level_flag)` once its special gauntlet condition is met. Round 8
uses `$FFFA77 (final_boss_presentation_active)` and the Mr. X registration path.

```text
function maybe_finish_encounter():
    if generic_tracked_count != 0:
        return

    if more_elc_sections_exist:
        load_or_spawn_next_section()
        return

    if round == 7:
        set end_of_level_flag
        return

    if round == 8 and final_mr_x_presentation_active:
        observe registered final boss / offer outcome
        set end_of_level_flag when final condition is complete
        return

    if late_phase_flag:
        set end_of_level_flag
```

The exact ordering is split across the end-of-frame level dispatcher and
`$117FC (stage_clear_monitor)`, but the ownership boundary is clear: boss AI removes or
registers combat objects; the engine advances the campaign.

### Confidence and unresolved details

#### Confirmed code-label findings (100%)

- ELC-derived type mapping and round order for Antonio/Souther/Bongo/twins.
- Abadede type `$30`, helper `$31`, charge behavior, and separate stats path.
- Shared `$55-$58` object fields, target/distance helpers, health/damage path,
  pair linking, and death cleanup.
- Mr. X type `$35`, bespoke dispatcher, difficulty stats, collision dispatch,
  type-`$33` office-controller hand-off, and final-encounter registration.
- Round 6 multi-boss structure, Round 7 no-boss exception, and Round 8 boss
  rush plus offer state machine.
- Arena locking belongs to the level camera/pipeline rather than boss objects.
- Generic `$FFFB1E (active_progression_entity_count)` draining and late-phase/HUD pointer coupling.

#### Remaining medium-confidence detail (80-95%)

- Exact visible role of every linked `$96-$99` object. Parentage and
  animation/contact synchronization are proved, while whether every state is a
  visible projectile, hitbox, or afterimage needs framebuffer/VRAM tracing.
- Exact semantic names for the several collision-result values returned in
  `d7` by `$AA22`/the older collision framework.

#### Open questions

1. Record `$F502/$F508/$F50E` frame by frame through every Round 8 boss-rush
   section to distinguish HUD registration from completion signaling.
2. Capture linked objects `$96-$99` in the framebuffer and SAT to assign exact
   names (boomerang, claw trail, flame, or invisible hitbox) to every state.
3. Decode every family primary/tactical table into named moves without relying
   on visible retail descriptions.
4. Match each raw `object+$59` selection bit to the exact displayed answer text
   in both Mr. X prompts; the static route matrix itself is now decoded in the
   story-flow manuscript.

### Boss analysis-data update ledger

The following duplicate-checked names were integrated into the shared CSV files.
All entries below were new except `$117FC (stage_clear_monitor)`, whose existing
`$117FC (stage_clear_monitor)` description was upgraded.

#### `labels.csv`

```csv
00012B5C, mr_x_office_controller_update, "100% - Type-$33 office controller primary-state dispatcher; creates linked scene objects and hands off to the type-$35 Mr. X body"
00012CE0, mr_x_office_controller_spawn_boss, "100% - When level-flow bit 3 is set, allocates type-$35 Mr. X at the controller position, removes the linked type-$34 object and clears the controller"
0001306A, mr_x_boss_update, "100% - Mr. X bespoke primary-state dispatcher and global reaction gate; dispatcher object type $35"
00013E4C, mr_x_final_encounter_init, "100% - Registers Mr. X with final-stage HUD/completion state via $FA77/$F50E/$F502 and initializes final combat presentation"
00013EBC, mr_x_init_combat_stats, "100% - Initializes Mr. X health and outgoing damage from the difficulty table at $13ED0"
00013ED8, bespoke_boss_collision_dispatch, "100% - Dispatches collision result d7 for Mr. X/Abadede-era bosses, retaining player target pointer at object+$5C"
000143D0, abadede_update, "100% - Abadede object type $30 top-level update and primary-state dispatch"
000144E0, abadede_init, "100% - Initializes Abadede, creates linked type $31 and optional type $39, loads animations/stats, and selects a player"
0001456A, abadede_init_combat_stats, "100% - Initializes Abadede health/damage from difficulty and ELC variant fields"
000158C4, onihime_yasha_update, "100% - Onihime/Yasha type $58 shared update; targeting, interaction maintenance and primary-state dispatch"
00015946, onihime_yasha_select_target, "100% - Selects/caches a player for a twin using availability, pair role and X distance"
00015E70, souther_update, "100% - Souther type $55 top-level update and primary-state dispatch"
00016294, souther_select_target, "100% - Selects target using player action, distance, lane, facing, pair role and hold counters"
00016CE4, antonio_update, "100% - Antonio type $56 top-level update and primary-state dispatch"
00016D40, antonio_select_target, "100% - Selects P1/P2 using availability, X distance and same-type pair separation"
000174E0, bongo_update, "100% - Bongo type $57 top-level update and primary-state dispatch"
0001753A, bongo_select_target, "100% - Selects/caches P1/P2 using alternation, pair role and nearest-X fallback"
00017C36, boss_apply_pending_damage, "100% - Applies pending damage to types $55-$58 and selects hitstun, knockback, or lethal reaction"
00017EDC, boss_init_combat_stats, "100% - Initializes types $55-$58 base damage and health from type, difficulty and encounter flags"
00017F2E, boss_link_same_type_pair, "100% - Links same-type bosses, assigns pair roles, and registers late-phase HUD pointers"
00017F9C, boss_unlink_pair, "100% - Clears reciprocal pair metadata when one of a same-type boss pair is removed"
```

Existing-row description upgrade:

```csv
000117FC, stage_clear_monitor, "100% - Converts boss/late-phase, Round 7, and final-stage presentation conditions into end_of_level_flag"
```

The final CSV uses boss/retail names for `$158C4-$17F9C`; the ELC round sequence
proves these are boss families rather than ordinary enemies.

#### `addresses.csv`

```csv
FFF502, boss_health_primary_object, "95% - W - Primary registered boss object used by late/final encounter HUD and stage-clear presentation"
FFF508, boss_health_secondary_object, "95% - W - Secondary registered boss object for paired/two-boss encounters"
FFF50E, boss_health_display_selector, "90% - W - Boss/final encounter HUD selector; Mr. X writes $000C"
FFF50F, boss_pair_display_variant, "90% - B - Late-phase boss pair/display variant updated from ELC object+$41"
FFFA53, boss_forced_reaction_flags, "95% - B - Coordination/forced-reaction bitfield used by paired bosses and Abadede multi-instance logic"
FFFA77, final_boss_presentation_active, "95% - B - Set by Mr. X final encounter initialization; selects final-stage branch in stage_clear_monitor"
```
