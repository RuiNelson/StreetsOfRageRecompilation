# Player Health, Lives, Continues, and Combat

## Scope and method

This document describes the playable-character systems in the original 68000 program, using `output/sor.asm` as the primary source. Addresses are ROM offsets unless prefixed with `FF`, in which case they are work-RAM addresses. Routine and data names follow `code-analysis/labels.csv` and `code-analysis/addresses.csv`; generated `sub_...` names remain only where no semantic label exists yet.

The strongest conclusions come from following both players through the fixed object slots at `$FFB800 (p1_object)` and `$FFB880 (p2_object)`, then tracing their per-frame update, collision response, HUD update, death, respawn, and continue paths. Some action-state meanings remain provisional because the state byte selects character-specific animation data indirectly.

## Executive summary

The game separates three kinds of player resource:

1. **Health** is a binary word in each live player object at offset `+$32`. Full health is `$0050` (80 units). `$4E6C (adjust_player_health)` is the shared saturating adjustment routine and also redraws the health bar.
2. **Lives** are one-byte packed-BCD counters at `$FFFF20 (p1_lives)` and `$FFFF23 (p2_lives)`. A life is deducted only after the KO animation reaches its death-resolution state, not at the instant health reaches zero.
3. **Continues** are words at `$FFFF1A (p1_continues)` and `$FFFF1C (p2_continues)`. They begin at 3. Accepting a continue subtracts one, clears that player's score and extra-life threshold progress, restores the configured starting lives, and re-enters the current round.

Normal attacks are not hard-coded as one damage value per button. The player's current action/animation and character select a per-frame hit descriptor. Its low nibble becomes the outgoing damage value at object `+$34`; its high nibble controls the victim reaction. Consequently an attack only becomes damaging on selected animation frames.

The three face buttons, after the optional control-layout remap, are represented as:

- bit 4: normal attack;
- bit 5: jump;
- bit 6: police special;
- bit 7: Start.

The police special is a global scripted event. It consumes one player-specific special counter, records the caller, freezes or suppresses several normal systems, spawns the police vehicle and bombardment objects, and marks enemies for the special-hit response. It is disabled in level index 7 (Round 8).

## Player object layout

P1 is always the object at `$FFB800 (p1_object)`; P2 is always at `$FFB880 (p2_object)`. Player code distinguishes them by comparing `a0` with `$FFB800 (p1_object)`, then selects the corresponding global counters and input addresses.

| Offset | Size | Meaning | Evidence / confidence |
|---:|---:|---|---|
| `+$00` | byte | Object type/lifecycle. `1` is an active playable character; `15` is the dead/game-over/continue object. | High: `$2B48 (resolve_player_death)`, `$5434`-`$56xx`. |
| `+$01` | byte | General object flags (visibility/activity/collision bits). | High at bit level; not all bits named. |
| `+$08` | word | Animation/action selector. Its low bits also encode facing/variant information in several routines. | High. |
| `+$0A` | byte | Current animation frame. | High: incremented by `sub_39E8`; used by hit tables. |
| `+$0D` | byte | Current frame duration. | High. |
| `+$10` | word/fixed pair | World X position (the surrounding long also carries fractional precision). | High. |
| `+$14` | word/fixed pair | Ground-plane Y position. | High. |
| `+$18` | word/fixed pair | Height/Z position. | High. |
| `+$1C`, `+$20`, `+$24` | long | X, plane-Y, and vertical velocities. | High. |
| `+$30` | byte | Player action-state/facing byte. Bit 0 commonly selects left/right; even values are base actions. | High structurally; individual action names vary by character/context. |
| `+$32` | word | Current health, clamped to `0..$50`. | Certain. |
| `+$34` | byte | Active outgoing hit damage (low nibble); zero on non-hitting animation frames. The source table's high nibble selects hit reaction metadata. | High. |
| `+$37` | byte | Contact/grab synchronization flags shared with attacker or partner. | Medium-high. |
| `+$40`, `+$41` | bytes | Hit/knockdown/death phase counters. | Medium. |
| `+$42` | byte | Hit property/reaction bits derived from attack tables. | Medium-high. |
| `+$4C` | word | Current contact/grab partner pointer. | High. |
| `+$50` | byte | Character ID: 0 Axel, 1 Adam, 2 Blaze. | Certain. |
| `+$51` | byte | Animation/object interaction gate, also used to reserve a grabbed target. | Medium-high. |
| `+$54` | word | Remapped held/pressed input pair; byte `+$55` is the edge/press byte used by action tests. | High. |
| `+$56` | byte | Deferred/fallback incoming damage when the attacker's live `+$34` cannot be used. | High. |
| `+$58`, `+$59` | bytes | Player action, combo, invulnerability, and transition flags. | Medium; individual bits need names per state. |
| `+$5A`, `+$5B` | bytes | Saved animation frame and duration during an interrupted/continued action. | High. |
| `+$5C` | byte | Short timer used by attack/combo and temporary-state logic. | Medium. |
| `+$5E` | word | Pointer to the object currently grabbed/held. | High. |
| `+$60` | byte | Grabbed-object type / nonzero "holding target" indicator. | High. |
| `+$61`-`+$63` | bytes | Combo/grab continuation state and timers. | Medium. |
| `+$64..+$7B` | 24 bytes | Current body collision box. | High. |
| `+$70..+$7B` | 12 bytes | Overlapping attack/contact box used by player-vs-player collision. | High. |
| `+$7C`, `+$7D` | bytes | Collision result and reaction selector. | High. |
| `+$7E` | word | Object responsible for the current hit/contact. | High. |

