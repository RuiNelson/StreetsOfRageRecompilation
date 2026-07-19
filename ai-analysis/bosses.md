# Boss Architecture and Round-by-Round Encounters

## Scope and method

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

## Executive summary

There is no single `Boss` class. There are three related implementation
strata:

1. **Abadede and Mr. X use older bespoke objects.** Abadede is type `$30`,
   dispatched to `$143D0`, and owns helper type `$31`. Mr. X uses the older
   `$1306A-$13EBC` subsystem (type `$35` by the dispatcher table); its terminal
   initialization at `$13E4C` explicitly registers the final encounter with
   the HUD/stage-clear system.
2. **Antonio, Souther, Bongo, and the twins share a later boss framework.**
   Types `$55-$58` have separate tactical state tables, but share target,
   movement, collision, damage, pairing, and death helpers in
   `$17924-$17F9C`.
3. **The level engine remains authoritative over progression.** Boss objects
   do not load the next round themselves. The ELC pipeline locks the arena,
   loads the required art, spawns the boss, and waits for encounter state to
   drain. Boss death updates counters or final-HUD pointers; `stage_clear_monitor`
   at `$117FC` converts the resulting late-phase condition into
   `end_of_level_flag`.

The types `$20-$2A` are ordinary/auxiliary objects from an earlier enemy
framework. Their common state tables and tracked-entity count matter to the
level pipeline, but they must not be confused with the retail bosses merely
because they use the same health offset and occur in late waves.

## Object dispatch and boss identity

The global object dispatcher at `$AD8E` indexes the word table at `$B236`.
Several entries are trampolines because the real handler lies outside the
signed 16-bit address range.

| Retail boss | Object type | Top-level update | Shared family | Important helper objects |
|---|---:|---:|---|---|
| Antonio | `$56` | `$16CE4` | later boss framework | `$96` linked boomerang/attack object |
| Souther | `$55` | `$15E70` | later boss framework | `$98/$99` linked claw/afterimage attack objects |
| Abadede | `$30` | `$143D0` | bespoke older framework | `$31` linked body/attack component; `$39` conditional effect |
| Bongo | `$57` | `$174E0` | later boss framework | `$97` linked flame/attack object |
| Onihime/Yasha | `$58` | `$158C4` | later boss framework | pairing metadata in the two boss objects |
| Mr. X | `$35` | `$1306A` | bespoke final-boss framework | attack/effect objects in the `$33-$38` family |

The type-to-name mapping for `$55-$58` is 100% as a sequence and 95% for each
individual retail label: the ELC streams place the types in exactly the known
round order, including the Round 6 and Round 8 repeats. Abadede's `$30` mapping
is also supported by Round 8's position between `$55` and `$57`, by its charge
state machine, and by the same-type exclusion scan at `$14486`. Mr. X's `$35`
mapping is 90%: the dispatcher, bespoke final-boss state table, final encounter
registration, and late Round 8 control flow agree, but the object is introduced
through the office controller rather than a simple six-byte ELC boss record.

## How a boss encounter starts

### ELC records, resource residency, and the late phase

At round initialization `$E5C` Nemesis-decompresses the selected ELC stream to
`$FF6800`. The level pipeline consumes its six-byte entity records, filters them
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

When the pipeline reaches the late phase, `$FFFA05` bit 6 is set. This has
several effects:

- `play_level_music` selects boss music (`$87`, or `$90` in Round 8);
- camera progression stops opening new corridors;
- boss initializers register HUD pointers through `$F502/$F508`;
- the level pipeline changes from spawning/scanning to waiting for completion;
- `stage_clear_monitor` begins considering the stage clear condition.

### Arena and camera locking

The camera is constrained by the two X boundaries at `$FFE01A` (maximum) and
`$FFE01E` (minimum). Normal waves open one side of the corridor through
`$19570`; the transition state at `$6A6` waits until the camera has reached the
new bound before normalizing active entities. A boss arena is therefore a
camera-boundary condition plus a spawn phase, not a rectangle owned by the
boss object.

