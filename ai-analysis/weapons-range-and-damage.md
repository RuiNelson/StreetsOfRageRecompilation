# Weapons: range and damage (mathematical model)

## Scope and method

Closed-form model of Streets of Rage carried weapons (object types `$08`–`$0C`)
derived from `output/sor.asm`, ROM bytes in `rom/SOR.bin`, and the existing
item/combat analyses. Addresses are ROM offsets unless prefixed with `FF`.

Narrative context, ownership protocol, and pickups live in
**[items-and-weapons.md](items-and-weapons.md)** (which also restates the core
constants). This file keeps the full hits-to-kill matrices, shape catalogue,
and formula summary.

Primary evidence:

| Topic | Location |
| --- | --- |
| Knife / shared wear & throw | `$5C1E (knife_weapon_dispatcher)`, `$5C66`, `$5D34`, `$5D84 (launch_released_weapon)`, `$5DEA` |
| Bottle / shards | `$6114 (bottle_weapon_dispatcher)`, `$614E (break_bottle_into_shards)`, `$61BE (bottle_shard_dispatcher)` |
| Bat / pipe | `$61F6 (baseball_bat_weapon_dispatcher)`, `$6226 (steel_pipe_weapon_dispatcher)` (`move.b #4, +$34`) |
| Pepper throw / smoke | `$6256 (pepper_spray_weapon_dispatcher)`, `$62DA (throw_pepper_spray)`, `$6312`, `$6372 (emit_pepper_smoke_sequence)` |
| Pickup search box | `$3136 (find_close_interaction_target)` |
| Attacker registration | `$95CE` / `$95E8` (`$FFFB22` list) |
| Box test | `$ABA4` / `$ABF8` / `$AC78`, shapes `$1A68E` |
| Enemy damage application | `$9B88 (ordinary_enemy_apply_contact_damage)` |
| Enemy HP / contact damage | `$26FCE (ordinary_enemy_combat_value_table)` |

---

## 1. Inventory identity

| Type | Name | Attack style | Init `+$34` damage | ROM init |
| ---: | --- | --- | ---: | --- |
| `$08` | Knife | Melee **or** throw (ROM `$3084 (player_held_object_attack_input)` picks `$46`/`$44`) | **5** | `$5C54`: `move.b #5, $34(a0)` |
| `$09` | Bottle | Held / break on impact (not attack-thrown) | **3** | `$613C`: `move.b #3, $34(a0)` |
| `$0A` | Baseball bat | Held melee swing | **4** | `$6214`: `move.b #4, $34(a0)` |
| `$0B` | Steel pipe | Held melee swing | **4** | `$6244`: `move.b #4, $34(a0)` |
| `$0C` | Pepper spray | Arc throw + smoke / immobilize | **2** | `$627A`: `move.b #2, $34(a0)` |

Damage ranking (raw): **knife 5 > bat/pipe 4 > bottle 3 > pepper 2**.

Player max health is **80** (`$50`). Ordinary-enemy health is a small byte
loaded from `$26FCE (ordinary_enemy_combat_value_table)` (see §6). Police special deals a fixed **10** to supported
bosses (separate system).

---

## 2. Damage application law

Weapons never add a bonus onto the player's punch descriptor. They are
independent objects that publish their own outgoing damage:

```text
weapon.outgoing_damage = weapon[+$34]          # constant for the weapon type
```

When the weapon is in a damaging phase it is registered in the short attacker
list at `$FFFB22` (`$95CE`). Ordinary enemies call `$AA22`, resolve box overlap,
then:

```c
// ordinary_enemy_apply_contact_damage ($9B88)
attacker = ptr(enemy[+0x3E]);
enemy.health -= attacker[+0x34];   // word health at +$32, subtract byte damage
```

Same field is used against players via `$351E (apply_player_damage)` (prefer attacker
`+$34`, else deferred `+$56`).

### Pepper special case

If the collision result code is `$05` and the attacker type is `$0C`:

```c
enemy.primary_state = 0x0400;   // immobilization / smoke reaction, not ordinary knockdown
// (loc at $9C1E; similar boss/alt path at $A9DC)
```

Pepper still carries damage **2**; the extra effect is the forced reaction
state, not a larger nibble. Shared ordinary-enemy handler `$A43E` (primary
state index 4) loads `enemy[+$50] = $A0` (**160 frames**) then returns to
state `$0100` when the timer expires (only when police special is not active).

### Duel modifier (P1 vs P2 only)

`$FFFA43 (duel_damage_modifier)` triples the **player attack-descriptor** low
nibble modulo 16. Weapon object `+$34` values above are not rewritten by that
path; they remain the constants in §1.

---

## 3. Durability / uses

