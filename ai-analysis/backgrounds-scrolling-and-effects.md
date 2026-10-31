# Streets of Rage Backgrounds, Scrolling, and Background Effects

## Summary

*Streets of Rage* draws every gameplay background with only the two Mega Drive
scroll planes. Plane A (VRAM `$C000`) belongs to the **primary camera**
`$FFE000 (primary_camera)`, which follows the players. Plane B (VRAM E000h)
belongs to the **secondary camera** `$FFE100 (secondary_camera)`, which usually
derives its position from the primary one. Depth comes from cutting each plane
into horizontal **strips**. Each strip scrolls at a fixed `numerator /
denominator` fraction of camera X, so one plane can carry several parallax
layers.

The pipeline has four stages:

1. **Level build.** `$19848 (load_level_graphics_maps_and_camera)` decompresses
   the round's tile art, two metatile dictionaries, the collision map, a
   palette, and two plane layouts. A layout is a list of strips, and each strip
   is a row of 256 × 256-pixel *chunks*. They are flattened into byte block maps
   at `$FF0000 (primary_plane_blockmap)` and `$FF2000 (secondary_plane_blockmap)`.
   One block is a 16 × 16-pixel metatile (four VDP tile words).
2. **Per-frame camera update.** `$18AF8 (update_cameras_and_queue_tilemaps)`
   runs one state machine per camera (11 primary states, 8 secondary states).
   The states cover player follow, wave corridors, the police-special camera
   excursion, and the Round 7 elevator. They then compare each strip's current
   and previous position. A strip that crossed a 16-pixel boundary queues one
   new metatile column or row.
3. **Scroll buffers.** `$190E0 (build_hscroll_strip_buffer)` converts camera X
   into one h-scroll word per 8-pixel cell band. Some rounds expand those bands
   to 256 per-line values or replace them with a procedural line effect.
4. **VBlank.** The handler writes both vertical-scroll words to VSRAM and the
   two base h-scroll words to F400h/`$F402`. It then picks one of three
   upload paths from the primary camera state: cell-band DMA, line DMA, or
   name-table updates only. The queued metatile columns and rows are written in
   every case.

Each round uses this machinery differently. The configuration comes from ROM
tables decoded for this manuscript:

| Round | VDP h-scroll | Plane A | Plane B | Special behaviour |
|---:|---|---|---|---|
| 1 | full screen | 1/1 | 1/1 (no parallax) | blinking palette colours |
| 2 | full screen | 1/1 | 1/4 | none |
| 3 | per line | top 96 px at 1/2, rest 1/1 | 1/4, 1/2, 1/1 bands | animated shoreline tiles and palettes, flashing colour, shake-corrected bands |
| 4 | per line | 1/1 | 1/2 plus a travelling line ripple | procedural ripple below line 98 |
| 5 | per cell | 1/1, deck bobs vertically | four bands drifting 1-4 px/frame on their own | ship bob, frozen Plane B |
| 6 | per cell | 1/1 | 3/4 top, 1/4 bottom | three palette cycles |
| 7 | full screen | elevator platform | 1280 × 1536 px shaft that scrolls vertically | elevator climb with jolts |
| 8 | per cell | 1/1, scrolls **leftward** | 1/2 | palette stages triggered by camera progress |

There are also global effects: an alternating ±3-pixel vertical screen shake;
the police special's 16 px/frame camera excursion, which moves away and back;
table-driven palette animation; and two rounds with ambient sound tied to the
scenery. The game state stays where the ROM puts it. Everything above lives in
work RAM at the addresses listed here, so `autoplay` and other observers can
read it directly.

---

## Scope and method

This manuscript covers the gameplay background: how it is built, scrolled,
streamed, uploaded, and animated. It goes deeper than the camera and parallax
sections of `graphics-engine.md` (§5-7) and `game-engine-and-levels.md` (§6),
which give the surrounding engine. Sprites, the HUD/Window plane, and the
generic palette fade are only mentioned where they touch the background.

Evidence:

- `output/sor.asm`, regenerated after the CSV updates listed in §14;
- ROM bytes, read only where the listing shows data tables as `org` gaps. These
  include the camera jump tables, the per-level package and layout tables, the
  bounds table, and the effect tables. Every decoded value quoted here came
  from `rom/SOR.bin` with a small script that read big-endian words;
- no live run was needed. Visual interpretations, such as "the sea" or "the
  elevator shaft", are labelled as interpretations. Arithmetic statements come
  directly from the code.

Conventions: "Round *N*" is level index *N*−1 (`$FFFF02 (level)`). Positions are
in pixels unless a value is called 16.16. A *block* is a 16 × 16-pixel metatile,
a *chunk* is 16 × 16 blocks, and a *cell* is an 8 × 8-pixel VDP tile.

---

## 1. Hardware frame of reference

The runtime VDP registers (`$7FA4 (runtime_vdp_register_values)`) set up:

| Item | Value |
|---|---|
| Plane A name table | VRAM `$C000`, 64 × 32 cells = 512 × 256 px |
| Plane B name table | VRAM E000h, 64 × 32 cells |
| Window plane | VRAM `$D000` (HUD; see `graphics-engine.md` §10) |
| H-scroll table | VRAM F400h; long `n` holds Plane A (word 0) and Plane B (word 1) for line `n` |
| Vertical scroll | full screen: VSRAM word 0 = Plane A, word 1 = Plane B |
| Display | H40, 320 × 224 |

R11 selects how the VDP reads the h-scroll table. `$19848
(load_level_graphics_maps_and_camera)` writes it once per round from `$199B6
(level_hscroll_mode_table)`:

| R11 | Mode | VDP reads | Rounds |
|---|---|---|---|
| `$8B00` | full screen | the first long only | 1, 2, 7 |
| `$8B02` | per cell | one long per 8 lines (every `$20` bytes) | 5, 6, 8 |
| `$8B03` | per line | 256 consecutive longs | 3, 4 |

The round-clear screen at `$18226` writes `$8B00` again before it draws.

A positive h-scroll value moves the plane right, so every producer below writes
`-x`. A positive VSRAM value moves plane content up.

---

## 2. Level background data

### 2.1 The per-level graphics package

`$5F5B8 (level_graphics_package_table)` holds eight relative offsets. Each
package that `$19848 (load_level_graphics_maps_and_camera)` reads contains:

```text
2 x { VDP write command (L), Kosinski art pointer (L) }   ; tile art
Enigma pointer -> $FF4000 (primary_metatile_table)
Enigma pointer -> $FF4800 (secondary_metatile_table)
Kosinski pointer -> $FFA000 (level_collision_class_map)
palette-list pointer -> target palette (load_palette_list_to_target)
primary plane header:   stride, word +$30, layout mode, X, Y, state
secondary plane header: stride, word +$30, layout mode, X, Y, state
```

