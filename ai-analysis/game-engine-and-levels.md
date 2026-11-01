# Streets of Rage Game Engine and Level Architecture

**Scope:** the main runtime dispatcher, round initialization, level data formats,
camera and scrolling, entity spawning, wave/scene progression, round completion,
and the RAM structures that connect those systems.

**Primary source:** `output/sor.asm` (68000 disassembly). ROM bytes from
`rom/SOR.bin` were used only to decode tables that the disassembler represents as
gaps. Existing C++ and analysis documents were used as cross-checks, not as the
basis of the reconstruction.

**Terminology:** the game calls the persistent campaign index `$FFFF02 (level)` and the
within-round index `$FFFF04 (wave)`. In this manuscript, *round* means one of the eight
playable stages, *wave* means the value of `$FFFF04 (wave)`, and *spawn group* means a
group of entity records. These are deliberately separate terms: a wave is also a
camera/background phase and may contain several spawn groups.

---

## 1. Main result

The level engine is not one monolithic state machine. It is a set of cooperating
machines whose shared state lives in work RAM:

1. `$FFFF00 (game_state)` selects the global screen/mode.
2. During gameplay, the low byte of `$FFFB14 (status_bitfield_1)` selects a 12-state level
   pipeline.
3. The camera objects at `$FFE000 (primary_camera)` and `$FFE100 (secondary_camera)` have their own dispatch states.
4. `$FFFF04 (wave)` indexes both camera boundaries and per-wave resource descriptors.
5. The decompressed enemy-load-cue stream at `$FF6800 (elc_buffer)` drives initial, timed, and
   gated entity creation.
6. Enemy/object counters and completion flags feed back into the level pipeline.

The central gameplay loop can therefore be summarized as:

```text
global state $14 init
  -> global state $16 gameplay
       -> clock / pause / join / HUD / objects
       -> camera state machines
       -> level-pipeline state machine
       -> entity deaths and scripted objects raise phase events
       -> phase event increments wave and opens next camera boundary
       -> next resource descriptor + next portion of spawn stream
       -> boss/final phase sets completion condition
       -> force players to stage exit
       -> fade to global state $18 (round clear)
```

This division explains several otherwise confusing features of the assembly:

- `$FFFF04 (wave)` changes in the camera code, not in `$CC0 (spawn_object_batch)`.
- the level descriptor table mostly contains art/palette selectors, not enemy
  coordinates;
- the enemy records live in a separately Nemesis-compressed stream;
- scrolling is stopped by collapsing the camera's minimum and maximum X bounds,
  then resumed by opening one bound at the next wave;
- round 8 uses the same mechanism in reverse.

---

## 2. Global game engine

### 2.1 Main dispatcher

`$3A2 (game_infinite_loop)` treats `$FFFF00 (game_state)` as an even
byte offset into the longword table at `$3BA (game_state_handler_table)`:

```asm
move.w game_state,d0
add.w  d0,d0
move.l $3BA(pc,d0.w),d0
movea.l d0,a0
jsr     (a0)
jsr     wait_vblank_and_upload_graphics
bra.s   game_infinite_loop
```

Every mode has an initializer and an updater. Initializers conventionally add
two to the state, so the next iteration enters the updater.

| `$FFFF00 (game_state)` | Initializer / updater | Role |
|---:|---|---|
| `$00/$02` | `$8EAC/$8F44` | Sega logo |
| `$04/$06` | `$8FD0/$904E` | story introduction |
| `$08/$0A` | `$90C8/$9102` | title |
| `$0C/$0E` | `$9128/$9152` | high scores |
| `$10/$12` | `$9170/$919A` | mode/options menu |
| `$14/$16` | `$10840/$1087A` | gameplay or attract mode |
| `$18/$1A` | `$91A0/$91CE` | round clear |
| `$1C/$1E` | `$9250/$9278` | bad ending |
| `$20/$22` | `$927C/$92A8` | character select |
| `$24/$26` | `$91D4/$9214` | good ending |
| `$28/$2A` | `$106EA/$1077C` | round presentation |

The level engine exists mainly inside `$28/$2A` and `$14/$16`, but it is useful
to keep the global boundary in mind. Finishing the playable space does not
increment `$FFFF02 (level)`; it exits to round clear, whose state machine performs the
campaign increment.

### 2.2 Frame update order

On the normal, unpaused path, `$1087A (game_mode_ingame)` performs the
following calls in order:

```text
$11A36 (update_demo_timeout)
$10D2E (handle_pause_start_input)
$10C88 (update_game_clock)
$115CC (update_join_and_continue_hud)
$11B4C (mr_x_offer_update)
$117FC (stage_clear_monitor)
$112F2 (dispatch_per_level_ambient_effects)
$11210 (update_animated_palettes)
$AD8E  (update_objects_and_build_sprites)
$11346 (update_level_index2_background_effect)
$17FC2 (prepare_special_player_art_dma_step)
$84BA  (begin_incremental_nemesis_decode)
$436   (sub_state_dispatcher)
```

The `calls.csv` runtime sample records the 12 direct `jsr`/`bsr` targets from
`$1087A (game_mode_ingame)` 6,658 times each before the final jump to
`$436 (sub_state_dispatcher)`. That proves the stable frame order, not that
every conditional body did useful work in the captured level. In particular,
`$10C88 (update_game_clock)` only calls `$10CF4
(check_required_players_for_clock_tick)` when `$FFFB58 (milli_second)` reaches
zero, so the sample's 69 observed calls reflect the clock divider rather than a
per-frame helper. `$10CF4 (check_required_players_for_clock_tick)` then dispatches by `$FFFF18 (player_mode)` and permits
the BCD timer decrement only when the required active player object slots still
contain type `$01`.

Separately,
`$112F2 (dispatch_per_level_ambient_effects)` indexes `$11300
(per_level_ambient_effect_jt)` by `$FFFF02 (level)`, and the table entries for
level indexes 0 and 1 both return immediately through `$11344
(per_level_ambient_effect_noop_b)`. `$11346
(update_level_index2_background_effect)` is called every gameplay frame, but it only
updates frame/palette data after `$112C0 (init_level_index2_background_effect)` has
armed `$FFFA0C (round3_wave_tiles_enabled)` for level index 2.

The final jump is `$436 (sub_state_dispatcher)`. This ordering matters. An
entity may die or a camera/object routine may raise a flag during the frame, and
the level pipeline consumes it at the end of that same frame.

### 2.3 The level-pipeline dispatcher

The low byte at `$FFFB15 (level_pipeline_state)` is an even offset into the relative-word table at
`$44C (level_pipeline_jt)`. The high byte, `$FFFB14 (status_bitfield_1)`, is reused by art-loading code. Reading the ROM
table gives the exact pipeline:

| Low-byte state | Target | Observed role |
|---:|---:|---|
| `$00` | `$5F4 (init_level_state)` | select next wave descriptor; prepare art/palettes and initial objects |
| `$02` | `$6A6 (update_camera_scroll_if_needed)` | wait until camera corridor is locked, then normalize entities |
| `$04` | `$784 (process_timed_spawn_records)` | consume timed spawn records |
| `$06` | `$810 (prepare_next_spawn_section)` | preprocess/filter the next length-prefixed spawn section |
| `$08` | `$936 (select_deferred_spawn)` | scan deferred records for a spawn whose palette/art can be resident |
| `$0A` | `$B76 (load_deferred_spawn_art_and_spawn)` | load required art, then materialize the deferred entity |
| `$0C` | `$C28 (clear_object_table)` | wait for tracked gameplay entities to drain |
| `$0E` | `$059C` | transitional level-resource reload/fade path |
| `$10` | `$0748` | short wait for a scripted acknowledgement flag |
| `$12` | `$D70 (write_art)` | staged art-transfer sequencer |
| `$14` | `$04CA` | reload/reset after a player or phase transition |
| `$16` | `$464 (level_flow_handler)` | boss/end-phase music and flow gates |

The labels in the last column are functional descriptions, not claims that each
routine has only that purpose. Several states deliberately branch to different
successors depending on level number and flags.

The dispatch is inhibited while `$FFFA1A (police_special_active)` is nonzero. That byte/word is also
used by the special-attack sequence, so the level pipeline is paused during a
screen-wide special transition.

---

## 3. Starting a round

### 3.1 Round presentation initializer

`$106EA (init_levelstart)` is the destructive boundary between rounds. It:

- disables interrupts and resets the sound/VDP environment;
- clears almost all 64 KiB of work RAM;
- restores persistent fields that live at the top of RAM;
- clears `$FFFF04 (wave)` to zero;
- sets the round clock divider to `$5A`;
- marks `$FFFA1F (level_intro_active)`;
- initializes the palette fade to `$40`;
- loads the level background, camera, HUD, palettes, and auxiliary effects;
- calls `$E5C (start_round_setup)` to unpack the enemy-load-cue stream and seed entities;
- recreates active P1/P2 objects;
- changes `$FFFF00 (game_state)` from `$28` to `$2A`.

The important calls and their proven effects are:

| Address | Role |
|---:|---|
| `$19848 (load_level_graphics_maps_and_camera)` | clear camera RAM; load/decompress per-round plane data and tile maps; initialize both camera-plane objects |
| `$110AC (load_round_hud_resources)` | load round/HUD resources and construct HUD tile data |
| `$1119E (init_animated_palette_sequences)` | build per-round animated-palette descriptors at `$FFEE00 (animated_palette_descriptors)` |
| `$10E42 (load_status_screen_graphics)` | load/remap HUD character art (skipped in demo mode) |
| `$11564 (load_status_palette_and_art)` | load additional HUD/display assets |
| `$E5C (start_round_setup)` | unpack ELC data, select difficulty, seed the spawn stream and initial objects |

### 3.2 Round banner state machine

After initialization, `$1077C (game_mode_levelstart)` dispatches through
`$FFFB48 (level_intro_substate)` and the table at `$11A5C (level_intro_jt)`:

```text
$00 wait for fade-in
  -> $02 move both halves of "ROUND n" to the center; start music
  -> $04 wait $60 frames
  -> $06 move banner off screen
  -> $08 wait $30 frames
  -> $0A set game_state=$14
```

`$10840 (init_ingame)` then sets the fade and sub-second clock and advances immediately
to `$16`. During the remaining intro fade, gameplay calls the object/camera pass
but suppresses ordinary play until the fade completes and
`$FFFA1F (level_intro_active)` is cleared.

Attract mode is an exception: `$10840 (init_ingame)` itself calls `$106EA (init_levelstart)`,
loads scripted controls, and adjusts `$FFFF00 (game_state)` so that the same gameplay
updater is reused.

---

## 4. ROM level tables

### 4.1 Four independent kinds of level data

The code uses several tables indexed by `$FFFF02 (level)`, and they should not be merged
conceptually:

| ROM address | Entry form | Purpose |
|---:|---|---|
| `$1B036 (level_elc_offset_table)` | eight relative words | pointers to Nemesis-compressed enemy-load-cue streams |
| `$1C1C4 (level_wave_resource_table)` | eight relative words | roots of per-round wave/resource-descriptor sections |
| `$1C378 (level_resource_table)` | eight 6-byte records | pointers and small parameters for a separate level resource load |
| `$5F5B8 (level_graphics_package_table)` | per-round relative records | background/plane compressed resources and camera initialization data |
| `$195D8 (wave_camera_boundary_table)` | words | X boundary reached by successive waves; forward and reverse halves |
| `$11B44 (level_music_table)` | eight bytes | level BGM IDs |

This is a major source of apparent entropy in generated C++: “level loading” is
spread across code, and each table describes a different subsystem.

### 4.2 Enemy-load-cue compressed streams

`$E5C (start_round_setup)` indexes the word table at `$1B036 (level_elc_offset_table)`, adds the selected
relative offset to the table base, and calls `$81A4 (nemesisdec_ram)` with destination
`$FF6800 (elc_buffer)`. The eight compressed stream starts are:

| Round | Compressed source | Decoded bytes |
|---:|---:|---:|
| 1 | `$1B046` | `$1C0` |
| 2 | `$1B1D8` | `$280` |
| 3 | `$1B3F4` | `$240` |
| 4 | `$1B5D0` | `$240` |
| 5 | `$1B7DE` | 800 |
| 6 | `$1BA8C` | `$2E0` |
| 7 | `$1BD12` | `$1E0` |
| 8 | `$1BEBE` | `$380` |

The decompressed buffer contains entity records and control framing. It is not
background map data. This is proved by the destination `$FF6800 (elc_buffer)`, the immediate
assignment of `$FF6800 (elc_buffer)` to `$FFFC14 (elc_spawn_stream_cursor)`, and the record consumers at
`$784 (process_timed_spawn_records)`, `$810 (prepare_next_spawn_section)`, `$C60 (spawn_single_object)`, and `$CC0 (spawn_object_batch)`.

### 4.3 Per-round wave descriptor roots

At `$1C1C4 (level_wave_resource_table)` are eight offsets relative to `$1C1C4 (level_wave_resource_table)`:

```text
0010 003E 006C 009A 00C8 0106 0144 0176
```

They select round sections at `$1C1D4`, `$1C202`, `$1C230`, `$1C25E`,
`$1C28C`, `$1C2CA`, `$1C308`, and `$1C33A`.

