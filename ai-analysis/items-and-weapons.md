# Items, Pickups, Breakable Props, and Weapons

## Scope and terminology

This document describes the gameplay objects that can be collected, carried, swung, thrown, broken, or converted into player resources. It is based primarily on `output/sor.asm`, especially the object handlers from `$5C1E (knife_weapon_dispatcher)` through `$6C84 (breakable_type19_dispatcher)`, the player interaction search at `$3136 (find_close_interaction_target)`, and the packed-BCD resource helpers at `$10DA6/$10DCA`.

The code makes a useful distinction that is easy to lose in a visual description of the game:

- a **consumable pickup** is linked to a player and immediately converted into health, a life, a police special, or score;
- a **weapon object** remains in the object table, records its holder, and follows the holder's animation until dropped or thrown;
- a **breakable prop** receives ordinary attack collisions and emits debris; telephone booths, crates, and similar props can visibly contain pickups or weapons, although the local prop object does not carry a universal reward-type field.

Gameplay observation confirms the five carried weapon classes as knife, bottle,
steel pipe, baseball bat, and pepper spray. It also resolves the two visually
anonymous long-weapon handlers: type `$0A` is the baseball bat and type `$0B` is
the steel pipe. Their code is nearly identical; the distinction comes from the
rendered art observed during play.

ROM init bytes fix the outgoing damage ranking exactly:

**knife 5 > bat/pipe 4 > bottle 3 > pepper 2**.

A deeper closed-form companion with hits-to-kill matrices and ballistic tables
lives in **[weapons-range-and-damage.md](weapons-range-and-damage.md)**; the
same constants are restated in context below so this document stays self-contained.

## Object-type map

The global object dispatcher at `$B236 (object_type_update_jt)` indexes a word table by object type. The relevant entries are:

| Object type | Handler | Role |
|---:|---:|---|
| `$08` | `$5C1E (knife_weapon_dispatcher)` | Knife; `+$34 = 5`; attack-button throw at \(v_x=\pm 16\). |
| `$09` | `$6114 (bottle_weapon_dispatcher)` | Bottle; `+$34 = 3`; breaks into three type-`$1E` shards. |
| `$0A` | `$61F6 (baseball_bat_weapon_dispatcher)` | Baseball bat; `+$34 = 4`; held melee, three counted uses. |
| `$0B` | `$6226 (steel_pipe_weapon_dispatcher)` | Steel pipe; `+$34 = 4`; same mechanics as bat, different art. |
| `$0C` | `$6256 (pepper_spray_weapon_dispatcher)` | Pepper spray; `+$34 = 2`; arc throw, smoke, immobilize reaction `$0400`. |
| `$11` | `$6AF4 (phone_booth_dispatcher)` | Telephone booth; shatters into up to ten type-$11 glass/booth fragments. |
| `$19` | `$6C84 (breakable_type19_dispatcher)` | Second breakable prop family; launches/bounces when struck, then despawns. |
| `$1E` | `$61BE (bottle_shard_dispatcher)` | Bottle-shard/debris projectile emitted by type `$09`. |
| `$3F` | `$68E2` | 3,000-point pickup. |
| `$40` | `$6904` | 10,000-point pickup. |
| `$47` | `$6988 (full_health_pickup_dispatcher)` | Full-health food pickup (`+$50`, 80 health; visually the large food). |
| `$48` | `$67A4 (wave_go_prompt_dispatcher)` | Flashing wave-advance prompt spawned after a camera boundary opens; not a collectible pickup. |
| `$49` | `$6862 (player_contact_effect_dispatcher)` | Short-lived player/contact visual effect spawned by attack/collision helpers; not a collectible pickup. |
| `$4B` | `$6926 (small_health_pickup_dispatcher)` | Small-health food pickup (`+$14`, 20 health; visually the apple). |
| `$4C` | `$6948` | Extra-life pickup. |
| `$4F` | `$6968` | Extra police-special pickup. |

The six pickup handlers are tiny wrappers. Each chooses an item-effect index in object `+$50`, installs its art/animation pointer, and then enters the shared pickup logic at `$699E`.
The adjacent type `$48/$49` handlers share the same object-dispatch region but
are effect/prompt objects rather than consumables; `$3136
(find_close_interaction_target)` never accepts them.

## Confirmed visible behavior and code interpretation

The following player-visible behavior is confirmed and resolves several points
that static code alone leaves visually anonymous:

- pressing attack with the knife or pepper throws it; the bottle is **not**
  attack-thrown (`$21E6` only types `$08`/`$0C`);
- the baseball bat is type `$0A`; the steel pipe is type `$0B`; both use almost
  identical long-weapon handlers at `$61F6/$6226` (live Axel origin reach 36 px);
- pepper spray is thrown, produces smoke/powder objects, and immobilizes for
  **160 frames** (`$A43E`);
- enemies can carry weapon objects; knocking an armed enemy down detaches the
  weapon, which falls to the floor and can then be collected by the player;
- bat/pipe have three counted uses; knife is effectively one throw arc; bottle
  shatters once; ground settle can exhaust wear (`+$50 ≥ 3` unpickable);