Each art stream is decoded into `$FF8000 (decompression_scratch_buffer)`. A
fixed 151 calls of `$10496 (vdp_copy_long_38)` then push it to the port. The
helper copies 32 longs (its name is older than the measured count), so each
call moves 128 bytes and each stream moves 19,328 bytes (604 tiles). The
first stream always starts at VRAM `$0000`. The second starts at a per-round
address: `$1400` for Round 1, `$1740`, `$0E00`, `$1320`, `$0FA0`, `$1420`,
`$09C0`, and `$1B00` for Round 8.

`$199AA (level_graphics_buffer_destinations)` lists the three destinations of
the map decode loop, which uses Enigma for both metatile tables and Kosinski for
the collision map. Rounds 1, 2, 3, 5, and 8 share one collision-map stream
(`$5FD1A`). Rounds 4, 6, and 7 have their own.

Decoded plane headers:

| Round | Stride (blocks) | Layout mode | Start X | Start Y | Primary state | Secondary state |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | `$150` (336) | 0 | `$300` | 0 | 1 | 1 |
| 2 | `$150` | 0 | `$300` | 0 | 1 | 1 |
| 3 | `$150` | 0 | `$300` | 0 | 0 | 0 |
| 4 | `$150` | 0 | `$300` | 0 | 2 | 2 |
| 5 | `$150` | 0 | `$300` | 0 | 0 | 0 |
| 6 | `$150` | 0 | `$300` | 0 | 0 | 0 |
| 7 | `$50` (80) | 2 | `$300` | 0 / `$500` | 3 | 3 |
| 8 | `$140` (320) | 1 | `$12C0` | 0 | 0 | 0 |

The header word stored at `+$30` is `$10`, or `$60` in Round 7. None of the
camera routines read it.

### 2.2 Bounds by layout mode

`$19922 (init_camera_plane)` takes the initial bounds from `$1998A
(plane_layout_bounds_table)`, indexed by the layout mode. In Round 7 the primary
plane adds one row and so uses mode 3:

| Mode | Min Y | Max Y | Min X | Max X | Used by |
|---:|---:|---:|---:|---:|---|
| 0 | 0 | 0 | `$300` | `$4C0` | Rounds 1-6 |
| 1 | 0 | 0 | `$1100` | `$12C0` | Round 8 |
| 2 | `$500` | `$500` | `$300` | `$300` | Round 7 secondary (shaft) |
| 3 | 0 | 0 | `$300` | `$300` | Round 7 primary (platform) |

The first corridor of Rounds 1-6 is therefore X `$300`-`$4C0`. That is exactly
the first entry of `$195D8 (wave_camera_boundary_table)`, so the world starts
768 px into the map and the chunks to the left are scenery that is never
reachable.

### 2.3 Plane layouts: strips of chunks

`$199C6 (build_camera_plane_buffers)` reads `$5F788 (level_plane_layout_table)`,
indexed by `level * 2 + plane`:

```text
Kosinski pointer  -> chunk art (each chunk = $100 bytes = 16 x 16 block IDs)
strip count - 1   -> camera +$44
per strip:
    height - 1 (blocks; negative ends the list)
    numerator, denominator  (signed words; the parallax ratio)
    first block row
    chunk indexes ... -1    (laid left to right)
    row advance              (bytes to add to the map pointer = height * stride)
```

In the normal layout, each chunk row is copied as 16 bytes into its map row.
Map rows are `$150` bytes apart (`$140` in Round 8), and the next chunk goes 16
bytes further right.
A Round 1-6 strip row holds 21 chunks (336 blocks = 5,376 px), and Round 8 holds
20 chunks (`$140` blocks). Both block maps are cleared first (16 KB of
`$103D0 (memfill_long_128)`).

**Round 8 is stored reversed.** Its chunk lists are `19, 18, ..., 0`, so chunk 0
(the start of the stage) sits at the far right, where the camera starts (X
`$12C0`). The player then walks left.

**Layout mode 2 (Round 7)** skips the strip loop. It reads 30 chunk indexes as
a row-major 5 × 6 grid (column offsets `$19AC0`, row offsets `$19ACA`). The
result is an 80 × 96-block map, 1,280 × 1,536 px, with stride `$50`. Plane A
uses only the top chunk row (chunks 1, 2, 3 in grid columns 2-4). Plane B
places two chunks per row in columns 3-4: chunks 17/18 at the top, then 15/16,
13/14, 11/12, 9/10, and a bottom row of chunks 4-8 across all five columns.
With the camera at X `$300` (column 3), the shaft scene is the column-3/4 pair
of each row. The bottom row also covers columns 0-2, which only the police
excursion shows (§11.6).

### 2.4 Strip tables by round

Decoded from `$5F788 (level_plane_layout_table)`. Rows count from the top of the
plane in blocks (16 px).

| Round | Plane | Strip rows | Ratio | Chunk pattern |
|---:|---|---|---|---|
| 1 | A | 0-14 | 1/1 | 0-3 repeating |
| 1 | B | 0-14 | 1/1 | 0-11 sequence |
| 2 | A | 0-14 | 1/1 | 0-11 sequence |
| 2 | B | 0-14 | 1/2 *(state 1 uses 1/4, §7.2)* | chunk 0 repeated |
| 3 | A | 0-5 | 1/2 | 0-4 repeating |
| 3 | A | 6-14 | 1/1 | 5-10 repeating |
| 3 | B | 0-5 | 1/4 | chunk 0 repeated |
| 3 | B | 6-8 | 1/2 | 1-5 repeating |
| 3 | B | 9-14 | 1/1 | chunk 6 repeated |
| 4 | A | 0-14 | 1/1 | 0-11 sequence |
| 4 | B | 0-14 | 1/2 *(the ripple ignores it, §11.3)* | 0, 1 alternating |
| 5 | A | 0-14 | 1/1 | 0 ×4, then 1-17 |
| 5 | B | 0-4 | 1/8 + drift | 0, 1 alternating |
| 5 | B | 5-6 | 1/4 + drift | chunk 2 repeated |
| 5 | B | 7 | 1/2 + drift | chunk 3 repeated |
| 5 | B | 8-9 | 3/4 + drift | chunk 4 repeated |
| 6 | A | 0-14 | 1/1 | 0-17 sequence |
| 6 | B | 0-3 | 3/4 | 0, 1 alternating |
| 6 | B | 4-14 | 1/4 | 2-5 repeating |
| 7 | A/B | grid | 1/1 | see §2.3 |
| 8 | A | 0-14 | 1/1 | 19 → 0 |
| 8 | B | 0-14 | 1/2 | chunk 0 repeated |

Fifteen block rows (240 px) cover the 224-line display. Distant layers are
built from one or two repeating chunks. A 256- or 512-pixel period divides the
512-pixel name-table width, so those layers stay correct even when they are
never re-streamed (§8.4).

### 2.5 Metatiles

Each 8-byte record in `$FF4000 (primary_metatile_table)` or `$FF4800
(secondary_metatile_table)` holds four VDP name-table words: top-left,
top-right, bottom-left, bottom-right. Tile index, palette line, flip, and
priority all come from the record, so the block map only stores identity. The
camera keeps pointers to the map and the dictionary at `+$2A` and `+$32`
(`$FFE02A (primary_plane_blockmap_ptr)`, `$FFE032 (primary_metatile_table_ptr)`,
and the secondary equivalents).

