# Streets of Rage Graphics Engine

## Scope and method

This manuscript reconstructs the original Mega Drive graphics engine used by
*Streets of Rage*: VDP setup, VRAM layout, frame scheduling, background
construction, camera and parallax processing, streamed name-table updates,
sprite ordering and SAT generation, dynamic art residency, palettes, fades,
HUD/UI writes, and the relationship between the main loop and VBlank.

The primary source is the freshly regenerated `output/sor.asm`. ROM bytes were
read only where the disassembler represents data tables as gaps. The existing
compression and level manuscripts were used as cross-checks. A live
`MegaDriveEnvironment` run was also inspected through the repository's
`megadrive_remote` client; the runtime evidence is isolated in its own section
and is not used to invent semantics absent from the assembly.

All semantic addresses and names come from `code-analysis/addresses.csv` and
`code-analysis/labels.csv`.

---

## 1. Main result

The renderer is a staged, mostly software-managed pipeline around the Mega
Drive VDP:

```text
ROM assets
  ├─ Nemesis  -> tile pixels -> VRAM, directly or incrementally in VBlank
  ├─ Enigma   -> tile/metatile words -> work RAM
  └─ Kosinski -> block maps, collision maps, and general graphics data -> RAM

game-time update
  ├─ update objects
  ├─ update two camera/plane state machines
  ├─ generate dirty tilemap row/column queues
  ├─ generate horizontal-scroll buffers
  ├─ depth-sort visible objects
  ├─ build the linked active SAT prefix and its terminator in RAM
  ├─ patch active palette / HUD shadows
  └─ raise dirty flags and VBlank request

VBlank
  ├─ vertical scroll -> VSRAM
  ├─ dirty palette -> CRAM DMA
  ├─ horizontal scroll -> VRAM DMA
  ├─ dirty background rows/columns -> VRAM
  ├─ dirty player/enemy art -> VRAM DMA or bounded Nemesis decode
  ├─ SAT shadow -> VRAM DMA
  ├─ HUD / screen-specific tile writes
  ├─ controller sampling
  └─ sound tick
```

The architecture has five especially important properties:

1. **World simulation and rendering are separated.** Objects retain world
   coordinates. `$AD8E (update_objects_and_build_sprites)` converts them to
   screen coordinates only after object logic and the camera update.
2. **Both scrolling planes are data-driven camera objects.** Their block maps,
   metatile dictionaries, bounds, positions, and parallax strips are stored in
   `$FFE000 (primary_camera)` and `$FFE100 (secondary_camera)`.
3. **The name tables are treated as circular caches.** Crossing a 16-pixel
   metatile boundary queues only the newly exposed row or column. Address masks
   wrap those writes within the 4 KiB Plane A or Plane B table.
4. **Sprites use a full RAM shadow.** The game rebuilds the linked active
   prefix of `$FFDA00 (sprite_attribute_table_buffer)` every frame, sorts
   objects by lane depth, emits a terminator, and DMA-copies exactly 640 bytes
   to the SAT at VRAM address F000h.
5. **VBlank is an explicit transaction.** Game code posts request 1 or 2 in
   `$FFFA00 (vblank_request)` and waits. The interrupt acknowledges the request,
   performs the appropriate bounded work, then clears the byte.

This is not a framebuffer renderer. Pixel art remains tile-based, scene
composition remains name-table based, and sprites remain VDP hardware sprites.
The engine's sophistication lies in scheduling and in maintaining coherent RAM
shadows and streaming queues around those fixed-function resources.

---

## 2. VDP configuration and VRAM layout

### 2.1 Runtime register setup

`$7F6C (init_vdp)` writes 19 bytes from
`$7FA4 (runtime_vdp_register_values)` to VDP registers R0-R18. The relevant
values are:

| Register | Value | Result |
|---:|---:|---|
| R0 | `$04` | normal mode-4 base configuration |
| R1 | `$14` initially | display disabled while destructive setup runs |
| R2 | `$30` | Plane A name table at VRAM `$C000` |
| R3 | `$34` | Window name table at VRAM `$D000` in H40 mode |
| R4 | `$07` | Plane B name table at VRAM address E000h |
| R5 | `$78` | sprite attribute table at VRAM address F000h |
| R11 | `$00` initially | full-screen horizontal scroll |
| R12 | `$81` | H40 / 320-pixel display |
| R13 | `$3D` | horizontal-scroll table at VRAM address F400h |
| R15 | `$02` | ordinary VDP data-port auto-increment of two bytes |
| R16 | `$01` | 64 by 32 cell scrolling planes |

The engine caches two R1 commands:

| Reference | NTSC | PAL | Meaning |
|---|---:|---:|---|
| `$FFFF46 (vdp_display_enable_command)` | `$8174` | `$817C` | display on |
| `$FFFF48 (vdp_display_disable_command)` | `$8134` | `$813C` | display off |

Initializers turn the display off, perform large direct transfers or clears,
then restore the cached display-on command. Ordinary gameplay does not rebuild
the scene with the display active.

### 2.2 Stable VRAM map

The register setup and all later DMA commands establish this high-VRAM layout:

| VRAM range / base | Purpose |
|---:|---|
| `$0000-$BFFF` | tile art, including fixed UI, level tiles, resident enemies, and dynamic player regions |
| `$C000` | Plane A name table, 64 × 32 words = `$1000` bytes |
| `$D000` | Window name table |
| E000h | Plane B name table, 64 × 32 words = `$1000` bytes |
| F000h | sprite attribute table, 80 × 8 bytes = `$280` bytes |
| F400h | horizontal-scroll table |