- telephone booths, crates, and other breakable scenery can reveal items or
  weapons when destroyed (producer path still external to local prop handlers).

The assembly explains these behaviors through object links and coordinated
level records rather than through a player inventory or a generic container
class.

## Shared pickup object layout

Consumable pickups use the ordinary 128-byte object structure but only a small subset is significant:

| Offset | Size | Meaning |
|---:|---:|---|
| `+$00` | byte | Object type, which determines the visible pickup wrapper. |
| `+$01` | byte | Visibility/activity flags; level-dependent bits are ORed by `$6AA6`. |
| `+$10`, `+$14`, `+$18` | word/fixed pair | World X, ground-plane Y, and height. |
| `+$24` | long | Vertical velocity while an item is falling into place. |
| `+$30` | byte | Pickup object state. |
| `+$50` | byte | Shared effect index 0 through 5. |
| `+$51` | byte | Interaction/reservation state. Value 1 means a player has collected the item. |
| `+$52` | word | Pointer to the collecting player. |

The effect index, not the object type itself, drives resource application. Current wrappers establish this exact mapping:

| Effect index | Wrapper type | Effect target |
|---:|---:|---|
| 0 | `$47` | Add 80 health, clamped to full. |
| 1 | `$4B` | Add 20 health, clamped to full. |
| 2 | `$4C` | Add one life. |
| 3 | `$4F` | Add one police special. |
| 4 | `$3F` | Add 3,000 score. |
| 5 | `$40` | Add 10,000 score. |

## How a pickup is collected

Ground objects are not collected by a passive overlap test in the movement
code. The acquisition attempt is part of the player's attack/interaction
handling. `$3028 (player_normal_attack_input)` and the adjacent held-weapon
attack path call `$3136 (find_close_interaction_target)` before they commit to
an ordinary punch or weapon swing. If the search succeeds, it consumes the
button action and sends the player to action family `$28`, preserving the
current facing bit through `$2DE6`.

`$3136 (find_close_interaction_target)` first refuses to run when the player is
already in action family `$28`; this prevents the pickup animation from
re-reserving the same target every frame. Otherwise it builds a small
three-dimensional search box around the player (decimal widths in parentheses):

| Axis | Search range relative to player | Width |
|---|---:|---:|
| X | `player.x - $14` … `player.x + $14` | 40 px total (±20) |
| Lane Y | `player.y - $10` … `player.y + $10` | 32 px total (±16) |
| Height Z | `player.z - $08` … `player.z + $08` | 16 px total (±8) |

Predicate for a free, still-usable weapon origin \(o\):

```text
|o.x − p.x| ≤ 20  ∧  |o.y − p.y| ≤ 16  ∧  |o.z − p.z| ≤ 8
∧ type ∈ [$08,$0C]  ∧  +$51 == 0  ∧  +$50 < 3
```

The routine scans the gameplay object table in slot order and accepts the first
matching ground object whose origin lies inside all three ranges. This slot
order matters in dense overlaps: there is no later best-distance comparison.

It first recognizes carried-object types `$08..$0C`; it also explicitly accepts
pickup types `$47`, `$4B`, `$4C`, `$4F`, `$3F`, and `$40`.

For a weapon, the target must be a free, still-usable ground object:

- object type is in `$08..$0C`;
- weapon `+$51` is zero, so it is not already reserved, held, dropped by a
  live command, or being thrown;
- weapon `+$50 < 3`, which excludes exhausted knife/bat/pipe-style wear states;
- if the player was already carrying another weapon, the old weapon's `+$51`
  is cleared before the new links are installed.

When these tests pass, the routine writes the selected type to player `+$60`
and the object pointer to player `+$5E`. The common tail then records the
collecting/holding player in target `+$52`, sets target `+$51 = 1`, and changes
the player to action family `$28`.

For a consumable it does not populate the player's carried-weapon fields. It only writes:

```c
pickup->collector_at_52 = player;
pickup->interaction_at_51 = 1;
```

The player still enters the same `$28` pickup animation, but no inventory field
is set. On the pickup's next update, `$69CC (consume_collected_pickup)` sees the
nonzero interaction byte, temporarily changes `a0` to the collecting player,
dispatches by pickup `+$50`, and deletes the pickup object after the effect
returns.

```c
void consume_pickup(Pickup *item) {
    if (item->interaction != 0) {
        Player *collector = ptr(item->collector);
        apply_pickup_effect(collector, item->effect_index);
        delete_object(item);
    }
}
```

The collector pointer is what makes resource ownership deterministic in 2P: an item cannot accidentally credit the other player merely because both overlap it during the same frame.

For held weapons, there is no immediate conversion step. The weapon remains an
object-table entry. Its next dispatcher pass reaches the shared hold/update path
at `$5E2E (update_held_weapon)`, follows the holder pointer in weapon `+$52`,
checks that the player's `+$5E` still points back to the same object, and then
places the sprite from the character/action attachment tables. The player-side
state helper at `$2E32` also checks action families `$28` and `$30`; when
player `+$60` is nonzero it applies a weapon-specific animation adjustment from
the small table at `$2E5A`. This is why picking up a knife, bottle, bat, pipe,
or pepper spray looks like a dedicated grab animation even though the logical
reservation is just the `+$5E/+$60` and `+$51/+$52` link pair.

