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

## Object-type map

The global object dispatcher at `$B236` indexes a word table by object type. The relevant entries are:

| Object type | Handler | Role |
|---:|---:|---|
| `$08` | `$5C1E (knife_weapon_dispatcher)` | Knife; damage 5, limited use count, and a straight-line attack-button throw. |
| `$09` | `$6114 (bottle_weapon_dispatcher)` | Bottle; damage 3, breaks and emits three shard objects. |
| `$0A` | `$61F6 (baseball_bat_weapon_dispatcher)` | Baseball bat; damage 4. |
| `$0B` | `$6226 (steel_pipe_weapon_dispatcher)` | Steel pipe; damage 4. |
| `$0C` | `$6256 (pepper_spray_weapon_dispatcher)` | Thrown pepper spray; damage 2, immobilizing reaction, and smoke/effect-emission states. |
| `$11` | `$6AF4 (phone_booth_dispatcher)` | Telephone booth; shatters into up to ten type-$11 glass/booth fragments. |
| `$19` | `$6C84 (breakable_type19_dispatcher)` | Second breakable prop family; launches/bounces when struck, then despawns. |
| `$1E` | `$61BE (bottle_shard_dispatcher)` | Bottle-shard/debris projectile emitted by type `$09`. |
| `$3F` | `$68E2` | 3,000-point pickup. |
| `$40` | `$6904` | 10,000-point pickup. |
| `$47` | `$6988` | Full-health food pickup (`+$50`, 80 health; visually the large food). |
| `$4B` | `$6926` | Small-health food pickup (`+$14`, 20 health; visually the apple). |
| `$4C` | `$6948` | Extra-life pickup. |
| `$4F` | `$6968` | Extra police-special pickup. |

The six pickup handlers are tiny wrappers. Each chooses an item-effect index in object `+$50`, installs its art/animation pointer, and then enters the shared pickup logic at `$699E`.

## Confirmed visible behavior and code interpretation

The following player-visible behavior is confirmed and resolves several points
that static code alone leaves visually anonymous:

- pressing attack with the knife throws it horizontally in a straight line;
- the baseball bat is type `$0A`; the steel pipe is type `$0B`; both use almost
  identical long-weapon handlers at `$61F6/$6226`;
- pepper spray is thrown, produces smoke/powder objects, and leaves the struck
  enemy locked in a reaction state for a short period;
- enemies can carry weapon objects; knocking an armed enemy down detaches the
  weapon, which falls to the floor and can then be collected by the player;
- weapons have finite durability, with ground impacts contributing to wear;
- telephone booths, crates, and other breakable scenery can reveal items or
  weapons when destroyed.

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

Pickups are acquired through the same close-interaction search that begins normal attacks. `$3136 (find_close_interaction_target)` builds a small three-dimensional box around the player and scans the object table. It first recognizes carried-object types `$08..$0C`; it also explicitly accepts pickup types `$47`, `$4B`, `$4C`, `$4F`, `$3F`, and `$40`.

For a consumable it does not populate the player's carried-weapon fields. It only writes:

```c
pickup->collector_at_52 = player;
pickup->interaction_at_51 = 1;
```

On the pickup's next update, `$69CC (consume_collected_pickup)` sees the nonzero interaction byte, temporarily changes `a0` to the collecting player, dispatches by pickup `+$50`, and deletes the pickup object after the effect returns.

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
                                        `--> ground impact consumes wear; become reusable if durability remains
```

The byte at weapon `+$51` is a command/state handshake rather than a simple Boolean. Values observed around `$5D84/$5E2E` mean approximately: 0 free/settling, 1 held/used, 2 dropped, and 3 thrown. The exact moment at which the player state machine writes each value varies by weapon action.

Durability is not universal across all five weapon types, but the knife,
baseball bat, and steel pipe do share one exact rule. Their state tables all
enter `$5C66`, which increments `+$50` when a held-use command begins and
retires the weapon when the counter has reached 3. The bottle instead has a
one-way intact-to-shattered transition; pepper spray uses `+$50` for its effect
lifecycle rather than this three-use rule.

## Individual weapon families

### Type `$08`: knife

The handler at `$5C1E (knife_weapon_dispatcher)` initializes damage `+$34 = 5`, the highest of the ordinary carried weapons here. It supports ground settling, holder attachment, a directed throw with X velocity, collision bounce, and eventual deletion.

The player action path at `$21E6-$222E` checks whether the carried object is type
`$08` (or the pepper weapon `$0C`). On the attack animation's release frame it
writes command 3 to weapon `+$51` and clears the player's carried-weapon type.
`$5D84 (launch_released_weapon)` then detaches the knife and assigns signed horizontal velocity according
to facing. This is the straight-line knife throw produced by the attack button.