This distinction explains why recurring bosses can appear mid-round. Round 6
can introduce Bongo and later the two Southers without either handler knowing
the round's map layout. Round 8 can run a whole boss-rush sequence in one fixed
office corridor by advancing ELC sections while retaining the late-phase arena.

## Shared later-boss framework (`$55-$58`)

### Object layout

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

### Statistics and difficulty

`$17EDC` indexes four bytes by `type-$55`: one base-damage table at `$17F26`
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

### Targeting, movement, and attack commitment

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

### Damage, vulnerability, and death

`$17C36` is the shared received-damage path. Collision code leaves damage in
`+$6C` and the attacker pointer in `+$70`; the routine subtracts it from health
`+$32`, clears movement, and chooses hitstun, knockback, or lethal reaction.
Attack states can temporarily suppress or redirect this path through flags and
interaction reservations, which is why some visible moves appear invulnerable
or counter jump attacks.

The airborne/bounce path at `$16400` returns a living boss to active AI. A
defeated boss proceeds to `$16512`, which counts down, blinks/removes the
sprite, awards score via `$16542`, clears its partner relationship through
`$17F9C`, and finally clears the object slot.

Pairing is significant for Round 5/6/8. `$17F2E` scans for another object of
the same type and writes reciprocal roles (`+$5D=1/2`) and partner pointers
(`+$5E`). Target selectors use those roles to split attention across P1/P2.
Death unlinks the survivor so it can return to unpaired target selection.

## Antonio (`$56`, `$16CE4`)

Antonio uses the family-C table rooted near `$16CF4`. Initialization at
`$16D0A` selects a player, initializes stats through `$17EDC`, loads animations
from `$2E8B4`, and discovers a same-type partner if the ELC supplied one.

The tactical code keeps wider spacing than the close-range bosses and selects
an attack when X is roughly `$28-$78` and lane separation is small. `$17206`
creates/maintains linked object `$96`; `$16C6E` positions that object from the
parent's animation phase and facing. The link is the code-side implementation
of the visible boomerang choreography: the attack object follows Antonio during
wind-up/catch phases and becomes independently active during the throw.

The target selector at `$16D40` has explicit pair-role thresholds in 2P. This
is used by the optional extra/variant record as well as by repeated Round 8
encounters; it is not evidence for a story-level second Antonio in every mode.

## Souther (`$55`, `$15E70`)

Souther's selector at `$16294` is the most elaborate of the four shared
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

## Abadede (`$30`, `$143D0`)

Abadede predates the `$55-$58` framework. His state byte still lives at `+$30`
and health at `+$32`, but he dispatches through the relative state table near
`$14466` and uses target pointer `+$5C` rather than `+$72`.

Initialization at `$144E0`:

- clears a global coordination bit;
- creates linked type `$31` and stores it at `+$50`;
- conditionally creates type `$39` for a variant;
- loads the `$34B94` animation set;
- calls `$1456A` for difficulty/variant health and damage;
- selects a player through `$129F8`;
- seeds strong X/lane velocities and faces the target.

The base `(health, damage)` pairs at `$145BC` are Easy `($20,$10)`, Normal
`($20,$20)`, Hard `($20,$40)`, and Hardest `($34,$40)`. `$1456A` can then add
variant bonuses from ELC fields, so a repeated Abadede need not have exactly
the canonical Round 3 values.

The core behavior is a charge/clothesline cycle. `$1401E` flips velocity signs
toward the selected player, while `$14048` updates facing. Collision dispatcher
`$13ED8` routes contact outcomes: a clean hit marks the player interaction,
received attacks subtract the attacker's `+$34` from `+$32`, and lethal damage
selects state `$0E`.

