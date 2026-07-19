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
jsr     sync_z80_1
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
demo timeout
clock
pause/start handling
timer/HUD support
P2 drop-in handling
Mr. X offer state machine
stage-clear monitor
round-specific effects
animated palette updates
player/enemy/object update and sprite construction
miscellaneous round effects
art-transfer maintenance
level sub-state dispatcher
```

The final call is `$436 (sub_state_dispatcher)`. This ordering matters. An
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
| `$04` | `$784 (object_manager_loop)` | consume timed spawn records |
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
| `$110AC` | load round/HUD resources and construct HUD tile data |
| `$1119E` | build per-round animated-palette descriptors at `$FFEE00` |
| `$10E42` | load/remap HUD character art (skipped in demo mode) |
| `$11564` | load additional HUD/display assets |
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
| `$5F5B8` | per-round relative records | background/plane compressed resources and camera initialization data |
| `$195D8 (wave_camera_boundary_table)` | words | X boundary reached by successive waves; forward and reverse halves |
| `$11B44 (level_music_table)` | eight bytes | level BGM IDs |

This is a major source of apparent entropy in generated C++: “level loading” is
spread across code, and each table describes a different subsystem.

### 4.2 Enemy-load-cue compressed streams

`$E5C (start_round_setup)` indexes the word table at `$1B036 (level_elc_offset_table)`, adds the selected
relative offset to the table base, and calls `$81A4 (nemesisdec_ram)` with destination
`$FF6800 (elc_buffer)`. The eight compressed stream starts are:

| Round | Compressed source |
|---:|---:|
| 1 | `$1B046` |
| 2 | `$1B1D8` |
| 3 | `$1B3F4` |
| 4 | `$1B5D0` |
| 5 | `$1B7DE` |
| 6 | `$1BA8C` |
| 7 | `$1BD12` |
| 8 | `$1BEBE` |

The decompressed buffer contains entity records and control framing. It is not
background map data. This is proved by the destination `$FF6800 (elc_buffer)`, the immediate
assignment of `$FF6800 (elc_buffer)` to `$FFFC14 (display_list_head)`, and the record consumers at
`$784 (object_manager_loop)`, `$810 (prepare_next_spawn_section)`, `$C60 (spawn_single_object)`, and `$CC0 (spawn_object_batch)`.

### 4.3 Per-round wave descriptor roots

At `$1C1C4 (level_wave_resource_table)` are eight offsets relative to `$1C1C4 (level_wave_resource_table)`:

```text
0010 003E 006C 009A 00C8 0106 0144 0176
```

They select round sections at `$1C1D4`, `$1C202`, `$1C230`, `$1C25E`,
`$1C28C`, `$1C2CA`, `$1C308`, and `$1C33A`.

Each section starts with word offsets to wave descriptors. The first descriptor
is special: `$E5C (start_round_setup)` reads a longword spawn-stream pointer and a word
geometry selector before passing the remaining six bytes to `$F46`. Later wave
loads at `$5F4 (init_level_state)` use the offset indexed by `wave + 1` and pass it directly to
`$F46`; those entries are six-byte resource descriptors.

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

`sub_00000F46` proves that the descriptor is six bytes, but it does not expose a
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

The selected pointers are passed to `$10538` during round start and `$1053E`
during later phases. Those routines consume resource command lists. It is safer
to call these *resource descriptors* than “palette headers”: they coordinate
palette residency and enemy art uploads together.

### 4.5 Separate six-byte resource table at `$1C378 (level_resource_table)`

`$576 (load_level_data)` multiplies `$FFFF02 (level)` by six, reads a longword pointer, calls
`$1053E`, and leaves the final two bytes available to its caller. The records are:

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

- `$FFFC14 (display_list_head)` to `$FF6800 (elc_buffer)`;
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

After the initial batches, `$784 (object_manager_loop)` treats the current stream position as a timed
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
payload pointer, advances `$FFFC14 (display_list_head)` by that length, and filters compact
six-byte records in place until a byte `$99` terminator. This creates a current
section and a pointer to the following section without allocating another
buffer.

The `$936 -> $B76` states scan this filtered section and enforce limited
palette/art residency. If a required resource is not resident, the record is
deferred while `$1053E` and `$8454 (queue_nemesis_art_cues)` queue the needed art/palette. Once
`$FFDCD0 (art_array_cue)` becomes zero and a slot is free, the entity is spawned and the
remaining records are compacted over it.

This explains the cluster at `$FFFA60..$FFFA70`: it is a small resource-residency
allocator for active enemy families, not merely a palette array.

### 5.6 Tracked-entity count closes a phase

Enemy-like types in the range `$20..$2A` are classified through `$9350 (is_nonordinary_enemy_type)`. When a
spawned type is in the tracked subset, the loader:

- sets object flag bit 3;
- increments the word at `$FFFB1E (active_progression_entity_count)`.

Death/removal paths at `$92F0` and `$9D8C` decrement the same counter. Several
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
`scenario_x_position` for `$FFE01E (camera_x_min)` were imprecise. The clamp code at `$19074`
proves that they are the primary camera's maximum and minimum X bounds;
`addresses.csv` now records them as `$FFE01A (camera_x_max)` and `$FFE01E (camera_x_min)`.

### 6.2 Player-follow velocity

`$18F5C` derives a horizontal velocity from player world positions:

- in one-player mode, the active player is kept within a horizontal dead zone;
- in two-player mode, ordered player positions are used so the camera tries to
  contain both;
- the result is clamped to approximately `-3..+3` pixels per frame.

`$18FE8` performs the analogous depth/vertical calculation, clamped to
approximately `-4..+4`. `$19074` and `$190AA` integrate those velocities and
clamp the resulting 16.16 positions to the active min/max bounds.

The global player-boundary routine at `$43AA` separately prevents players from
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

The camera state machine at `$18C22` updates strip descriptors and emits dirty
rows/columns into command buffers at `$FFEA00/$FFEB20` and
`$FFEC40/$FFECE0`. `$19656` converts world/map coordinates into VDP tile-map
addresses. `$196A4` and the VBlank handler transfer those commands to VRAM.

`$190E0` derives horizontal-scroll values from camera X and per-strip ratios,
which is the parallax mechanism. The secondary plane has its own state table at
`$19332`; in some rounds it follows primary X at a fraction (round 1 uses direct
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
  at `$7678` and `$7E42`.

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
the player reaches the target, it waits on `$FFFA74`, sets `$FFFA71 (fade_out_flag)`, and
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
| `$FFDCD0 (art_array_cue)` | long/list | pending art array cue; zero means queued art is drained |
| `$FFE000 (primary_camera)` | structure | primary camera/plane state |
| `$FFE002 (cam_x)` | long root | primary camera X 16.16; integer word is named `$FFE002 (cam_x)` |
| `$FFE01A (camera_x_max)` | long | primary camera maximum X boundary |
| `$FFE01E (camera_x_min)` | long | primary camera minimum X boundary |
| `$FFE100 (secondary_camera)` | structure | secondary camera/plane state |
| `$FFEA00` | buffer | primary-plane row/column update commands |
| `$FFEB20` | buffer | secondary-plane row/column update commands |
| `$FFEC40` | buffer | primary-plane auxiliary strip updates |
| `$FFECE0` | buffer | secondary-plane auxiliary strip updates |
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
| `$FFFC14 (display_list_head)` | long | active ELC stream cursor |
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
| `$FFE01A/$FFE01E` are camera max/min X | clamp at `$19074`, phase update at `$19570 (advance_wave_camera_boundary)` | 100% |
| round 8 reverses camera-boundary direction | `$1957E..$195D0`, second half of `$195D8 (wave_camera_boundary_table)` | 100% |
| `$FFFF04 (wave)` is a camera/resource phase index | `$5F4 (init_level_state)`, `$E98`, `$18F32 (advance_round7_vertical_wave)`, `$19570 (advance_wave_camera_boundary)` | 100% |
| `$FFFB1E (active_progression_entity_count)` counts progression-gating entities | increments `$C60/$CC0`, decrements `$92F0/$9D8C`, zero tests in pipeline | 99% |
| all semantics of each six-byte resource descriptor | overlapping decoder `$F46` | 75% |
| exact semantic name of every `$FFFA05 (level_spawn_flow_flags)` bit | many distributed producers/consumers | 75% |
| repeated round-7 descriptor offsets encode vertical phases | ROM table plus round-7 camera exceptions | 70% |

---

## 11. Code-label confirmation audit

The open descriptor-field questions above are intentionally narrower than the
code labels. The following previously sub-100% entries are now confirmed for
their stated, bounded behavior:

| Address | Confirmed boundary | Direct evidence |
|---:|---|---|
| `$464 (level_flow_handler)` | Late-phase entity-drain, art, music, palette, and status gates in `$FFFA72 (level_flow_flags)`. | Every read/write is a bit operation on `$FFFA72 (level_flow_flags)` or a documented phase side effect; it is pipeline state `$16`. |
| `$576 (load_level_data)` | Select the six-byte per-level record at `$1C378 (level_resource_table)`, process its pointer, and leave its trailing parameters to the caller. | `level*6`, `(a3)+` longword, call `$1053E`, then return with `a3` advanced four bytes. |
| `$E5C (start_round_setup)` | Decode the selected ELC stream, choose the effective difficulty, seed the first wave descriptor, and create the round-specific fixed objects. | The level-indexed `$1B036 (level_elc_offset_table)` decode to `$FF6800 (elc_buffer)` and all eight level branches are explicit. |
| `$810 (prepare_next_spawn_section)` | Split the next length-prefixed ELC section and compact records that pass difficulty/player-count gates. | The word length advances `$FFFC14 (display_list_head)`; surviving six-byte records are copied in place until `$99`. |
| `$936 (select_deferred_spawn)` | Scan the filtered section and select a record whose ordinary-enemy palette/art residency can be satisfied. | Record/type checks feed the `$FFFA60..$FFFA70` residency counters and save the selected pointer at `$FFFC28 (pending_spawn_record_ptr)`. |
| `$B76 (load_deferred_spawn_art_and_spawn)` | Finish any required resource load, spawn the selected record, and compact the remainder over it. | It waits for `$FFDCD0 (art_array_cue)`, calls the resource loader/spawner, then shifts six-byte records through the `$99` terminator. |
| `$8454 (queue_nemesis_art_cues)` | Resolve an art-set list and append six-byte source/destination records to the incremental Nemesis queue. | The producer and `$84BA/$8510` consumer agree on the exact longword-source/word-VRAM record layout. |
| `$18F32 (advance_round7_vertical_wave)` | Consume the round-7 wave request and start its vertical-camera transition. | It clears the request, increments `$FFFF04 (wave)`, and writes the round-7 camera/transition flags and bounds. |
| `$19848 (load_level_graphics_maps_and_camera)` | Load the per-level mixed-codec graphics/map package and initialize both camera-plane structures. | Its fixed table walk performs two Kosinski uploads, two Enigma RAM decodes, map construction, then two calls to `$19922 (init_camera_plane)`. |

These confirmations do not assign names to every byte inside a resource
descriptor; they establish the entry points and observable contracts actually
recorded in `labels.csv`.

---

## 12. Remaining uncertainties and useful next experiments

1. **ELC framing after the timed section.** The length-prefixed and `$99`-ended
   behavior is clear in code, but a fully decoded dump of all eight decompressed
   streams would let every control word be named and every spawn assigned to a
   screen.
2. **Resource descriptor field names.** `$F46` combines nibbles, table families,
   and overlapping words. Runtime logging of calls to `$10538/$1053E` would
   distinguish palette lists, tile art, and mappings precisely.
3. **Round 7 repeated descriptor offsets.** A trace of `$FFFF04 (wave)`, pipeline state,
   camera state, and `$FFFA14..16` during the elevator/vertical sequence would
   prove why offset `$0E` and `$2C` are reused.
4. **Late-stage flags.** `$FFFA05 (level_spawn_flow_flags)` is clearly a level-flow bitset, but some bits
   combine “boss phase,” “resource drain,” and “special scene” behavior. Naming
   individual bits should follow producer/consumer traces rather than one call
   site.
5. **Background resource formats.** The camera structure and streaming algorithm
   are established, but the exact packed format below `$5F5B8/$5F788` deserves a
   separate graphics-engine manuscript.

The highest-value runtime trace would record once per change:

```text
frame, level, wave, $FA05, $FA0D, $FA72, $FB15, $FB1E,
camera_state, camera_x, min_x, max_x, display_list_head
```

That small trace would validate the static state graph without requiring a full
instruction log.