The separation is visible in command constants:

- `$70000003` is a normal VRAM write to F000h;
- `$70000083` is the DMA form of that SAT destination;
- `$74000003` writes the first h-scroll entry at F400h;
- `$74000083` is the DMA form used for Plane A scroll values;
- `$74020083` addresses the interleaved Plane B h-scroll word at `$F402`.

The low byte's bit 7 selects VDP DMA. The remaining command bits encode the
upper VRAM address bits, so these constants should not be read as linear CPU
addresses.

### 2.3 Reset and destructive clears

`$7FB8 (reset_vdp_and_graphics_state)` is the reusable screen-transition reset.
Despite its former historical name, it is a graphics reset, not the sound
driver loader. It:

1. calls `$7F6C (init_vdp)`;
2. requests the Z80 bus before VDP DMA;
3. uses DMA-fill commands to clear large VRAM regions;
4. restores data-port auto-increment to two;
5. clears `$FFDA00 (sprite_attribute_table_buffer)`;
6. calls `$804C (clear_camera_structures)`;
7. calls `$805A (clear_palette_buffers)`;
8. clears the high VRAM scroll/SAT tail used by the next scene.

The separate `$1061C (load_z80_dac_driver)` is what actually installs the Z80
PCM program. Keeping these two operations distinct matters when reasoning about
screen transitions.

---

## 3. Frame handoff and VBlank scheduling

### 3.1 Two wait primitives

The two routines formerly described as Z80 synchronization are VBlank
transactions:

| Reference | Posted value | Interrupt work |
|---|---:|---|
| `$10502 (wait_vblank_and_upload_graphics)` | `1` | full mode-specific video update, pads, incremental art, sound |
| `$10514 (wait_vblank_without_graphics_upload)` | `2` | pads, incremental art, sound; skip the frame's palette/background/SAT uploads |

Both write `$FFFA00 (vblank_request)`, set `SR=$2500`, and spin until
`$19D16 (vblank_handler)` clears the byte.

The request-2 wait occurs inside
`$AD8E (update_objects_and_build_sprites)`. Before that wait, the routine
updates both players, resolves player collision, and advances the camera and
tilemap queues. After the wait, it snapshots the newly sampled controls,
updates the 66 general object slots, and rebuilds the SAT. The enclosing global
loop later calls `$10502 (wait_vblank_and_upload_graphics)`. Consequently one
logical gameplay update deliberately straddles two VBlank transactions:

```text
players and camera/streaming queues are advanced
  -> request-2 VBlank samples controllers but does not publish video state
  -> CPU updates general objects and builds the next SAT
  -> global loop posts request 1
  -> next VBlank uploads the completed frame state
```

### 3.2 VBlank mode

`$FFFF06 (vblank_mode)` is independent of the request byte. Request 1 enables
video work; `$FFFF06 (vblank_mode)` chooses which screen-specific video program runs.
`$19D92 (vblank_mode_dispatch_table)` contains five `BRA.w` entries:

| Mode | Main users | Special work |
|---:|---|---|
| `0` | gameplay, logo, top-10 | cameras/backgrounds, player art, SAT, gameplay HUD |
| `1` | mode select, character select | character-preview dynamic art and SAT |
| `2` | intro and good-ending story scenes | story-specific VDP command paths |
| `3` | bad ending | bad-ending text/map updates and SAT |
| `4` | round clear | tally/HUD updates and SAT |

This is why VBlank is not one fixed DMA list. The common resources are shared,
but each global screen can attach its own small update protocol.

### 3.3 Full graphics-VBlank order

For request 1, `$19D16 (vblank_handler)` performs this order:

1. save all registers and acknowledge `$FFFA00 (vblank_request)`;
2. read VDP status to clear the pending interrupt condition;
3. select VSRAM and write primary and secondary vertical-scroll words;
4. consume bit 0 of `$FFFA01 (palette_dirty)`;
5. if dirty, DMA 64 words from `$FFF400 (palette)` to CRAM `$0000`;
6. dispatch through `$19D92 (vblank_mode_dispatch_table)`;
7. update mode-specific HUD/tile data;
8. sample controllers;
9. run `$8510 (continue_incremental_nemesis_decode)`;
10. run `$72914 (sound_engine)`;
11. restore registers and `RTE`.

The gameplay branch additionally performs:

```text
$19DA6 (vblank_write_scroll_and_backgrounds)
$1A0B4 (upload_dirty_player_art_dma)
$181BC special per-level VDP command upload
SAT DMA: 320 words from $FFDA00 to VRAM $F000
$10F80 gameplay HUD writes
$114DA queued dialogue / rectangular tilemap writes
```

The SAT length is programmed as `$0140` words, exactly `80 * 8 / 2`. This is a
full shadow upload, not a variable-length DMA.

### 3.4 Why BUSREQ surrounds DMA

ROM-to-VRAM and RAM-to-VRAM DMA repeatedly:

1. write `$0100` to `$A11100 (z80_busreq)`;
2. program VDP length/source registers R19-R23;
3. issue the destination command with DMA bit set;
4. restore data-port auto-increment where needed;
5. release the Z80 bus.

VDP DMA reads the 68000 address space. The explicit arbitration prevents the
Z80 PCM side from contending for the same cartridge/main bus window while a DMA
source is being consumed.

---