Abadede also has explicit multi-instance coordination. `$14486` scans all
object slots for another type `$30`; if one is active outside selected reaction
states, `$FA53` is used to coordinate forced transitions. This explains why
Round 8 and two-player variants do not reduce to two completely independent
charge loops.

## Bongo (`$57`, `$174E0`)

Bongo's family-D state machine circles in the lane, corrects screen-edge
position, and then commits to a multi-stage acceleration/charge. The attack
chain around `$176B4-$177E2` uses distance and phase counters rather than a
single random decision.

Linked type `$97`, created at `$1781E`, is positioned from Bongo's animation
and facing and implements the flame/contact portion of the attack. Parent and
linked object exchange animation-phase information so the hit region appears
only during the appropriate breath/charge frames.

The target selector `$1753A` alternates players more aggressively than
Antonio's and uses pair roles to avoid duplicate targets. Round 6 uses Bongo as
a mid-round boss-strength encounter; Round 7 reuses the family in the elevator
gauntlet; Round 8 repeats it as the fourth boss-rush family.

## Onihime and Yasha (`$58`, `$158C4`)

The twins are two objects of the same type rather than a controller with two
hard-coded child actors. ELC metadata causes `$17F2E` to pair them and assign
reciprocal roles. Their family-A selector at `$15946` normally chooses the
nearest usable player but uses the pair role to bias the two bosses apart.

Their state table contains close-range approach, rapid jump/airborne attacks,
and explicit player-position synchronization for grabs and throws around
`$15B2A-$15BD8`. `$15ABA` starts a jumping reaction/attack with signed X
velocity and upward vertical velocity; later states wait for the stored ground
height before recovering.

The two-object design gives the desired phase change for free: after one twin
dies, `$17F9C` clears the survivor's pair role and pointer, so its selector no
longer tries to maintain split targeting. There is no separate low-health
enrage variable in the inspected code; the apparent second phase is the
survivor operating without pair constraints.

## Mr. X and the final encounter

### Offer scene before combat

Round 8 differs from every other round. The ELC boss rush first reintroduces
the five earlier families. The office controller then sets
`mr_x_offer_flag` (`$FFDE00`) and `stop_clock` (`$FFFA79`).
`mr_x_offer_update` at `$11B4C` runs every gameplay frame and dispatches the
dialogue/choice machine through `$11B94`.

The offer can:

- freeze or restore player control;
- stream dialogue art;
- branch on one-player or two-player answers;
- enable modified friendly-fire damage for the P1-vs-P2 branch;
- return one branch to Round 6;
- eventually restore normal combat for the Mr. X fight.

This is narrative state layered over gameplay, not a separate global game
mode. The boss object and level pipeline continue to exist beneath it.

### Mr. X body and attack state machine

The final boss uses the bespoke handler at `$1306A` (dispatcher type `$35`).
Its relative state table at `$130B8` reaches movement, charge, firing,
hit-reaction, and death states through `$130D6-$13E3E`. It uses:

- `+$5C` for the selected player;
- `+$32` for health and `+$34` for outgoing damage;
- `$129F8/$12A4E/$12A78` for target selection and range tests;
- `$1401E/$14048` for velocity steering and facing;
- `$13ED8` for collision-result dispatch;
- effect/projectile objects in the neighboring `$33-$38` type family for the
  machine-gun/impact choreography.

`$13EBC` selects health and damage from a difficulty table. The common
collision reaction at `$13F9A` subtracts the attacker's `+$34`; health at or
below zero selects the terminal state. Unlike the shared later-boss death
path, Mr. X's terminal initialization `$13E4C` explicitly:

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

This is why `stage_clear_monitor` has a special branch for `$FA77`: final-stage
completion is coupled to the registered Mr. X object and presentation state,
not merely to the generic tracked-enemy count.

## Round-by-round behavior