## Pickup effects

### Health food

Effects 0 and 1 enter `$6A04/$6A08` with signed health deltas `$50` and `$14`. Both call the shared player health routine at `$4E6C (adjust_player_health)`, which clamps object `+$32` to `0..$50` and redraws the correct player's bar. The result is:

- large food: restore up to 80 units, effectively full health from any nonnegative value;
- small food: restore 20 units;
- neither can raise health above 80.

Both use the same health-pickup sound.

### Extra life

Effect 2 at `$6A14 (apply_extra_life_pickup)` selects `$FFFF21 (p1_special_attacks)` or `$FFFF24 (p2_special_attacks)` as `a6`, then calls `$10DCA (add_bcd_resource_value)` with BCD table entry 0 (`$00000100`). The helper uses `ABCD -(a5),-(a6)`, so the predecrement changes the byte immediately **before** the special counter: the corresponding life counter.

```c
// a6 initially points to special counter
--a6;                 // now points to lives
*a6 = bcd_add(*a6, 1);
```

It plays the extra-life/reward sound and refreshes the lives/special HUD.

### Extra police special

Effect 3 at `$6A2A (apply_extra_special_pickup)` deliberately points `a6` one byte beyond the special counter: `$FFFF22 (p1_out_or_continue_flag)` for P1 or `$FFFF25 (p2_out_or_continue_flag)` for P2. The BCD helper's predecrement therefore lands on `$FFFF21/$FFFF24` and adds one police special. This is also why `$FFFF22/$FFFF25` must not be mistaken for the counter modified by the pickup.

### Score items

Effects 4 and 5 select an end pointer at `$FFFF0C` for P1 or `$FFFF14` for P2 and call the three-byte packed-BCD score adder at `$10DA6`:

- effect 4 uses table index `$0E`, packed value `$00003000`: 3,000 points;
- effect 5 uses table index `$0A`, packed value `$00010000`: 10,000 points.

`$10DA6` performs three chained `ABCD` operations and saturates an overflow at `$999900`. Score ownership again follows the collector object address (`$FFB800 (p1_object)` means P1; otherwise P2). These awards can immediately trigger the independent extra-life threshold check at `$4D60 (update_score_hud_and_check_extra_life)` on a subsequent player update.

## Weapon ownership and interaction protocol

Carried weapons use two linked records:

### Player fields

| Player offset | Meaning |
|---:|---|
| `+$5E` | Pointer to carried weapon object. |
| `+$60` | Carried weapon object type (`$08..$0C`), zero when unarmed. |

### Weapon fields

| Weapon offset | Meaning |
|---:|---|
| `+$34` | Weapon's active damage value. |
| `+$50` | Weapon-specific use/durability counter (not universal). |
| `+$51` | Ownership/action command: free/held/drop/throw phases. |
| `+$52` | Holder pointer (player or enemy). |
| `+$56` | Short lifetime/effect timer in broken states. |
| `+$7C/$7D` | Recorded collision/reaction data. |

The same proximity search at `$3136 (find_close_interaction_target)` accepts types `$08..$0C` only when weapon `+$51` is clear and its subtype is allowed. It then writes the player/weapon links, reserves the weapon, and changes the player to the pickup/grab action family at `$28`.

The common weapon positioning code at `$5E2E (update_held_weapon)` distinguishes a player holder (`holder->type == 1`) from an enemy holder. For players it verifies that `player->weapon_at_5e` still points back to the object, then uses:

- player action/animation at `+$08`;
- current animation frame at `+$0A`;
- character ID at `+$50`;
- facing bit;
- ROM attachment tables at `$5FC8` and `$60A0`;

to place and flip the weapon sprite in the character's hand each frame.
Enemy-held weapons use a more generic attachment table but the same holder
pointer. When an armed enemy enters a knockdown/drop transition, the ownership
command detaches the weapon, gives it ballistic motion, and eventually clears
`+$51` after it settles. The object is then eligible for the same `$3136 (find_close_interaction_target)` pickup
scan used for weapons originally placed on the ground. Therefore weapons are
independent objects even while they look like part of a character sprite.

### Ownership state transitions

The code supports these high-level phases:

```text
free/on ground
    |
    | player close-interaction search
    v
reserved/held (weapon +$52 = holder; player +$5E = weapon)
    |
    +---- normal weapon attack ---> follows holder animation; damage box active on selected frames
    |
    +---- holder knocked down ----> detach, apply small ballistic motion, settle on ground
    |
    +---- explicit drop command --> same detached/ground path
    |
    `---- throw command ----------> detach, apply X/Z velocity, become collision projectile
                                        |
                                        +--> hit/break/despawn
                                        `--> ground impact may exhaust wear or shatter
```

The byte at weapon `+$51` is a command/state handshake rather than a simple Boolean. Values observed around `$5D84/$5E2E` mean approximately: 0 free/settling, 1 held/used, 2 dropped, and 3 thrown. The exact moment at which the player state machine writes each value varies by weapon action.