Each section starts with word offsets to wave descriptors. The first descriptor
is special: `$E5C (start_round_setup)` reads a longword spawn-stream pointer and a word
geometry selector before passing the remaining six bytes to `$F46 (load_resource_descriptor_palettes)`. Later wave
loads at `$5F4 (init_level_state)` use the offset indexed by `wave + 1` and pass it directly to
`$F46 (load_resource_descriptor_palettes)`; those entries are six-byte resource descriptors.

The decoded index arrays are:

| Round | Descriptor offsets from round-section base |
|---:|---|
| 1 | `$0A,$16,$1C,$22,$28` |
| 2 | `$0A,$16,$1C,$22,$28` |
| 3 | `$0A,$16,$1C,$22,$28` |
| 4 | `$0A,$16,$1C,$22,$28` |
| 5 | `$0E,$1A,$20,$26,$2C,$32,$38` |
| 6 | `$0E,$1A,$20,$26,$2C,$32,$38` |
| 7 | `$0E,$0E,$1A,$20,$26,$2C,$2C` |
| 8 | `$0E,$1A,$20,$26,$2C,$32,$38` |

The repeated offsets in round 7 are present in the ROM; they are not a parsing
error. They fit round 7's unusual vertical-camera phase handling, but the exact
semantic meaning of every repeated descriptor field remains uncertain.

### 4.4 Six-byte resource descriptor

`$F46 (load_resource_descriptor_palettes)` proves that the descriptor is six bytes, but it does not expose a
simple six-field structure. Its accesses are overlapping:

```text
bytes 0..3  copied to $FFFA60..$FFFA63 (palette/art residency selectors)
bytes 4..5  copied as a word to $FFFA6A..$FFFA6B
byte 4      also decoded as either two nibbles or an indexed enemy-art family
byte 5      decoded as another art/palette index
```

Bit 7 of byte 4 selects between two lookup families:

- clear: its high and low nibbles select entries through `$1FACC`;
- set: the low seven bits select a six-byte entry through `$1C3A8`.

The selected pointers are passed to `$10538 (load_palette_list_to_target)` during round start and `$1053E (load_palette_list_to_active)`
during later phases. Those routines consume resource command lists. It is safer
to call these *resource descriptors* than “palette headers”: they coordinate
palette residency and enemy art uploads together.

### 4.5 Separate six-byte resource table at `$1C378 (level_resource_table)`

`$576 (load_level_data)` multiplies `$FFFF02 (level)` by six, reads a longword pointer, calls
`$1053E (load_palette_list_to_active)`, and leaves the final two bytes available to its caller. The records are:

| Round | Resource-list pointer | Byte 4 | Byte 5 |
|---:|---:|---:|---:|
| 1 | `$02F4A2` | `$1C` | `$00` |
| 2 | `$02F44E` | `$1B` | `$00` |
| 3 | `$03513C` | `$08` | `$00` |
| 4 | `$02F4DE` | `$1D` | `$00` |
| 5 | `$02F532` | `$1D` | `$00` |
| 6 | `$02F44E` | `$1B` | `$00` |
| 7 | `$03513C` | `$08` | `$00` |
| 8 | `$035172` | `$05` | `$00` |

`$59C` consumes byte 4 as a resource/sound-load parameter and waits for the art
queue. The meaning of byte 5 is not proved by the inspected path.

---

## 5. Spawn stream and entity records

### 5.1 Object RAM

Players occupy fixed `$80`-byte objects at `$FFB800 (p1_object)` and `$FFB880 (p2_object)`. General
gameplay entities use 32 `$80`-byte slots beginning at `$FFB900 (object_table)`. The free-slot
test at `$E46 (find_free_object_slot)` is simply `object+0 == 0`.

Common fields used by the level loader are:

| Object offset | Width | Loader use |
|---:|---:|---|
| `+$00` | byte | entity type; bit 7 in the script is stripped before activation |
| `+$01` | byte | object flags; loader sets round-specific and tracked-enemy bits |
| `+$10` | word / long root | world X integer / 16.16 position |
| `+$14` | word / long root | depth-axis position |
| `+$18` | word / long root | height/screen-axis position |
| `+$40` | byte | script parameter copied verbatim |
| `+$41` | byte | script parameter copied verbatim |
| `+$49` | byte | variant/palette parameter copied verbatim |

The later object-specific initializer interprets these generic parameters. The
level loader intentionally does not need to know whether the resulting object
is an enemy, item, weapon, breakable, or controller object.

### 5.2 Six-byte entity record

Both `$C60 (spawn_single_object)` and `$CC0 (spawn_object_batch)` consume the same compact record:

```text
+0 type_and_player_count_flag
+1 object+$40 parameter
+2 object+$41 parameter
+3 object+$49 variant
+4 X or side selector
+5 depth/difficulty bits
```

This last description needs qualification. In batch mode, byte `+4` becomes the
low byte of X after a phase base is loaded, while byte `+5` becomes object
`+$15`. Before spawning, the low two bits of byte `+5` are compared with
`$FFFB5A (difficulty_copy)`; the record is skipped if it requires a higher difficulty.

Bit 7 of the type byte is a player-count qualifier. The loader clears it and
spawns the record only when `player_mode_copy == 3` (two-player mode). Thus the
same stream scales both by difficulty and by active-player count without
separate lists.

In `$C60 (spawn_single_object)`, byte `+4` is instead treated as a side selector: zero
spawns at `cam_x + $148`, nonzero at `cam_x - 8`. This is the timed/off-screen
spawn interpretation of the same compact record.

### 5.3 Initial three-column batches

At round start, the first six bytes of the round header set:

- `$FFFC14 (elc_spawn_stream_cursor)` to `$FF6800 (elc_buffer)`;
- a geometry index into the four-word table at `$1C1BC (spawn_geometry_table)`.

The geometry record supplies an X stride and base. `$CC0 (spawn_object_batch)` is then
called three times; between calls the base is advanced by the stride. Each batch
has this framing:

```text
signed word count_minus_one
six-byte record × (count_minus_one + 1)
```

A negative count means an empty batch. This produces up to three spatial columns
from a compact shared record format.

### 5.4 Timed list

After the initial batches, `$784 (process_timed_spawn_records)` treats the current stream position as a timed
list:

```text
word delay
six-byte entity record
word delay
six-byte entity record
...
word $0099 terminator
```

Each record is filtered by difficulty and the two-player bit before the delay is
armed. When the timer expires, one free object slot is acquired and `$C60 (spawn_single_object)`
spawns the entity just outside the visible camera edge. `$0099` moves the
pipeline to the next phase rather than spawning an entity.

### 5.5 Length-prefixed and gated sections

`$810 (prepare_next_spawn_section)` handles the next layer of the ELC stream. It reads a word length, saves the
payload pointer, advances `$FFFC14 (elc_spawn_stream_cursor)` by that length, and filters compact
six-byte records in place until a byte `$99` terminator. This creates a current
section and a pointer to the following section without allocating another
buffer.

ROM decoding proves the higher-level framing. A regular wave block is exactly:

```text
three signed count-minus-one batches
timed (delay, six-byte record) list, terminated by word $0099
word byte_length
six-byte deferred records, terminated by bytes $99,$00
```

The byte after the length-delimited section begins the next wave's first
counted batch. This parses without guessing through four complete blocks in
Rounds 1-4 and six in Rounds 5-8. Round 7's counted batches are all `$FFFF`
(empty), so its entities come entirely from timed and deferred records. Round 8
then has a separately consumed final-scene/boss tail after its six regular
blocks.

The ROM census below records `batch/timed/deferred` counts for each regular
block and provides a compact consistency check:

| Round | Per-block record counts |
|---:|---|
| 1 | `6/2/2, 10/3/3, 12/3/3, 10/3/2` |
| 2 | `15/2/3, 22/2/2, 16/2/4, 16/2/3` |
| 3 | `13/2/3, 11/3/4, 18/3/3, 9/2/2` |
| 4 | `9/3/2, 14/3/3, 17/3/2, 13/3/3` |
| 5 | `15/6/0, 12/8/6, 11/0/2, 11/1/1, 7/8/0, 13/0/5` |
| 6 | `8/3/4, 11/2/2, 10/3/3, 10/2/2, 9/3/3, 11/3/4` |
| 7 | `0/5/5, 0/4/8, 0/4/6, 0/4/7, 0/6/10, 0/0/0` |
| 8 | `9/2/3, 12/2/4, 14/2/4, 12/2/4, 12/2/4, 11/2/6` |

The `$936 -> $B76` states scan this filtered section and enforce limited
palette/art residency. If a required resource is not resident, the record is
deferred while `$1053E (load_palette_list_to_active)` and `$8454 (queue_nemesis_art_cues)` queue the needed art/palette. Once
`$FFDCD0 (art_array_cue)` becomes zero and a slot is free, the entity is spawned and the
remaining records are compacted over it.

The incremental-art queue is a fixed eight-record FIFO. Each record is six
bytes, and the completion path copies 12 longwords from source `+6` to the
queue head: seven remaining slots plus the zero sentinel. The producer has no
bounds check, so the level resource lists must never leave a ninth record
queued.

This explains the cluster at `$FFFA60..$FFFA70`: it is a small resource-residency
allocator for active enemy families, not merely a palette array.

### 5.6 Tracked-entity count closes a phase

Enemy-like types in the range `$20..$2A` are classified through `$9350 (is_nonordinary_enemy_type)`. When a
spawned type is in the tracked subset, the loader:

- sets object flag bit 3;
- increments the word at `$FFFB1E (active_progression_entity_count)`.

Death/removal paths at `$92F0 (release_progression_entity)` and `$9D8C` decrement the same counter. Several
pipeline states wait for it to reach zero before clearing their local flags or
starting the next phase. The exact count is therefore “active level-script
entities that gate progression,” not a count of all allocated objects.

### 5.7 Pseudocode

```c
void start_round_setup(void) {
    nemesis_decode(level_elc[level], 0xFF6800);
    difficulty_copy = difficulty_override ? NORMAL : difficulty;

    section = level_wave_sections[level];
    initial = section + section.offset[wave]; // wave == 0 here
    spawn_cursor = initial.spawn_stream_ptr;  // normally 0x00FF6800
    geometry = spawn_geometry[initial.geometry_index];
    load_resource_descriptor(initial.resources);

    for (int column = 0; column != 3; ++column) {
        spawn_counted_batch(spawn_cursor, geometry.x_base);
        geometry.x_base += geometry.x_stride;
    }
}

void spawn_timed_record(void) {
    if (!timer_armed) {
        delay = read_word(spawn_cursor);
        if (delay == 0x0099) {
            enter_next_pipeline_phase();
            return;
        }
        record = spawn_cursor + 2;
        if (!record_allowed(record, difficulty_copy, player_mode_copy)) {
            spawn_cursor += 8;
            return;
        }
        timer = delay & 0x7FFF;
    }
    if (--timer == 0 && free_slot()) {
        spawn_single(record);
        spawn_cursor = record + 6;
    }
}
```

---

## 6. Camera and scrolling

### 6.1 Two camera-plane objects

`$19848 (load_level_graphics_maps_and_camera)` creates two related structures:

- primary plane/camera at `$FFE000 (primary_camera)`;
- secondary plane/camera at `$FFE100 (secondary_camera)`.

Each begins with a dispatch state word and uses 16.16 positions and velocities:

| Offset | Meaning established by arithmetic |
|---:|---|
| `+$00` | even camera state index |
| `+$02` | current X (16.16; `$FFE002 (cam_x)` is its integer word) |
| `+$06` | previous X |
| `+$0A` | X velocity |
| `+$0E` | current Y |
| `+$12` | previous Y |
| `+$16` | Y velocity |
| `+$1A` | maximum X boundary |
| `+$1E` | minimum X boundary |
| `+$22` | maximum Y boundary |
| `+$26` | minimum Y boundary |
| `+$2A` | pointer to decompressed plane map |
| `+$2E` | map width/stride |
| `+$32` | tile-definition pointer |
| `+$44...` | strip/parallax descriptors and previous positions |

The former CSV names `scene_length` for `$FFE01A (camera_x_max)` and
`scenario_x_position` for `$FFE01E (camera_x_min)` were imprecise. The clamp code at `$19074 (integrate_camera_x_clamped)`
proves that they are the primary camera's maximum and minimum X bounds;
`addresses.csv` now records them as `$FFE01A (camera_x_max)` and `$FFE01E (camera_x_min)`.

### 6.2 Player-follow velocity

`$18F5C (compute_camera_follow_x_velocity)` derives a horizontal velocity from player world positions:

- in one-player mode, the active player is kept within a horizontal dead zone;
- in two-player mode, ordered player positions are used so the camera tries to
  contain both;
- the result is clamped to approximately `-3..+3` pixels per frame.

`$18FE8 (compute_camera_follow_y_velocity)` performs the analogous depth/vertical calculation, clamped to
approximately `-4..+4`. `$19074 (integrate_camera_x_clamped)` and `$190AA (integrate_camera_y_bounded)` integrate those velocities and
clamp the resulting 16.16 positions to the active min/max bounds.