| Round | Boss-side implementation | Important exception |
|---:|---|---|
| 1 | Antonio, type `$56` | ELC contains adjacent 1P/2P-qualified variant records. |
| 2 | Souther, type `$55` | Counter/target logic reacts to player action and facing, not only distance. |
| 3 | Abadede, type `$30` + linked `$31` | Bespoke charge framework; does not use `$17EDC`. |
| 4 | Bongo, type `$57` + linked `$97` | Flame/charge link is synchronized to parent animation. |
| 5 | Onihime/Yasha, two type `$58` objects | Same-type pairing splits targets; survivor becomes unpaired automatically. |
| 6 | Bongo encounter, then two Southers | Multiple boss families inside one round; only the final drain closes the stage. |
| 7 | No canonical terminal boss | Elevator/gauntlet progression uses special camera logic and pre-created controller objects `$50-$53`; completion is the level-index-6 special case in `$117FC`. |
| 8 | `$56 -> $55 -> $30 -> $57 -> $58`, then Mr. X | Boss rush shares one late-phase pipeline; office offer interrupts control/clock; final completion uses `$FA77` and Mr. X HUD pointers. |

## Progression counters and stage clear

Two completion mechanisms coexist.

### Generic tracked entities

The ELC loader classifies the older `$20-$2A` objects through `$9350`. Tracked
objects increment `$FFFB1E` when spawned, set an object flag, and decrement the
counter in the common death path at `$9D8C`. When the counter reaches zero,
level-flow flags can be cleared and the next ELC section or pipeline state can
run. This is especially important for adds around boss encounters: killing the
visual boss alone is not sufficient if a tracked add remains.

### Registered boss health and final completion

The `$55-$58` initializer `$17F2E` registers one or two boss pointers at
`$FFF502/$FFF508` while late-phase bit 6 is set. `$F50F` identifies the display
variant. `stage_clear_monitor` uses these pointers to render/observe health and
to decide when presentation state can advance.

For normal rounds, `$FFFA05` bit 6 is the gate that says a late/boss phase is
eligible to finish. Round 7 bypasses the normal boss expectation and directly
sets `end_of_level_flag` once its special gauntlet condition is met. Round 8
uses `$FA77` and the Mr. X registration path.

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
`stage_clear_monitor`, but the ownership boundary is clear: boss AI removes or
registers combat objects; the engine advances the campaign.

## Confidence and unresolved details

### High confidence (95-100%)

- ELC-derived mapping and round order for Antonio/Souther/Bongo/twins.
- Abadede type `$30`, helper `$31`, charge behavior, and separate stats path.
- Shared `$55-$58` object fields, target/distance helpers, health/damage path,
  pair linking, and death cleanup.
- Round 6 multi-boss structure, Round 7 no-boss exception, and Round 8 boss
  rush plus offer state machine.
- Arena locking belongs to the level camera/pipeline rather than boss objects.
- Generic `$FFFB1E` draining and late-phase/HUD pointer coupling.

### Medium confidence (80-95%)

- Mr. X dispatcher type `$35`. The handler and terminal final-stage effects are
  unambiguous; the creation route through Round 8 office controllers is less
  direct than the other ELC records.
- Exact visible role of every linked `$96-$99` object. Parentage and
  animation/contact synchronization are proved, while whether every state is a
  visible projectile, hitbox, or afterimage needs framebuffer/VRAM tracing.
- Exact semantic names for the several collision-result values returned in
  `d7` by `$AA22`/the older collision framework.

### Open questions

1. Trace the office controller's exact instruction that materializes Mr. X and
   annotate the controller type-to-body hand-off.
2. Record `$F502/$F508/$F50E` frame by frame through every Round 8 boss-rush
   section to distinguish HUD registration from completion signaling.
3. Capture linked objects `$96-$99` in the framebuffer and SAT to assign exact
   names (boomerang, claw trail, flame, or invisible hitbox) to every state.
4. Decode every family primary/tactical table into named moves without relying
   on visible retail descriptions.