| Weapon | Rule | Code |
| --- | --- | --- |
| Bat, pipe (and shared path) | `+$50` is a use counter. While `+$50 < 3` and command `+$51 == 1`, each held-use entry does `+$50++` and continues. When `+$50 >= 3`, weapon retires (hide, timer `+$56 = $10` frames, then delete). | `$5C66` |
| Knife | Same wear routine is in the state table, but player attack **throws** (command `+$51 = 3` → `$5D84 (launch_released_weapon)`). On solid hit the object is deleted; on ground settle `$5DEA` forces `+$50 = 3` (no longer pickable). Effective lifetime is one throw arc, not three swings. | `$5D34`, `$5DEA`, `$21E6 (player_release_thrown_weapon)` |
| Bottle | One-way shatter: first impact sets `+$54`, swaps art, spawns **3** type-`$1E` shards, no re-collect as bottle. | `$614E (break_bottle_into_shards)` |
| Pepper | `+$50` used for effect lifecycle (thrown instance can force `+$50 = 3` on special anims). Canister becomes timed smoke emitter. | `$6270+`, `$6328 (begin_pepper_smoke_emission)` |

Pickup eligibility (`$3136 (find_close_interaction_target)`): weapon free (`+$51 == 0`) and **`+$50 < 3`**.

---

## 4. Geometry: pickup, throw spawn, speeds

### 4.1 Pickup / grab search box (`$3136 (find_close_interaction_target)`)

Accept first matching object slot whose **origin** lies in:

| Axis | Interval relative to player origin |
| --- | --- |
| X | `[player.x − 20, player.x + 20]`  (`$14` each side, width `$28`) |
| Lane Y | `[player.y − 16, player.y + 16]` |
| Height Z | `[player.z − 8, player.z + 8]` |

No distance ranking: slot order wins.

### 4.2 Throw spawn offsets (relative to **held weapon** origin)

Knife launcher `$5D84 (launch_released_weapon)` and pepper `$62DA
(throw_pepper_spray)`. Bottle is **not** attack-thrown (`$21E6` only `$08`/`$0C`).

| Quantity | Facing right | Facing left |
| --- | ---: | ---: |
| X spawn delta | `+$30` (48) | `−$30` (−48) |
| Z spawn delta | `+$10` (16) | `+$10` (16) |

Coordinate convention for `+$18` (Z): **increases downward** (toward the floor).
Ground test `$AD2A` reports “on floor” when `Z ≥ floor_height(map cell)`.
Jump/upward motion uses **negative** Z velocity (e.g. pepper `vz = −3`).

### 4.3 Throw velocities (integer high word of 16.16 longs)

| Weapon | `vx` (`+$1C`) | `vz` (`+$24`) | Gravity on `vz` |
| --- | ---: | ---: | --- |
| Knife | `±16` px/frame | `0` | impact/fall `+$8800` ≈ 0.53125 px/fr² |
| Pepper | `±6` px/frame | `−3` (up) | flight `+$A800` ≈ 0.65625 px/fr² |

Position integration (`$B20E`): `X += vx; Y += vy; Z += vz` (16.16).

### 4.4 Launch geometry (ROM + live, final)

Offsets apply to the **attached weapon origin** at release, not the feet:

\[
X_{\mathrm{launch}} = X_{\mathrm{hold}} \pm 48,\quad
Z_{\mathrm{launch}} = Z_{\mathrm{hold}} + 16
\]

**Natural Axel knife** (fresh ground pickup, wear 1, Round 1, port 6969):

| Quantity | Value |
| --- | ---: |
| Feet \(Z_{\mathrm{stand}}\) | 160 |
| Hold \(\Delta x, \Delta z\) vs player | −5, ≈ −60 |
| Launch vs hold | \(\Delta x=+48\), \(\Delta z=+16\) → \(Z=115\) |
| \(v_x, v_z\) | +16, 0 |
| Flight | level at \(Z=115\); ≥160 px horizontal from launch |

**Pepper** (ROM + held launch): \(\Delta x=\pm48\), \(\Delta z=+16\), \(v_x=\pm6\),
\(v_z=-3\).

**Bounce** (knife `$5D34`): \(v_x \leftarrow -v_x/4\) (forced-path live: −16→+4).

Policy heuristics in `autoplay`: throwable mid-range **20–100** px; ally throw
exclusion **140** px.

### 4.5 Bottle shards (type `$1E`)

Three shards, tables at `$61B2` / `$61B8`:

| Shard | `vz` | `vx` (sign flipped if facing) |
| ---: | ---: | ---: |
| 0 | −6 | +3 |
| 1 | −7 | +4 |
| 2 | −7 | −4 |