## 4. Asset formats and art residency

### 4.1 Division of labour among codecs

The graphics engine uses three codecs for different structures:

| Codec | Graphics role |
|---|---|
| Nemesis | 4-bpp tile pixels; can decode directly to the VDP data port |
| Enigma | 16-bit tile words and four-word metatile definitions |
| Kosinski | large general byte streams: art blocks, level block maps, and collision-class maps |

`$A63A (load_nemesis_art_bundle)` processes four packed IDs. Each ID indexes an
eight-byte record in `$A662 (nemesis_art_bundle_table)`:

```text
long  VDP destination command
long  Nemesis source pointer
```

The selected stream is passed to `$8192 (nemesisdec_vram)`, so screen
initializers can decode tile pixels directly into their final VRAM slot.

`$A82A (load_enigma_map_bundle)` similarly resolves ten-byte records in
`$A85A (enigma_map_bundle_table)`, but its destination is RAM. Enigma output is
tile-word data that will later be copied or expanded into VDP name tables.

### 4.2 Blocking scene setup

Large blocking transfers occur while the display is disabled or during a
dedicated initializer. `$19848 (load_level_graphics_maps_and_camera)` performs
the playable-level version:

1. clear both camera structures and `$FF0000-$FF3FFF`;
2. choose the per-level package rooted at ROM `$5F5B8`;
3. write the level's R11 h-scroll mode;
4. Kosinski-decode two large art streams through
   `$FF8000 (decompression_scratch_buffer)` and upload each to VRAM;
5. decode two Enigma streams to `$FF4000 (primary_metatile_table)` and
   `$FF4800 (secondary_metatile_table)`;
6. Kosinski-decode a third stream to
   `$FFA000 (level_collision_class_map)`;
7. construct the two camera-plane block maps and strip descriptors;
8. populate the initially visible name-table cache.

The art uploads use `$10496 (vdp_copy_long_38)` in a fixed 151-pass loop. The
transfer envelope is therefore fixed; the useful compressed art size and the
reserved VRAM region are related but not identical.

### 4.3 Incremental enemy art

Gameplay enemy art is not always resident. `$8454 (queue_nemesis_art_cues)`
appends `(source long, VRAM destination word)` records to
`$FFDCD0 (art_array_cue)`.

`$84BA (begin_incremental_nemesis_decode)` prepares the head stream in game
time. `$8510 (continue_incremental_nemesis_decode)` runs from VBlank and
decodes no more than five tiles, or 160 bytes, per call. State is retained in:

| Reference | Meaning |
|---|---|
| `$FFDCD4 (nemesis_art_vram_destination)` | next VRAM byte destination |
| `$FFDD10 (nemesis_incremental_writer)` | normal or XOR-row output function |
| `$FFDD1C (nemesis_incremental_xor_row)` | previous decoded row |
| `$FFDD20 (nemesis_incremental_bit_buffer)` | payload bit reservoir |
| `$FFDD24 (nemesis_incremental_bits_remaining)` | valid-bit count |
| `$FFDD28 (nemesis_incremental_tiles_remaining)` | stream tiles left |
| `$FFDD2A (nemesis_incremental_tile_budget)` | five-tile VBlank allowance |

Three interchangeable enemy-art destinations let the spawn/resource system
maintain several visual families without dedicating permanent VRAM to every
enemy in the ROM.

### 4.4 Dynamic player art

Player frames use a different strategy. While
`$AF46 (emit_object_sprite_mapping)` resolves the selected mapping, it calls
`$B118 (update_object_dynamic_art_cues)` for player and type-`$07` mappings.
That helper copies two art IDs into object fields `+$52` and `+$53` and sets the
object dirty field at `+$51`.

In the following full VBlank, `$1A0B4 (upload_dirty_player_art_dma)`:

1. checks `+$51` for P1 and P2;
2. clears the dirty byte;
3. uses `+$52` to index `$1A160 (player_art_dma_upper_table)`;
4. uses `+$53` to index `$1A53E (player_art_dma_lower_table)`;
5. programs a six-byte VDP DMA descriptor for each nonzero ID;
6. targets four reserved VRAM regions, two per player.

The destinations encoded by `$70000082`, `$74000082`, `$78000082`, and
`$7C000082` are VRAM addresses B000h, B400h, B800h, and BC00h. Sprite mappings
can therefore refer to stable tile bases while the exact frame pixels in those
slots change.

The character-select path `$1802 (char_select_upload_preview_dma)` reuses the
same descriptor tables and dirty-field protocol for its three large preview
objects.

---

## 5. Runtime background representation

### 5.1 RAM buffers

The level renderer separates three representations:

| Reference | Representation |
|---|---|
| `$FF0000 (primary_plane_blockmap)` | byte block IDs for the primary scene |
| `$FF2000 (secondary_plane_blockmap)` | byte block IDs for the secondary scene |
| `$FF4000 (primary_metatile_table)` | 8-byte block definition: four VDP tile words |
| `$FF4800 (secondary_metatile_table)` | same for the secondary plane |
| `$FFA000 (level_collision_class_map)` | two packed 4-bit collision classes per byte |

The visual block map and collision-class map are distinct. Rendering expands a
block ID through a four-tile dictionary. Movement/collision code samples the
packed nibble map and then resolves class-specific height/behavior tables.

This is a useful engine boundary:

```text
visual world: byte block ID -> four tile attributes -> Plane A/B
physical world: X/lane -> packed collision class -> collision response
```

