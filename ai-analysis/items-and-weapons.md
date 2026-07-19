# Items, Pickups, Breakable Props, and Weapons

## Scope and terminology

This document describes the gameplay objects that can be collected, carried, swung, thrown, broken, or converted into player resources. It is based primarily on `output/sor.asm`, especially the object handlers from `$5C1E` through `$6C84`, the player interaction search at `$3136`, and the packed-BCD resource helpers at `$10DA6/$10DCA`.

The code makes a useful distinction that is easy to lose in a visual description of the game:

- a **consumable pickup** is linked to a player and immediately converted into health, a life, a police special, or score;
- a **weapon object** remains in the object table, records its holder, and follows the holder's animation until dropped or thrown;
- a **breakable prop** receives ordinary attack collisions and emits debris, but does not itself contain a generic reward/drop field.

Some visual names, especially the assignment of object types `$0A` and `$0B` to bat versus pipe, are medium-confidence. Their mechanics are identical enough that static code identifies them as two long melee weapons but does not spell out their art names.

## Object-type map

The global object dispatcher at `$B236` indexes a word table by object type. The relevant entries are:

| Object type | Handler | Role |
|---:|---:|---|
| `$08` | `$5C1E` | Knife-like carried/thrown weapon; damage 5 and a three-use counter. |
| `$09` | `$6114` | Bottle; damage 3, breaks and emits three shard objects. |
| `$0A` | `$61F6` | Long melee weapon A (bat/pipe); damage 4. |
| `$0B` | `$6226` | Long melee weapon B (pipe/bat); damage 4. |
| `$0C` | `$6256` | Pepper shaker / pepper-effect weapon; damage 2 and effect-emission states. |
| `$11` | `$6AF4` | Breakable prop/container family; emits ten debris objects of the same type with fragment subtypes. |
| `$19` | `$6C84` | Second breakable prop family; launches/bounces when struck, then despawns. |
| `$1E` | `$61BE` | Bottle-shard/debris projectile emitted by type `$09`. |
| `$3F` | `$68E2` | 3,000-point pickup. |
| `$40` | `$6904` | 10,000-point pickup. |
| `$47` | `$6988` | Full-health food pickup (`+$50`, 80 health; visually the large food). |
| `$4B` | `$6926` | Small-health food pickup (`+$14`, 20 health; visually the apple). |
| `$4C` | `$6948` | Extra-life pickup. |
| `$4F` | `$6968` | Extra police-special pickup. |

The six pickup handlers are tiny wrappers. Each chooses an item-effect index in object `+$50`, installs its art/animation pointer, and then enters the shared pickup logic at `$699E`.

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

Pickups are acquired through the same close-interaction search that begins normal attacks. `sub_3136` builds a small three-dimensional box around the player and scans the object table. It first recognizes carried-object types `$08..$0C`; it also explicitly accepts pickup types `$47`, `$4B`, `$4C`, `$4F`, `$3F`, and `$40`.

For a consumable it does not populate the player's carried-weapon fields. It only writes:

```c
pickup->collector_at_52 = player;
pickup->interaction_at_51 = 1;
```

On the pickup's next update, `$69CC` sees the nonzero interaction byte, temporarily changes `a0` to the collecting player, dispatches by pickup `+$50`, and deletes the pickup object after the effect returns.

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

Effects 0 and 1 enter `$6A04/$6A08` with signed health deltas `$50` and `$14`. Both call the shared player health routine at `$4E6C`, which clamps object `+$32` to `0..$50` and redraws the correct player's bar. The result is:

- large food: restore up to 80 units, effectively full health from any nonnegative value;
- small food: restore 20 units;
- neither can raise health above 80.

Both use the same health-pickup sound.

### Extra life

Effect 2 at `$6A14` selects `p1_special_attacks` or `p2_special_attacks` as `a6`, then calls `$10DCA` with BCD table entry 0 (`$00000100`). The helper uses `ABCD -(a5),-(a6)`, so the predecrement changes the byte immediately **before** the special counter: the corresponding life counter.

```c
// a6 initially points to special counter
--a6;                 // now points to lives
*a6 = bcd_add(*a6, 1);
```

It plays the extra-life/reward sound and refreshes the lives/special HUD.

### Extra police special

Effect 3 at `$6A2A` deliberately points `a6` one byte beyond the special counter: `$FFFF22` for P1 or `$FFFF25` for P2. The BCD helper's predecrement therefore lands on `$FFFF21/$FFFF24` and adds one police special. This is also why `$FFFF22/$FFFF25` must not be mistaken for the counter modified by the pickup.

### Score items

Effects 4 and 5 select an end pointer at `$FFFF0C` for P1 or `$FFFF14` for P2 and call the three-byte packed-BCD score adder at `$10DA6`:

- effect 4 uses table index `$0E`, packed value `$00003000`: 3,000 points;
- effect 5 uses table index `$0A`, packed value `$00010000`: 10,000 points.

`$10DA6` performs three chained `ABCD` operations and saturates an overflow at `$999900`. Score ownership again follows the collector object address (`$B800` means P1; otherwise P2). These awards can immediately trigger the independent extra-life threshold check at `$4D60` on a subsequent player update.

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

The same proximity search at `$3136` accepts types `$08..$0C` only when weapon `+$51` is clear and its subtype is allowed. It then writes the player/weapon links, reserves the weapon, and changes the player to the pickup/grab action family at `$28`.

The common weapon positioning code at `$5E2E` distinguishes a player holder (`holder->type == 1`) from an enemy holder. For players it verifies that `player->weapon_at_5e` still points back to the object, then uses:

- player action/animation at `+$08`;
- current animation frame at `+$0A`;
- character ID at `+$50`;
- facing bit;
- ROM attachment tables at `$5FC8` and `$60A0`;

to place and flip the weapon sprite in the character's hand each frame. Enemy-held weapons use a more generic attachment table but the same holder pointer. Therefore weapons are independent objects even while they look like part of a character sprite.

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
    +---- drop command -----------> detach, apply small ballistic motion, settle on ground
    |
    `---- throw command ----------> detach, apply X/Z velocity, become collision projectile
                                        |
                                        +--> hit/break/despawn
                                        `--> land and become reusable if weapon type permits