### Durability by family

| Family | Rule | Code |
| --- | --- | --- |
| Bat / pipe | `+$50` use counter. While `+$50 < 3` and `+$51 == 1`, each held-use entry does `+$50++`. When `+$50 >= 3`, retire (hide, `+$56 = $10` frames, then delete). | `$5C66` |
| Knife | Shares `$5C66` in the state table, but player attack **throws** (`+$51 = 3` → `$5D84`). Solid hit deletes the object; ground settle at `$5DEA` forces `+$50 = 3` (no longer pickable). Effective lifetime is one throw arc, not three melee swings. | `$5D34`, `$5DEA`, `$21E6` |
| Bottle | One-way shatter on first impact (`+$54` guard); three shards; never re-collectable as a bottle. | `$614E` |
| Pepper | `+$50` is effect lifecycle, not a three-swing counter; canister becomes a timed smoke emitter. | `$6270+`, `$6328` |

Pickup eligibility always requires free (`+$51 == 0`) and **`+$50 < 3`**.

## Weapon damage and range (mathematical model)

### Outgoing damage constants

| Type | Name | Style | Init `+$34` | ROM init |
| ---: | --- | --- | ---: | --- |
| `$08` | Knife | Straight throw | **5** | `$5C54` `move.b #5, $34(a0)` |
| `$09` | Bottle | Throw / break | **3** | `$613C` `move.b #3, $34(a0)` |
| `$0A` | Baseball bat | Held melee | **4** | `$6214` `move.b #4, $34(a0)` |
| `$0B` | Steel pipe | Held melee | **4** | `$6244` `move.b #4, $34(a0)` |
| `$0C` | Pepper spray | Arc throw + immobilize | **2** | `$627A` `move.b #2, $34(a0)` |

Weapons never add a bonus onto the player's punch descriptor. Damage is the
weapon object's own `+$34`. Player max health is 80 (`$50`). The forced-duel
flag `$FFFA43` triples **player attack-descriptor** low nibbles only; it does
not rewrite these weapon constants.

### Damage application law

While damaging, a weapon is registered in the short attacker list at
`$FFFB22` (`$95CE` insert / `$95E8` remove). Ordinary enemies call `$AA22`,
resolve box overlap, then:

```c
// ordinary_enemy_apply_contact_damage ($9B88)
attacker = ptr(enemy[+0x3E]);
enemy.health_word_at_32 -= attacker[+0x34];
```

Players use the same `+$34` preference in `$351E (apply_player_damage)` (else
deferred `+$56`).

Pepper special case when collision result is `$05` and attacker type is `$0C`
(`$9C1E`):

```c
enemy.primary_state = 0x0400;   // immobilize / smoke reaction
// damage nibble remains 2; the extra effect is the forced state
```

Hits to kill ordinary enemies:

\[
\text{hits} = \left\lceil \frac{H}{D} \right\rceil
\]

with enemy health \(H\) from `$26FCE` (index `6*(type−$20)+variant`; hardest
difficulty adds +4 to \(H\)). Full matrices are in
[weapons-range-and-damage.md](weapons-range-and-damage.md) §6. Examples
(variant 0, Easy/Normal): Garcia `$20` \(H=6\) dies in 2 knife / 2 bat / 3
pepper hits; Jack `$27` \(H=9\) dies in 2 / 3 / 5.

### Throw spawn and velocities

Coordinate convention: object `+$18` (Z) **increases downward** toward the
floor. Ground test `$AD2A` is on-floor when `Z ≥ floor_height(map cell)`.
Upward motion uses **negative** Z velocity. Integration is `$B20E`
(`X += vx; Y += vy; Z += vz` as 16.16 longs).

Only **knife** and **pepper** are attack-thrown (`$21E6` → command 3). Bottle is
held/break-on-impact and is **not** released by that path.

| Quantity | Knife `$5D84` | Pepper `$62DA` |
| --- | ---: | ---: |
| X spawn delta vs **weapon** origin | \(\pm\$30\) (±48) by facing | \(\pm\$30\) (±48) |
| Z spawn delta vs **weapon** origin | `+$10` (+16) | `+$10` (+16) |
| \(v_x\) (`+$1C` high word) | \(\pm 16\) px/frame | \(\pm 6\) px/frame |
| \(v_z\) (`+$24` high word) | 0 | `−3` (up) |
| Gravity on \(v_z\) / frame | impact/fall paths `+$8800` ≈ 0.53 px/fr² | flight `+$A800` ≈ 0.66 px/fr² |

### Live measurements (Axel, Round 1, port 6969)

Standing feet \(Z_{\mathrm{stand}} = 160\).

#### Knife — natural ground pickup + attack throw

| Quantity | Value |
| --- | ---: |
| Wear at throw | 1 (usable; `+$50 < 3`) |
| Damage | 5 |
| Hold pose (ready) | \(w_x-p_x=-5\), \(w_z-p_z \approx -59\ldots-62\) |
| Launch \(\Delta x\) vs hold | **+48** (facing right) |
| Launch \(\Delta z\) vs hold | **+16** → launch \(Z=115\) |
| \(v_x, v_z\) | **+16, 0** |
| Flight | \(Z\) holds **115** while \(X\) runs; **≥160 px** horizontal from launch in sample |