### 2.6 The physical layer beside the visual one

`$19BD8 (init_level_collision_tables_and_background_effects)` runs at the end of
the level load. It first stores two per-level table pointers from `$19C04
(level_collision_class_table_offsets)` into `$FFFC34
(collision_class_attribute_table_ptr)` and `$FFFC38
(collision_class_height_table_ptr)`. Rounds 1-5 share one pair; Rounds 6, 7,
and 8 each have their own. The collision query near `$AD30` reads a nibble from
`$FFA000 (level_collision_class_map)`, using `$FFE02E
(primary_plane_blockmap_stride)` as its row stride. It returns the class byte
only when the height is at least the class word. The visual map and the
physical map share coordinates and stride but are separate data. The routine
then dispatches the per-level effect initializer (§10).

---

## 3. The camera-plane structure

Both cameras use one `$100`-byte layout. `$804C (clear_camera_structures)`
clears both before every scene build. Primary aliases exist in
`addresses.csv`; the secondary uses the same offsets from `$FFE100 (secondary_camera)`.

| Offset | Size | Meaning |
|---:|---|---|
| `+$00` | W | state index (dispatch, and the VBlank upload selector for the primary) |
| `+$02` | L | X, 16.16 (`$FFE002 (cam_x)`, `$FFE102 (secondary_cam_x)`) |
| `+$06` | L | previous X (`$FFE006 (camera_prev_x)`) |
| `+$0A` | L | X velocity, cleared after every update (`$FFE00A (camera_x_velocity)`) |
| `+$0E` | L | Y, 16.16 (`$FFE00E (camera_y)`, `$FFE10E (secondary_camera_y)`) |
| `+$12` | L | previous Y |
| `+$16` | L | Y velocity, cleared after every update |
| `+$1A` / `+$1E` | L | max / min X (`$FFE01A (camera_x_max)`, `$FFE01E (camera_x_min)`) |
| `+$22` / `+$26` | L | max / min Y |
| `+$2A` | L | block map pointer |
| `+$2E` | W | block map stride |
| `+$30` | W | header word (no reader found) |
| `+$32` | L | metatile dictionary pointer |
| `+$42` | W | layout mode (0/1/2) |
| `+$44` | W | strip count − 1 |
| `+$46 + $12·k` | `$12` | strip descriptor *k* (below) |
| `+$8C` / `+$90` | L | police snapshot X / Y |
| `+$94` | W | police snapshot state |
| `+$96 .. +$A5` | 4 L | police snapshot max X, min X, max Y, min Y |
| `+$A6` | B | primary, Round 7: "secondary is returning" flag |
| `+$A8` / `+$A9` | B | secondary, Round 5: bob timer / reload |
| `+$AA` | L | secondary, Round 5: bob acceleration (±`$800`) |
| `+$AE` | L | secondary, Round 5: bob velocity added to `$FFE00E (camera_y)` |
| `+$B2` | B | stream columns at Y 0 and never stream rows |
| `+$B3` | B | never stream anything for this plane |

Strip descriptor (`+$46` onward; built in §2.3 and seeded by `$19CC6
(initialize_parallax_strip_state)`):

| Offset | Meaning |
|---:|---|
| `+$0` | height − 1, in blocks |
| `+$2` | first block row in the map |
| `+$4` | map width − 1, in blocks (stride − 1) |
| `+$6` / `+$8` | ratio numerator / denominator |
| `+$A` / `+$C` | strip X, current / previous |
| `+$E` / `+$10` | strip Y, current / previous |

X integer words are masked with `$3FFF` after every integration, so world X wraps
at 16,384 px.

---

## 4. Where the background runs in a frame

During gameplay, `$1087A (game_mode_ingame)` calls `$AD8E
(update_objects_and_build_sprites)`. In game states `$14`/`$16`, that routine
updates both player objects, resolves player-vs-player collision, and calls
`$18AF8 (update_cameras_and_queue_tilemaps)`. It then calls `$43AA
(clamp_players_to_gameplay_bounds)` against the new camera. The camera
therefore always sees the players' positions from the current frame, and the
players are clamped to the camera rather than the other way round.

Around the object pass, `$1087A (game_mode_ingame)` also runs `$112F2
(dispatch_per_level_ambient_effects)`, `$11210 (update_animated_palettes)`, and,
afterwards, `$11346 (update_level_index2_background_effect)`.

During the round-intro fade (`$FFFA1F (level_intro_active)`), the same camera
update runs from `$1087A (game_mode_ingame)`'s intro branch. Round 8 waits until
`$FFFB0C (palette_fade_counter)` drops below `$3F`.

`$18AF8 (update_cameras_and_queue_tilemaps)`:

```text
dispatch_primary_camera(primary)        ; state machine, §5
queue_camera_tilemap_updates(primary)   ; §8
dispatch_secondary_camera(secondary)    ; §7.2
queue_camera_tilemap_updates(secondary)
primary.vx = primary.vy = secondary.vx = secondary.vy = 0
```

Velocities are recomputed every frame. Camera motion has no momentum.

---

## 5. The primary camera state machine

`$18C22 (dispatch_primary_camera)` jumps through `$18C34
(primary_camera_state_table)`. The table has 11 entries; the words that follow
it are code. The same state also selects the VBlank upload path through `$196B2
(background_vblank_upload_table)`, which repeats every four states:

| State | Handler | Behaviour | VBlank path |
|---:|---|---|---|
| 0 | `$18C4A (primary_camera_state_follow_strips)` | follow, then rebuild strip buffer | cell-band DMA |
| 1 | `$18C70 (primary_camera_state_follow_flat)` | follow, single flat strip | name tables only |
| 2 | `$18C70 (primary_camera_state_follow_flat)` | same, for a line-scroll round | line DMA |
| 3 | `$18C92 (primary_camera_state_round7_vertical)` | Round 7: X and Y follow, elevator jolt | name tables only |
| 4 | `$18CC4 (primary_camera_state_police_out_strips)` | excursion out, with strips | cell-band DMA |
| 5, 6 | `$18D46 (primary_camera_state_police_out_flat)` | excursion out, flat | names / line DMA |
| 7 | `$18D62 (primary_camera_state_round7_police_follow)` | Round 7 excursion: copies secondary motion | name tables only |
| 8 | `$18DC2 (primary_camera_state_police_return_strips)` | excursion back, with strips | cell-band DMA |
| 9, 10 | `$18E20 (primary_camera_state_police_return_flat)` | excursion back, flat | names / line DMA |

States 4-10 are the base state plus 4 or plus 8, so the upload path stays
correct through the excursion. Non-gameplay screens reuse the field only to pick
an upload path. `$FE8 (init_game_start_screen)` and `$1260E` write 3, and the
bad ending and round clear write 1. In both cases name tables are updated with
no scroll DMA.

The follow states (0-2) run:

```text
advance_wave_camera_boundary   ; open corridors, ratchet the back edge (§6.2)
begin_police_special_camera_excursion
apply_screen_shake             ; §11.1
compute_camera_follow_x_velocity
[state 0 only] if $FFFA56: vx = -4 px/frame     ; §6.3
integrate_camera_x_clamped
state 0: build_hscroll_strip_buffer
states 1/2: strip[0].x = X; strip[0].prev_x = prev X
```

---

## 6. Following the players

### 6.1 Dead zones

`$18F5C (compute_camera_follow_x_velocity)` reads the players' biased screen X at
object `+$28`. That value is computed near `$A56C` as `world X + $80 - cam_x`.
The dead zone is `$110`-`$130` biased, which is 144-176 px from the left edge:
a 32-pixel window around the screen centre (160).

- **One player:** left of the zone gives `vx = x - $110`; right of it gives
  `vx = x - $130`; inside it, velocity stays 0.
- **Two players:** the camera moves right by `min(leader - $130, trailer -
  $110)`, so neither player is pushed out. If the leader is right of the zone
  while the trailer is left of it, the camera does not move. Leftward motion
  uses the mirror rule.
- The result is clamped to ±3 px/frame (`±$30000`).

`$18FE8 (compute_camera_follow_y_velocity)` applies the same rule to biased
screen Y (`+$2C`) with zone `$78`-`$A8`, clamped to ±4 px/frame. Only Round 7's
state 3 uses it. The two-player Y branch differs in one case. When the lower
player is past the bottom edge and the upper player is above the top edge, the
camera follows the lower player instead of stopping.

Integration: `$19074 (integrate_camera_x_clamped)` clamps X into
`[min_x, max_x]`. `$194E4 (integrate_camera_y_clamped)` does the same for Y.
`$190AA (integrate_camera_y_bounded)` instead *reverts* to the previous Y when
the step would leave the bounds; the Round 7 primary uses that version.

### 6.2 Corridors and the one-way ratchet

`$19570 (advance_wave_camera_boundary)` does two jobs:

1. When bit 0 of `$FFFA0D (wave_advance_pending)` is set, it increments
   `$FFFF04 (wave)` and loads the next forward bound from `$195D8
   (wave_camera_boundary_table)`: `$4C0, $7C0, ..., $13C0` into `max_x` (or,
   for Round 8, `$1100 ... $0080` into `min_x`). `$FFFF` ends the round with a
   fade instead. Opening a corridor also spawns the type-`$48` "GO" prompt
   (`$19646 (spawn_wave_go_prompt)`) and resets the wave clock. In Round 3 it
   re-arms the shoreline sound (§11.8).
2. **Every frame**, it copies the current X into `min_x`. Round 8 copies it into
   `max_x` instead.

The second job is the ratchet: the camera can never scroll back. Combined with
the dead zone, a corridor scrolls forward only while a player pushes into the
zone's leading edge. `$6A6 (update_camera_scroll_if_needed)` sees `max_x -
min_x < 4` and treats the corridor as a locked arena (`game-engine-and-levels.md`
§6.4).

### 6.3 Scripted pan

`$FFFA56 (camera_scripted_left_pan)` overrides the follow velocity in state 0
with a constant −4 px/frame. Pipeline state `$14` (`$4CA`) and the Mr. X offer
logic at `$11E72 (mr_x_offer_wait_duel_end)` set it. It is cleared when the players reach their marks
(`$50A6`, which then raises `$FFDE00 (mr_x_offer_flag)`) and by `$13E4C
(mr_x_final_encounter_init)`. Round 8 is the only state-0 round that scrolls
left, so this is the camera half of the scripted walk toward Mr. X. The
attribution to Round 8 is inferred; it was not observed live.

---

## 7. Parallax

### 7.1 Strip conversion

`$190E0 (build_hscroll_strip_buffer)` takes the camera's integer X (for the
secondary, the primary's `$FFE002 (cam_x)`):

```text
dst = plane == primary ? $FF5000 : $FF5400
for each strip k:
    x_k = trunc(camX * num_k / den_k)       ; muls / divs
    strip.prev_x = strip.x ; strip.x = x_k
    strip.prev_y = camera.prev_y ; strip.y = camera.y
    repeat 2 * height_k times: *dst++ = -x_k ; one word per 8-pixel band