### 5.2 Camera-plane structure

The common fields in `$FFE000 (primary_camera)` and
`$FFE100 (secondary_camera)` are:

| Offset | Meaning |
|---:|---|
| `+$00` | camera dispatch state |
| `+$02` | current X, 16.16 |
| `+$06` | previous X, 16.16 |
| `+$0A` | X velocity, 16.16 |
| `+$0E` | current vertical/depth scroll, 16.16 |
| `+$12` | previous vertical/depth scroll |
| `+$16` | vertical velocity |
| `+$1A/+$1E` | maximum/minimum X bounds |
| `+$22/+$26` | maximum/minimum vertical bounds |
| `+$2A` | block-map pointer |
| `+$2E` | block-map row stride |
| `+$32` | metatile-definition pointer |
| `+$42` | plane-layout special mode |
| `+$44` | parallax-strip count |
| `+$46...` | `$12`-byte strip descriptors |

The primary aliases are
`$FFE02A (primary_plane_blockmap_ptr)`,
`$FFE02E (primary_plane_blockmap_stride)`, and
`$FFE032 (primary_metatile_table_ptr)`. The secondary plane has the analogous
aliases at `$FFE12A (secondary_plane_blockmap_ptr)`,
`$FFE12E (secondary_plane_blockmap_stride)`, and
`$FFE132 (secondary_metatile_table_ptr)`.

### 5.3 Building plane maps

`$199C6 (build_camera_plane_buffers)` selects a per-level/per-plane Kosinski
stream through ROM `$5F788`. It decodes source block data to scratch RAM, reads
the strip descriptor list, and populates the plane block map.

For ordinary layouts it copies 16-byte slices from the decompressed block data
into the output map with a level-dependent row span. A special layout mode
builds a fixed 6 × 5 arrangement of 16-row regions. Both paths finish with the
same byte block-map representation consumed by the streaming code.

`$19AD6 (initialize_visible_tilemaps)` walks both planes and expands the
initially visible block rows. It uses the same address resolver and VBlank
flush logic as later incremental scrolling, which prevents setup and runtime
from having incompatible map formats.

---

## 6. Camera, parallax, and horizontal scrolling

### 6.1 Per-level VDP scroll mode

Before building the level, `$19848 (load_level_graphics_maps_and_camera)` writes
one word from `$199B6 (level_hscroll_mode_table)` to the VDP:

| Level index | R11 command | Horizontal-scroll mode |
|---:|---:|---|
| 0 | `$8B00` | full-screen |
| 1 | `$8B00` | full-screen |
| 2 | `$8B03` | per-line |
| 3 | `$8B03` | per-line |
| 4 | `$8B02` | per-cell |
| 5 | `$8B02` | per-cell |
| 6 | `$8B00` | full-screen |
| 7 | `$8B02` | per-cell |

The camera engine is therefore shared, but the VDP consumes its scroll output
at different granularity per round.

### 6.2 Strip ratios

`$190E0 (build_hscroll_strip_buffer)` walks the `$12`-byte parallax
descriptors. For each strip it:

1. starts with camera X;
2. multiplies by a signed numerator;
3. divides by a signed denominator;
4. preserves current and previous strip X;
5. negates the result for VDP h-scroll convention;
6. repeats it for the descriptor's vertical extent;
7. records current/previous vertical positions for dirty detection.

Primary strip values go to
`$FF5000 (primary_hscroll_strip_buffer)`; secondary values go to
`$FF5400 (secondary_hscroll_strip_buffer)`.

The secondary camera can follow primary X directly or at a reduced fraction.
Special level states can animate it independently. This is how the engine
produces depth without a third background plane: different vertical bands of
the same name table receive different horizontal offsets.

### 6.3 Per-line expansion

`$1919E (expand_hscroll_strips_to_lines)` expands each of 32 strip values eight
times:

- primary output -> `$FF5A00 (primary_hscroll_line_buffer)`;
- secondary output -> `$FF5800 (secondary_hscroll_line_buffer)`.

`$191E0 (update_round3_line_scroll_effect)` builds an additional animated
per-line distortion for level index 2. It shifts a workspace, advances a signed
phase, combines the result with camera X, and writes the line buffers. This is
the code path behind the round's non-rigid background motion.

### 6.4 VBlank h-scroll DMA

`$196A4 (dispatch_background_vblank_upload)` uses primary-camera state and
`$196B2 (background_vblank_upload_table)` to choose among:

- normal strip/cell DMA plus tilemap queues;
- tilemap queues without a new scroll DMA;
- full per-line DMA plus tilemap queues.

`$196CA (dma_hscroll_strips_and_flush_tilemaps)` uploads 32 primary values from
`$FF5000 (primary_hscroll_strip_buffer)` to VRAM address F400h with auto-increment
`$20`, then 32 secondary values from
`$FF5400 (secondary_hscroll_strip_buffer)` to `$F402`. The `$20`-byte spacing is
exactly eight line-scroll records, so the same transfer naturally supplies
per-cell bands.

`$19758 (dma_hscroll_lines_and_flush_tilemaps)` instead uploads 256 words per
plane with auto-increment four:

```text
primary   $FF5A00 -> $F400, $F404, $F408, ...
secondary $FF5800 -> $F402, $F406, $F40A, ...
```

That is the VDP's interleaved Plane A / Plane B line-scroll table.

### 6.5 Vertical scrolling

At the start of every full graphics VBlank, the handler selects VSRAM and
writes:

```text
VSRAM word 0 = primary camera vertical/depth scroll
VSRAM word 1 = secondary camera vertical/depth scroll
```

The primary value comes from `$FFE00E (camera_y)`. Unlike horizontal
parallax, which may vary by strip or scanline, vertical scroll is ordinarily
one value per plane. Round 7's vertical camera state changes these values and
bounds over time rather than introducing a separate renderer.

---

## 7. Incremental name-table streaming

### 7.1 Dirty detection

`$18AF8 (update_cameras_and_queue_tilemaps)` runs the primary camera through
`$18C22 (dispatch_primary_camera)` and the secondary through
`$19332 (dispatch_secondary_camera)`. It then calls
`$18B3C (queue_camera_tilemap_updates)` for each plane.

Each parallax descriptor stores current and previous X/Y positions. The routine
XORs them and masks off the low four bits. If the result is nonzero, the strip
crossed a 16-pixel metatile boundary:

- changed X exposes a vertical metatile column;
- changed Y exposes a horizontal metatile row.

Only the newly exposed edge is generated.

### 7.2 Four update queues

| Reference | Plane / direction |
|---|---|
| `$FFEA00 (primary_tilemap_row_update_queue)` | Plane A horizontal row |
| `$FFEB20 (secondary_tilemap_row_update_queue)` | Plane B horizontal row |
| `$FFEC40 (primary_tilemap_column_update_queue)` | Plane A vertical column |
| `$FFECE0 (secondary_tilemap_column_update_queue)` | Plane B vertical column |

`$1951A (build_tilemap_row_update)` reads 32 block IDs and expands every ID
through the plane's 8-byte metatile dictionary. The two tile rows are stored in
split queue areas so VBlank can write both name-table rows efficiently.

`$1953A (build_tilemap_column_update)` walks block IDs at block-map-stride
intervals and emits each metatile's two adjacent tile words.

### 7.3 World-to-name-table address conversion

`$19656 (resolve_blockmap_and_vdp_address)` performs two calculations from the
same world/map coordinates:

1. divide/shift into the byte block map and return a pointer to the first block
   ID;
2. convert the low coordinate bits into a VDP name-table write command.

The name-table offset is masked with `$0FFC`, wrapping inside one `$1000`-byte
plane table. The routine then selects Plane A or Plane B command bits. This
implements a toroidal cache: world coordinates can grow across the level while
the visible 64 × 32 cell VRAM table is continually overwritten at wrapped
positions.

### 7.4 VBlank queue consumption

`$197D8 (flush_tilemap_update_queues)` invokes
`$19800 (flush_one_tilemap_update_queue)` for primary and secondary planes.

The column queue consumer:

1. reads a VDP destination and count;
2. writes one longword, representing two horizontal tiles;
3. advances the VDP address by `$80`, one 64-cell name-table row;
4. repeats until the column entry is complete.

The row queue consumer:

1. reads the first-row and second-row buffers;
2. writes one longword to the first tile row;
3. advances the VDP address by `$80`;
4. writes the second longword;
5. advances horizontally by four bytes;
6. repeats across the exposed row.

Zero headers terminate both queues. The producer and consumer therefore need
no global count and can remain bounded to only the edges dirtied that frame.

---

## 8. Sprite engine

### 8.1 Object pass and render eligibility

`$AD8E (update_objects_and_build_sprites)` is the common gameplay and
screen-object dispatcher. It:

1. dispatches P1 and P2 by object type;
2. performs gameplay-only collision/camera work;
3. executes `$10514 (wait_vblank_without_graphics_upload)`;
4. scans P1, P2, and 66 `$80`-byte object slots;
5. dispatches each active object's update function;
6. sends eligible objects to a render-depth bucket;
7. rebuilds the linked active SAT prefix and its zero-link terminator.

The old name suggesting a character-select-only pass was too narrow. This is
the central object/render bridge used during normal gameplay as well as menus
and ending scenes.

`$AE4C (enqueue_object_render_bucket)` excludes empty objects and objects whose
non-render flag is set. It clamps object lane Y to `0..$F0`, quantizes it, and
inserts the object's 16-bit RAM pointer into
`$FFE200 (render_depth_buckets)`.

There are 64 buckets, each `$20` bytes:

```text
+$00 word  byte count used by pointer entries
+$02 ...   object pointers
```

### 8.2 Depth order

`$AE96 (build_sprite_attribute_table)` traverses buckets from `$FFE9E0`
downward to `$FFE200 (render_depth_buckets)`, meaning greater lane/depth values are handled first.
Within a populated bucket, `$AEEE (sort_render_bucket_by_depth)` sorts object
pointers by descending object `+$14`; flag bit 7 breaks equal-depth ties.

Mega Drive sprite-to-sprite priority favors the earlier SAT link, so emitting
foreground objects first gives them the stronger hardware ordering. This is
the beat-'em-up overlap rule: a fighter lower on the street covers one farther
up the street even when both use ordinary VDP priority.

### 8.3 Mapping lookup

`$AF46 (emit_object_sprite_mapping)` begins with:

| Object field | Renderer use |
|---:|---|
| `+$04` | animation/mapping-set pointer |
| `+$08` | animation offset-table index |
| `+$0A` | current animation frame |
| `+$0E` | base tile attribute word |
| `+$10` | world X |
| `+$14` | lane/depth coordinate |
| `+$18` | vertical/elevation coordinate |
| `+$28` | cached screen X |
| `+$2C` | cached screen Y |
| `+$51` | dynamic player-art dirty byte |
| `+$52/+$53` | two player-art DMA IDs |