The global player-boundary routine at `$43AA (clamp_players_to_gameplay_bounds)` separately prevents players from
leaving the visible combat region. This is why the camera constraint and player
constraint should not be conflated.

### 6.3 Wave boundaries are scroll gates

`$19570 (advance_wave_camera_boundary)` consumes bit 0 of `$FFFA0D (wave_advance_pending)`, increments `$FFFF04 (wave)`, and loads a boundary
from `$195D8 (wave_camera_boundary_table)`.

For rounds 1–7, the forward boundary list is:

```text
$04C0, $07C0, $0AC0, $0DC0, $10C0, $13C0, $FFFF
```

The new value is written to camera `max_x` (`+$1A`), and current X is copied to
`min_x` (`+$1E`). In plain language, the next corridor opens to the right while
the player is prevented from walking back beyond the previous stop.

For round 8, the code adds `$0E` to the table index and uses:

```text
$1100, $0E00, $0B00, $0800, $0500, $0200, $0080, $FFFF
```

Here the new value is written to `min_x`, while current X is copied to `max_x`.
The exact same machine therefore opens the corridor to the left. This is direct
assembly evidence that round 8 is a reverse-scrolling level, not a separate
camera implementation.

When `$FFFF` is reached, `$FFFF04 (wave)` is restored and the fade-out flag is set. This
is one of the generic end-of-scroll paths.

### 6.4 Detecting a locked combat arena

`$6A6 (update_camera_scroll_if_needed)` computes:

```text
camera.max_x - camera.min_x
```

If the difference is four or more, scrolling is still open and the routine
returns. When it is below four, the camera is effectively locked at a scene
boundary. The routine then:

- clears the spawn-flow busy flag;
- enters pipeline state `$04`;
- scans all 32 general object slots;
- pushes relevant objects to the appropriate off-screen edge;
- normalizes their depth to `$60` (or `$20` in round 4);
- forces a movement state for most entity classes.

Round 8 reverses the edge comparison and adds `$180` to the reference point,
matching its leftward progression.

### 6.5 Tile-map streaming and parallax

The camera state machine at `$18C22 (dispatch_primary_camera)` updates strip descriptors and emits dirty
rows/columns into command buffers at `$FFEA00/$FFEB20` and
`$FFEC40/$FFECE0`. `$19656 (resolve_blockmap_and_vdp_address)` converts world/map coordinates into VDP tile-map
addresses. `$196A4 (dispatch_background_vblank_upload)` and the VBlank handler transfer those commands to VRAM.

`$190E0 (build_hscroll_strip_buffer)` derives horizontal-scroll values from camera X and per-strip ratios,
which is the parallax mechanism. The secondary plane has its own state table at
`$19332 (dispatch_secondary_camera)`; in some rounds it follows primary X at a fraction (round 1 uses direct
handling, later rounds may quarter X), while special round states animate it
independently.

The separation is architecturally important:

```text
camera state -> 16.16 position and bounds
             -> strip dirty detection
             -> map-to-VRAM commands
             -> per-strip horizontal-scroll values
             -> VBlank DMA/VDP writes
```

Gameplay objects use world coordinates. Rendering subtracts camera coordinates
later; the level script does not store screen-space positions.

### 6.6 Round 7 vertical transition

Round index 6 is exceptional. `$18F32 (advance_round7_vertical_wave)` also consumes `$FFFA0D (wave_advance_pending)`, increments
`$FFFF04 (wave)`, and drives a vertical transition through flags `$FFFA14..$FFFA16` and
secondary-camera boundary values. `$192F4` saves the ordinary camera bounds,
changes the camera dispatch state, clears the horizontal minimum, and configures
a `$0500` vertical target. Later states can restore the saved bounds.

This path is why round 7 cannot be accurately described as a simple sequence of
horizontal rooms. Its phase event drives camera/background movement in another
axis before the ordinary completion monitor marks the round finished.

---

## 7. Wave progression and end of a round

### 7.1 What raises a wave event

Bit 0 of `$FFFA0D (wave_advance_pending)` is the common “advance camera/wave phase” event. It is set by:

- scripted object completion paths;
- the art/flow sequencer after a phase is ready;
- particular entity/controller types such as the destructible/scripted objects
  at `$7678 (final_section_type46_shatter_state)` and `$7E42 (round7_fixed_type53_delay_delete_state)`.

It is consumed with `bclr`, so it is an edge-like request rather than a durable
mode. The ordinary camera machine (`$19570 (advance_wave_camera_boundary)`) consumes it for most rounds; round
7 also has the special consumer at `$18F32 (advance_round7_vertical_wave)`.

### 7.2 Resource and spawn transition

When a phase changes, the code coordinates four things:

1. increment `$FFFF04 (wave)`;
2. change the camera bound, opening the next corridor;
3. reset the round timer through `$195F6 (reset_wave_timer)`;
4. use the next `$1C1C4 (level_wave_resource_table)` descriptor and remaining ELC stream to prepare the next
   resident art and entities.

The timer values also depend on phase:

- rounds 1–4 normally use `$40`, later/boss phases `$50`;
- rounds 5–7 normally use `$50`;
- round 8 uses `$60`.

These are stored as BCD-like display values in `$FFFB00 (game_timer)`, not frame counts;
`$FFFB58 (milli_second)` supplies the `$5A` frame divider.

### 7.3 Boss/end-phase gate

`$5F4 (init_level_state)` applies hard-coded thresholds in addition to the data tables:

- rounds 1–4 switch to the late/end path when `wave >= 3`;
- rounds 5–7 do so when `wave >= 5`;
- round 8 sets an additional phase flag after `wave >= 5`;
- round 7's late path stops the clock and sets `$FFFA73 (end_of_level_flag)` directly.

`$464 (level_flow_handler)` waits for tracked entities to drain, changes
music/art state, and raises the boss/late-phase flags. `$11B12 (play_level_music)` uses
bit 6 of `$FFFA05 (level_spawn_flow_flags)` to select boss music (`$90` for round 8, `$87` otherwise)
instead of the ordinary per-level BGM.

The boss implementation itself is documented separately; the engine-level
point is that bosses are introduced through the same object/resource pipeline,
then completion is signaled back through counters and flags rather than a
special global `BOSS` game state.

### 7.4 Completion monitor

`$117FC (stage_clear_monitor)` bridges late-stage conditions to
`$FFFA73 (end_of_level_flag)`:

- it normally waits for the late/boss phase flag;
- round 8 is withheld from the generic immediate path because its final offer
  and encounter have separate control;
- round 7 sets `$FFFA73 (end_of_level_flag)` when its special phase is complete;
- alternate scoreboard/versus-related flags route through additional UI logic.

`$502C (end_level_player_exit_update)`, called from player logic, then removes
ordinary control and walks each active player to the stage's exit target. Once
the player reaches the target, it waits on `$FFFA74 (end_of_level_exit_timer)`, sets `$FFFA71 (fade_out_flag)`, and
initializes `$FFFB0C (palette_fade_counter)` to `$40`.

After the fade, `$108CC (ingame_finish_fade)` writes `game_state = $18` in normal campaign
play. Round clear later increments `$FFFF02 (level)` and writes `$28` for the next round.

### 7.5 End-to-end pseudocode

```c
void gameplay_frame(void) {
    if (intro_fade_active()) {
        update_intro_objects_and_camera();
        if (fade_finished()) level_intro_active = 0;
        return;
    }
    if (fade_out_flag) {
        if (fade_finished()) game_state = NORMAL_CAMPAIGN ? 0x18 : demo_exit();
        return;
    }

    update_clock_pause_hud_and_objects();
    update_primary_and_secondary_camera();
    dispatch_level_pipeline(level_pipeline_state);
}

void consume_phase_advance(void) {
    if (!test_and_clear(phase_advance_pending)) return;
    ++wave;

    if (level == ROUND_8) {
        camera.max_x = camera.x;
        camera.min_x = reverse_boundaries[wave];
    } else {
        camera.min_x = camera.x;
        camera.max_x = forward_boundaries[wave];
    }

    if (selected_boundary == 0xFFFF) {
        --wave;
        begin_fade_out();
        return;
    }

    reset_wave_timer();
    prepare_next_resource_descriptor_and_spawn_section();
}
```

---

## 8. RAM map relevant to levels

The following map separates established structure from provisional naming.

| Address | Size | Meaning / evidence |
|---:|---:|---|
| `$FF6800 (elc_buffer)` | buffer | Nemesis-decoded enemy-load-cue/spawn stream |
| `$FFB800 (p1_object)` | `$80` | P1 object |
| `$FFB880 (p2_object)` | `$80` | P2 object |
| `$FFB900 (object_table)` | `$1000` | 32 general `$80`-byte entity slots |
| `$FFDCD0 (art_array_cue)` | 8 × 6-byte records | incremental Nemesis art FIFO; zero head means drained |
| `$FFE000 (primary_camera)` | structure | primary camera/plane state |
| `$FFE002 (cam_x)` | long root | primary camera X 16.16; integer word is named `$FFE002 (cam_x)` |
| `$FFE01A (camera_x_max)` | long | primary camera maximum X boundary |
| `$FFE01E (camera_x_min)` | long | primary camera minimum X boundary |
| `$FFE100 (secondary_camera)` | structure | secondary camera/plane state |
| `$FFEA00 (primary_tilemap_row_update_queue)` | buffer | primary-plane horizontal-row update commands |
| `$FFEB20 (secondary_tilemap_row_update_queue)` | buffer | secondary-plane horizontal-row update commands |
| `$FFEC40 (primary_tilemap_column_update_queue)` | buffer | primary-plane vertical-column update commands |
| `$FFECE0 (secondary_tilemap_column_update_queue)` | buffer | secondary-plane vertical-column update commands |
| `$FFFA05 (level_spawn_flow_flags)` | byte | level/spawn flow flags; bits 2–6 gate filtering, drain, boss, and transitions |
| `$FFFA0D (wave_advance_pending)` | byte | bit 0: pending wave/camera phase advance |
| `$FFFA14..16` | bytes | round-7 vertical transition timing/state |
| `$FFFA60..70` | bytes | enemy palette/art residency slots and reference counts |
| `$FFFA71 (fade_out_flag)` | byte | fade-out request |
| `$FFFA72 (level_flow_flags)` | byte | high-level phase/music/art gates |
| `$FFFA73 (end_of_level_flag)` | byte | round completion / player exit request |
| `$FFFB12 (spawn_x_base)` | word | current spawn X base (low byte advanced between columns) |
| `$FFFB14 (status_bitfield_1)` | word | high byte: art sequencer bits; low byte: level pipeline state |
| `$FFFB16 (level_pipeline_timer)` | word | shared pipeline delay timer |
| `$FFFB1E (active_progression_entity_count)` | word | tracked scripted gameplay entities alive |
| `$FFFB20 (spawn_x_stride)` | word | spawn-column X stride |
| `$FFFC14 (elc_spawn_stream_cursor)` | long | active ELC stream cursor |
| `$FFFC28 (pending_spawn_record_ptr)` | long | deferred record selected for spawn |
| `$FFFC2C (current_spawn_section_ptr)` | long | pointer to current filtered/length-prefixed ELC section |
| `$FFFC30 (pending_spawn_resource_ptr)` | long | required resource-list pointer for deferred spawn |
| `$FFFF02 (level)` | word | round index, `0..7` |
| `$FFFF04 (wave)` | word | wave/camera phase index |

One subtlety is address width. Camera values are manipulated as longwords, but
many comparisons access only the integer word. Thus `$FFE002 (cam_x)` is both the start
of a 16.16 X longword and the conventional integer-word alias used by gameplay.

---

## 9. Special cases by round

| Round index | Confirmed engine exception |
|---:|---|
| 0 | resource-residency scans use fewer slots; first-level enemy palette logic has special branches |
| 2 | two type `$4E` controller objects are created at X `$288/$2B8`; special background animation and camera DMA paths |
| 3 | locked-arena object depth is `$20` rather than `$60` |
| 4 | spawned entities receive object flag bit 4; special palette/background behavior |
| 6 | four fixed objects `$53,$52,$51,$50` are seeded; vertical camera transition; clock stops at completion |
| 7 | initial geometry selector is 4; camera progression runs right-to-left; final Mr. X offer/ending branch |

These exceptions are embedded as `cmpi.w #level` branches rather than described
entirely by data. The engine is data-driven, but not data-only.

---

## 10. Evidence ledger