if level == 2: expand_hscroll_strips_to_lines(plane)
```

The 32-word buffers `$FF5000 (primary_hscroll_strip_buffer)` and `$FF5400
(secondary_hscroll_strip_buffer)` are what per-cell rounds DMA directly. Bands
count from the top of the screen, not the plane. That is correct as long as
vertical scroll is 0. In the per-cell rounds it is nonzero only during a screen
shake (§11.1) or Round 5's bob (§11.4).

`$1919E (expand_hscroll_strips_to_lines)` repeats each band value eight times
into `$FF5A00 (primary_hscroll_line_buffer)` or `$FF5800
(secondary_hscroll_line_buffer)`. If `camera_y` is nonzero, it starts writing
three lines early (`-6` bytes). §11.2 explains why.

### 7.2 Secondary camera states

`$19332 (dispatch_secondary_camera)` uses `$1933A (secondary_camera_state_table)`:

| State | Handler | Behaviour |
|---:|---|---|
| 0 | `$1934A (secondary_camera_state_parallax_strips)` | strips from primary `$FFE002 (cam_x)` through Plane B's ratios. Copies `$FFE00E (camera_y)` to its own Y, except in Round 5 |
| 1 | `$19368 (secondary_camera_state_follow_scaled)` | X = `$FFE002 (cam_x)` in Round 1, `cam_x / 4` otherwise; full-screen use only |
| 2 | `$1938A (secondary_camera_state_round4_ripple)` | Round 4 ripple (§11.3) |
| 3 | `$1938E (secondary_camera_state_round7_shaft)` | Round 7 elevator (§11.5) |
| 4-7 | `$193E4 (secondary_camera_state_round7_police_descend)`, `$1940E (secondary_camera_state_round7_police_out)`, `$1942E (secondary_camera_state_round7_police_back)`, `$19484 (secondary_camera_state_round7_police_ascend)` | Round 7 police excursion (§11.6) |

State 1 ignores the descriptor ratio. In Round 2 the table says 1/2, but the
plane actually moves at 1/4. Only the initial strip seeding uses the 1/2. Plane
B is a single repeated chunk there, so the seeding mismatch never shows.

---

## 8. Name-table streaming

### 8.1 A 512-pixel resident window

The VDP plane is 512 px wide. For each strip, `$18B3C
(queue_camera_tilemap_updates)` keeps world X `[x - $60, x + $1A0)` resident:
96 px to the left of the screen, 320 px visible, and 96 px to the right, for
exactly 512 px. A refresh is queued when `(x XOR prev_x) & $FFF0 ≠ 0`, that is,
when the strip crossed a 16-pixel block boundary:

- moving right, the new column is `prev_x + $1A0`;
- moving left, it is `x - $60`.

`(x + $1A0) mod 512 = (x - $60) mod 512`, so the refreshed name-table column
is always the one that just left the opposite margin. Vertically, the plane is
256 px tall. A row is refreshed at `prev_y + $100` when moving down, or at `y`
when moving up, and spans 32 blocks starting at `x - $60`.

### 8.2 Address resolution

`$19656 (resolve_blockmap_and_vdp_address)` takes `(x, y)`, the strip's
pixel width and height (the moduli), and its first row. It returns two things:

- a block map pointer, `map + ((y mod H) / 16 + first_row) * stride + (x mod W)
  / 16`;
- a name-table write command, `((x & $1FF) >> 2) + ((((y + first_row·16) & $FF)
  << 4) & $FF00)` masked with `$0FFC`, with `$4000` added for Plane A or `$6000`
  for Plane B.

The world therefore wraps toroidally into the 64 × 32-cell table.

### 8.3 Queue formats

`$1953A (build_tilemap_column_update)` writes `[VDP cmd, 2·h+1, then per block:
top long, bottom long]`, stepping the map by its stride. Normally `h` is the
strip height; layout mode 2 uses 16 blocks. `$1951A (build_tilemap_row_update)`
writes `[VDP cmd, 31]` followed by 32 top-row longs, with the matching bottom
longs `$80` bytes further on. The four queues are `$FFEA00
(primary_tilemap_row_update_queue)`, `$FFEB20
(secondary_tilemap_row_update_queue)`, `$FFEC40
(primary_tilemap_column_update_queue)`, and `$FFECE0
(secondary_tilemap_column_update_queue)`. Each is terminated by a zero
command word.

### 8.4 Planes that never stream

The `+$B2`/`+$B3` flags set by `$19C62 (init_round5_ship_background)` exist so
that Round 5's vertical bob (§11.4) does not trigger row reloads:

- primary `+$B2`: column refreshes use Y = 0 and no rows are refreshed;
- secondary `+$B2` and `+$B3`: Plane B is never touched after the initial draw.

Round 4's secondary state never advances its X (§11.3), so Plane B is not
re-streamed there either. In both cases the layers are periodic chunks whose
period divides 512 px, so the h-scroll wrap is enough.

### 8.5 Initial draw

`$19AD6 (initialize_visible_tilemaps)` runs inside the display-off load. For
each strip row (or 16 rows in layout mode 2), it builds a 32-block row update at
`strip.x - $60` and flushes it immediately through `$196A4
(dispatch_background_vblank_upload)`. Normal planes use a Y modulus of `$100`;
the Round 7 grid uses moduli `$500` × `$600`. Strip X/Y were seeded just before
by `$19CBC (seed_both_parallax_strip_states)`.

---

## 9. VBlank

At the top of every full graphics VBlank, `$19D16 (vblank_handler)` writes
`$FFE00E (camera_y)` and `$FFE10E (secondary_camera_y)` (integer words) to VSRAM 0/1. It does this
for every `$FFFF06 (vblank_mode)`, which is why story scenes can scroll vertically just by
changing `$FFE00E (camera_y)` (§11.9). The gameplay path then calls `$19DA6
(vblank_write_scroll_and_backgrounds)`, which:

1. writes `-cam_x` and `-secondary_cam_x` to VRAM F400h/`$F402`. In
   full-screen mode this is the whole horizontal scroll;
2. tail-calls `$196A4 (dispatch_background_vblank_upload)`, indexed by the
   primary state:
   - `$196CA (dma_hscroll_strips_and_flush_tilemaps)`: two DMAs of 32 words
     with auto-increment `$20`, `$FF5000 → $F400` and `$FF5400 → $F402`. In
     level index 2 it diverts to the line path;
   - `$19758 (dma_hscroll_lines_and_flush_tilemaps)`: two DMAs of 256 words with
     auto-increment 4, `$FF5A00 → $F400` and `$FF5800 → $F402`;
   - `$197D8 (flush_tilemap_update_queues)` only.

   Every path ends in `$19800 (flush_one_tilemap_update_queue)` for both planes.
   Columns step the VDP address by `$80` (one row) with `& $EFFF`; rows write
   the top long, then the bottom long `$80` further on, stepping by 4 with
   `& $FF7F` to wrap within the row.

Later in the same VBlank, `$114DA (vblank_upload_gameplay_vdp_updates)` uploads
Round 3's animated tile band (§11.2). Each DMA is wrapped in Z80 BUSREQ
(`graphics-engine.md` §3.4).

---

## 10. Per-level effect initialisation

`$19BD8 (init_level_collision_tables_and_background_effects)` dispatches
through `$19C4C (level_background_effect_init_table)`:

| Round | Target | Work |
|---:|---|---|
| 1, 2, 6, 8 | `$19CBC (seed_both_parallax_strip_states)` | seed strips |
| 3 | `$19C5C (init_round3_background_effect)` | `$19D08 (arm_round3_shoreline_sfx)`, then seed |
| 4 | `$19C92 (init_round4_ripple_effect)` | prime the ripple 32 times, then seed |
| 5 | `$19C62 (init_round5_ship_background)` | stream flags, bob oscillator, `$CC` timer, then seed |
| 7 | `$19D06` | `rts` (state 3 writes its strip each frame; the grid draw uses camera X) |

Separately, `$104D8 (rng_draw)`'s level init calls `$1119E (init_animated_palette_sequences)`
and `$112C0 (init_level_index2_background_effect)` (§11.2, §11.7).

---

## 11. Effects

### 11.1 Screen shake

`$4D40 (request_screen_shake)` sets `$FFFA44 (screen_shake_active)` with
`$FFFA45 (screen_shake_timer)` = 8 and queues sound `$A3`. It refuses while the
police special is active or a shake is already running. It is called from nine
places in player, object, Mr. X, and boss code. `$64F2
(p2_police_shell_impact)` starts a longer shake directly (`$74` frames) without
the guard.

`$19282 (apply_screen_shake)` runs in primary states 0-2. Each frame it
decrements the timer, then adds +3 to `$FFE00E (camera_y)` when the timer is odd and −3
when it is even. The value therefore alternates between 0 and ±3, and the
sign depends on the starting parity. Plane A and every sprite shake, since
sprites subtract `$FFE00E (camera_y)`. Plane B shakes only where secondary state 0 copies
`$FFE00E (camera_y)` (Rounds 3, 6, and 8). Round 7's state 3 does not call the routine.

### 11.2 Round 3: animated shoreline, flashing colour, band correction

**Animated tiles.** `$112C0 (init_level_index2_background_effect)` sets
`$FFFA0C (round3_wave_tiles_enabled)` and decodes the Enigma map `$65976
(round3_wave_tile_frames)` into `$FF8000 (decompression_scratch_buffer)`. Every
gameplay frame, `$11346 (update_level_index2_background_effect)` counts down
`$FFFA0B (round3_wave_tile_timer)` (reload `$FFFA0A (round3_wave_tile_period)` =
5). It does nothing while `$FFFA5D (round3_wave_tiles_suspended)` is nonzero.
On expiry it advances `$FFFB1A (round3_wave_tile_step)` through 0-4 and:

- picks frame `$11398 (round3_wave_tile_frame_sequence)[step]`, which is 0, 1,
  1, 2, 3. Each frame is `$180` bytes, 32 × 6 cells. The frame address goes in
  `$FFFB1C (round3_wave_tile_upload_ptr)`;
- loads the step's four-colour palette list from `$1139E + 8·step` directly
  into the active palette. The lists recolour entries `$2E`, `$2F`, `$3E`, and
  `$3F`, cycling `$668`/`$222`/`$446` greys with `$AA8`/`$642`/`$886`/`$444`
  tones.

In VBlank, `$11518 (upload_round3_wave_tile_rows)` writes the frame to Plane B
rows 23-28 at columns 0-31 (`$EB80`), then again at columns 32-63 (`$EBC0`).
It uses six 64-byte rows through `$104B6 (vdp_copy_long_22)`. This animated
48-pixel band lies inside Plane B's 1/1 bottom layer (block rows 9-14), and
that layer is one repeated chunk, so both name-table halves must hold the same
frame. The full cycle is 25 frames. Because the band moves at camera speed with
the ground, it reads as surf at the waterline (interpretation).

**Flashing colour.** Round 3's animated-palette record (§11.7) holds entry
`$3A` at `$0EE` (yellow). Every two frames it steps through `$0AC`, `$02C`,
`$AE0`, and `$E44`, then holds yellow for 44 steps: a 98-frame flash.

**Line correction.** Round 3 is the only round whose strip buffers are expanded
to lines (§7.1). While `camera_y ≠ 0`, the expansion writes three lines early,
so every band moves up three lines. That matches a +3-pixel shake (§11.1), and
the parallax boundaries stay on the plane features they separate. A −3 shake is
not corrected. The early start also writes three words just below each line
buffer, landing in the tail of `$FF5800 (secondary_hscroll_line_buffer)` for Plane A and in unused space
below `$FF5800 (secondary_hscroll_line_buffer)` for Plane B.

**Ambient sound.** See §11.8.

### 11.3 Round 4: the travelling line ripple

Round 4's primary uses state 2 (flat follow, line DMA). Its secondary uses state
2, which runs `$191E0 (update_round4_line_scroll_ripple)` every frame:

```text
if --$FFFB34 == 0: $FFFB34 = $FFFB32 (16); $FFFC18 = -$FFFC18
copy 31 longs of $FF5100 history to $FF5000 (scratch)
shift $FF5100 history down one long
$FFFC1C += $FFFC18 ; history[31] = $FFFC1C        ; newest sample last
base = -cam_x / 2
Plane B lines   0- 97: base
Plane B lines  98-105: base - int(sample[0..7]) / 2
Plane B lines 106-129: base - int(sample[8..31])
Plane A lines   0-255: -cam_x
```

`$FFFC1C (round4_ripple_phase)` is a triangle wave. `$19C92
(init_round4_ripple_effect)` sets it to step ±0.25 px (`$4000`) and flip every
16 frames, so it ranges over about 1.75-5.75 px with a period of 32 frames.
`$FF5100 (round4_ripple_history)` holds the last 32 samples with the oldest
first. The top ripple line therefore shows the phase from 31 frames ago, and the
pattern travels **up** the screen one line per frame. Lines 98-105 move at half
amplitude, which tapers the effect at its upper edge.

The routine never advances the secondary camera's X, and it never writes Plane B
lines 130-255. Those keep whatever the buffer last held, so Plane A presumably
covers them. In practice this is a half-speed background whose lower band
shimmers like water (interpretation; the round is a bridge over water).

Because the 32nd scratch long (`$FF507C`) is not refreshed by the copy, line 129
reads a stale sample.

### 11.4 Round 5: a moving ship

Three mechanisms combine.

**Drifting sea and sky.** For level index 4, `$190E0 (build_hscroll_strip_buffer)` takes
a special path for the secondary. Per strip, it adds
`(cam_x - prev_cam_x + drift) * num / den` to the existing buffer value instead
of overwriting it. `drift` comes from `$19196 (round5_strip_drift_table)`
(6, 6, 8, 8, indexed by remaining strips). The bands therefore move every frame
even when the camera is still:

| Plane B rows | Ratio | Drift alone |
|---|---:|---:|
| 0-4 | 1/8 | 1 px/frame |
| 5-6 | 1/4 | 2 px/frame |
| 7 | 1/2 | 3 px/frame |
| 8-9 | 3/4 | 4 px/frame |

The buffer value decreases, so the layers slide left as if the ship sails right.
Truncation absorbs small camera deltas in the slow bands: the top band stays at
1 px/frame while the camera moves 0-7 px/frame. Plane B is frozen (§8.4), so
this is pure scrolling.

**Deck bob.** The same path then integrates an oscillator stored in the
*secondary* structure and adds it to the *primary* `$FFE00E (camera_y)`:

```text
+$AE += +$AA                          ; +$AA = -$800 (-1/32 px/frame^2)
if --(+$A8) == 0: +$A8 = +$A9 (40); +$AA = -(+$AA)    ; first flip after 20
camera_y += +$AE
```

The velocity is a ±0.625 px/frame triangle. A simulation of the recurrence
shows `$FFE00E (camera_y)` sweeping smoothly between 0 and −12.5 px with an 80-frame
(1.33 s) period. Secondary state 0 deliberately skips copying `$FFE00E (camera_y)` in
Round 5, so Plane A (the deck) and every sprite bob together while Plane B (sea
and sky) stays level. The primary's `+$B2` flag keeps the bob from triggering
row streaming.

**Police special.** Activation in `$3FCC (try_activate_police_special)` clears
both cameras' Y. In Round 5 it also restarts the oscillator (`+$AA = -$800`,
timers `$14`/`$28`, `+$AE = 0`), so the deck is level during the excursion.

### 11.5 Round 7: the elevator

Both cameras start in state 3, on the 5 × 6-chunk grid (§2.3). Plane A holds
the elevator platform at Y 0 with fixed bounds. Plane B is the shaft, starting
at Y `$500` (the bottom chunk row).

1. `$18F32 (advance_round7_vertical_wave)` consumes the wave request,
   increments the wave, and starts a **departure jolt**. It sets `$FFFA14
   (round7_jolt_active)`, `$FFFA12 (round7_jolt_timer)` /
   `$FFFA13 (round7_jolt_period)` = 2, and clears `$FFFB36 (round7_jolt_step)`
   and `$FFFA16 (round7_elevator_climb_started)`.
2. `$18ECA (update_round7_elevator_jolt)` ticks every two frames. Ticks 1 and 2
   add −3 and then +3 to the primary Y (table `$18F2C`), and both set
   `$FFFA27 (scripted_spawn_release_flag)`. On tick 3 the jolt ends. If no climb
   has started, it sets `$FFFA15 (round7_elevator_moving)` and loads the
   secondary minimum Y from `$18F20 (round7_elevator_floor_table)[wave]`:
   `$400, $300, $200, $100, 0`.
3. `$1938E (secondary_camera_state_round7_shaft)` moves Plane B at −1 px/frame
   while climbing. The shaft scrolls 256 px per wave, about 4.3 s. Each frame it
   copies Y into the maximum, so the shaft cannot move back. On reaching the
   floor it clears `$FFFA15 (round7_elevator_moving)` and starts an **arrival jolt** (the climb latch
   stops another climb).

The platform stays still while the walls slide down. The climb is a camera
effect: no gameplay object moves vertically. Objects that ride the scenery
(`$7DAA`, `$7DC4`) check `$FFFA15 (round7_elevator_moving)`. `$3FCC (try_activate_police_special)`
refuses while `$FFFA14 (round7_jolt_active)` or `$FFFA15 (round7_elevator_moving)` is set.

`$FFFA27 (scripted_spawn_release_flag)` is the acknowledgement that level-pipeline state `$10` (`$748`) waits
for after its timer. Each jolt is what releases the next scripted spawn step.

### 11.6 Police special camera excursion

`$192A4 (begin_police_special_camera_excursion)` runs on the one-frame
`$FFFA1B (police_special_start_pulse)`. It snapshots bounds, X, Y, and state into
`+$8C..+$A5`.

**Rounds other than 7** (primary camera, follow states):

```text
state += 4 ; min_x = max(0, X - $300)
states 4-6: vx = -16 px/frame          ; swing back over the cleared ground
            at X <= snap - $200: spawn type-$05 vehicle at snap - $340
            ($18CD4 police_excursion_spawn_vehicle)