The mapping frame resolves to a list of five-byte pieces:

```text
signed byte  Y offset
byte         VDP sprite size
word         tile attributes / relative tile index
signed byte  X offset
```

The frame header supplies the piece count and object collision/size metadata.
A flag in the frame offset selects mirrored decoding. The mirrored path toggles
the tile horizontal-flip bit and adjusts X by the piece width so the same source
pieces can face both directions.

### 8.4 World-to-screen conversion

For ordinary world objects:

```text
screen_x = world_x + $80 - primary_camera_x
screen_y = lane_y / 2 + elevation_y + $80 - primary_camera_y
```

The `$80` bias is the VDP's sprite-coordinate convention. Objects with the
screen-space flag bypass camera subtraction, which is used for UI and scene
actors.

After a broad object cull, every mapping piece is clipped independently:

- `$B0E8 (cull_sprite_piece_y)` accepts biased Y `$60-$17F`;
- `$B0FE (cull_sprite_piece_x)` accepts biased X `$60-$1DF`.

Converted back from the `$80` bias, those ranges provide approximately a
32-pixel margin around the 320 × 224 viewport. A rejected X piece rolls back
the provisional SAT record and link index, so culling does not leave holes in
the chain.

### 8.5 SAT record construction

Each emitted record in `$FFDA00 (sprite_attribute_table_buffer)` is:

```text
+0 word  biased Y
+2 byte  width/height size code
+3 byte  link to next SAT record
+4 word  priority, palette, flips, and tile index
+6 word  biased X
```

The renderer begins with:

```text
sprite_link_index     = 0
sprite_slots_remaining = $4F
destination           = $FFDA00
```

For each accepted piece it increments
`$FFFA02 (sprite_link_index)`, writes that value as the current record's next
link, and decrements `$FFFA03 (sprite_slots_remaining)`. Some screens reserve
the first two records for fixed overlays and subtract them from the same
budget.

When construction finishes, the renderer clears the next record's first
longword. Its link byte becomes zero, terminating the hardware chain. Starting
with a `$4F` budget lets it emit at most 79 visible records while preserving
the 80th record as that zero-link terminator. The next full VBlank copies all
80 entries to VRAM address F000h, including the terminator and inaccessible
stale records beyond it.

The Mega Drive still enforces its per-scanline sprite and pixel limits. The
software's 79-visible-record cap prevents SAT overflow, but it does not remove the
hardware's line-limit behavior.

---

## 9. Palette engine

### 9.1 Active and target buffers

The palette system has two complete 64-word buffers:

| Reference | Role |
|---|---|
| `$FFF400 (palette)` | active CRAM shadow |
| `$FFF480 (target_palette_buffer)` | fade-in destination |

`$805A (clear_palette_buffers)` zeros both. Palette ROM lists are sparse: each
word encodes a destination index, a 12-bit Mega Drive colour (masked with
`$0EEE`), and a terminator bit.

- `$10538 (load_palette_list_to_target)` decodes into the target buffer.
- `$1053E (load_palette_list_to_active)` decodes directly into the active
  buffer.

Both set `$FFFA01 (palette_dirty)`.

### 9.2 Fade out and fade in

`$10576 (palette_fade_out_step)` walks all 64 active colours. The low two bits
of `$FFFB0C (palette_fade_counter)` choose which RGB component is processed on
that call. A component is reduced by one Mega Drive intensity step without
underflow.

`$105CC (palette_fade_in_step)` compares each active component with the same
component in `$FFF480 (target_palette_buffer)` and raises it toward the target.

The component rotation spreads a full-colour transition across frames while
keeping the per-call loop simple. Both routines decrement the shared fade
counter and request a CRAM upload.

### 9.3 Animated palettes

`$1119E (init_animated_palette_sequences)` selects a per-level list rooted at
ROM `$1C400`. It builds descriptors in RAM near `$FFEE00` and expands their
colour/index frames near `$FFEE40`.

`$11210 (update_animated_palettes)` runs once per gameplay frame. Each
descriptor contains:

- frame delay and reload delay;
- current animation frame;
- frame count;
- pointer to expanded `(palette byte offset, colour word)` pairs;
- number of colour pairs.

When a descriptor advances, only its selected words in `$FFF400 (palette)` are
patched, then `$FFFA01 (palette_dirty)` is set. Level index 7 adds camera/wave
gating so some palette frames advance only after particular scroll positions.

### 9.4 CRAM upload

VBlank consumes only bit 0 of `$FFFA01 (palette_dirty)`. If set, it:

1. requests the Z80 bus;
2. sets DMA length to `$0040` words;
3. sets source to `$FFF400 (palette)`;
4. issues CRAM DMA command `$C0000080`;
5. releases the Z80 bus.

This uploads the entire 128-byte CRAM image. Individual gameplay routines patch
only the necessary RAM words, but the interrupt deliberately favors one fixed,
coherent transfer over variable CRAM transactions.

---

## 10. HUD, Window plane, and direct tilemap APIs

### 10.1 Rectangular map writers

The engine has three small generic VDP layout writers:

| Reference | Source format |
|---|---|
| `$8064 (vdp_copy_stride)` | header plus literal 16-bit tile words |
| `$808C (vdp_fill_stride)` | destination/width/height plus constant word |
| `$80DC (vdp_copy_stride_encoded)` | byte codes converted to tile words by adding a base |