| Claim | Assembly evidence | Confidence |
|---|---|---:|
| main modes are init/update pairs | `$3A2 (game_infinite_loop)`, long table `$3BA (game_state_handler_table)` | 100% |
| gameplay level pipeline has 12 states | `$436 (sub_state_dispatcher)`, ROM words `$44C..$463` | 100% |
| ELC data is Nemesis-decoded to `$FF6800 (elc_buffer)` | `$E5C..$E82` | 100% |
| entity records are six bytes | `$C60..$CBE`, `$CC0..$D60` | 100% |
| low two bits of record byte 5 gate difficulty | `$790..$7C0`, `$CCA..$CF0` | 100% |
| type bit 7 means two-player-only | same paths, comparison with `player_mode_copy == 3` | 100% |
| `$FFE01A/$FFE01E` are camera max/min X | clamp at `$19074 (integrate_camera_x_clamped)`, phase update at `$19570 (advance_wave_camera_boundary)` | 100% |
| round 8 reverses camera-boundary direction | `$1957E..$195D0`, second half of `$195D8 (wave_camera_boundary_table)` | 100% |
| `$FFFF04 (wave)` is a camera/resource phase index | `$5F4 (init_level_state)`, `$E98`, `$18F32 (advance_round7_vertical_wave)`, `$19570 (advance_wave_camera_boundary)` | 100% |
| `$FFFB1E (active_progression_entity_count)` counts progression-gating entities | increments `$C60/$CC0`, decrements `$92F0/$9D8C`, zero tests in pipeline | 99% |
| all semantics of each six-byte resource descriptor | overlapping decoder `$F46 (load_resource_descriptor_palettes)` | 75% |
| exact semantic name of every `$FFFA05 (level_spawn_flow_flags)` bit | many distributed producers/consumers | 75% |
| repeated round-7 descriptor offsets encode vertical phases | ROM table plus round-7 camera exceptions | 70% |
| incremental Nemesis queue holds eight records | `$8454 (queue_nemesis_art_cues)` append scan and fixed 12-longword shift at `$8592` | 100% |

---

## 11. Code-label confirmation audit

The open descriptor-field questions above are intentionally narrower than the
code labels. The following previously sub-100% entries are now confirmed for
their stated, bounded behavior:

| Address | Confirmed boundary | Direct evidence |
|---:|---|---|
| `$464 (level_flow_handler)` | Late-phase entity-drain, art, music, palette, and status gates in `$FFFA72 (level_flow_flags)`. | Every read/write is a bit operation on `$FFFA72 (level_flow_flags)` or a documented phase side effect; it is pipeline state `$16`. |
| `$576 (load_level_data)` | Select the six-byte per-level record at `$1C378 (level_resource_table)`, process its pointer, and leave its trailing parameters to the caller. | `level*6`, `(a3)+` longword, call `$1053E (load_palette_list_to_active)`, then return with `a3` advanced four bytes. |
| `$E5C (start_round_setup)` | Decode the selected ELC stream, choose the effective difficulty, seed the first wave descriptor, and create the round-specific fixed objects. | The level-indexed `$1B036 (level_elc_offset_table)` decode to `$FF6800 (elc_buffer)` and all eight level branches are explicit. |
| `$810 (prepare_next_spawn_section)` | Split the next length-prefixed ELC section and compact records that pass difficulty/player-count gates. | The word length advances `$FFFC14 (elc_spawn_stream_cursor)`; surviving six-byte records are copied in place until `$99`. |
| `$936 (select_deferred_spawn)` | Scan the filtered section and select a record whose ordinary-enemy palette/art residency can be satisfied. | Record/type checks feed the `$FFFA60..$FFFA70` residency counters and save the selected pointer at `$FFFC28 (pending_spawn_record_ptr)`. |
| `$B76 (load_deferred_spawn_art_and_spawn)` | Finish any required resource load, spawn the selected record, and compact the remainder over it. | It waits for `$FFDCD0 (art_array_cue)`, calls the resource loader/spawner, then shifts six-byte records through the `$99` terminator. |
| `$8454 (queue_nemesis_art_cues)` | Resolve an art-set list and append six-byte source/destination records to the incremental Nemesis queue. | The producer and `$84BA/$8510` consumer agree on the exact longword-source/word-VRAM record layout. |
| `$112F2 (dispatch_per_level_ambient_effects)` | Dispatch per-frame level-specific ambient effects without implying they run in every level. | `$11300 (per_level_ambient_effect_jt)` maps level indexes 0 and 1 to a no-op, level index 2 to `$11310 (round3_delayed_sfx_burst_update)`, and level index 4 to `$1132E (round5_periodic_sfx_update)`; the early-level `calls.csv` sample observes the dispatcher call, not an active effect body. |
| `$11346 (update_level_index2_background_effect)` | Poll the level-index-2 background effect state during gameplay. | `$112C0 (init_level_index2_background_effect)` only arms `$FFFA0C (round3_wave_tiles_enabled)` when `$FFFF02 (level) == 2`; otherwise `$11346 (update_level_index2_background_effect)` returns without changing the decoded frame/palette pointers. |
| `$17FC2 (prepare_special_player_art_dma_step)` | Stage one chunk of special player-art DMA metadata for the VBlank-side uploader. | It runs only while `$FFFA5A` is set, advances `$FFFB54` by ten bytes, writes `$FFFB64..$FFFB6E`, and `$181BC (upload_special_player_art_dma_step)` emits those prepared VDP commands during VBlank. |
| `$18F32 (advance_round7_vertical_wave)` | Consume the round-7 wave request and start its vertical-camera transition. | It clears the request, increments `$FFFF04 (wave)`, and writes the round-7 camera/transition flags and bounds. |
| `$19848 (load_level_graphics_maps_and_camera)` | Load the per-level mixed-codec graphics/map package and initialize both camera-plane structures. | Its fixed table walk performs two Kosinski uploads, two Enigma RAM decodes, map construction, then two calls to `$19922 (init_camera_plane)`. |

These confirmations do not assign names to every byte inside a resource
descriptor; they establish the entry points and observable contracts actually
recorded in `labels.csv`.

---

## 12. Remaining uncertainties and useful next experiments

1. **Resource descriptor field names.** `$F46 (load_resource_descriptor_palettes)` combines nibbles, table families,
   and overlapping words. Runtime logging of calls to `$10538/$1053E` would
   distinguish palette lists, tile art, and mappings precisely.
2. **Round 7 repeated descriptor offsets.** A trace of `$FFFF04 (wave)`, pipeline state,
   camera state, and `$FFFA14..16` during the elevator/vertical sequence would
   prove why offset `$0E` and `$2C` are reused.
3. **Late-stage flags.** `$FFFA05 (level_spawn_flow_flags)` is clearly a level-flow bitset, but some bits
   combine “boss phase,” “resource drain,” and “special scene” behavior. Naming
   individual bits should follow producer/consumer traces rather than one call
   site.
4. **Background resource formats.** The camera structure and streaming algorithm
   are established, but the exact packed format below `$5F5B8/$5F788` deserves a
   separate graphics-engine manuscript.

The highest-value runtime trace would record once per change:

```text
frame, level, wave, $FA05, $FA0D, $FA72, $FB15, $FB1E,
camera_state, camera_x, min_x, max_x, elc_spawn_stream_cursor
```

That small trace would validate the static state graph without requiring a full
instruction log.

## Routines identified from the call map

Named from `call-map.sqlite` callers and read in `output/sor.asm`. The confidence and exact contract of each name are in `code-analysis/labels.csv`.

| Routine | What it does |
| --- | --- |
| `$188BE (round_clear_player_tally_dispatch)` | From round_clear_substate $14 to $51, outside level index 7, dispatches on player_mode through the table at $188EC |
| `$1260E (build_top10_score_screen)` | Loads the screen, starts music $8D and draws the ten entries with colour from each entry flag |
| `$12730 (high_score_check_player_entry)` | Compares a player score with the 10th entry and marks the name entry in +$4B bit 7 when it beats it |
| `$12832 (init_top10_score_table)` | Enigma-decodes the default ten-entry table at $12842 into top10_score_table at power-on |
| `$18324 (round_clear_add_bonus_repeated)` | Adds the score table entry d0 to the total at a4 d6 times through add_bcd_score_value |
| `$18824 (vblank_round_clear_draw_tally_request)` | Consumes round_clear_tally_draw_request and draws one of the tally rows through the table at $18840 |
| `$18A02 (vblank_round_clear_dispatch_request)` | Consumes round_clear_vblank_request and runs its entry of the table at $18A1E |
| `$18A96 (vblank_round_clear_draw_player_scores)` | Draws the score digits of the active players, by player_mode through the table at $18AB4 |
| `$12766 (high_score_clear_player_new_flags)` | Clears the player bit (7 for P1, 6 for P2) in all ten top10_score_table entries and returns d0 = 0 |
| `$127BA (sort_top10_score_table)` | Sorts the twelve-byte entries of top10_score_table by score, ignoring the two flag bits, then by the fields at +$0A and +$04 |

## Object type map

`$B236 (object_type_update_jt)` holds one word per type. `$B36A` is a plain `rts` and marks an unused type; those rows are left out. Types `$96`-`$99` are words whose handlers are in bank `$1xxxx`: the call map observed `$17858 (bongo_flame_update)`, `$16C60 (souther_claw_type98_update)` and `$16C24 (souther_afterimage_type99_update)` for `$97`-`$99`, and the table's low words match. A name of the form `object_typeNN_dispatcher` means the type is certain and its visual identity is not.