The health and outgoing-hit fields are especially important: health belongs to the victim, but damage belongs to the attacker's active animation frame.

## Input and action selection

`$568A (remap_player_gameplay_input)` copies the correct player's controller word into object `+$54`. If `$FFFFC8 (control_scheme)` is nonzero, it permutes the three face-button bits while preserving directions and Start. This lets the rest of player code use a fixed logical layout regardless of the OPTIONS setting.

The idle/ground action path around `$2CD2` applies a priority chain:

```text
resolve collision/throw contact state ($3266)
resolve attack+jump chord / rear attack ($322A)
resolve jump press, bit 5 ($2FCC)
resolve normal attack, bit 4 ($3028)
otherwise dispatch directional movement from low input nibble
```

The normal-attack path also checks for a nearby collectible.
`$3136 (find_close_interaction_target)` scans the object table for weapon types
`$08-$0C` and the six consumable pickup types. A weapon is accepted only when
its reservation byte is clear and its subtype is below 3. For a weapon it:

- records the target type in player `+$60`;
- records its pointer in player `+$5E`;

For either a weapon or consumable, the common tail records the player in target
`+$52`, marks target `+$51 = 1`, and changes the player to action `$28` (or its
facing variant).

This is the bridge from a close normal attack into the weapon/item pickup
animation, not the enemy-grab detector. Enemy grabs are negotiated by the
separate collision/contact paths. Once an enemy is held, the alternate path at
`$2D20` selects grab strikes and throws from directional input and the target's
state. The target and player exchange reaction values through `+$37`, `+$51`,
`+$52`, `+$5E`, `+$60`, `+$7C`, and `+$7D`; throws apply explicit X/Z
velocities and clear the relationship on both objects.

The attack+jump chord (`$322A (player_attack_jump_chord)`) recognizes either order: attack held plus a new jump press, or jump held plus a new attack press. It selects action `$20` (plus character/facing variants), or `$4A` while carrying a target. This is the two-button rear/escape attack family, distinct from the police special.

### Action-state numbers

The byte at `+$30` is not a simple enum in isolation. Bit 0 is commonly facing, while helpers map action numbers through the table at `$2F2C` into character animation indices. Examples that are well supported are:

| State family | Observed role |
|---:|---|
| `$00/$01` | Initial/neutral facing state. |
| `$02/$03` | Normal ground/idle state. |
| `$10/$11` | Jump initiation selected by bit-5 press. |
| `$18+` | Normal attack/combo family. |
| `$20+` | Attack+jump chord / rear attack family. |
| `$28+` | Grab acquisition and held-target actions. |
| `$44+` | Grab attack/throw selection. |
| `$50..$5F` | Hurt, knockdown, and death transitions; these states are protected from ordinary re-entry by `$333E (resolve_player_hit_or_ko)`. |
| `$56/$57` | Fatal-hit launch/death animation entry. |
| `$60+` | Player/player or grab/throw reaction families. |

Because the same base state maps differently for Axel, Adam, and Blaze, names should be applied to state-transition routines before assigning universal names to every state value.

## Outgoing attacks and damage descriptors

`$41EA (compute_player_attack_descriptor)` derives the active hit descriptor every frame:

```c
descriptor_table = table_for_action(player.action_animation);
index = descriptor_table[player.character_id] + player.animation_frame;
descriptor = descriptor_table[3 + index];

player.outgoing_damage = descriptor & 0x0f;       // object +$34
player.hit_property = (descriptor >> 4);          // merged into object +$42
```

The exact table layout is compact and partly self-indexing, but the behavioral result is unambiguous: `+$34 == 0` means the current pose does not inflict damage; a nonzero low nibble is the amount applied on a valid collision. The high nibble is later used to select the victim's reaction/knockdown response.

The forced P1-versus-P2 fight enables
`$FFFA43 (duel_damage_modifier)`. `$41EA (compute_player_attack_descriptor)`
then transforms the descriptor exactly as follows:

```c
modified = (descriptor & 0xF0) + 3 * (descriptor & 0x0F);
outgoing_damage = modified & 0x0F;
hit_property = modified >> 4;
```

The low damage nibble is therefore tripled modulo 16, not halved; a carry can
also increment the reaction nibble. Other player-contact paths select alternate
reaction values while the same flag is set.

### Player-versus-player contact

The gameplay object pass calls `$4478 (resolve_player_vs_player_collision)` once
per frame even in a one-player game. The routine immediately returns unless:

- both player-mode bits are set (`player_mode == 3`);
- neither player is in excluded spawn/invulnerability states;
- no police special is active;
- both players have valid collision-state entries.

It alternates which player is tested first, compares one player's body box (`+$64`) with the other's attack box (`+$70`), and writes reciprocal pointers to `+$7E`. If the attacker has a nonzero `+$34`, the high nibble of the attack descriptor becomes the other player's reaction (`+$7D`). With no damaging attack active, the same collision machinery negotiates push/grab reactions instead.

## Receiving damage and health

### Shared health adjuster

`$4E6C (adjust_player_health)` takes a signed delta in `d7`, adds it to object `+$32`, and clamps the result:

```c
void adjust_player_health(Player *p, int delta) {
    p->health += delta;
    if (p->health < 0)  p->health = 0;
    if (p->health > 80) p->health = 80;  // $50
    redraw_health_bar(p, p->health);
}
```

The routine divides health by 8 to select bar segments and uses the remainder to choose the partially filled end tile. P1 and P2 have separate tile destinations and art tables, but identical health arithmetic.

Fresh spawn and respawn (`$1E0E (player_spawn_or_respawn)`) pass `+$50` to this routine, guaranteeing full health. A large-food item also passes `+$50`; the smaller healing item passes `+$14` (20 units) through item dispatch at `$6A04/$6A08`.

### Applying an incoming hit

`$351E (apply_player_damage)` is the central player-damage application routine. It reads the attacker pointer from victim `+$7E`, prefers attacker `+$34`, and falls back to victim `+$56` when the live descriptor is zero. It then clears the deferred field and selected hit flags, negates the damage, and calls `$4E6C (adjust_player_health)`.

```c
bool apply_player_damage(Player *victim) {
    Object *attacker = ptr(victim->attacker_at_7e);
    unsigned damage = attacker->outgoing_damage_at_34;
    if (damage == 0)
        damage = victim->deferred_damage_at_56;

    victim->deferred_damage_at_56 = 0;
    adjust_player_health(victim, -(int)damage);
    return victim->health == 0;
}
```

`$333E (resolve_player_hit_or_ko)` is the hit/KO gate. It ignores already protected hurt/death states, checks health, interprets the reaction selector in `+$7D`, and jumps through a reaction table. When health is zero, it enters the fatal path at `$33E4`: release any grabbed object, detach contact partners, orient away from the attacker, enter state `$56/$57`, and apply an upward velocity of `$FFF70000`.

Several hurt/throw paths call `$351E (apply_player_damage)` only when the relevant animation/contact phase is reached. Thus collision detection records a pending result first; damage and animation response are synchronized later by the player's state machine.

### Falls and out-of-bounds deaths

`sub_358C` treats a player whose height/position field reaches `$01C0` as having fallen out of the playable area. If health is still nonzero it deducts a life through the same death bookkeeping used by KO, then resolves respawn/game-over. In level index 7 the same boundary instead starts a fade, reflecting Round 8's special ending/arena flow.

## KO, life deduction, and respawn

There are three distinct moments:

1. **Fatal hit:** health reaches zero; `$333E/$33E4` starts the fatal launch/animation.
2. **Life deduction:** the death animation reaches the routine at `$2AE0`, which calls `$3448`.
3. **Respawn or game-over resolution:** `$2B48 (resolve_player_death)` examines the now-decremented life counter.

`sub_3448` selects the byte immediately before the player's special counter. Because lives and specials are adjacent (`$FFFF20/$FFFF21` and `$FFFF23/$FFFF24`), this is the life byte. It invokes the BCD arithmetic helper at `$10DCA (add_bcd_resource_value)` with table entry `$0C`, whose packed value is `$99999999`; one `ABCD` operation therefore subtracts one from the one-byte packed-BCD life counter. It also refreshes the lives/special HUD.

`$2B48 (resolve_player_death)` then behaves as follows:

```c
if (player_lives != 0) {
    player.type = ACTIVE_PLAYER;       // 1
    player.spawn_flags |= RESPAWN;
    spawn_or_respawn_player(player);   // full health, placement, specials
} else {
    player.type = DEAD_CONTINUE_UI;    // $0f
    detach_player_from_high_score_state();
}
```

On respawn, `$1E0E (player_spawn_or_respawn)`:

- clears stale grab/contact/death state;
- selects a level-specific spawn point (or the current camera-relative point for respawns);
- separates P2 vertically by `$20`;
- selects character animation data;
- restores health to `$50`;
- grants `RAM[$FFFF35] + 1` police specials (normally 1 because `$FFFF35 (respawn_specials_minus_one)` is initialized to zero);
- grants zero specials on level index 7;
- returns to an active neutral state.

At the beginning of a round, `$107F2 (spawn_round_players)` creates each active player and sets their special counter to 2. The spawn routine subsequently enforces the Round 8 no-special rule. Therefore round entry and post-death respawn intentionally grant different special counts: normally 2 at round start, 1 after losing a life.

## Lives and extra lives

### Initial lives

When character select finishes, `$17A2 (initialize_player_continues)` writes:

```c
p1_continues = p2_continues = 3;
p1_lives = p2_lives = 2 * lives_setting + 1;
p1_specials = p2_specials = 0;
```

`$FFFFCA (lives_setting)` is an index from 0 through 3, producing 1, 3, 5, or 7 lives. The ordinary configuration is index 1 (3 lives). The counters are displayed and modified as packed BCD, although the allowed starting values are all single decimal digits.

### Score-awarded extra lives

Every active player frame, `$4D60 (update_score_hud_and_check_extra_life)` compares the player's six-digit packed-BCD score against a threshold chosen by `lives_via_points_ptr_p1/p2`. The ROM table at `$4DEC` contains:

| Threshold index | Packed value | Decimal score |
|---:|---:|---:|
| 0 | `$00050000` | 50,000 |
| 1 | `$00150000` | 150,000 |
| 2 | `$00250000` | 250,000 |
| 3 | `$00350000` | 350,000 |
| 4 | `$00450000` | 450,000 |
| 5 | `$00550000` | 550,000 |
| 6 | `$00650000` | 650,000 |
| 7 | `$00750000` | 750,000 |
| 8 | `$00850000` | 850,000 |
| 9 | `$00950000` | 950,000 |

Crossing a threshold increments that player's threshold index, plays the reward sound, and uses BCD table entry 0 (`$00000100`) to add one to the life byte immediately before the special counter. It does **not** grant a police special. The HUD renderer caps the displayed digit at 9 if the stored value reaches `$10` or more.

## Continues and 1P/2P lifecycle

The player-mode byte is a live mask, not merely the original menu choice:

- bit 0: P1 active;
- bit 1: P2 active;
- value 1: P1 only;
- value 2: P2 only;
- value 3: both active.

When a player reaches the continue/game-over object type, the corresponding mode bit is cleared. The other player can continue alone, so one player's death does not end a two-player game.

The continue UI is implemented by the dead player's object (`type $0F`) in the `$52xx-$58xx` region. Direction presses toggle the yes/no selection in object `+$63`; a face-button press confirms. `$5334 (confirm_player_continue)` selects the correct player's continue, character, and status fields:

- choosing **No** clears the remaining continue word and marks that player as out;
- choosing **Yes** subtracts one continue, recreates an active player object with the selected character, restores the corresponding player-mode bit, and clears the player's extra-life threshold index;
- `$565C (reset_player_after_continue)` clears that player's score and restores lives from `2*lives_setting+1` before normal spawn logic restores health and specials.