5. Test all 2P Mr. X offer answer combinations and document which route returns
   to Round 6 versus the final fight/bad ending.

## Analysis-data update ledger

The following duplicate-checked names were integrated into the shared CSV files.
All entries below were new except `$117FC`, whose existing
`stage_clear_monitor` description was upgraded.

### `labels.csv`

```csv
0001306A, mr_x_boss_update, "90% - Mr. X bespoke primary-state dispatcher and global reaction gate; dispatcher object type $35"
00013E4C, mr_x_final_encounter_init, "95% - Registers Mr. X with final-stage HUD/completion state via $FA77/$F50E/$F502 and initializes final combat presentation"
00013EBC, mr_x_init_combat_stats, "95% - Initializes Mr. X health and outgoing damage from the difficulty table at $13ED0"
00013ED8, bespoke_boss_collision_dispatch, "95% - Dispatches collision result d7 for Mr. X/Abadede-era bosses, retaining player target pointer at object+$5C"
000143D0, abadede_update, "100% - Abadede object type $30 top-level update and primary-state dispatch"
000144E0, abadede_init, "100% - Initializes Abadede, creates linked type $31 and optional type $39, loads animations/stats, and selects a player"
0001456A, abadede_init_combat_stats, "95% - Initializes Abadede health/damage from difficulty and ELC variant fields"
000158C4, onihime_yasha_update, "95% - Onihime/Yasha type $58 shared update; targeting, interaction maintenance and primary-state dispatch"
00015946, onihime_yasha_select_target, "95% - Selects/caches a player for a twin using availability, pair role and X distance"
00015E70, souther_update, "95% - Souther type $55 top-level update and primary-state dispatch"
00016294, souther_select_target, "95% - Selects target using player action, distance, lane, facing, pair role and hold counters"
00016CE4, antonio_update, "95% - Antonio type $56 top-level update and primary-state dispatch"
00016D40, antonio_select_target, "95% - Selects P1/P2 using availability, X distance and same-type pair separation"
000174E0, bongo_update, "95% - Bongo type $57 top-level update and primary-state dispatch"
0001753A, bongo_select_target, "95% - Selects/caches P1/P2 using alternation, pair role and nearest-X fallback"
00017C36, boss_apply_pending_damage, "100% - Applies pending damage to types $55-$58 and selects hitstun, knockback, or lethal reaction"
00017EDC, boss_init_combat_stats, "100% - Initializes types $55-$58 base damage and health from type, difficulty and encounter flags"
00017F2E, boss_link_same_type_pair, "95% - Links same-type bosses, assigns pair roles, and registers late-phase HUD pointers"
00017F9C, boss_unlink_pair, "100% - Clears reciprocal pair metadata when one of a same-type boss pair is removed"
```

Existing-row description upgrade:

```csv
000117FC, stage_clear_monitor, "100% - Converts boss/late-phase, Round 7, and final-stage presentation conditions into end_of_level_flag"
```

The final CSV uses boss/retail names for `$158C4-$17F9C`; the ELC round sequence
proves these are boss families rather than ordinary enemies.

### `addresses.csv`

```csv
FFF502, boss_health_primary_object, "95% - W - Primary registered boss object used by late/final encounter HUD and stage-clear presentation"
FFF508, boss_health_secondary_object, "95% - W - Secondary registered boss object for paired/two-boss encounters"
FFF50E, boss_health_display_selector, "90% - W - Boss/final encounter HUD selector; Mr. X writes $000C"
FFF50F, boss_pair_display_variant, "90% - B - Late-phase boss pair/display variant updated from ELC object+$41"
FFFA53, boss_forced_reaction_flags, "95% - B - Coordination/forced-reaction bitfield used by paired bosses and Abadede multi-instance logic"
FFFA77, final_boss_presentation_active, "95% - B - Set by Mr. X final encounter initialization; selects final-stage branch in stage_clear_monitor"
```