The headered copy routines add `$00800000` to the VDP command between rows,
which corresponds to the `$80`-byte stride of a 64-cell plane.

`$A5F4 (load_vdp_tilemap_bundle)` processes four packed IDs through
`$A612 (vdp_tilemap_bundle_table)`. It is used for UI panels, text layouts, and
screen decorations.

`$A8B8 (load_encoded_vdp_tilemap_bundle)` is the byte-coded counterpart. It is
useful when a compact byte grid can be mapped onto a contiguous font/tile base.

### 10.2 Window and HUD composition

The Window name table is fixed at VRAM `$D000`, between Plane A and Plane B.
Gameplay uses it and high-priority Plane A cells for HUD material that must not
move with the stage camera.

HUD logic generally edits RAM tile-word blocks during game time:

- clock digits are written in the `$FF6000` workspace;
- score and life/special routines patch their tile words;
- boss-health pointers feed `$10F80` during VBlank;
- dialogue and rectangular updates are queued around `$FFED80`;
- `$114DA` flushes those requests through the generic VDP writers.

Small menus may write tile words directly while the display is disabled.
Gameplay and animated screens defer volatile writes to their VBlank mode.

### 10.3 Priority

Both name-table entries and sprite tile words carry the VDP priority bit:

- Enigma can encode tile-word bit `$8000`;
- sprite mappings add object base attributes and may force bit `$8000`;
- HUD/name-table builders use high-priority tile bases where the overlay must
  cover the playfield.

Depth sorting solves sprite-vs-sprite overlap. The VDP priority bit solves
sprite/plane and high-plane/low-plane ordering. They are separate mechanisms.

---

## 11. End-to-end gameplay-frame pseudocode

```text
function gameplay_update():
    update clock, pause/join state, HUD state, palette animations

    update_objects_and_build_sprites():
        update P1 and P2
        update gameplay camera/collisions
        update parallax strips and queue exposed metatile rows/columns

        wait_vblank_without_graphics_upload()
            VBlank:
                sample controllers
                decode <= 5 queued Nemesis tiles
                tick sound

        for each active object slot:
            dispatch object logic
            if renderable:
                enqueue pointer by lane depth

        sprite_count = 0
        for bucket from foreground to background:
            sort bucket by depth
            for object in bucket:
                frame = resolve animation mapping
                screen = world - camera + VDP bias
                for piece in frame:
                    if visible and sprite_count < 79:
                        emit linked SAT record
        append zero-link terminator

    patch HUD and active palette shadows
    begin any pending incremental Nemesis stream

function global_loop_tail():
    wait_vblank_and_upload_graphics()
        VBlank:
            write VSRAM
            if palette dirty: DMA 64 words to CRAM
            DMA/flush h-scroll and dirty name-table edges
            DMA dirty player art
            DMA all 640 SAT bytes
            flush HUD / screen-specific tile writes
            sample controllers
            decode <= 5 queued Nemesis tiles
            tick sound
```

One subtle ordering point is important: the request-2 VBlank occurs inside the
object pass; the request-1 VBlank occurs at the global-loop tail. The engine is
not uploading a half-built SAT. It explicitly creates a safe interval between
input sampling and construction, then publishes the completed shadow on the
next full graphics transaction.

---

## 12. Live runtime verification

### 12.1 Procedure

The existing executable was run under `timeout -k 3` with local remote access.
`tools/reach_gameplay.py axel` navigated the real logo, story, title, mode
select, character select, and level intro using only controller input. The probe
then used:

- `read_vdp_state()`;
- `read_framebuffer()`;
- `read_palettes()`;
- `read_sat()`;
- `read_tilemap(Plane A/B)`;
- bounded work-RAM and VRAM reads;
- lockstep at a complete-frame boundary.

No ROM or work-RAM patches were used.

### 12.2 Observed level-1 gameplay state

The reached state was:

| Observation | Value |
|---|---:|
| `$FFFF00 (game_state)` | `$16` |
| `$FFFF02 (level)` | `0` |
| `$FFFF04 (wave)` | `0` |
| `$FFFF06 (vblank_mode)` | `0` |
| `$FFE002 (cam_x)` | `$0300` |
| `$FFE00E (camera_y)` | `$0000` |
| framebuffer | 320 × 224 |
| plane geometry | 64 × 32 cells |

The VDP snapshot reported:

```text
R1=$74  R2=$30  R4=$07  R5=$78
R11=$00 R12=$81 R13=$3D R15=$02
Plane A=$C000, Window=$D000, Plane B=$E000
SAT=$F000, h-scroll=$F400
```

These values exactly match `$7FA4 (runtime_vdp_register_values)` and the
statically decoded VRAM layout.

### 12.3 SAT and scroll evidence

The 640-byte VDP SAT shadow matched
`$FFDA00 (sprite_attribute_table_buffer)` byte for byte. The decoded active
chain contained six emitted records followed by a seventh, zero-link
terminator. This
confirms both the RAM-shadow address and the linked-record interpretation.

With `$FFE002 (cam_x)=$0300` and R11 full-screen mode, the first two h-scroll
words at VRAM address F400h were both `-$0300`; the remaining sampled words were zero.
This matches `$19DA6 (vblank_write_scroll_and_backgrounds)`, which negates and
writes primary and secondary X before the background-upload dispatcher.

VSRAM's first two words were both zero, matching the observed primary and
secondary vertical positions.

### 12.4 Palette handoff evidence