vehicle exit ($5BEC): state += 4
states 8-10: vx = +16 px/frame
            restore_police_special_palette_and_art (once)
            at X >= snap: restore bounds and state, spawn $0E (P1) / $0D (P2)
            ($18DD2 police_excursion_return_check)
```

The vehicle code at `$5BEC` is a dynamic entry with no label. When the vehicle's
`+$0A` reaches 3 it deletes itself, then either adds 4 to the primary state or,
in Round 7, adds 1 to the secondary state. While the excursion runs, the
ratchet, the dead zone, and the shake are all bypassed. Activation clears both
cameras' Y in every round except 7. In Round 3 it also patches the palette
snapshot's entry `$2D` to `$668` before saving.

**Round 7** runs the excursion on the secondary camera and makes the primary
follow it:

| Secondary state | Motion |
|---:|---|
| 4 `$193E4 (secondary_camera_state_round7_police_descend)` | Y +16 px/frame down to `$500` (shaft bottom) |
| 5 `$1940E (secondary_camera_state_round7_police_out)` | X −16 px/frame (minimum 0); vehicle spawn |
| 6 `$1942E (secondary_camera_state_round7_police_back)` | X +16 px/frame back to `$300`, pins both cameras' X, sets primary `+$A6` |
| 7 `$19484 (secondary_camera_state_round7_police_ascend)` | Y −16 px/frame up to the snapshot; restores everything; spawns the attack |

The primary sits in state 7, `$18D62 (primary_camera_state_round7_police_follow)`,
and copies the secondary's per-frame X/Y deltas. Its maximum Y is set to `$500 −
snapshot Y` so both planes reach the ground floor together. Once `+$A6` is set,
it ratchets down and drops back to state 3 below Y 8. The camera visibly
descends the whole shaft to the bottom chunk row and pans left over chunks 4-6.
There the vehicle enters from world X −`$40`. Afterwards the camera climbs back.

### 11.7 Animated palettes

`$1119E (init_animated_palette_sequences)` decodes the level's records from
`$1C400 (animated_palette_table)` into `$FFEE00 (animated_palette_descriptors)`
and `$FFEE40 (animated_palette_frames)`. It uses the sparse palette-word format
of `$10538 (load_palette_list_to_target)`: colour in `$0EEE`, palette index from
bits 12-15, 4, and 8, and bit 0 ending the list. Every gameplay frame, `$11210
(update_animated_palettes)` counts down each record. On expiry it writes the
next frame's entries into `$FFF400 (palette)` and sets `$FFFA01
(palette_dirty)`.

| Round | Entries | Period × frames | Pattern |
|---:|---|---|---|
| 1 | `$2A`, `$2B`, `$2D`, `$2E` | 4 × 24 | pairs swap `$E8E`/`$2E2` and `$0AE`/`$044`: blinking signs |
| 2 | — | — | none |
| 3 | `$3A` | 2 × 49 | five-hue flash, then yellow hold (§11.2) |
| 4 | `$2F` | 5 × 8 | ramp `$000 → $64E`, then wrap |
| 5 | `$2F` | 5 × 12 | red pulse `$000 → $00C → $002` |
| 5 | `$3C` | 8 × 4 | `$000, $440, $664, $884` |
| 5 | `$3D` | 10 × 4 | `$000, $000, $220, $440` |
| 6 | `$3C` | 5 × 14 | red pulse to `$00E` and back |
| 6 | `$2F` | 2 × 11 | `$00A`/`$222` flicker, then `$222` |
| 6 | `$3D`-`$3F` | 4 × 3 | three colours rotating through three entries (a classic cycling stripe) |
| 7 | `$3F` | 5 × 7 | `$22E`, then red decaying `$00C → $002` |
| 8 | `$2A`-`$2F` | 1 × 8 | **progress-gated** (below) |

Round 8 is gated by camera progress. Frames 1, 3, 4, and 6 advance on the
one-frame period. Frames 0, 2, 5, and 7 hold until `$FFE002 (cam_x)` drops to the next
threshold in `$112B8 (round8_palette_stage_thresholds)` (`$C00`, `$900`,
`$600`). Each release increments `$FFFB2E (round8_palette_stage)` and gives
the next frame three frames. The six colours therefore step through three short
transitions as the player moves left through the building, then stay on frame
7. The fourth threshold (`$300`) is never consulted.

### 11.8 Ambient companions

`$112F2 (dispatch_per_level_ambient_effects)` runs through `$11300 (per_level_ambient_effect_jt)`. The two rounds whose backgrounds move on
their own also get sound:

- Round 3, `$11310 (round3_delayed_sfx_burst_update)`: `$19D08
  (arm_round3_shoreline_sfx)` sets `$FFFA55 (round3_shoreline_sfx_bursts)` = 4
  and `$FFFB50 (ambient_sfx_timer)`/`$FFFB51` = 2/`$38`. That queues sound `$C3`
  four times, 56 frames apart. It is armed at round start and again at every
  wave advance;
- Round 5, `$1132E (round5_periodic_sfx_update)`: sound `$CC` after `$20`
  frames, then every `$300` frames (12.8 s).

### 11.9 Camera fields outside gameplay

Because VSRAM is always loaded from `$FFE00E (camera_y)`/`$FFE10E (secondary_camera_y)`, cut-scenes
reuse the camera structure as a plain scroll register:

- the bad ending's text crawl (`$8A62`) adds 0.5 px/frame to `$FFE00E (camera_y)`. At each
  16-pixel step it writes the next text row into the name table, 240 px ahead;
- presentation object type `$65`, `$CCE8
  (presentation_type65_camera_y_drift_dispatcher)`, adds one pixel every four
  frames;
- story, title, and ending initializers set `$FFE00E (camera_y)` to fixed values.

---

## 12. End-to-end pseudocode

```text
level load (display off):
    clear cameras, clear both block maps
    R11 = level_hscroll_mode_table[level]
    decode 2 art streams -> VRAM; 2 metatile dictionaries; collision map; palette
    for plane in (A, B): init_camera_plane(header) -> build_camera_plane_buffers
    init_level_collision_tables_and_background_effects  ; per-round effect init
    initialize_visible_tilemaps
    init_animated_palette_sequences ; init_level_index2_background_effect