```

The byte at weapon `+$51` is a command/state handshake rather than a simple Boolean. Values observed around `$5D84/$5E2E` mean approximately: 0 free/settling, 1 held/used, 2 dropped, and 3 thrown. The exact moment at which the player state machine writes each value varies by weapon action.

## Individual weapon families

### Type `$08`: knife-like weapon

The handler at `$5C1E` initializes damage `+$34 = 5`, the highest of the ordinary carried weapons here. It supports ground settling, holder attachment, a directed throw with X velocity, collision bounce, and eventual deletion.

Object `+$50` is a use counter. While it is below 3, a new held-use phase increments it. At 3 it enters a terminal state, hides/disables normal collision, starts a `$10`-frame timer in `+$56`, and deletes itself when the timer expires. Static code therefore gives this weapon three counted uses; whether a particular pickup/throw animation consumes a count can be confirmed by watching `+$50` during play.

### Type `$09`: bottle

The bottle handler at `$6114` initializes damage 3. When its collision result becomes nonzero for the first time, it changes to broken art, plays the break sound, and spawns three objects of type `$1E` with different X/Z velocities. Object `+$54` prevents the shatter path from running twice.

The type `$1E` children use the small debris handler at `$61BE`: they move under gravity and delete on ground contact. The original bottle continues through common holder/drop code until its broken state is retired. This is a one-way transition; there is no path from shards back to a collectable bottle.

### Types `$0A` and `$0B`: long melee weapons

The two handlers at `$61F6` and `$6226` differ mainly in art data (`$6FA9A` versus `$6FB5A`) and both initialize damage 4. They reuse the common ownership, attachment, drop, throw, and collision code without a local durability counter or shatter sequence.

The object order and art organization strongly indicate the baseball bat and metal pipe, but assigning `$0A` to one and `$0B` to the other is only medium confidence without rendering the two art streams. Mechanically the distinction is small in this code region: both provide the same outgoing damage and persistent/reusable behavior.

### Type `$0C`: pepper weapon/effect

The `$6256` handler initializes damage 2. Its initial animation/state depends on object `+$08`, and a used/thrown instance can become a short-lived effect emitter. The terminal path sets a timer, spawns additional type-`$0C` objects with special animation selectors (`+$08 = 4` or `6`), and emits a sequence of effect objects from a ROM position table before deleting the source.

This behavior, coupled with the low damage and multiple emitted sprites, identifies the pepper shaker/powder family more strongly than a conventional impact weapon. A framebuffer trace would still be useful to name each subtype (shaker, powder puff, and lingering effect) exactly.

## Collision, damage, and credit

Weapon objects participate in the shared object collision routines at `$97E6` and `$AA22`. Their `+$34` field is the damage offered to a struck player/enemy, just as player attack frames expose damage in player `+$34`. The common handlers use collision result `+$7C` and the collided object returned in `a1` to:

- set the victim's reaction selector (`+$7D`);
- clear stale reciprocal collision state when an impact is ignored;
- bounce, break, or hide the weapon;
- preserve the holder/thrower pointer for attribution while the projectile is active.

The weapon remains an object rather than becoming a player damage bonus. This is why thrown weapons can continue moving and hit after they have visually left the player's hand.

## Breakable props, debris, and drops

### Type `$11` breakable family

`$6AF4/$6B0A` initializes a collision-enabled breakable prop. Its `+$31` byte selects fragment variants: zero is the intact object; a nonzero value chooses an already broken/debris animation and begins in a later state.

On an accepted damaging collision (`$6B34`) the intact object:

1. disables its intact collision bit;
2. plays the break sound;
3. allocates up to ten type-`$11` fragment objects;
4. copies position into each fragment;
5. assigns X and Z velocities from the table at `$6BD8`;
6. assigns fragment subtype/art through `+$31`;
7. enters a short broken-state timer, falls under gravity, and is cleared.

### Type `$19` breakable family

`$6C84/$6C96` initializes another collision-enabled prop. A valid attack chooses horizontal launch velocity from a small table, gives it upward velocity `$FFF7`, disables its intact collision, and enters a bouncing/despawn state. It does not emit the ten-fragment cloud used by type `$11`.

### Are rewards stored inside containers?

Neither breakable handler contains a generic item-type field, reward table lookup, or call to one of the six pickup constructors. Type `$11` spawns debris only; type `$19` changes its own physics only. Therefore the static code does **not** support a universal "container owns drop X" model.

Where a visible pickup appears to come from a breakable prop, the likely level-engine arrangement is that the pickup is separately described or co-located and its visibility/activation is coordinated by object/script flags. This should be confirmed per level descriptor, but it is important not to invent a drop pointer in the prop structure.

## Spawning, despawning, and level behavior

Items and weapons enter through the ordinary level object stream and share the global 128-byte object pool. Initializers call `$6AA6`, which ORs level-specific flag bits into object `+$01`; this accounts for round-dependent orientation/priority behavior without separate item classes.

The common off-screen cleanup at `$6A70` compares object X to the camera. In ordinary rounds, objects sufficiently behind the camera are deleted. Round 8 reverses/changes the boundary because its scrolling direction and arena behavior differ. Weapon-specific falling states also delete objects that cannot settle or whose terminal timer expires.

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

| Address | Current symbol | Analytical role |
|---:|---|---|
| `$3136` | `sub_00003136` | Finds weapons and consumable pickups in the player's close-interaction box. |
| `$5C1E` | `sub_00005C1E` | Type-$08 knife-like weapon dispatcher. |
| `$5E2E` | `loc_00005E2E` | Shared held/drop/throw ownership and attachment logic. |
| `$6114` | `sub_00006114` | Type-$09 bottle dispatcher. |
| `$614E` | `sub_0000614E` | Bottle break and three-shard spawn. |
| `$61BE` | `sub_000061BE` | Type-$1E bottle-shard/debris dispatcher. |
| `$61F6` | `sub_000061F6` | Type-$0A long melee weapon dispatcher. |
| `$6226` | `sub_00006226` | Type-$0B long melee weapon dispatcher. |
| `$6256` | `sub_00006256` | Type-$0C pepper weapon/effect dispatcher. |
| `$69CC` | `sub_000069CC` | Converts a reserved pickup into an effect on its collector, then deletes it. |
| `$69E6` | `sub_000069E6` | Dispatches pickup effect index 0..5. |
| `$6A04` | `sub_00006A04` | Full/small health pickup effects. |
| `$6A14` | `sub_00006A14` | Extra-life pickup effect. |
| `$6A2A` | `sub_00006A2A` | Extra-special pickup effect. |
| `$6A46` | `sub_00006A46` | 3,000/10,000 score pickup effects. |
| `$6A70` | `loc_00006A70` | Camera-relative off-screen cleanup shared by pickups. |
| `$6AF4` | `sub_00006AF4` | Type-$11 breakable prop dispatcher. |
| `$6B34` | `sub_00006B34` | Break type-$11 prop and emit ten debris objects. |
| `$6C84` | `sub_00006C84` | Type-$19 breakable/bouncing prop dispatcher. |
| `$10DA6` | `sub_00010DA6` | Add three-byte packed-BCD score value. |
| `$10DCA` | `sub_00010DCA` | Add one-byte packed-BCD life/special value using predecrement. |

## Uncertainties and recommended traces

1. Render art streams `$6FA9A` and `$6FB5A` to assign object types `$0A/$0B` definitively to baseball bat versus pipe.
2. Watch type `$08` `+$50` across pickup, melee use, throw, enemy hit, and ground recovery to state exactly which events consume one of its three uses.
3. Capture type `$0C` states with object `+$08` values 0, 4, and 6 to distinguish the shaker sprite from powder/projectile subtypes.
4. Trace a visually hidden reward behind a type `$11` or `$19` prop back to its level descriptor. Static prop handlers have no drop pointer, so the coordination must occur outside their local break routines.
5. Name weapon interaction values `+$51 = 0..3` only after a per-frame trace across player pickup, enemy pickup, drop, and throw; the high-level phases are clear, but some values are momentary commands rather than durable states.