\[
X_{\mathrm{launch}} = X_{\mathrm{hold}} \pm 48,\quad
Z_{\mathrm{launch}} = Z_{\mathrm{hold}} + 16,\quad
v_x = \pm 16,\quad v_z = 0
\]

Offsets are relative to the **attached weapon origin**, not the feet. A
forced launch of a ground-resting object uses that object's \(Z\) (often
\(Z_{\mathrm{stand}}\)) and can bounce early; in play the hand attach height is
what matters.

#### Pipe / bat — natural equip (pipe) and swings

| Quantity | Value |
| --- | ---: |
| Hold pose (pipe) | \(w_x-p_x=-3\), \(w_z-p_z=-61\) |
| Hit frames (in `$FFFB22`) | player action `$48` |
| Max origin \(\lvert w_x-p_x\rvert\) | **36** |
| At peak | \(w_z-p_z=-42\); damage 4 |

Bat matched the same **36 px** origin reach in the same session. Policy commit
band ≤36 px is therefore ROM-live for **Axel** long weapons.

#### Pepper (ROM + held launch path)

\(\Delta x=\pm48\), \(\Delta z=+16\), \(v_x=\pm6\), \(v_z=-3\). Immobilize: 160
frames (`$A43E`).

#### Knife bounce (ROM + forced path)

On ground impact in `$5D34`: \(v_x \leftarrow -v_x/4\) (live: \(-16\rightarrow+4\)).

### Melee reach (bat / pipe)

Bat and pipe remain linked via `$5E2E (update_held_weapon)`. On attachment
frames whose collision enable bit is set, the weapon registers in `$FFFB22`,
runs `$AA22`, and deals **4**. Attach tables: `$5FC8` (player), `$60A0` (enemy).

Collision shapes are 5-byte records at `$1A68E`. For shape id with bytes
\((b_0,b_1,\ldots)\):

```text
X_left  = origin_x + s8(b0)
X_right = X_left + u8(b1)     // width = b1
```

Long melee examples: shape `$06` width 44 forward; `$07` width 44 mirrored;
`$0C`/`$0D` width 40. Effective reach ≈ attach offset + shape extent (not a single constant).

Live Axel numbers are in the probe table above. Unarmed first-punch boxes
(autoplay): Axel ~57, Adam ~54, Blaze ~68. Co-op ally melee exclusion 80.

### Bottle shard velocities (type `$1E`)

Tables `$61B2` (Z) / `$61B8` (X); X sign flipped by facing. Gravity `+$8800`
per frame (`$61E0`). Treated as debris (not re-collectable weapons).

| Shard | \(v_z\) | \(v_x\) |
| ---: | ---: | ---: |
| 0 | −6 | +3 |
| 1 | −7 | +4 |
| 2 | −7 | −4 |

**Shards deal no damage.** Type `$1E` never writes `+$34`, never calls `$95CE`
(attacker-list registration), and only runs gravity + ground-delete
(`$61CA`/`$61E0`). Confirmed in ROM and by play observation.

### Weapon `+$51` command values (static)

| Value | Meaning | Producers / consumers |
| ---: | --- | --- |
| 0 | Free / settled on ground | Cleared by launch, drop settle, retire; required by `$3136` pickup |
| 1 | Reserved / held | `$3136` on pickup; enemy attach spawn; `$5C66` held-use gate |
| 2 | Drop / detach | `$5E2E` drop branch; enemy knockdown writes `#2` to held weapon |
| 3 | Throw command | `$21E6 (player_release_thrown_weapon)` on knife/pepper release frame; consumed by `$5D84` / `$62DA` |

### Pepper immobilize duration

Ordinary-enemy primary state word `$0400` selects shared handler `$A43E` (every
Garcia/Signal/Nora/Jack table entry 4). When `$FFFA1A (police_special_active)`
is clear (normal pepper hit, not police sweep):

```text
first entry:  enemy[+$50] = $A0   // 160 frames
each frame:   enemy[+$50]--
when zero:    enemy primary state → $0100  // resume normal AI
```

So immobilize lasts **160 frames** (≈ 2.67 s at 60 Hz). The police-special
path also forces `$0400` but pairs it with health `$FFFF` and sweep logic; that
is a different use of the same state index.

Pepper object `+$08` animation selectors observed in spawn paths: **4** (first
smoke object) and **6** (sequence emissions). Intact/held canister uses the
default init anim from table `$6FD42`.

## Individual weapon families

### Type `$08`: knife

The handler at `$5C1E (knife_weapon_dispatcher)` initializes damage `+$34 = 5`
(`$5C54`), the highest of the ordinary carried weapons. It supports ground
settling, holder attachment, a directed throw, collision bounce, and deletion.

The player action path at `$21E6-$222E` checks whether the carried object is type
`$08` (or the pepper weapon `$0C`). On the attack animation's release frame it
writes command 3 to weapon `+$51` and clears the player's carried-weapon type.
`$3084 (player_held_object_attack_input)` chooses the player action on B:

- **`$46`** if any object is in front within **`$90` (144)** px and lane
  `[Y−12, Y+12]` — **melee/stab** (weapon stays held; can deal damage without
  releasing);
- **`$44`** otherwise — **throw** anim.

`$21E6 (player_release_thrown_weapon)` only launches on family **`$44`** for
types `$08`/`$0C` (release frame). `$5D84` then adds \(\pm 48\) X and +16 Z to
the held origin and sets \(v_x=\pm 16\). Live Axel throw: launch
\(Z=Z_{\mathrm{hold}}+16\), level flight, ≥160 px travel observed.

On a solid hit the projectile is deleted; ground settle `$5DEA` can force
`+$50 = 3` (unpickable). Exhausted knives (`+$50 ≥ 3`) may remain held but do
not usefully attack/throw.

### Type `$09`: bottle

The bottle handler at `$6114 (bottle_weapon_dispatcher)` initializes damage 3
(`$613C`). When its collision result becomes nonzero for the first time, it
changes to broken art, plays the break sound, and spawns three objects of type
`$1E` with the velocities in the shard table above. Object `+$54` prevents the
shatter path from running twice.

The type `$1E` children use the small debris handler at `$61BE (bottle_shard_dispatcher)`: they move under gravity and delete on ground contact. They never install damage or register as attackers (no damage). The original bottle continues through common holder/drop code until its broken state is retired. This is a one-way transition; there is no path from shards back to a collectable bottle. `$21E6` does **not** attack-throw the bottle (only knife `$08` and pepper `$0C`).

### Types `$0A` and `$0B`: long melee weapons

The two handlers at `$61F6 (baseball_bat_weapon_dispatcher)` and `$6226 (steel_pipe_weapon_dispatcher)` differ mainly in art data (`$6FA9A` versus `$6FB5A`) and both initialize damage 4. Type `$0A` is the baseball bat and type `$0B` is the steel pipe. Their dispatch tables route through the shared `$5C66` wear path (three held uses), `$5CE4`, `$5D34`, `$5DDE` → `update_held_weapon` (not the knife throw launcher), and `$5DE0`. Unlike the bottle, they have no shatter flag or shard-spawn path.

The visual mapping was confirmed during gameplay. Mechanically the distinction
is small: both provide the same outgoing damage, can be dropped and collected
again while the shared use counter remains below 3, and use identical
impact/landing transitions. Melee collision is attachment-frame driven (§
Weapon damage and range).

### Type `$0C`: pepper spray and smoke

The `$6256 (pepper_spray_weapon_dispatcher)` handler initializes damage 2
(`$627A`). Its initial animation/state depends on object `+$08`, and a
used/thrown instance can become a short-lived effect emitter. The terminal path
sets a timer, spawns additional type-`$0C` objects with special animation
selectors (`+$08 = 4` or `6`), and emits a sequence of effect objects from a ROM
position table before deleting the source.

The player release path treats `$0C` like the knife and throws it from the
holder via `$62DA (throw_pepper_spray)`: spawn \(\pm 48\) X / +16 Z,
\(v_x=\pm 6\), \(v_z=-3\), then gravity `+$A800`. On impact, `$6328-$63C2`
converts the source into a timed emitter and creates additional type-`$0C`
objects with animation selectors 4 and 6 (visible pepper cloud/smoke). Enemies
hit with result `$05` from a type-`$0C` attacker enter primary state `$0400`,
handled by shared routine `$A43E` with a **160-frame** (`$A0`) timer on enemy
`+$50` before returning to state `$0100`.

## Collision, damage, and credit

Weapon objects participate in the shared object collision routines at `$97E6`
(on-screen gate) and `$AA22` / `$ABA4` (shape AABB via `$1A68E`). Damaging
phases register the weapon in `$FFFB22` through `$95CE`. Their `+$34` field is
the damage offered to a struck player/enemy, just as player attack frames expose
damage in player `+$34`. The common handlers use collision result `+$7C` and the
collided object returned in `a1` to:

- set the victim's reaction selector (`+$7D`);
- clear stale reciprocal collision state when an impact is ignored;
- bounce, break, or hide the weapon;
- preserve the holder/thrower pointer for attribution while the projectile is active.

The weapon remains an object rather than becoming a player damage bonus. This is why thrown weapons can continue moving and hit after they have visually left the player's hand.

## Breakable props, debris, and drops

### Type `$11`: telephone booth

`$6AF4 (phone_booth_dispatcher)` / `$6B0A` initializes the collision-enabled telephone booth. Its `+$31` byte selects fragment variants: zero is the intact booth; a nonzero value chooses an already shattered glass/booth fragment animation and begins in a later state.

On an accepted damaging collision (`$6B34 (shatter_phone_booth)`) the intact booth:

1. disables its intact collision bit;
2. plays the break sound;
3. allocates up to ten type-`$11` fragment objects;
4. copies position into each fragment;
5. assigns X and Z velocities from the table at `$6BD8`;
6. assigns fragment subtype/art through `+$31`;
7. enters a short broken-state timer, falls under gravity, and is cleared.

### Type `$19` breakable family