each gameplay frame:
    ambient sfx; update_animated_palettes
    update players
    update_cameras_and_queue_tilemaps:
        primary state:   corridors/ratchet, police start, shake, follow, integrate,
                         strips | flat | Round 7 | excursion
        queue dirty columns/rows for plane A
        secondary state: strips | scaled | ripple | shaft | excursion
        queue dirty columns/rows for plane B
        zero velocities
    clamp players to the camera window
    Round 3 tile animation step

VBlank:
    VSRAM = camera_y, secondary_camera_y
    $F400/$F402 = -cam_x, -secondary_cam_x
    by primary state: strip DMA | line DMA | none
    flush column/row queues for both planes
    Round 3 tile band, other queued VDP copies
```

---

## 13. Evidence ledger

| Claim | Evidence |
|---|---|
| Camera state tables, sizes, targets | ROM words at `$18C34 (primary_camera_state_table)` (11) and `$1933A (secondary_camera_state_table)` (8); the next word is code |
| Upload path follows the primary state | `$196A4 (dispatch_background_vblank_upload)` indexes `$196B2 (background_vblank_upload_table)` with `+$00`; 12 decoded entries repeat strip/names/lines/names |
| One-way ratchet every frame | `loc_195C0` runs unconditionally after the wave test |
| Dead zones and clamps | immediates `$01100000`, `$01300000`, `±$30000` (X), `$780000`, `$A80000`, `±$40000` (Y); `+$28` / `+$2C` written as biased screen coordinates at `$A56C` |
| 512-pixel resident window | `-$60` / `+$1A0` edge offsets in `$18B3C (queue_camera_tilemap_updates)`; `$1FF` X mask in `$19656 (resolve_blockmap_and_vdp_address)` |
| Round configuration | decoded `$5F5B8 (level_graphics_package_table)`, `$5F788 (level_plane_layout_table)`, `$1998A (plane_layout_bounds_table)`, `$199B6 (level_hscroll_mode_table)`, `$19C4C (level_background_effect_init_table)` |
| Round 5 drift and bob | `$19138` path selected by `level == 4 && a1 == $E100`; init `$19C62 (init_round5_ship_background)`; recurrence simulated |
| Round 4 ripple | `$191E0 (update_round4_line_scroll_ripple)`; init `$19C92 (init_round4_ripple_effect)` (`$4000`, `$20000`, `$00100010`) |
| Round 3 tile band | `$11346 (update_level_index2_background_effect)`, frame table `$11398 (round3_wave_tile_frame_sequence)`, palette lists `$1139E`, upload on the `$114DA (vblank_upload_gameplay_vdp_updates)` path to `$6B800003`/`$6BC00003`; `$104B6 (vdp_copy_long_22)` measured as 16 longs |
| Round 7 elevator | `$18ECA (update_round7_elevator_jolt)`, `$18F32 (advance_round7_vertical_wave)`, `$1938E (secondary_camera_state_round7_shaft)`, tables `$18F20 (round7_elevator_floor_table)`/`$18F2C`, grid tables `$19AC0`/`$19ACA` |
| Excursion return trigger | ROM `$5BEC`: `cmpi.b #3,$A(a0)` … `addq.w #4,$E000` / `addq.w #1,$E100` |
| Animated palettes | decoded `$1C400 (animated_palette_table)`; gating at `$11272` with thresholds `$112B8 (round8_palette_stage_thresholds)` |
| Copy helper lengths | opcode `$2499` runs from `$10496 (vdp_copy_long_38)` (32), `$104B2 (vdp_copy_long_24)` (18), `$104B6 (vdp_copy_long_22)` (16), `$104C6 (vdp_copy_long_8)` (8) |