Object `+$50` is a use counter. While it is below 3, a new held-use phase increments it. At 3 it enters a terminal state, hides/disables normal collision, starts a `$10`-frame timer in `+$56`, and deletes itself when the timer expires. Static code therefore gives this weapon three counted uses; whether a particular pickup/throw animation consumes a count can be confirmed by watching `+$50` during play.

### Type `$09`: bottle

The bottle handler at `$6114 (bottle_weapon_dispatcher)` initializes damage 3. When its collision result becomes nonzero for the first time, it changes to broken art, plays the break sound, and spawns three objects of type `$1E` with different X/Z velocities. Object `+$54` prevents the shatter path from running twice.

The type `$1E` children use the small debris handler at `$61BE (bottle_shard_dispatcher)`: they move under gravity and delete on ground contact. The original bottle continues through common holder/drop code until its broken state is retired. This is a one-way transition; there is no path from shards back to a collectable bottle.

### Types `$0A` and `$0B`: long melee weapons

The two handlers at `$61F6 (baseball_bat_weapon_dispatcher)` and `$6226 (steel_pipe_weapon_dispatcher)` differ mainly in art data (`$6FA9A` versus `$6FB5A`) and both initialize damage 4. Type `$0A` is the baseball bat and type `$0B` is the steel pipe. Their six-entry dispatch tables are identical after the family-specific initializer and route through the knife's shared `$5C66`, `$5CE4`, `$5D34`, `$5DDE`, and `$5DE0` states. They therefore use the same `+$50` three-use limit; unlike the bottle, they have no shatter flag or shard-spawn path.

The visual mapping was confirmed during gameplay. Mechanically the distinction
is small: both provide the same outgoing damage, can be dropped and collected
again while the shared use counter remains below 3, and use identical
impact/landing transitions.

### Type `$0C`: pepper spray and smoke

The `$6256 (pepper_spray_weapon_dispatcher)` handler initializes damage 2. Its initial animation/state depends on object `+$08`, and a used/thrown instance can become a short-lived effect emitter. The terminal path sets a timer, spawns additional type-`$0C` objects with special animation selectors (`+$08 = 4` or `6`), and emits a sequence of effect objects from a ROM position table before deleting the source.

The player release path treats `$0C` like the knife and throws it from the
holder. On impact, `$6328-$63C2` converts the source into a timed emitter and
creates additional type-`$0C` objects with animation selectors 4 and 6. These
are the visible pepper cloud/smoke sequence. The collision reaction written to
the enemy keeps it in a non-controllable reaction state while the effect runs,
which is the observed temporary immobilization rather than ordinary knockback.

## Collision, damage, and credit

Weapon objects participate in the shared object collision routines at `$97E6` and `$AA22`. Their `+$34` field is the damage offered to a struck player/enemy, just as player attack frames expose damage in player `+$34`. The common handlers use collision result `+$7C` and the collided object returned in `a1` to:

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
    for (Object *o : object_table) {
        if (!inside_pickup_box(p, o))
            continue;

        if (o->type >= 0x08 && o->type <= 0x0c &&
            o->interaction == 0 && o->subtype < 3) {
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
| `$3136 (find_close_interaction_target)` | Finds free weapon types `$08-$0C` and six consumable pickup types in the player's close-interaction box. |
| `$5C1E (knife_weapon_dispatcher)` | Type-$08 knife dispatcher and counted-use lifecycle. |
| `$5D84 (launch_released_weapon)` | Detaches and launches a command-3 weapon according to holder facing. |
| `$5E2E (update_held_weapon)` | Shared held/drop/throw ownership and attachment logic. |
| `$6114 (bottle_weapon_dispatcher)` | Type-$09 bottle dispatcher. |
| `$614E (break_bottle_into_shards)` | Bottle break and three-shard spawn. |
| `$61BE (bottle_shard_dispatcher)` | Type-$1E bottle-shard/debris dispatcher. |
| `$61F6 (baseball_bat_weapon_dispatcher)` | Type-$0A baseball-bat dispatcher. |
| `$6226 (steel_pipe_weapon_dispatcher)` | Type-$0B steel-pipe dispatcher. |
| `$6256 (pepper_spray_weapon_dispatcher)` | Type-$0C thrown pepper-spray and smoke/effect dispatcher. |
| `$62DA (throw_pepper_spray)` | Applies the pepper-spray throw position and X/Z velocity. |
| `$6328 (begin_pepper_smoke_emission)` | Converts an impact into the first smoke/effect object. |
| `$6372 (emit_pepper_smoke_sequence)` | Emits the remaining smoke/effect sequence. |
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

## Uncertainties and recommended traces

1. Map every type-`$0C` animation selector to the thrown canister, smoke/powder cloud, and lingering immobilization frames.
2. Trace the producer that creates or converts hidden rewards; the regular ELC
   wave records contain the props but no direct collectible-type records.
3. Name weapon interaction values `+$51 = 0..3` only after a per-frame trace across player pickup, enemy pickup, knockdown drop, and throw; the high-level phases are clear, but some values are momentary commands rather than durable states.