Gravity on shards: `+$8800` per frame (`$61E0`). **No damage:** type `$1E`
never writes `+$34` and never registers via `$95CE`. Confirmed by ROM and play
observation. Not collectible weapons.

---

## 5. Melee reach (bat / pipe)

Bat and pipe stay linked to the holder (`$5E2E (update_held_weapon)`). On
animation frames whose attachment record has the collision enable bit, the
weapon:

1. is registered in `$FFFB22` (`$95CE`);
2. runs `$AA22` against players / listed objects;
3. deals **`+$34 = 4`** on a valid enemy contact.

Sprite attach tables: `$5FC8` (player holder), `$60A0` (enemy holder). Attach
bytes reposition the weapon on the character’s hand each frame (facing flips X).

### Shape AABB model (`$1A68E`, 5-byte records)

For shape id \(s\), bytes \((b_0,b_1,b_2,b_3,b_4)\):

```text
X_left  = origin_x + s8(b0)
X_right = X_left + u8(b1)          // width = b1
// lane/height use b2 as index into extent pairs at $1AB8E, then (b3,b4) for Z
```

Long forward boxes used by wide melee (examples):

| Shape | X offset | Width | Notes |
| ---: | ---: | ---: | --- |
| `$06` | 0 | 44 | Forward-long |
| `$07` | −44 | 44 | Backward-long (mirror) |
| `$0C` | +8 | 40 | Forward |
| `$0D` | −48 | 40 | Backward |
| `$12`/`$13` | 0 / −40 | 40 | Forward / back |

Effective hit reach ≈ **attach offset + shape extent**, not a single constant.
Empirical first-punch body boxes (unarmed) measured for autoplay:

| Character | First-punch reach (facing right) |
| --- | ---: |
| Axel | ~57 px |
| Adam | ~54 px |
| Blaze | ~68 px |

**Live Axel** (natural steel-pipe equip + swings; bat matched in session):

| Quantity | Value |
| --- | ---: |
| Hold \(\Delta x, \Delta z\) | −3, −61 |
| Hit frames | in `$FFFB22`, player action `$48` |
| Max \(\lvert w_x-p_x\rvert\) | **36** |
| Peak \(\Delta z\) | −42 |

Co-op ally melee exclusion **80 px**. Policy commit ≤36 px matches origin lag.
Adam/Blaze unmeasured (deferred).

---

## 6. Hits-to-kill (ordinary enemies)

Enemy init (`$93CE (ordinary_enemy_init_combat_values)`): health low byte from `$26FCE (ordinary_enemy_combat_value_table)`, index
`6*(type−$20) + variant`. Highest difficulty adds **+4** to health and to the
enemy’s own contact damage.

\[
\text{hits} = \left\lceil \frac{H}{D_w} \right\rceil
\]

### 6.1 Baseline health (Easy/Normal, no +4)

| Type | Name (variant 0 / 1 / 2 HP) | Knife 5 | Bottle 3 | Bat/pipe 4 | Pepper 2 |
| ---: | --- | --- | --- | --- | --- |
| `$20` | Garcia 6 / 9 / 11 | 2 / 2 / 3 | 2 / 3 / 4 | 2 / 3 / 3 | 3 / 5 / 6 |
| `$21` | Garcia 4 / 7 / 9 | 1 / 2 / 2 | 2 / 3 / 3 | 1 / 2 / 3 | 2 / 4 / 5 |
| `$22` | Garcia 4 / 7 / 9 | 1 / 2 / 2 | 2 / 3 / 3 | 1 / 2 / 3 | 2 / 4 / 5 |
| `$23` | Garcia 6 / 9 / 11 | 2 / 2 / 3 | 2 / 3 / 4 | 2 / 3 / 3 | 3 / 5 / 6 |
| `$24` | Signal 4 / 7 / 9 | 1 / 2 / 2 | 2 / 3 / 3 | 1 / 2 / 3 | 2 / 4 / 5 |
| `$25` | Haku-Ro 4 / 7 / 9 | 1 / 2 / 2 | 2 / 3 / 3 | 1 / 2 / 3 | 2 / 4 / 5 |
| `$26` | Nora 7 / 11 / 14 | 2 / 3 / 3 | 3 / 4 / 5 | 2 / 3 / 4 | 4 / 6 / 7 |
| `$27` | Jack 9 / 14 / 17 | 2 / 3 / 4 | 3 / 5 / 6 | 3 / 4 / 5 | 5 / 7 / 9 |
| `$2A` | Haku-Ro 7 / 11 / 14 | 2 / 3 / 3 | 3 / 4 / 5 | 2 / 3 / 4 | 4 / 6 / 7 |