| Type | Handler |
| --- | --- |
| `$01` | `$1C44 (update_player_object)` |
| `$04` | `$8E2C (object_type04_dispatcher)` |
| `$05` | `$597C (police_vehicle_dispatcher)` |
| `$06` | `$18D4 (char_select_cursor_dispatcher)` |
| `$07` | `$1A12 (char_select_character_preview_dispatcher)` |
| `$08` | `$5C1E (knife_weapon_dispatcher)` |
| `$09` | `$6114 (bottle_weapon_dispatcher)` |
| `$0A` | `$61F6 (baseball_bat_weapon_dispatcher)` |
| `$0B` | `$6226 (steel_pipe_weapon_dispatcher)` |
| `$0C` | `$6256 (pepper_spray_weapon_dispatcher)` |
| `$0D` | `$63F6 (p2_police_bombardment_dispatcher)` |
| `$0E` | `$654E (p1_police_blast_dispatcher)` |
| `$0F` | `$5218 (continue_object_dispatcher)` |
| `$10` | `$56E6 (high_score_name_entry_dispatcher)` |
| `$11` | `$6AF4 (phone_booth_dispatcher)` |
| `$12` | `$6E2A (object_type12_dispatcher)` |
| `$13` | `$6DDC (object_type13_dispatcher)` |
| `$14` | `$6D40 (object_type14_dispatcher)` |
| `$15` | `$760C (object_type15_dispatcher)` |
| `$16` | `$7214 (object_type16_dispatcher)` |
| `$17` | `$6F7C (object_type17_dispatcher)` |
| `$18` | `$70C4 (object_type18_dispatcher)` |
| `$19` | `$6C84 (breakable_type19_dispatcher)` |
| `$1A` | `$77B6 (object_type1a_dispatcher)` |
| `$1B` | `$7370 (object_type1b_dispatcher)` |
| `$1C` | `$7448 (object_type1c_dispatcher)` |
| `$1D` | `$746A (object_type1d_dispatcher)` |
| `$1E` | `$61BE (bottle_shard_dispatcher)` |
| `$1F` | `$7CBC (object_type1f_dispatcher)` |
| `$20` | `$D606 (garcia_type20_dispatcher)` |
| `$21` | `$D99A (garcia_type21_dispatcher)` |
| `$22` | `$DD78 (garcia_type22_32_dispatcher)` |
| `$23` | `$E326 (garcia_type23_dispatcher)` |
| `$24` | `$E4D2 (signal_type24_dispatcher)` |
| `$25` | `$E8F0 (haku_ro_type25_dispatcher)` |
| `$26` | `$F038 (nora_type26_dispatcher)` |
| `$27` | `$F27E (jack_type27_dispatcher)` |
| `$28` | `$FFFC14 (elc_spawn_stream_cursor)` |
| `$29` | `$B3C0 (police_special_enemy_sweep_trampoline)` |
| `$2A` | `$F7D0 (haku_ro_type2a_dispatcher)` |
| `$30` | `$B36C (abadede_update_trampoline)` |
| `$31` | `$B372 (abadede_linked_sprite_update_trampoline)` |
| `$32` | `$DD78 (garcia_type22_32_dispatcher)` |
| `$33` | `$B37E (mr_x_office_controller_update_trampoline)` |
| `$34` | `$B384 (mr_x_office_scene_type34_trampoline)` |
| `$35` | `$B378 (mr_x_boss_update_trampoline)` |
| `$36` | `$B38A (mr_x_bullet_update_trampoline)` |
| `$37` | `$B390 (mr_x_shell_update_trampoline)` |
| `$38` | `$B396 (mr_x_type38_update_trampoline)` |
| `$39` | `$B39C (abadede_variant_type39_trampoline)` |
| `$3E` | `$673C (demo_mode_type3e_dispatcher)` |
| `$3F` | `$68E2 (points_3000_pickup_dispatcher)` |
| `$40` | `$6904 (points_10000_pickup_dispatcher)` |
| `$41` | `$791A (object_type41_dispatcher)` |
| `$42` | `$7A6C (object_type42_dispatcher)` |
| `$43` | `$7C70 (object_type43_dispatcher)` |
| `$44` | `$7644 (object_type44_dispatcher)` |
| `$45` | `$7534 (round8_table_dispatcher)` |
| `$46` | `$7662 (final_section_type46_dispatcher)` |
| `$47` | `$6988 (full_health_pickup_dispatcher)` |
| `$48` | `$67A4 (wave_go_prompt_dispatcher)` |
| `$49` | `$6862 (player_contact_effect_dispatcher)` |
| `$4A` | `$6894 (object_ground_shadow_dispatcher)` |
| `$4B` | `$6926 (small_health_pickup_dispatcher)` |
| `$4C` | `$6948 (extra_life_pickup_dispatcher)` |
| `$4D` | `$6E9E (object_type4d_dispatcher)` |
| `$4E` | `$71DC (round3_controller_type4e_dispatcher)` |
| `$4F` | `$6968 (extra_special_pickup_dispatcher)` |
| `$50` | `$7D4A (round7_fixed_type50_dispatcher)` |
| `$51` | `$7D7A (round7_fixed_type51_dispatcher)` |
| `$52` | `$7DF0 (round7_fixed_type52_dispatcher)` |
| `$53` | `$7E20 (round7_fixed_type53_dispatcher)` |
| `$54` | `$B3BA (object_type54_dispatcher)` |
| `$55` | `$B3A2 (souther_update_trampoline)` |
| `$56` | `$B3A8 (antonio_update_trampoline)` |
| `$57` | `$B3AE (bongo_update_trampoline)` |
| `$58` | `$B3B4 (onihime_yasha_update_trampoline)` |
| `$60` | `$CD22 (presentation_type60_plane_fill_dispatcher)` |
| `$61` | `$D112 (presentation_type61_palette_fade_dispatcher)` |
| `$62` | `$D210 (presentation_type62_dispatcher)` |
| `$63` | `$D2A4 (presentation_type63_palette_swap_dispatcher)` |
| `$64` | `$CBE6 (presentation_type64_tile_strip_anim_dispatcher)` |
| `$65` | `$CCE8 (presentation_type65_camera_y_drift_dispatcher)` |
| `$66` | `$D32C (presentation_type66_dispatcher)` |
| `$67` | `$D362 (presentation_type67_dispatcher)` |
| `$68` | `$D39C (presentation_type68_dispatcher)` |
| `$69` | `$CD66 (presentation_type69_dispatcher)` |
| `$6A` | `$CA7C (presentation_type6a_dispatcher)` |
| `$6B` | `$CFB8 (presentation_type6b_dispatcher)` |
| `$6C` | `$D150 (presentation_type6c_dispatcher)` |
| `$6D` | `$D08E (presentation_type6d_dispatcher)` |
| `$6E` | `$C6E0 (presentation_type6e_slide_in_sprite_dispatcher)` |
| `$6F` | `$C756 (presentation_type6f_slide_in_sprite_dispatcher)` |
| `$70` | `$C618 (presentation_type70_dispatcher)` |
| `$71` | `$D3D6 (presentation_type71_dispatcher)` |
| `$72` | `$D444 (presentation_type72_dispatcher)` |
| `$73` | `$D4B2 (presentation_type73_dispatcher)` |
| `$74` | `$D0AC (presentation_type74_dispatcher)` |
| `$75` | `$C7D0 (presentation_type75_slide_in_sprite_dispatcher)` |
| `$76` | `$C846 (presentation_type76_slide_in_sprite_dispatcher)` |
| `$77` | `$C394 (presentation_type77_dispatcher)` |
| `$78` | `$C924 (presentation_type78_blinking_sprite_dispatcher)` |
| `$79` | `$C58C (presentation_type79_palette_cycle_dispatcher)` |
| `$7A` | `$C97A (presentation_type7a_vertical_sprite_dispatcher)` |
| `$7B` | `$C9D0 (presentation_type7b_horizontal_sprite_dispatcher)` |
| `$7C` | `$CA26 (presentation_type7c_horizontal_sprite_dispatcher)` |
| `$7D` | `$D548 (presentation_type7d_dispatcher)` |
| `$7E` | `$C36E (presentation_type7e_region_select)` |
| `$7F` | `$D5C8 (presentation_type7f_palette_fade_signal_dispatcher)` |
| `$80` | `$C89C (presentation_type80_dispatcher)` |
| `$81` | `$C492 (presentation_type81_dispatcher)` |
| `$82` | `$C114 (presentation_type82_dispatcher)` |
| `$83` | `$C17C (presentation_type83_dispatcher)` |
| `$84` | `$BF76 (presentation_type84_dispatcher)` |
| `$85` | `$BEBA (presentation_type85_dispatcher)` |
| `$86` | `$BF00 (presentation_type86_dispatcher)` |
| `$87` | `$BBEE (presentation_type87_dispatcher)` |
| `$88` | `$C8F2 (presentation_type88_dispatcher)` |
| `$89` | `$BC5C (presentation_type89_dispatcher)` |
| `$8A` | `$BCA2 (presentation_type8a_dispatcher)` |
| `$8B` | `$BD10 (presentation_type8b_dispatcher)` |
| `$8C` | `$BB94 (presentation_type8c_dispatcher)` |
| `$8D` | `$BB18 (presentation_type8d_dispatcher)` |
| `$8E` | `$BB58 (presentation_type8e_static_sprite_dispatcher)` |
| `$96` | `$17262 (antonio_linked_attack_dispatcher)` |
| `$97` | `$17858 (bongo_flame_update)` |
| `$98` | `$16C60 (souther_claw_type98_update)` |
| `$99` | `$16C24 (souther_afterimage_type99_update)` |

## Object state tables

Each object update hands `+$30` to `$B186 (dispatch_object_primary_state_table)` (absolute words) or `$12B3C (dispatch_presentation_object_state_table)` (offsets from the table). A name of the form `<object>_stateN` comes from the table alone: the binding is certain and the role is unread. `$103A2 (jack_projectile_type28_state_table)` is relative and has six words. A handler reached from several states lists them all in its name.

| Object | Table | States |
| --- | --- | --- |
| continue_object | `$5236 (continue_object_state_table)` | 3 |
| high_score_name_entry | `$56EE (high_score_name_entry_state_table)` | 3 |
| police_vehicle | `$5984 (police_vehicle_state_table)` | 13 |
| bottle_weapon | `$6126 (bottle_weapon_state_table)` | 6 |
| p2_police_bombardment | `$640A (p2_police_bombardment_state_table)` | 6 |
| p1_police_blast | `$6562 (p1_police_blast_state_table)` | 6 |
| demo_mode_type3e | `$6744 (demo_mode_type3e_state_table)` | 2 |
| wave_go_prompt | `$67AC (wave_go_prompt_state_table)` | 5 |
| player_contact_effect | `$686A (player_contact_effect_state_table)` | 2 |
| object_ground_shadow | `$689C (object_ground_shadow_state_table)` | 2 |
| points_3000_pickup | `$68F2 (points_3000_pickup_state_table)` | 3 |
| points_10000_pickup | `$6914 (points_10000_pickup_state_table)` | 3 |
| small_health_pickup | `$6936 (small_health_pickup_state_table)` | 3 |
| extra_life_pickup | `$6958 (extra_life_pickup_state_table)` | 3 |
| extra_special_pickup | `$6978 (extra_special_pickup_state_table)` | 3 |
| full_health_pickup | `$6998 (full_health_pickup_state_table)` | 3 |
| phone_booth | `$6B00 (phone_booth_state_table)` | 5 |
| breakable_type19 | `$6C90 (breakable_type19_state_table)` | 3 |
| object_type14 | `$6D48 (object_type14_state_table)` | 7 |
| object_type13 | `$6DEA (object_type13_state_table)` | 2 |
| object_type12 | `$6E3E (object_type12_state_table)` | 4 |
| object_type4d | `$6EA6 (object_type4d_state_table)` | 6 |
| object_type17 | `$6F8C (object_type17_state_table)` | 4 |
| object_type18 | `$70D0 (object_type18_state_table)` | 3 |
| round3_controller_type4e | `$71EA (round3_controller_type4e_state_table)` | 2 |
| object_type16 | `$7226 (object_type16_state_table)` | 9 |
| object_type1b | `$737C (object_type1b_state_table)` | 3 |
| object_type1c | `$7454 (object_type1c_state_table)` | 3 |
| object_type1d | `$7476 (object_type1d_state_table)` | 3 |
| round8_table | `$7540 (round8_table_state_table)` | 3 |
| object_type15 | `$7618 (object_type15_state_table)` | 2 |
| object_type44 | `$764C (object_type44_state_table)` | 2 |
| final_section_type46 | `$766A (final_section_type46_state_table)` | 2 |
| object_type1a | `$77BE (object_type1a_state_table)` | 2 |
| object_type41 | `$7926 (object_type41_state_table)` | 4 |
| object_type42 | `$7A7C (object_type42_state_table)` | 9 |
| object_type43 | `$7C78 (object_type43_state_table)` | 2 |
| object_type1f | `$7CC8 (object_type1f_state_table)` | 3 |
| round7_fixed_type50 | `$7D52 (round7_fixed_type50_state_table)` | 2 |
| round7_fixed_type51 | `$7D82 (round7_fixed_type51_state_table)` | 3 |
| round7_fixed_type52 | `$7DF8 (round7_fixed_type52_state_table)` | 2 |
| round7_fixed_type53 | `$7E28 (round7_fixed_type53_state_table)` | 2 |
| object_type04 | `$8E34 (object_type04_state_table)` | 5 |
| presentation_type8d | `$BB2A (presentation_type8d_state_table)` | 2 |
| presentation_type8e_static_sprite | `$BB60 (presentation_type8e_static_sprite_state_table)` | 2 |
| presentation_type8c | `$BBA6 (presentation_type8c_state_table)` | 2 |
| presentation_type87 | `$BC00 (presentation_type87_state_table)` | 2 |
| presentation_type89 | `$BC6E (presentation_type89_state_table)` | 2 |
| presentation_type8a | `$BCB4 (presentation_type8a_state_table)` | 2 |
| presentation_type8b | `$BD18 (presentation_type8b_state_table)` | 10 |
| presentation_type85 | `$BECC (presentation_type85_state_table)` | 2 |
| presentation_type86 | `$BF12 (presentation_type86_state_table)` | 2 |
| presentation_type84 | `$BF7E (presentation_type84_state_table)` | 7 |
| presentation_type82 | `$C126 (presentation_type82_state_table)` | 3 |
| presentation_type83 | `$C184 (presentation_type83_state_table)` | 7 |
| presentation_type77 | `$C39C (presentation_type77_state_table)` | 5 |
| presentation_type81 | `$C49A (presentation_type81_state_table)` | 4 |
| presentation_type79_palette_cycle | `$C594 (presentation_type79_palette_cycle_state_table)` | 3 |
| presentation_type70 | `$C620 (presentation_type70_state_table)` | 4 |
| presentation_type6e_slide_in_sprite | `$C6E8 (presentation_type6e_slide_in_sprite_state_table)` | 3 |
| presentation_type6f_slide_in_sprite | `$C75E (presentation_type6f_slide_in_sprite_state_table)` | 3 |
| presentation_type75_slide_in_sprite | `$C7D8 (presentation_type75_slide_in_sprite_state_table)` | 3 |
| presentation_type76_slide_in_sprite | `$C84E (presentation_type76_slide_in_sprite_state_table)` | 3 |
| presentation_type80 | `$C8A4 (presentation_type80_state_table)` | 3 |
| presentation_type88 | `$C8FA (presentation_type88_state_table)` | 2 |
| presentation_type78_blinking_sprite | `$C92C (presentation_type78_blinking_sprite_state_table)` | 2 |
| presentation_type7a_vertical_sprite | `$C982 (presentation_type7a_vertical_sprite_state_table)` | 2 |
| presentation_type7b_horizontal_sprite | `$C9D8 (presentation_type7b_horizontal_sprite_state_table)` | 2 |
| presentation_type7c_horizontal_sprite | `$CA2E (presentation_type7c_horizontal_sprite_state_table)` | 2 |
| presentation_type6a | `$CA84 (presentation_type6a_state_table)` | 5 |
| presentation_type64_tile_strip_anim | `$CBF8 (presentation_type64_tile_strip_anim_state_table)` | 3 |
| presentation_type65_camera_y_drift | `$CCFA (presentation_type65_camera_y_drift_state_table)` | 2 |
| presentation_type60_plane_fill | `$CD34 (presentation_type60_plane_fill_state_table)` | 2 |
| presentation_type69 | `$CD6E (presentation_type69_state_table)` | 10 |
| presentation_type6b | `$CFCA (presentation_type6b_state_table)` | 2 |
| presentation_type74 | `$D0B4 (presentation_type74_state_table)` | 2 |
| presentation_type61_palette_fade | `$D124 (presentation_type61_palette_fade_state_table)` | 2 |
| presentation_type6c | `$D162 (presentation_type6c_state_table)` | 3 |
| presentation_type62 | `$D224 (presentation_type62_state_table)` | 4 |
| presentation_type63_palette_swap | `$D2BC (presentation_type63_palette_swap_state_table)` | 3 |
| presentation_type66 | `$D334 (presentation_type66_state_table)` | 3 |
| presentation_type67 | `$D36A (presentation_type67_state_table)` | 3 |
| presentation_type68 | `$D3A4 (presentation_type68_state_table)` | 3 |
| presentation_type71 | `$D3E8 (presentation_type71_state_table)` | 3 |
| presentation_type72 | `$D456 (presentation_type72_state_table)` | 3 |
| presentation_type73 | `$D4C4 (presentation_type73_state_table)` | 3 |
| presentation_type7d | `$D550 (presentation_type7d_state_table)` | 3 |
| presentation_type7f_palette_fade_signal | `$D5D0 (presentation_type7f_palette_fade_signal_state_table)` | 2 |
| garcia_type20 | `$D60E (garcia_type20_state_table)` | 14 |
| garcia_type21 | `$D9A2 (garcia_type21_state_table)` | 13 |
| garcia_type22_32 | `$FFDD80 (police_special_palette_snapshot)` | 20 |
| garcia_type23 | `$E32E (garcia_type23_state_table)` | 13 |
| signal_type24 | `$E4DA (signal_type24_state_table)` | 14 |
| haku_ro_type25 | `$E8F8 (haku_ro_type25_state_table)` | 20 |
| nora_type26 | `$10362 (nora_type26_state_table)` | 13 |
| jack_type27 | `$1037C (jack_type27_state_table)` | 19 |
| haku_ro_type2a | `$103AE (haku_ro_type2a_state_table)` | 17 |
| mr_x_office_controller | `$12B64 (mr_x_office_controller_state_table)` | 24 |
| mr_x_office_scene_type34 | `$12F1C (mr_x_office_scene_type34_state_table)` | 6 |
| mr_x_type38 | `$12FBC (mr_x_type38_state_table)` | 4 |
| mr_x_bullet | `$140E4 (mr_x_bullet_state_table)` | 3 |
| mr_x_shell | `$1426E (mr_x_shell_state_table)` | 2 |
| abadede_variant_type39 | `$14320 (abadede_variant_type39_state_table)` | 2 |
| abadede | `$14466 (abadede_state_table)` | 15 |