`$6C84/$6C96` initializes another collision-enabled prop. A valid attack chooses horizontal launch velocity from a small table, gives it upward velocity `$FFF7`, disables its intact collision, and enters a bouncing/despawn state. It does not emit the ten-fragment cloud used by type `$11`.

### How breakable containers reveal rewards

Telephone booths, crates, and similar breakable props visibly contain items or
weapons. The important code-level qualification is that neither local breakable
handler contains a generic item-type field, reward-table lookup, or call to one
of the six pickup constructors. The type `$11` telephone booth emits its glass
and booth debris, while type `$19`
changes its own physics.

The container/reward relationship is therefore implemented outside the local
prop structure. A complete parse of the regular wave blocks in all eight
Nemesis-decoded ELC streams finds type `$11` props in Round 1 and type `$19`
props in Round 2, but no direct records of weapon types `$08-$0C` or pickup
types `$3F/$40/$47/$4B/$4C/$4F`. This disproves the simple hypothesis that a
regular ELC block always places a collectible record beside its container.

Operationally the reward is inside the container, but statically it must enter
through another controller/conversion path or a separately consumed special
tail. The local `$11/$19` handlers still have no universal `drop_type` member,
and the decoded ROM data narrows where the missing producer can be sought.

## Spawning, despawning, and level behavior

Items and weapons enter through the ordinary level object stream and share the global 128-byte object pool. Initializers call `$6AA6`, which ORs level-specific flag bits into object `+$01`; this accounts for round-dependent orientation/priority behavior without separate item classes.

The common off-screen cleanup at `$6A70 (delete_pickup_behind_camera)` compares object X to the camera. In ordinary rounds, objects sufficiently behind the camera are deleted. Round 8 reverses/changes the boundary because its scrolling direction and arena behavior differ. Weapon-specific falling states also consume durability and delete objects that cannot settle, have exhausted their wear state, or reach a terminal timer.

There is no separate inventory array. At most one carried object is represented by the player's `+$5E/+$60` pair, while every ground or airborne weapon continues to consume a normal object-table slot.

## End-to-end pseudocode

```c
void player_close_interaction(Player *p) {
    if ((p->action & 0xfe) == 0x28)
        return;

    for (Object *o : object_table) {
        if (!inside_pickup_box(p, o))
            continue;

        if (o->type >= 0x08 && o->type <= 0x0c &&
            o->interaction == 0 && o->subtype < 3) {
            if (p->carried_type != 0)
                p->carried_object->interaction = 0;
            p->carried_type = o->type;
            p->carried_object = o;
            o->holder = p;
            o->interaction = 1;
            enter_pickup_weapon_action(p);
            return;
        }

        if (o->type == 0x47 || o->type == 0x4b ||
            o->type == 0x4c || o->type == 0x4f ||
            o->type == 0x3f || o->type == 0x40) {
            o->collector = p;
            o->interaction = 1;
            enter_pickup_item_action(p);
            return;
        }
    }
}

void apply_pickup_effect(Player *p, unsigned effect) {
    switch (effect) {
    case 0: adjust_health(p, 80); break;
    case 1: adjust_health(p, 20); break;
    case 2: bcd_increment(p->lives); break;
    case 3: bcd_increment(p->specials); break;
    case 4: add_bcd_score(p, 3000); break;
    case 5: add_bcd_score(p, 10000); break;
    }
    refresh_relevant_hud(p);
}
```

## Evidence map