P2 also has an in-game join path at `$115CC (update_join_and_continue_hud)`: if P2 is inactive, a second controller is present, Round 8 is not active, and no police special is running, Start can create P2 directly with two specials. A small table chooses a character relative to P1's character. This is separate from accepting a continue.

The bytes at `$FFFF22 (p1_out_or_continue_flag)` and `$FFFF25 (p2_out_or_continue_flag)` are persistent per-player out/continue-display flags. They drive the flashing join/continue HUD and are consulted by round-clear and join logic. Their precise UI-state naming is clearer than treating them as lives or continues themselves.

## Police special attacks

### Activation gate

The active-player update calls `$3FCC (try_activate_police_special)` every frame;
the call itself does not mean that a special was requested. Its first test is
logical face-button bit 6, and it immediately rejects activation when that bit
is clear or any of the following is true:

- player health is zero;
- the round-intro sequence is active;
- the player is in a blocked transition/invulnerability state;
- level index 6 has one of its special scripted blockers active;
- another police special is already active;
- the caller has zero specials.

On success it:

1. decrements the correct player's special byte;
2. stores caller index 0/1 at `$FFFA1C (police_special_caller)`;
3. releases the player's held/grabbed object;
4. clears several special-event hit and timer flags;
5. snapshots the palette;
6. sets `$FFFA1A (police_special_active)` to `$0101`, making the event globally active;
7. applies level-specific setup to the scrolling/background state.

The routine returns without activating on level index 7, and spawn logic forces the counters to zero there. This is the code-level reason the police special is unavailable in Round 8.

### Scripted event and damage delivery

The object routines around `$599E-$673A` implement the special sequence. They select different object/animation tables based on caller index `$FFFA1C (police_special_caller)`, spawn the police vehicle/launcher, projectiles, explosions, and delayed effect objects, and manage special sound and palette state. `$FFFA1A (police_special_active)` suppresses the game clock, ordinary player/player collision, repeated specials, and several normal object behaviors while the sequence is running.

The global hit sweep at `$100B6` applies a special reaction to eligible enemy objects. For caller 0 it can process the enemy table broadly as the blast line reaches them; for caller 1 it uses P2 as the credited source and, in one phase, advances through enemy slots at a controlled cadence. The result written to enemies is reaction state `$0300` with the caller's player object stored as the responsible source, preserving score attribution.

The final special objects clear `$FFFA1A (police_special_active)` at `$653E` or `$672C`, returning control to normal systems. Round-clear scoring snapshots the sum of unused special counters and converts it into a special bonus.

### Special pickups

The generic BCD helper predecrements `a6` before modifying its destination. Consequently `$6A14 (apply_extra_life_pickup)`, which points `a6` at the player's special counter, actually increments the preceding **life** byte. `$6A2A (apply_extra_special_pickup)` points one byte beyond the special counter (`$FFFF22 (p1_out_or_continue_flag)` or `$FFFF25 (p2_out_or_continue_flag)`), so its predecrement increments the **special** byte itself. This uses the same decimal-counter machinery as score-awarded extra lives. The HUD refresh at `$4E14 (draw_player_lives_and_specials)` draws the two adjacent digits: lives first, specials second.

## Health, life, and continue flow

```text
enemy/player hit overlaps player hurt box
        |
        v
collision records attacker (+$7E) and reaction (+$7C/+$7D)
        |
        v
apply_player_damage reads attacker +$34 (or deferred +$56)
        |
        v
adjust_player_health: health = clamp(health - damage, 0, 80), redraw bar
        |
        +---- health > 0 ----> hurt/knockdown state ----> regain control
        |
        `---- health == 0 ---> fatal state $56/$57
                                  |
                                  v
                            death animation completes
                                  |
                                  v
                         sub_3448: lives -= 1 (BCD)
                                  |
                 +----------------+----------------+
                 |                                 |
             lives > 0                         lives == 0
                 |                                 |
        player_spawn_or_respawn respawn                    type $0F continue UI
        health=80; specials=1                      |
                                            +------+------+
                                            |             |
                                          YES             NO
                                            |             |
                                   continues -= 1   continues = 0
                                   score = 0         player remains out
                                   lives reset
                                   player-mode bit restored