### State roles

Read from the code; the street enemies, Jack and Abadede also from `enemy-ai.md`. The confidence of each name is in `code-analysis/labels.csv`.

| State handler | Role |
| --- | --- |
| `$5256 (continue_object_init_state)` | State 0: rebuilds the slot as type $0F |
| `$5278 (continue_object_countdown_state)` | State 1: the first one sets the overlay flag and plays $82; counts +$60 down |
| `$529A (continue_object_game_over_fade_state)` | State 2: sets $FFFA4B and starts the fade |
| `$56F4 (high_score_name_entry_idle_state)` | State 2: rts |
| `$56F6 (high_score_name_entry_init_state)` | State 0: clears the player side of the HUD and draws the entry |
| `$57D2 (high_score_name_entry_input_state)` | State 1: letter input on the press bits |
| `$5A12 (police_vehicle_arrive_state)` | State 1: waits for the art queue, plays $CE, loads a palette list and spawns a linked slot |
| `$5A96 (police_vehicle_drive_in_state)` | State 2: decelerates and stops at cam_x + $80 |
| `$5ACC (police_vehicle_spawn_rising_child_state)` | State 3: after 20 updates creates a child type $05 rising from this one |
| `$5B14 (police_vehicle_idle_state)` | State 4: rts |
| `$5B16 (police_vehicle_follow_parent_state)` | State 5: follows the X of the object in +$50 |
| `$5B22 (police_vehicle_rise_state)` | State 6: moves until +$60, then waits 12 updates |
| `$5B40 (police_vehicle_spawn_child_state)` | State 7: after +$52 creates another type $05 child |
| `$5B94 (police_vehicle_show_from_frame2_state)` | State 8: clears +$01 bit 2 from frame 2 |
| `$5BA4 (police_vehicle_spawn_timed_child_state)` | State 9: after +$52 creates a type $05 child with +$52 = $40 |
| `$5BE2 (police_object_delete_state)` | State 2 of the shell, and exit of the shell and vehicle: clears the slot |
| `$5BEC (police_vehicle_return_on_frame3_state)` | State $0A: from frame 3 ends the excursion through state $0C |
| `$5BF8 (police_vehicle_end_excursion_state)` | State $0C: advances the camera state of the excursion and clears the slot |
| `$5C0E (police_vehicle_timed_end_state)` | State $0B: counts +$54 down and ends the excursion |
| `$6132 (bottle_weapon_init_state)` | State 0: damage 3 and art $6FC1A |
| `$6532 (p2_police_bombardment_end_on_frame7_state)` | State 4: deletes itself on frame 7 |
| `$65BE (p1_police_blast_delay_state)` | State 1: counts +$50 down, then starts the fall |
| `$65DC (p1_police_blast_fall_state)` | State 2: falls to the floor and starts the next animation |
| `$6748 (demo_mode_type3e_init_state)` | State 0: fixed position and flags, no animation data |
| `$676C (demo_mode_type3e_idle_state)` | State 1: rts |
| `$67B6 (wave_go_prompt_init_state)` | State 0: art $6F06E, screen space, position by round |
| `$67F2 (wave_go_prompt_start_flash_state)` | State 1: four flashes, sound $CF, shown for 12 updates |
| `$6810 (wave_go_prompt_shown_state)` | State 2: counts down shown, then hides for 6 |
| `$6828 (wave_go_prompt_hidden_state)` | State 3: counts down hidden, then flashes again or ends the flashes |
| `$6856 (wave_go_prompt_repeat_state)` | State 4: counts down and goes back to state 1 |
| `$68F8 (points_3000_pickup_init_state)` | State 0: effect 4 |
| `$691A (points_10000_pickup_init_state)` | State 0: effect 5 |
| `$693C (small_health_pickup_init_state)` | State 0: effect 1 |
| `$695E (extra_life_pickup_init_state)` | State 0: effect 2 |
| `$697E (extra_special_pickup_init_state)` | State 0: effect 3 |
| `$699E (full_health_pickup_init_state)` | State 0: effect 0; the shared body of the pickup inits |
| `$6B0A (phone_booth_init_state)` | State 0: the booth, or a fragment when +$31 is set |
| `$6C52 (phone_booth_fragment_wait_state)` | State 3: counts +$47 down and goes to state 4 |
| `$6C66 (phone_booth_fragment_end_state)` | State 4: counts +$47 down and deletes it |
| `$6C96 (breakable_type19_init_state)` | State 0: on the floor at height $A0, solid |
| `$6CAC (breakable_type19_contact_state)` | State 1: waits for a hit, then loses solidity, drops its item and launches |
| `$6D0E (breakable_type19_launched_state)` | State 2: flies and bounces until +$47 deletes it |
| `$6D56 (object_type14_init_state)` | State 0: height $50 kept in +$60, 20-update timer |
| `$6D72 (object_type14_frame_step_state)` | States 1, 2 and 5: counts +$47 down and steps the frame and the state |
| `$6D88 (object_type14_start_fall_state)` | State 3: starts falling at 3 px |
| `$6D94 (object_type14_fall_state)` | State 4: falls to a floor 24 px under it |
| `$6DB2 (object_type14_respawn_state)` | State 6: deletes itself and reinstalls type $14 at the starting height |
| `$6DEE (object_type13_init_state)` | State 0: height from the table at $6E06 by +$49 |
| `$6E0E (object_type13_wait_flag_state)` | State 1: with +$40 set waits on $FFFA54 and then hides |
| `$6E46 (object_type12_init_state)` | State 0: art $6D32E, animation by +$60 |
| `$6E60 (object_type12_launch_state)` | State 1: launches and advances |
| `$6E7E (object_type12_fly_state)` | State 2: flies up until screen X $60 deletes it |
| `$6E94 (object_type12_offscreen_delete_state)` | State 3: deleted off screen |
| `$6EB2 (object_type4d_init_state)` | State 0: flags $4B |
| `$6EC2 (object_type4d_spawn_schedule_state)` | State 1: reads the X table $6F3E against the camera; a negative entry deletes it |
| `$6F4A (object_type4d_wait_state)` | States 2 and 3: counts +$62 down and spawns |
| `$6F5E (object_type4d_start_wait_state)` | State 4: loads a 64-update wait |
| `$6F6A (object_type4d_finish_wait_state)` | State 5: at the end goes back to state 1 and clears $FFFA54 |
| `$6F94 (object_type17_wait_on_screen_state)` | State 0: waits until the camera reaches it |
| `$6FEE (object_type17_spawn_children_state)` | State 1: cycles the palette and spawns copies of the type |
| `$7056 (object_type17_wait_batch_state)` | State 2: cycles the palette and counts down to the next batch |
| `$707C (object_type17_fall_state)` | State 3: moves until height $A0, then deleted |
| `$70D6 (object_type18_init_state)` | State 0: art $6D628 |
| `$7142 (object_type18_contact_state)` | State 1: waits for a hit, then breaks |
| `$71EE (round3_controller_type4e_init_state)` | State 0: height $60, art $6D672 |
| `$71FE (round3_controller_type4e_frame_flag_state)` | State 1: with $FFFA05 bit 6 and $FFF50E set shows frame 1 |
| `$7238 (object_type16_init_state)` | State 0: starts in the state +$40 selects |
| `$7250 (object_type16_idle_state)` | State 1: rts |
| `$7252 (object_type16_random_drift_state)` | State 2: picks a random X velocity |
| `$728A (object_type16_random_wait_state)` | State 3: picks a random wait and velocity |
| `$72E2 (object_type16_wait_state)` | State 5: counts +$50 down and returns to state 3 |
| `$72F4 (object_type16_wait_wave_step_state)` | State 4: at round3_wave_tile_step 1 moves up the lanes |
| `$7312 (object_type16_move_out_state)` | State 6: moves up the lanes until lane $30 |
| `$733A (object_type16_turn_wait_state)` | State 7: counts +$50 down, then moves back |
| `$7356 (object_type16_move_back_state)` | State 8: moves back down to lane $58 |
| `$7382 (object_type1b_init_state)` | State 0: art $6DB60 |
| `$745A (object_type1c_init_state)` | State 0: art $6DB94 |
| `$747C (object_type1d_init_state)` | State 0: art $6DBC8, or a fragment when +$0B is set |
| `$74A6 (object_type1d_contact_state)` | State 1: waits for a hit, then breaks |
| `$761C (object_type15_init_state)` | State 0: art $6EDC2, animation by +$40 |
| `$763C (object_type15_keep_priority_state)` | State 1: keeps +$01 bit 7 set |
| `$7650 (object_type44_init_state)` | State 0: art $6ED4A, frame from +$0B |
| `$766E (final_section_type46_init_state)` | State 0: art $6EDBC |
| `$7678 (final_section_type46_shatter_state)` | State 1: plays $C2, sets $FFFA0D and spawns fragments from the table $773E |
| `$77EC (object_type1a_init_state)` | State 0: art $6EDBC, plays $C2 |
| `$77FC (object_type1a_build_layout_state)` | State 1: clears $FFA000 and builds a layout by player_mode |
| `$792E (object_type41_init_state)` | State 0: art $6E46C, or a fragment when +$0B is set |
| `$795A (object_type41_contact_state)` | State 1: waits for a hit, then breaks |
| `$7A62 (object_type41_delete_timer_state)` | State 2: counts +$47 down and deletes it |
| `$7A8E (object_type42_init_state)` | State 0: height $40, damage $14 |
| `$7AA4 (object_type42_wait_player_near_state)` | State 1: waits for a player in a box around its X |
| `$7ADE (object_type42_shake_state)` | State 2: requests tile updates during +$57, then falls |
| `$7B48 (object_type42_fall_state)` | State 3: falls with two contact boxes |
| `$7BFA (object_type42_bounce_state)` | State 4: falls, plays $C6 on landing and bounces |
| `$7C36 (object_type42_wait_state)` | States 5 and 7: counts +$50 down, then starts rising |
| `$7C4A (object_type42_rise_state)` | State 6: rises to height $40, then waits 50 updates |
| `$7C68 (object_type42_restart_state)` | State 8: back to state 1 |
| `$7C7C (object_type43_init_state)` | State 0: 27 px above, art shared with type $42 |
| `$7C94 (object_type43_follow_parent_state)` | State 1: follows the type $42 in +$50, deleted from its state 7 |
| `$7CCE (object_type1f_init_state)` | State 0: art $6DEBE, solid |
| `$7CEA (object_type1f_contact_state)` | State 1: waits for a hit, then breaks |
| `$7D56 (round7_fixed_type50_init_state)` | State 0: fixed at X $3B0 |
| `$7D78 (round7_fixed_type50_idle_state)` | State 1: rts |
| `$7D88 (round7_fixed_type51_init_state)` | State 0: fixed at X $300, lane 0 |
| `$7DAA (round7_fixed_type51_wait_elevator_state)` | State 1: waits on round7_elevator_moving, then plays $C8 and moves |
| `$7DC4 (round7_fixed_type51_move_state)` | State 2: moves while the elevator runs, wrapping at height $160 |
| `$7DFC (round7_fixed_type52_init_state)` | State 0: fixed at X $300, lane 1 |
| `$7E1E (round7_fixed_type52_idle_state)` | State 1: rts |
| `$7E2C (round7_fixed_type53_init_state)` | State 0: 8-update timer |
| `$7E42 (round7_fixed_type53_delay_delete_state)` | State 1: at the end sets $FFFA0D and deletes it |
| `$8E3E (object_type04_init_state)` | State 0: art and position by the story index $FFF90C |
| `$8E7A (object_type04_idle_state)` | State 1: rts |
| `$8E7C (object_type04_start_pose_state)` | State 2: pose 2 and a 90-update timer in $FFF91C |
| `$8E96 (object_type04_wait_pose_state)` | State 3: at the end advances $FFF910 and returns to pose 0 |
| `$8EAA (object_type04_final_idle_state)` | State 4: rts |
| `$BB2E (presentation_type8d_init_state)` | State 0: initialization |
| `$BB40 (presentation_type8d_scroll_state)` | State 1: counts +$54 down, scrolling rows with presentation_add_hscroll_rows |
| `$BB64 (presentation_type8e_static_sprite_init_state)` | State 0: initialization |
| `$BB92 (presentation_type8e_static_sprite_idle_state)` | State $1: rts |
| `$BBAA (presentation_type8c_init_state)` | State 0: initialization |
| `$BBE2 (presentation_type8c_fade_in_state)` | State 1: steps line 3 in |
| `$BC04 (presentation_type87_init_state)` | State 0: initialization |
| `$BC72 (presentation_type89_init_state)` | State 0: initialization |
| `$BCB8 (presentation_type8a_init_state)` | State 0: initialization |
| `$BCF0 (presentation_type8a_move_to_y78_state)` | State 1: moves until height $78 |
| `$BD2C (presentation_type8b_init_state)` | State 0: initialization |
| `$BD42 (presentation_type8b_timed_step1_state)` | State 1: at the timer loads the next palette list and the next four colours |
| `$BD5E (presentation_type8b_timed_step2_state)` | State 2: at the timer loads the next palette list |
| `$BD76 (presentation_type8b_timed_step3_state)` | State 3: at the timer loads the next four colours |
| `$BD8E (presentation_type8b_timed_step4_state)` | State 4: at the timer loads palette and colours |
| `$BDAA (presentation_type8b_timed_step5_state)` | State 5: at the timer loads the next palette list |
| `$BDBC (presentation_type8b_spawn_87_89_state)` | State 6: with +$5A set creates types $87 and $89 |
| `$BDF4 (presentation_type8b_timed_step7_state)` | State 7: at the timer loads the palette and starts $BE74 |
| `$BE10 (presentation_type8b_timed_step8_state)` | State 8: at the timer runs $BE74 |
| `$BE22 (presentation_type8b_timed_step9_state)` | State 9: at the timer runs $BE74 |
| `$BEEC (presentation_type85_cycle_state)` | State 1: every 4 updates runs $BF2A with d6 = 0 |
| `$BF16 (presentation_type86_cycle_state)` | State 1: every 4 updates runs $BF2A with d6 = 1 |
| `$BF8C (presentation_type84_init_state)` | State 0: initialization |
| `$BFAE (presentation_type84_write_priority_on_state)` | State 1: every 2 updates writes tiles with priority set |
| `$BFFA (presentation_type84_wait_a_state)` | State 2: counts down and goes to state 3 |
| `$C00C (presentation_type84_write_priority_off_state)` | State 3: every 2 updates writes tiles with priority clear |
| `$C05E (presentation_type84_wait_restart_state)` | State 4: counts down and goes back to state 1 |
| `$C076 (presentation_type84_wait_flag_state)` | State 5: waits on +$37 and goes to state 6 |
| `$C088 (presentation_type84_write_priority_final_state)` | State 6: every 2 updates writes tiles with priority set |
| `$C12C (presentation_type82_init_state)` | State 0: initialization |
| `$C160 (presentation_type82_fade_in_state)` | State 1: steps line +$58 in |
| `$C16E (presentation_type82_fade_out_state)` | State 2: steps line +$58 out |
| `$C192 (presentation_type83_init_state)` | State 0: initialization |
| `$C1BA (presentation_type83_timed_step1_state)` | State 1: at the timer spawns a type $82 |
| `$C1DA (presentation_type83_timed_step2_state)` | State 2: at the timer draws the next tilemap with palette 1 |
| `$C218 (presentation_type83_timed_step3_state)` | State 3: at the timer spawns two type $82 |
| `$C244 (presentation_type83_timed_step4_state)` | State 4: at the timer draws the next tilemap with palette 3 |
| `$C26C (presentation_type83_timed_step5_state)` | State 5: at the timer spawns a type $82 |
| `$C28C (presentation_type83_timed_step6_state)` | State 6: at the timer draws the next tilemap with palette 4 |
| `$C3A6 (presentation_type77_init_state)` | State 0: initialization |
| `$C3DE (presentation_type77_spawn_type76_state)` | State 1: at the timer creates a type $76 |
| `$C3FE (presentation_type77_draw_tilemaps_state)` | State 2: at the timer draws tilemaps 9 and $0A |
| `$C442 (presentation_type77_fade_in_state)` | State 3: steps line 0 in until the timer ends |
| `$C46E (presentation_type77_spawn_type79_state)` | State 4: at the timer creates a type $79 |
| `$C4A2 (presentation_type81_init_state)` | State 0: initialization |
| `$C4E6 (presentation_type81_draw_tilemaps_state)` | State 1: at the timer draws tilemaps $0B and $0C |
| `$C536 (presentation_type81_fade_in_state)` | State 2: steps line 0 in until the timer ends |
| `$C562 (presentation_type81_spawn_type79_state)` | State 3: at the timer creates a type $79 |
| `$C59A (presentation_type79_palette_cycle_init_state)` | State 0: initialization |
| `$C5AC (presentation_type79_palette_cycle_step_state)` | State 1: every 5 updates copies the next of four palettes |
| `$C604 (presentation_type79_palette_cycle_wait_state)` | State 2: waits 5 updates, then state 1 |
| `$C628 (presentation_type70_init_state)` | State 0: initialization |
| `$C642 (presentation_type70_draw_tilemap_state)` | State 1: draws tilemap 8 |
| `$C65A (presentation_type70_clear_blockmap_state)` | State 2: clears the start of $FF0000 and sets $FFFA0E |
| `$C690 (presentation_type70_animate_state)` | State 3: every 6 updates runs $C6B2 until the timer ends |
| `$C6EE (presentation_type6e_slide_in_sprite_init_state)` | State 0: initialization |
| `$C732 (presentation_type6e_slide_in_sprite_slide_state)` | State 1: accelerates until X $78 |
| `$C754 (presentation_type6e_slide_in_sprite_idle2_state)` | State $2: rts |
| `$C764 (presentation_type6f_slide_in_sprite_init_state)` | State 0: initialization |
| `$C7AC (presentation_type6f_slide_in_sprite_slide_state)` | State 1: accelerates until X $D0 |
| `$C7CE (presentation_type6f_slide_in_sprite_idle2_state)` | State $2: rts |
| `$C7DE (presentation_type75_slide_in_sprite_init_state)` | State 0: initialization |
| `$C854 (presentation_type76_slide_in_sprite_init_state)` | State 0: initialization |
| `$C8AA (presentation_type80_init_state)` | State 0: initialization |
| `$C8FE (presentation_type88_init_state)` | State 0: initialization |
| `$C922 (presentation_type88_idle_state)` | State $1: rts |
| `$C930 (presentation_type78_blinking_sprite_init_state)` | State 0: initialization |
| `$C956 (presentation_type78_blinking_sprite_blink_state)` | State 1: toggles the render flag every $14 updates |
| `$C986 (presentation_type7a_vertical_sprite_init_state)` | State 0: initialization |
| `$C9BE (presentation_type7a_vertical_sprite_move_state)` | State 1: moves until height $C8 |
| `$C9DC (presentation_type7b_horizontal_sprite_init_state)` | State 0: initialization |
| `$CA14 (presentation_type7b_horizontal_sprite_move_state)` | State 1: moves until X $80 |
| `$CA32 (presentation_type7c_horizontal_sprite_init_state)` | State 0: initialization |
| `$CA6A (presentation_type7c_horizontal_sprite_move_state)` | State 1: moves until X $C8 |
| `$CA8E (presentation_type6a_init_state)` | State 0: initialization |
| `$CAA2 (presentation_type6a_wait_state)` | State 1: counts down, then state 2 and $CBAA |
| `$CAB4 (presentation_type6a_sound_step_state)` | State 2: every 4 updates plays $A1 and steps |
| `$CB36 (presentation_type6a_wait_flag_state)` | State 3: waits on +$58 |
| `$CB5C (presentation_type6a_timed_tile_state)` | State 4: every 16 updates writes a tile until the timer ends |
| `$CBFE (presentation_type64_tile_strip_anim_init_state)` | State 0: initialization |
| `$CC1E (presentation_type64_tile_strip_anim_step_state)` | State 1: every 64 updates writes the next tile strip |
| `$CCAC (presentation_type64_tile_strip_anim_repeat_state)` | State 2: every 64 updates writes the strips from $3FB0E |
| `$CCFE (presentation_type65_camera_y_drift_init_state)` | State 0: initialization |
| `$CD10 (presentation_type65_camera_y_drift_state)` | State 1: every 4 updates adds 1 to camera_y |
| `$CD38 (presentation_type60_plane_fill_init_state)` | State 0: initialization |
| `$CD4A (presentation_type60_plane_fill_scroll_state)` | State 1: every 4 updates moves the scroll rows by -1 |
| `$CD82 (presentation_type69_init_state)` | State 0: initialization |
| `$CDCA (presentation_type69_scroll_a_state)` | State 1: moves rows until the timer ends |
| `$CE06 (presentation_type69_wait_flag_a_state)` | State 2: on +$58 plays $A1 and goes to state 3 |
| `$CE30 (presentation_type69_camera_down_state)` | State 3: moves the secondary camera by 8 and the linked object against it |
| `$CE88 (presentation_type69_scroll_b_state)` | State 4: moves rows until the timer ends |
| `$CEC2 (presentation_type69_wait_flag_b_state)` | State 5: on +$58 plays $A1 and goes to state 6 |
| `$CEEC (presentation_type69_camera_up_state)` | State 6: moves camera_y by -8 and the linked object against it |
| `$CF38 (presentation_type69_scroll_c_state)` | State 7: moves rows until the timer ends |
| `$CF72 (presentation_type69_wait_flag_c_state)` | State 8: on +$58 creates a type $6A and sets a colour |
| `$CF96 (presentation_type69_delay_next_state)` | State 9: counts +$56 down, then the state in +$59 |
| `$CFCE (presentation_type6b_init_state)` | State 0: initialization |
| `$D014 (presentation_type6b_slide_apart_state)` | State 1: moves its linked objects apart by 8 until the timer ends |
| `$D0B8 (presentation_type74_init_state)` | State 0: initialization |
| `$D0E6 (presentation_type74_slide_state)` | State 1: moves its linked objects by 10 until the timer ends |
| `$D128 (presentation_type61_palette_fade_init_state)` | State 0: initialization |
| `$D146 (presentation_type61_palette_fade_step_state)` | State 1: steps line 1 in |
| `$D168 (presentation_type6c_init_state)` | State 0: initialization |
| `$D19E (presentation_type6c_draw_tilemap_state)` | State 1: sets the plane size and draws tilemap 1 |
| `$D206 (presentation_type6c_fade_in_state)` | State 2: steps line 1 in |
| `$D22C (presentation_type62_init_state)` | State 0: initialization |
| `$D246 (presentation_type62_fade_out_state)` | State 1: steps line 1 out |
| `$D250 (presentation_type62_clear_palette_and_plane_state)` | State 2: clears line 0 and plane C000h and sets the plane size |
| `$D280 (presentation_type62_draw_tilemaps_state)` | State 3: draws tilemaps from 7 down |
| `$D2C2 (presentation_type63_palette_swap_init_state)` | State 0: initialization |
| `$D2D4 (presentation_type63_palette_swap_wait_state)` | State 1: counts down, then state 2 |
| `$D2E2 (presentation_type63_palette_swap_cycle_state)` | State 2: every 16 updates moves to the next of six colours |
| `$D33A (presentation_type66_init_state)` | State 0: initialization |
| `$D35C (presentation_type66_idle_state)` | State $1: rts |
| `$D35E (presentation_type66_delete_state)` | State $2: clears the object |
| `$D370 (presentation_type67_init_state)` | State 0: initialization |
| `$D396 (presentation_type67_idle_state)` | State $1: rts |
| `$D398 (presentation_type67_delete_state)` | State $2: clears the object |
| `$D3AA (presentation_type68_init_state)` | State 0: initialization |
| `$D3D0 (presentation_type68_idle_state)` | State $1: rts |
| `$D3D2 (presentation_type68_delete_state)` | State $2: clears the object |
| `$D3EE (presentation_type71_init_state)` | State 0: initialization |
| `$D40E (presentation_type71_fade_in_state)` | State 1: steps line 2 in until the timer ends |
| `$D438 (presentation_type71_fade_out_state)` | State 2: steps line 2 out |
| `$D45C (presentation_type72_init_state)` | State 0: initialization |
| `$D47C (presentation_type72_fade_in_state)` | State 1: steps line 3 in until the timer ends |
| `$D4A6 (presentation_type72_fade_out_state)` | State 2: steps line 3 out |
| `$D4CA (presentation_type73_init_state)` | State 0: initialization |
| `$D4EA (presentation_type73_fade_in_state)` | State 1: steps line 1 in until the timer ends |
| `$D53C (presentation_type73_fade_out_state)` | State 2: steps line 1 out |
| `$D556 (presentation_type7d_init_state)` | State 0: initialization |
| `$D57E (presentation_type7d_fade_in_state)` | State 1: steps line 0 in until the timer ends |
| `$D5B0 (presentation_type7d_fade_out_state)` | State 2: steps line 0 out until the timer ends |
| `$D5D4 (presentation_type7f_palette_fade_signal_init_state)` | State 0: initialization |
| `$D5F4 (presentation_type7f_palette_fade_signal_step_state)` | State 1: steps the palettes until the timer ends |
| `$D6AC (garcia_type20_personality_reset_state)` | State 1: reset, picks the walk $0D or the run $0C from +$40 |
| `$D6C0 (garcia_type20_entry_run_state)` | State $0C: the entry run at 5 px, every frame with attack box $01, braking past the target |
| `$D76A (garcia_type20_armed_walk_state)` | State $0D: armed walk to the approach point at 2.25 |
| `$D7BE (garcia_type20_face_target_state)` | State 8: 24 updates facing the target, then dodge, stab or throw |
| `$D856 (garcia_type20_stab_state)` | State 9: closes to 32 px and stabs with box $14 |
| `$D8EC (garcia_type20_throw_knife_state)` | State $0A: animation $28, releasing the knife on frame 1 |
| `$D9BC (garcia_type21_walk_to_point_state)` | State 1: walks to the approach point at 2 for 80 updates |
| `$DA10 (garcia_type21_wait_at_point_state)` | State $0C: waits there 40 updates |
| `$DA98 (garcia_type21_rush_hold_state)` | State 8: rushes the target at 4.5 while it faces him; the touch is his hold |
| `$DB18 (garcia_type21_hold_player_state)` | State 9: holds the player |
| `$DDA8 (garcia_type22_32_activate_state)` | State 0: activation, setting +$01 bit 0 for personality 4 |
| `$DDCA (garcia_type22_32_personality_reset_state)` | State 1: reset through the personality table $DDDA |
| `$DE78 (garcia_type22_32_lie_in_wait_state)` | State $12: lies in wait 128 updates or until the target is within 80 px |
| `$DEC6 (garcia_type22_32_rush_state)` | State $10: rushes to 32 px at 5 |
| `$DF0A (garcia_type22_32_triple_jab_state)` | State $11: jabs three times at reload 3 |
| `$FFE01E (camera_x_min)` | State $0E: stalks the approach point at 2.5 for 80 updates |
| `$E07C (garcia_type22_32_stand_state)` | State 8: stands; close by it dodges, box $12 on the target is the punch |
| `$E20A (garcia_type22_32_wander_state)` | State $0B: wanders towards a random offset of the approach point for one walk cycle |
| `$E2DC (garcia_type22_32_stand_close_state)` | State $0C: stands, as state 8 |
| `$E348 (garcia_type23_approach_state)` | State $0C: to 64 px from the target on its lane at 3.5, 48 updates |
| `$E3A0 (garcia_type23_spawn_weapon_walk_state)` | States 1 and $0A: spawns the bat or pipe and walks to the approach point at 2.5 |
| `$E404 (garcia_type23_stand_state)` | State 8: stands 16 updates |
| `$E47E (garcia_type23_swing_state)` | State 9: the swing, wind-up on frames 0-2 and box $18 on frames 3-5 |
| `$E4F6 (signal_type24_walk_to_point_state)` | State 1: walks to the approach point at 2 for 96 updates |
| `$E54E (signal_type24_dash_state)` | State $0A: dashes at the point, X until past it, then the lane |
| `$E5EC (signal_type24_stand_decide_state)` | State 8: stands 24 updates, then dodges, walks in or slides |
| `$E684 (signal_type24_walk_in_state)` | State 9: at the target spot at 3.5, 48 updates |
| `$E6F2 (signal_type24_hold_throw_state)` | State $0C: holds the player 48 px in front and throws on +$0D = 1 |
| `$E80A (signal_type24_slide_state)` | State $0B: the slide, box $0F on the feet and no body |
| `$E8B8 (signal_type24_stand_state)` | State $0D: stands, entered when the target is in action $5C |
| `$E920 (haku_ro_type25_activate_state)` | State 0: activation, setting +$01 bit 0 for personality 3 |
| `$E942 (haku_ro_type25_personality_reset_state)` | State 1: reset through the personality table $E94A |
| `$EA5C (haku_ro_type25_knockdown_state)` | State 3: his knockdown |
| `$EA8A (haku_ro_type25_hop_state)` | State $10: plays $B2 and hops at X 5, height -10 away from the attacker |
| `$EB2A (haku_ro_type25_walk_to_point_state)` | State $11: walks to the approach point at 2, facing |
| `$EBAC (haku_ro_jump_kick_state)` | State $0D of $25 and $2A: the strike animation in a hop |
| `$EC88 (haku_ro_type25_stalk_state)` | State 8: stalks the point, counting changes of the target X velocity |
| `$ED44 (haku_ro_type25_lane_step_state)` | State 9: steps 4 lanes per update onto the target lane |
| `$EDD8 (haku_ro_flying_kick_state)` | State $0B of $25 and $2A: the flying kick, 10 px per update with box $0C |
| `$EFB6 (haku_ro_type25_rush_turn_back_state)` | State $0F: rushes at 5 and turns back 64 px short |
| `$FFF040 (sound_music_fm0)` | State 0: activation, setting +$01 bit 0 for personality 1 |
| `$F286 (jack_juggle_walk_state)` | State $12: the juggle walk, going on to the wander state |
| `$F2E6 (jack_jump_state)` | State $0F: the jump |
| `$F3B6 (jack_look_about_state)` | State $10: looks about, with no contact test |
| `$F410 (jack_ranged_throw_state)` | State $0E: the ranged throw, three axes |
| `$F55E (jack_juggle_approach_state)` | State $0C: the juggle approach |
| `$F728 (jack_aligned_throw_state)` | State $0B: the aligned throw of both juggled axes |
| `$F7D8 (haku_ro_type2a_spawn_trio_state)` | State 0: spawns two more of the type and links the three |
| `$F876 (haku_ro_type2a_formation_state)` | State 1: keeps the formation around the screen centre |
| `$F9B6 (haku_ro_type2a_charge_wait_state)` | State 9: the 16-update wait after the charge |
| `$F9F0 (haku_ro_type2a_formation_handoff_b_state)` | State $10: formation, handing off as a group |
| `$FA92 (haku_ro_type2a_formation_handoff_a_state)` | State $0F: formation, handing off as a group |
| `$FFFB40 (police_special_blast_lane)` | State 8: the charge at 4 px with the lane snapped to the target |
| `$FFFC1C (round4_ripple_phase)` | State 5: the juggle, creating the axes |
| `$FC66 (jack_projectile_init_state)` | State 0: art and flags |
| `$FCB6 (jack_projectile_juggle_state)` | State 1: the juggled axe |
| `$FE46 (jack_projectile_falling_state)` | State 2: falls and lands |
| `$FED6 (jack_projectile_flight_state)` | State 3: flies on its velocities and falls |
| `$FEE4 (jack_projectile_catch_state)` | State 4: back with Jack, setting his +$31 bit 4 |
| `$12B94 (mr_x_office_controller_init_state)` | State 0: pose $28 and creates the type $34 scene object |
| `$12C0A (mr_x_office_controller_wait_signal_a_state)` | State 1: on $FFFA78 bit 0 queues next state 2 with a 60-update pose |
| `$12C26 (mr_x_office_controller_raise_signal_state)` | State 2: sets $FFFA78 bit 1 and starts a pose with next state 3 |
| `$12C3E (mr_x_office_controller_wait_signal_b_state)` | State 3: waits on $FFFA78 bit 0 |
| `$12CA4 (mr_x_office_controller_trigger_type38_state)` | State 4: moves the type $38 object to state 3 and queues state 5 |
| `$12CCA (mr_x_office_controller_wait_flow_state)` | State 5: waits on level_flow_flags bit 1, queuing state 6 |
| `$12D22 (mr_x_office_controller_start_delay_state)` | State 7: a 10-update delay, then state $15 |
| `$12D30 (mr_x_office_controller_queue_state9_state)` | State 8: starts a pose with next state 9 |
| `$12D42 (mr_x_office_controller_wait_signal_c_state)` | State 9: waits on $FFFA78 bit 0 |
| `$12D84 (mr_x_office_controller_wait_signal_d_state)` | State $17: on $FFFA78 bit 0 starts a pose with next state 4 |
| `$12DB4 (mr_x_office_controller_trigger_both_state)` | State $15: after +$56 triggers the scene objects |
| `$12DDA (mr_x_office_controller_wait_clear_signal_state)` | State $16: after +$54 clears $FFFA78 bit 0 and queues state 8 |
| `$12DF4 (mr_x_office_controller_trigger_scene_state)` | State $0A: after +$56 triggers the scene object +$58 |
| `$12E10 (mr_x_office_controller_raise_signal_late_state)` | State $0B: after +$54 sets $FFFA78 bit 1 |
| `$12E1E (mr_x_office_controller_pose_2a_state)` | State $0C: pose $2A |
| `$12E34 (mr_x_office_controller_pose_2a_wait_state)` | State $0D: after +$54 pose $28 and the queued state |
| `$12E52 (mr_x_office_controller_pose_28_state)` | State $0E: pose $28 |
| `$12E68 (mr_x_office_controller_pose_28_wait_state)` | State $0F: after +$54 frame 0 and the queued state |
| `$12E82 (mr_x_office_controller_pose_2a_signal_state)` | State $10: pose $2A |
| `$12E98 (mr_x_office_controller_pose_2a_signal_wait_state)` | State $11: on $FFFA78 bit 0 frame 0 and the queued state |
| `$12EB6 (mr_x_office_controller_pose_2c_state)` | State $12: pose $2C, 10 updates |
| `$12ED2 (mr_x_office_controller_pose_2c_step_state)` | State $13: steps the frame, then 20 updates |
| `$12EE8 (mr_x_office_controller_pose_2c_end_state)` | State $14: pose $28 and sets a level_flow_flags bit chosen by +$58 |
| `$12F28 (mr_x_office_scene_type34_init_state)` | State 0: pose $26 |
| `$12F42 (mr_x_office_scene_type34_wait_trigger_state)` | State 1: waits on +$58 bit 0 |
| `$12F5E (mr_x_office_scene_type34_delay_state)` | State 2: 13 updates hidden |
| `$12F78 (mr_x_office_scene_type34_shake_state)` | State 3: after 30 updates starts the screen shake |
| `$12F92 (mr_x_office_scene_type34_step_frame_state)` | State 4: steps the frame, 20 updates |
| `$12FA0 (mr_x_office_scene_type34_step_wait_state)` | State 5: at the end frame 0 and back to state 1 |
| `$12FC4 (mr_x_type38_init_state)` | State 0: art $6FB5A |
| `$12FD8 (mr_x_type38_wait_launch_state)` | State 1: on +$58 launches |
| `$1300C (mr_x_type38_fall_state)` | State 2: falls to the floor |
| `$13048 (mr_x_type38_spawn_pipe_state)` | State 3: creates a type $0B pipe at its place and clears itself |
| `$14204 (mr_x_bullet_bounce_state)` | State 2: mr_x_projectile_bounce_update until it ends |
| `$14272 (mr_x_shell_init_state)` | State 0: art and animation $24 |
| `$14308 (mr_x_shell_bounce_state)` | State 1: mr_x_projectile_bounce_update until it ends |
| `$14324 (abadede_variant_type39_init_state)` | State 0: creates a type $30 and links it |
| `$1434C (abadede_variant_type39_monitor_state)` | State 1: on +$59 = 3 runs kill_ordinary_enemies_and_release_players, then watches the boss in +$50 |
| `$145EC (abadede_approach_state)` | State 1: approach, substate 0 aims and 1 walks |
| `$14656 (abadede_pause_decide_state)` | State 2: the pause, then the decision |
| `$146F0 (abadede_retreat_state)` | State 3: retreat, substate 0 aims and 1 walks away |
| `$1477A (abadede_strike_shake_a_state)` | State 4: a strike landed, the shake with no contact test |
| `$147A6 (abadede_strike_shake_b_state)` | State 5: a strike landed, then primary 3 |
| `$147C0 (abadede_type0c_hit_state)` | State $0F: hit by a type $0C object |
| `$1485C (abadede_held_state)` | State $0B: held |
| `$14A8A (abadede_get_up_state)` | State $0A: getting up, 8 updates, then primary 1 |
| `$14B0E (abadede_knocked_down_state)` | State 6: knocked down: flight, floor, then $0A or death |
| `$14BDC (abadede_charge_state)` | State 7: the charge: set-up, run, brake, punch |
| `$14D04 (abadede_hold_throw_state)` | State 8: holds and throws a player |
| `$14EA2 (abadede_held_from_behind_state)` | State $0D: held from behind |
| `$14F46 (abadede_thrown_state)` | State $0E: thrown, 4 points on landing |
| `$1512E (abadede_suplexed_state)` | State 9: suplexed, 5 points |
| `$15266 (abadede_death_state)` | State $0C: the death |