### 6.2 Hardest difficulty (HP + 4)

Add 4 to every \(H\) above, then re-ceil. Examples (variant 0):

| Enemy | H | Knife | Bottle | Bat/pipe | Pepper |
| --- | ---: | ---: | ---: | ---: | ---: |
| Garcia `$20` | 10 | 2 | 4 | 3 | 5 |
| Signal `$24` | 8 | 2 | 3 | 2 | 4 |
| Nora `$26` | 11 | 3 | 4 | 3 | 6 |
| Jack `$27` | 13 | 3 | 5 | 4 | 7 |

Boss HP and weapon interaction are separate (boss damage paths at
`$17C36 (boss_apply_pending_damage)` etc.); not expanded here.

---

## 7. Control / utility scores (not ROM, policy)

For AI preference only (`autoplay` fuzzy values), not cartridge math:

| Type | Base value | Notes |
| ---: | ---: | --- |
| Bat / pipe | 0.82 | Best sustained melee |
| Knife | 0.62 | Highest damage, one throw |
| Pepper | 0.52 | Low damage, immobilize |
| Bottle | 0.32 | One-way break |

Character modifiers: preferred weapons +0.12; Blaze weak (knife/bottle) −0.20.
Upgrade threshold: new value ≥ held + 0.08.

---

## 8. End-to-end formulas (summary)

```text
// Damage on successful weapon collision
D = weapon[+$34] ∈ {5, 3, 4, 4, 2}  // knife, bottle, bat, pipe, pepper

// Enemy KO
hits = ceil(H / D)   // H from $26FCE; hardest difficulty H += 4

// Knife $08: $3084 picks action $46 if front object |ΔX|<$90 (144), else $44
// Throw release ($21E6) only on family $44 for knife/pepper (not bottle)
X_launch = X_hold ± 48
Z_launch = Z_hold + 16
// knife throw:  vx = ±16, vz = 0
// pepper throw: vx = ±6,  vz = -3; then vz += 0xA800/65536 per frame

// Knife bounce on ground impact
vx := -vx / 4

// Pickup
|dx|≤20 ∧ |dy|≤16 ∧ |dz|≤8 ∧ type∈[8..12] ∧ +$51==0 ∧ +$50<3

// Bat/pipe hit (Axel live)
in $FFFB22 ∧ |w_x - p_x| ≤ 36   // origin lag on connecting frames
```

---

## 9. Status

Combat model **complete** for ordinary weapons (ROM + Axel live). Deferred only:
Adam/Blaze long-weapon reach, per-cell hang-time, booth/crate loot producer.

| Claim | Status | Evidence |
| --- | --- | --- |
| Damage 5/3/4/4/2 | Closed | ROM inits |
| Knife melee `$46` / throw `$44` (same B) | Closed | `$3084 (player_held_object_attack_input)` front cone `$90`; `$21E6 (player_release_thrown_weapon)` only on `$44` |
| Launch vs **hold** ±48 X, +16 Z | Closed | `$5D84 (launch_released_weapon)`/`$62DA (throw_pepper_spray)` + natural knife |
| Knife flight ≥160 px sample, \(Z=115\) | Closed | natural throw |
| Pipe/bat origin reach 36 (Axel) | Closed | natural pipe + bat |
| Pepper immobilize 160 fr | Closed | `$A43E` |
| Shards no damage | Closed | `$61BE (bottle_shard_dispatcher)` |
| `+$51` 0..3 | Closed | static |

## 10. Evidence checklist

| Claim | Evidence |
| --- | --- |
| Damage 5/3/4/4/2 | `$5C54`, `$613C`, `$6214`, `$6244`, `$627A` |
| Knife \(v_x=\pm16\); pepper \(v_x=\pm6,v_z=-3\) | `$5D84 (launch_released_weapon)`, `$62DA (throw_pepper_spray)`; live knife |
| Launch vs hold origin | natural Axel knife: hold→launch +48 X, +16 Z |
| Bounce \(-v_x/4\) | `$5D34`; forced live −16→+4 |
| Bat/pipe 36 px (Axel) | natural pipe in `$FFFB22`, action `$48` |
| Wear three uses (bat/pipe) | `$5C66` |
| Pickup ±20/±16/±8 | `$3136 (find_close_interaction_target)` |
| Enemy HP table | `$26FCE (ordinary_enemy_combat_value_table)` + `$93CE (ordinary_enemy_init_combat_values)` |
| Pepper `$0400` + 160 fr | `$9C1E` → `$A43E` `$A0` |
| Shards inert | `$61BE (bottle_shard_dispatcher)` no `+$34` / `$95CE` |
| Attacker list | `$95CE` / `$FFFB22` |