---

## 14. Analysis-data changes made with this manuscript

`labels.csv`:

- **renamed** `$191E0 (update_round4_line_scroll_ripple)`: `update_round3_line_scroll_effect` →
  `$191E0 (update_round4_line_scroll_ripple)`. The only caller is secondary state 2,
  which only level index 3 (Round 4) selects, and its initializer is the
  level-3 entry of `$19C4C (level_background_effect_init_table)`. The previous description, and
  `graphics-engine.md` §6.3, attributed it to level index 2;
- **corrected descriptions** for `$10496 (vdp_copy_long_38)`, `$104B2
  (vdp_copy_long_24)`, and `$104B6 (vdp_copy_long_22)` to the measured counts
  of 32, 18, and 16 longs. The names were kept;
- **added** labels for the two camera state tables, their 18 handlers and two shared steps, the
  follow, integration, and shake helpers, the per-level effect initializers,
  `$4D40 (request_screen_shake)`, `$11518 (upload_round3_wave_tile_rows)`, and
  the effect data tables at `$112B8 (round8_palette_stage_thresholds)` and `$11398 (round3_wave_tile_frame_sequence)`.

`addresses.csv`: the secondary camera's X/Y words; the Round 3, 4, 5, and 7
effect variables; `$FFFA27 (scripted_spawn_release_flag)`, `$FFFA56 (camera_scripted_left_pan)`, and `$FFFB2E (round8_palette_stage)`; the collision-class
table pointers; the animated-palette RAM; and the ROM tables `$5F5B8 (level_graphics_package_table)`,
`$5F788 (level_plane_layout_table)`, `$65976 (round3_wave_tile_frames)`, `$1998A (plane_layout_bounds_table)`, `$18F20 (round7_elevator_floor_table)`, `$19196 (round5_strip_drift_table)`, and `$1C400 (animated_palette_table)`.

---

## 15. Open questions

- Who writes `$FFFA5D (round3_wave_tiles_suspended)`? No absolute-address
  writer exists in the listing, so it may be a word write at `$FFFA5C`.
- Camera header word `+$30` (`$10` / `$60`) has no reader in the camera code.
- Whether pipeline state `$14` (`$4CA`), and so `$FFFA56 (camera_scripted_left_pan)`, is reachable outside
  Round 8. A bounded live run that logs `$FFFA56 (camera_scripted_left_pan)` per round would settle §6.3.
- The visual identity of each band (sea, sky, shaft walls) is inferred from the
  code and the round themes. A screenshot pass with layers toggled in
  `MegaDriveEnvironment` would confirm it.