| Reference | Analytical role |
| --- | --- |
| `$21E6 (player_release_thrown_weapon)` | Commands a carried knife or pepper spray to detach on the attack-animation release frame. |
| `$3136 (find_close_interaction_target)` | Finds free weapon types `$08-$0C` and six consumable pickup types in the ±20/±16/±8 box. |
| `$5C1E (knife_weapon_dispatcher)` | Type-$08 knife dispatcher; damage 5 at `$5C54`. |
| `$5C66` | Shared bat/pipe three-use wear (`+$50`); retire when `+$50 >= 3`. |
| `$5D34` | Active throw impact: collide, bounce \(v_x \leftarrow -v_x/4\), or delete on hit. |
| `$5D84 (launch_released_weapon)` | Command-3 launch: spawn ±48 X / +16 Z, \(v_x=\pm 16\). |
| `$5DEA` | Ground settle forces `+$50 = 3` (exhaust pickability). |
| `$5E2E (update_held_weapon)` | Shared held/drop/throw ownership, attach tables `$5FC8`/`$60A0`, collision enable. |
| `$6114 (bottle_weapon_dispatcher)` | Type-$09 bottle dispatcher; damage 3 at `$613C`. |
| `$614E (break_bottle_into_shards)` | Bottle break and three-shard spawn (vel tables `$61B2`/`$61B8`). |
| `$61BE (bottle_shard_dispatcher)` | Type-$1E bottle-shard/debris dispatcher; gravity `+$8800`. |
| `$61F6 (baseball_bat_weapon_dispatcher)` | Type-$0A baseball-bat dispatcher; damage 4 at `$6214`. |
| `$6226 (steel_pipe_weapon_dispatcher)` | Type-$0B steel-pipe dispatcher; damage 4 at `$6244`. |
| `$6256 (pepper_spray_weapon_dispatcher)` | Type-$0C pepper dispatcher; damage 2 at `$627A`. |
| `$62DA (throw_pepper_spray)` | Pepper launch: ±48 X / +16 Z, \(v_x=\pm 6\), \(v_z=-3\). |
| `$6312` / `$6328` / `$6372` | Pepper flight gravity `+$A800`, smoke emission sequence. |
| `$95CE` / `$95E8` | Register/unregister weapon in attacker list `$FFFB22`. |
| `$9B88 (ordinary_enemy_apply_contact_damage)` | `health -= attacker[+$34]`; pepper branch `$9C1E` → state `$0400`. |
| `$AA22` / `$ABA4` / `$1A68E` | Attacker-list collision and shape AABB table. |
| `$26FCE (ordinary_enemy_combat_value_table)` | Enemy HP/damage by type×variant for hits-to-kill. |
| `$67A4 (wave_go_prompt_dispatcher)` | Type-$48 screen-space prompt flashed after a wave camera boundary opens. |
| `$6862 (player_contact_effect_dispatcher)` | Type-$49 short-lived player/contact visual effect emitted by player attack/collision helpers. |
| `$69CC (consume_collected_pickup)` | Converts a reserved pickup into an effect on its collector, then deletes it. |
| `$69E6 (dispatch_pickup_effect)` | Dispatches pickup effect index 0..5. |
| `$6A04 (apply_health_pickup)` | Full/small health pickup effects. |
| `$6A14 (apply_extra_life_pickup)` | Extra-life pickup effect. |
| `$6A2A (apply_extra_special_pickup)` | Extra-special pickup effect. |
| `$6A46 (apply_score_pickup)` | 3,000/10,000 score pickup effects. |
| `$6A70 (delete_pickup_behind_camera)` | Camera-relative off-screen cleanup shared by pickups. |
| `$6AF4 (phone_booth_dispatcher)` | Type-$11 intact telephone-booth and fragment dispatcher. |
| `$6B34 (shatter_phone_booth)` | Shatter the telephone booth and emit up to ten glass/booth fragments. |
| `$6C84 (breakable_type19_dispatcher)` | Type-$19 breakable/bouncing prop dispatcher. |
| `$10DA6 (sub_00010DA6)` | Add three-byte packed-BCD score value. |
| `$10DCA (add_bcd_resource_value)` | Add one-byte packed-BCD life/special value using predecrement. |

## Code-label confirmation audit

The formerly conservative confidence values on the weapon/prop entry points
are now 100% for the contracts stated in `labels.csv`. The confirmation is
structural: `$21E6 (player_release_thrown_weapon)` issues command 3 only to
carried types `$08/$0C`; `$5D84 (launch_released_weapon)` consumes command 3
and installs facing-dependent position/velocity; `$5E2E (update_held_weapon)`
owns the shared holder link and attach/drop/throw transitions. The type
dispatcher fixes `$6114 (bottle_weapon_dispatcher)`, `$61BE (bottle_shard_dispatcher)`,
and `$6256 (pepper_spray_weapon_dispatcher)` to types `$09/$1E/$0C`.
`$614E (break_bottle_into_shards)` changes the bottle art and creates exactly
three type-`$1E` objects, while the shard handler applies gravity and deletes
on landing. Finally, `$6A70 (delete_pickup_behind_camera)` contains the explicit
normal/Round-8 boundary comparison, and `$6C84 (breakable_type19_dispatcher)`
is the type-`$19` state dispatcher whose impact path installs bounce velocity,
timer, airborne flag, and eventual deletion.

This does not resolve the separate visual question of which ELC container owns
every hidden reward; that relationship is external to the local type-`$19`
dispatcher and remains correctly listed below.

## Resolution status (combat math)

| Topic | Status | Evidence |
| --- | --- | --- |
| Damage 5 / 3 / 4 / 4 / 2 | **Closed** | ROM inits `$5C54`…`$627A` |
| Knife/pepper attack throw | **Closed** | `$21E6` → `$5D84` / `$62DA`; bottle **not** thrown |
| Launch \(\Delta x=\pm48\), \(\Delta z=+16\) vs **hold** | **Closed** | ROM + natural Axel knife |
| Knife \(v_x=\pm16\), flight ≥160 px sample | **Closed** | Natural throw, level \(Z=115\) |
| Pipe/bat origin reach 36 px (Axel) | **Closed** | Natural pipe equip + swings in `$FFFB22` |
| Pepper immobilize 160 frames | **Closed** | `$A43E` timer `$A0` |
| Shards non-damaging | **Closed** | `$61BE` no `+$34` / no `$95CE` |
| `+$51` ∈ {0,1,2,3} | **Closed** | Static writers |
| Adam/Blaze long-weapon reach | Deferred | Unmeasured; not required for base model |
| Hang-time per map cell | Deferred | Launch law fixed; `$AD2A` floor varies |
| Booth/crate reward producer | Open | Outside `$11`/`$19` handlers |

Companion matrices and formulas:
[weapons-range-and-damage.md](weapons-range-and-damage.md).