```

## Evidence map

| Reference | Analytical role |
| --- | --- |
| `$1E0E (player_spawn_or_respawn)` | Spawn/respawn active player; restore health and specials. |
| `$2B48 (resolve_player_death)` | Resolve completed death into respawn or continue object. |
| `$3028 (player_normal_attack_input)` | Normal-attack entry and combo continuation. |
| `$3136 (find_close_interaction_target)` | Find/reserve a nearby free weapon or consumable pickup. |
| `$322A (player_attack_jump_chord)` | Attack+jump chord / rear attack. |
| `$333E (resolve_player_hit_or_ko)` | Player hit/KO reaction gate. |
| `$351E (apply_player_damage)` | Apply pending incoming damage. |
| `$3FCC (try_activate_police_special)` | Per-player-frame gate; validate input and conditions, then start a police special only on success. |
| `$41EA (compute_player_attack_descriptor)` | Compute per-frame outgoing attack descriptor. |
| `$4478 (resolve_player_vs_player_collision)` | Per-gameplay-frame 2P gate; return in 1P, otherwise resolve collision and friendly fire. |
| `$4D60 (update_score_hud_and_check_extra_life)` | Redraw score digits, check the next threshold, and conditionally award an extra life. |
| `$4E14 (draw_player_lives_and_specials)` | Draw player lives and specials counters. |
| `$4E6C (adjust_player_health)` | Saturating health adjustment and health-bar draw. |
| `$5334 (confirm_player_continue)` | Confirm per-player continue selection. |
| `$565C (reset_player_after_continue)` | Reset score and configured lives after continue. |
| `$568A (remap_player_gameplay_input)` | Copy/remap controller input into player object. |
| `$107F2 (spawn_round_players)` | Create active players at round start and grant two specials. |
| `$10C88 (update_game_clock)` | Run round clock unless paused/special-active; start time-over. |
| `$10DCA (add_bcd_resource_value)` | Packed-BCD resource adjustment helper. |
| `$115CC (update_join_and_continue_hud)` | P2 join and per-player continue/out HUD logic. |

## Label-confidence audit and corrected legacy names

Every code entry in the evidence map above is now confirmed at 100% for the
bounded behavior stated in `labels.csv`. Remaining uncertainty about individual
polymorphic object bits does not make those entry-point descriptions
uncertain. Two older gameplay names required correction rather than promotion:

| Address | Final finding | Decisive evidence |
|---:|---|---|
| `$2550 (player_reaction_type8_damage)` | Collision-reaction `$08`: apply pending damage; branch to the common fatal/KO path when health reaches zero; otherwise play sound `$A2`, write `+$41=8`, and select animation `$8A`. | It is target 8 in the reaction table rooted at `$24CC`; its body contains no P1/P2 ownership test. The former `players_beating_each_other` name described one possible producer of reaction metadata, not this routine. |
| `$544A (clear_inactive_player_hud)` | `player_mode=0` branch of the type-`$0F` continue/out dispatcher; selects the HUD by whether `a0==$B800` and fills its two 36-byte tile regions with `$06D7`. | The four-entry table at `$5442` selects `$544A (clear_inactive_player_hud)` only for mode zero. A live probe initialized both type-`$0F` player slots with mode zero; runtime entry logging reached `$544A (clear_inactive_player_hud)` twice and all four sentinel regions `$FF600A/$FF6032/$FF605A/$FF6082` became `$06D7`. It is not the Game Over screen initializer. |

The dynamic probe exercised the actual per-frame object dispatcher. It did not
call either target directly or infer execution merely from a successful remote
connection.

## Remaining uncertainties and useful runtime checks

1. `$FFFF35 (respawn_specials_minus_one)` is read as "additional respawn specials" and is normally zero; no writer appears in the static code. A watchpoint would establish whether an undocumented mode or RAM side effect ever changes it.
2. Several `+$58/+$59` bits combine invulnerability, combo continuation, grab state, and temporary locks. They should be named only after per-state traces, not globally from one call site.
3. The time-over path enters a global timed display state before resuming object updates. The indirect branch at `$109D4` should be traced in 1P, P2-only, and 2P modes to document exactly when each active player is forced into the fatal state.
4. The police event has two caller-index-dependent object scripts. The high-level behavior and attribution are clear, but naming every spawned object (car, officer, projectile, blast marker) would benefit from framebuffer/object-table capture during both P1 and P2 calls.