One running snapshot caught `$FFFA01 (palette_dirty)=1`: CRAM differed from
`$FFF400 (palette)` in two animated entries. A later complete-frame lockstep
snapshot with the dirty flag clear showed all 64 CRAM words equal to the active
RAM shadow.

This is the expected producer/consumer boundary:

```text
game-time palette patch -> dirty bit set
next full graphics VBlank -> fixed 64-word CRAM DMA -> dirty bit clear
```

The target buffer did not equal the active buffer during gameplay, which is
also expected after fade-in has completed and active palette animation has
continued to patch selected colours.

---

## 13. Evidence ledger

| Claim | Primary evidence | Confidence |
|---|---|---:|
| VRAM bases are A C000h, Window D000h, B E000h, SAT F000h, h-scroll F400h | `$7F6C-$7FA3`, VDP registers, live snapshot | 100% |
| graphics handoff uses request values 1 and 2 | `$10502 (wait_vblank_and_upload_graphics)`, `$10514 (wait_vblank_without_graphics_upload)`, `$19D16 (vblank_handler)` | 100% |
| request 2 skips frame graphics | early compare/branch at `$19D32` to `$1A07C` | 100% |
| full SAT is 80 × 8 bytes and DMA-copied each graphics VBlank | `$AE96 (build_sprite_attribute_table)`, `$19F9E-$1A06D`, live RAM/SAT equality | 100% |
| sprite mappings contain five-byte pieces | `$AFFA-$B0B5` and mirrored `$B05C-$B0B5` | 100% |
| objects are ordered by lane/depth buckets | `$AE4C (enqueue_object_render_bucket)`, `$AEEE (sort_render_bucket_by_depth)` | 100% |
| background maps stream on 16-pixel boundary crossings | `$18B3C (queue_camera_tilemap_updates)` masks X/Y deltas with `$FFF0` | 100% |
| plane name tables are circular 4 KiB caches | `$19656 (resolve_blockmap_and_vdp_address)` masks with `$0FFC` | 100% |
| level h-scroll modes are full/full/line/line/cell/cell/full/cell | ROM words at `$199B6 (level_hscroll_mode_table)` | 100% |
| active palette is a 64-word CRAM shadow | `$10576-$10615`, VBlank `$19D3E-$19D85`, live equality | 100% |
| incremental enemy art is capped at five tiles per VBlank | `$8510 (continue_incremental_nemesis_decode)` and saved budget | 100% |
| exact visual identity of every art-bundle ID | requires exhaustive VRAM-to-asset atlas | 75-95% |
| semantic purpose of every camera special state | state arithmetic is known; some one-off scene effects remain visually unmapped | 80-95% |

---

## 14. Analysis-data update ledger

The graphics study rejected three misleading historical labels:

- the former `build_sprite_table` at `$1C44 (update_player_object)` is the
  main player-object update;
- the former `object_manager_loop` at `$784 (process_timed_spawn_records)`
  consumes timed ELC spawn records;
- the former `display_list_head` at `$FFFC14 (elc_spawn_stream_cursor)` is the
  current cursor in that spawn stream.

The actual sprite pipeline is now named around:

- `$AD8E (update_objects_and_build_sprites)`;
- `$AE4C (enqueue_object_render_bucket)`;
- `$AE96 (build_sprite_attribute_table)`;
- `$AEEE (sort_render_bucket_by_depth)`;
- `$AF46 (emit_object_sprite_mapping)`;
- `$B0E8 (cull_sprite_piece_y)`;
- `$B0FE (cull_sprite_piece_x)`.

The background/VBlank path is now named around:

- `$18AF8 (update_cameras_and_queue_tilemaps)`;
- `$18B3C (queue_camera_tilemap_updates)`;
- `$190E0 (build_hscroll_strip_buffer)`;
- `$1951A (build_tilemap_row_update)`;
- `$1953A (build_tilemap_column_update)`;
- `$19656 (resolve_blockmap_and_vdp_address)`;
- `$196A4 (dispatch_background_vblank_upload)`;
- `$197D8 (flush_tilemap_update_queues)`;
- `$19D16 (vblank_handler)`;
- `$19DA6 (vblank_write_scroll_and_backgrounds)`.

The principal new RAM symbols cover:

- the two block maps and two metatile dictionaries;
- strip and per-line horizontal-scroll buffers;
- render-depth buckets;
- four tilemap update queues;
- the active/target palette pair and dirty flag;
- the VBlank handshake and mode;
- the SAT construction cursor and sprite budget;
- the cached display-on/display-off VDP commands.

---

## 15. Remaining useful experiments

1. Capture one stable frame in every level and correlate each R11 mode with the
   exact `$196B2 (background_vblank_upload_table)` target selected by the
   primary camera state.
2. Build a VRAM atlas that maps every
   `$A662 (nemesis_art_bundle_table)` ID to tile range, screen, and lifetime.
3. Trace a deliberately crowded fight to determine which visible artifacts are
   caused by the 20-sprites/320-pixels per-line hardware limits rather than the
   79-visible-record software cap.
4. Record the special per-level VDP command block at `$181BC` for every round
   where `$FFFA5A` is active and attach each DMA to its visible effect.
5. Decode the remaining animation-frame header bytes into a formal mapping
   schema, including the exact collision-box metadata shared with gameplay.

The central renderer, frame transaction, RAM/VRAM layout, map streamer, sprite
builder, and palette pipeline are nevertheless established at high confidence.
