# Data Compression and Decompression

## Scope and method

This document describes the compressed-data formats used by *Streets of Rage* and the 68000 routines that consume them. The primary evidence is the original assembly in `output/sor.asm`; the local ROM was used only to validate the reconstructed bitstreams, output sizes, pointer tables, and termination rules.

Three Sega-era formats are present:

| Format | Main entry points | Natural output | Principal uses |
|---|---:|---|---|
| Nemesis | `$8192 (nemesisdec_vram)`, `$81A4 (nemesisdec_ram)`; incremental path `$84BA (begin_incremental_nemesis_decode)`/`$8510 (continue_incremental_nemesis_decode)` | 4-bpp tile rows (or arbitrary nibble streams) | Tile art, enemy load cues, HUD/status graphics |
| Enigma | `$82D2 (enigmadec_with_plane_header)`, `$82D6 (enigmadec)` | 16-bit tilemap words | Plane maps, level maps, UI maps |
| Kosinski | `$85A2 (kosinskidec)` | Arbitrary bytes | Art, maps, level data, demo input, ending assets, Z80 driver |

The division is deliberate. Nemesis exploits repeated pixel nibbles, Enigma exploits repeated or sequential tile indices and shared attribute bits, and Kosinski is a general LZ back-reference codec.

All addresses below are ROM addresses unless explicitly prefixed with `FF`/`A0`/`C0` as RAM or hardware addresses.

## High-level loading architecture

The game has two visibly different loading paths:

1. **Blocking loads during screen or level setup.** Nemesis, Enigma, or Kosinski runs to completion, usually into `$FF8000 (decompression_scratch_buffer)` or directly into the VDP data port.
2. **Incremental Nemesis art loads during gameplay.** A queue of `(compressed source, VRAM destination)` records is consumed at no more than five tiles (160 uncompressed bytes) per VBlank.

Earlier notes treated `$FF8000 (decompression_scratch_buffer)` as Nemesis-specific. The current name reflects that it is a general scratch area used by all three formats. Kosinski commonly receives `a1=$FF8000`; Nemesis RAM mode and Enigma commonly receive their destination in `a4` and `a1`, respectively.

The codecs do not allocate memory, carry bounds, or validate malformed streams. Correct source, destination, and sufficient output space are contracts imposed by each call site.

## Nemesis

### Entry points and register contract

The common decoder begins at `$0081AE`. Two public entries select the output writer:

| Reference | Inputs | Destination |
| --- | --- | --- |
| `$8192 (nemesisdec_vram)` | `a0` = compressed stream; VDP command already selected | `a4=$C00000`, no address-register increment |
| `$81A4 (nemesisdec_ram)` | `a0` = compressed stream; `a4` = RAM destination | `(a4)+` |

An earlier analysis row used the erroneous address `$018192`. The CSV now
records `$8192 (nemesisdec_vram)`; there is no decompressor at `$018192`.

Both entries preserve `d0-a1/a3-a5` with `movem`. The decoder uses a 512-byte lookup table at `$FFF600 (nemesis_decode_table)` (`mem_ram+$F600`), built by `$8280 (nemesis_build_decode_table)`. The RAM entry does **not** take its destination in `a1`; callers consistently preload `a4`.

### Stream header

The first big-endian word is:

```text
bit 15       bits 14..0
+---------+----------------+
| XOR mode|  tile count    |
+---------+----------------+
```

The assembly proves the unit:

- `$81AE` reads the word.
- Two shifts detect and discard bit 15.
- A further multiplication by four converts `tile_count` to `tile_count * 8` 32-bit writes.
- Eight longs are the eight rows of one 8x8, 4-bpp tile, hence `tile_count * 32` output bytes.

If bit 15 is set, the entry advances the writer address by ten bytes, selecting `$00825E` (VDP) or `$008274` (RAM). These variants XOR every decoded long with the preceding decoded long held in `d2`, initially zero. This is vertical XOR/delta reconstruction: the stored row is a delta from the previous row, which often increases runs of zero nibbles.

### Code table

After the header comes a canonical prefix-code description. `$8280 (nemesis_build_decode_table)` expands it into the direct lookup table at `$FFF600 (nemesis_decode_table)`:

```text
group byte:
    $FF                  end of code table
    otherwise low nibble = output pixel nibble

descriptor byte (< $80):
    bits 6..4 = repeat_count_minus_1
    bits 3..0 = prefix length (1..8)
next byte:
    prefix code

descriptor byte (>= $80):
    begins the next group; its low nibble is the next pixel nibble
```

For prefixes shorter than eight bits, `$0082BA-$0082CE` fills all suffix variants in the 256-entry direct table. Each lookup-table word stores the number of compressed bits in its high byte and a decoded token in its low byte. The token is:

```text
bits 6..4 = output repetitions minus 1
bits 3..0 = pixel nibble
```

### Data bitstream and inline escape

The payload is read MSB-first. `$0081DC` peeks the next eight bits and normally resolves them through `$FFF600 (nemesis_decode_table)`. The decoder then emits one through eight copies of the token's low nibble into the 32-bit row accumulator `d4`.

Prefixes `$FC-$FF` are reserved. `$008228` consumes six one-bits and then reads a raw seven-bit token with the same `rrr0vvvv` meaning (three repeat bits and four value bits). This escape permits values that are not economical enough to appear in the stream's prefix table.

There is no end token. Decoding stops only after the exact number of 32-bit rows declared by the header has been written.

### Pseudocode

```text
function nemesis_decode(src, write_long):
    header = read_be16(src)
    xor_mode = header.bit15
    rows_remaining = (header & $7FFF) * 8
    decode_table = expand_prefix_table(src)
    bits = msb_bitstream(src)
    previous_row = 0

    row = 0
    nibbles = 0
    while rows_remaining != 0:
        if bits.peek(8) >= $FC:
            bits.consume(6)
            token = bits.read(7)
        else:
            (length, token) = decode_table[bits.peek(8)]
            bits.consume(length)

        repeat = (token >> 4) + 1
        value = token & $0F
        repeat times:
            row = (row << 4) | value
            nibbles += 1
            if nibbles == 8:
                if xor_mode:
                    row ^= previous_row
                    previous_row = row
                write_long(row)
                rows_remaining -= 1
                row = 0
                nibbles = 0
```

A repeat token may cross an eight-nibble row boundary. The decoder writes a
completed row and continues the remaining repetitions in the next row; the
offline decoder's crossing-token regression test exercises this exact case.

### Direct consumers

`$A63A (load_nemesis_art_bundle)` is a four-ID bundle loader. Each nonzero byte in `d0` selects an eight-byte record from the table at `$00A662`: a VDP command long and a Nemesis source pointer. It then calls `$8192 (nemesisdec_vram)`, so these art streams decode directly to VRAM.

`$E5C (start_round_setup)` selects one Nemesis stream per level through the relative-word table at `$1B036 (level_elc_offset_table)`, sets `a4=$FF6800`, and calls `$81A4 (nemesisdec_ram)`. The resulting data is the enemy load-cue data consumed during that round, demonstrating that Nemesis is a nibble-stream codec rather than a tile-only API.

ROM validation gives the following enemy-load-cue sizes:

| Level index | Source | Tiles in header | Output bytes |
|---:|---:|---:|---:|
| 0 | `$01B046` | 14 | 448 |
| 1 | `$01B1D8` | 20 | 640 |
| 2 | `$01B3F4` | 18 | 576 |
| 3 | `$01B5D0` | 18 | 576 |
| 4 | `$01B7DE` | 25 | 800 |
| 5 | `$01BA8C` | 23 | 736 |
| 6 | `$01BD12` | 15 | 480 |
| 7 | `$01BEBE` | 28 | 896 |

`$010E42` also uses the RAM entry:

- `$06F3D4`: XOR-mode, 26 tiles, 832 output bytes. A level-indexed 64-byte portion beginning at `$FF8060` is remapped and uploaded.
- `$01D0DA`: XOR-mode, 87 tiles, 2,784 output bytes. The code masks every long with `$11111111`, retaining one bit from each pixel nibble before several VDP uploads.

The exact visual identity of every `$010E42` region remains less certain than the codec behavior; its use during both level and round-clear setup indicates shared HUD/status-screen graphics.

### Incremental art queue

The gameplay path avoids decompressing large art streams in one frame:

1. `$8454 (queue_nemesis_art_cues)` receives an art-set index in `d0`, resolves a relative list through the table rooted at `$8672 (nemesis_art_cue_offset_table)`, and appends six-byte records to `$FFDCD0 (art_array_cue)`: a source long plus a VRAM-destination word.
2. `$84BA (begin_incremental_nemesis_decode)` starts the queue head when no stream is active. It reads the Nemesis header, selects normal/XOR output, builds `$FFF600 (nemesis_decode_table)`, primes the 16-bit payload buffer, and saves decoder state in `$FFDD10-$FFDD28`.
3. `$8510 (continue_incremental_nemesis_decode)`, called from the VBlank handler path at `$01A07C`, selects the queued VRAM address and resumes the decoder.
4. One call decodes at most five tiles. It sets `a5=8` for each tile, reuses the common nibble decoder, decrements the remaining tile count, and advances the stored VRAM address by `$00A0` after a full five-tile slice (`5 * 32` bytes).
5. On completion `$008592` shifts the following records toward the queue head.
   Its fixed 12-longword copy moves seven remaining six-byte slots plus the
   zero sentinel, proving a capacity of eight queued records.

The ordinary-enemy producer makes the table structure concrete. `$A4E (ordinary_enemy_art_family_table)` maps
types `$20-$2A` to five art families; each family owns three consecutive cue
IDs for the three possible resident VRAM destinations:

| Visual family | Internal types | Cue IDs | Nemesis source | Decoded tiles | Cue destinations |
|---|---|---:|---:|---:|---|
| Garcia | `$20-$23,$29` | `$0B-$0D` | `$20172 (garcia_nemesis_art)` | 295 | `$7100,$4B80,$2600` |
| Signal | `$24` | `$0E-$10` | `$21708 (signal_nemesis_art)` | 257 | `$7100,$4B80,$2600` |
| Haku-Ro | `$25,$2A` | `$11-$13` | `$22BFE (haku_ro_nemesis_art)` | 296 | `$7100,$4B80,$2600` |
| Nora | `$26` | `$14-$16` | `$245E0 (nora_nemesis_art)` | 261 | `$7100,$4B80,$2600` |
| Jack | `$27,$28` | `$17-$19` | `$258F8 (jack_nemesis_art)` | 298 | `$7100,$4B80,$2600` |

All three cue IDs in a row point to the same compressed bytes; only the VRAM
destination changes. The names come from visual inspection of PNG tile sheets
produced by `tools/decompress.py`; the cue IDs, source pointers, output sizes,
type grouping, and destinations come directly from the ROM tables and Nemesis
headers.

`$1087A (game_mode_ingame)` calls `$84BA (begin_incremental_nemesis_decode)` once per update to start pending work; VBlank calls `$8510 (continue_incremental_nemesis_decode)` to make bounded progress. This separation is the important scheduling property: table construction and stream setup occur in game time, while VDP writes occur in the safe display interval.

The producer scans for the first zero source longword but performs no bounds
check. The ROM cue lists and their call pattern must therefore keep the queue
at or below eight records; appending a ninth would overwrite the fixed sentinel
area and break the completion shift.

The saved state has the following observed meanings:

| RAM | Width | Meaning |
|---:|---:|---|
| `$FFDCD0 (art_array_cue)` | long | Queue-head source pointer; while active, the current compressed-stream cursor |
| `$FFDCD4 (nemesis_art_vram_destination)` | word | Current VRAM byte destination |
| `$FFDD10 (nemesis_incremental_writer)` | long | Selected normal/XOR writer address |
| `$FFDD14` | long | Saved `d0` decode scratch/state |
| `$FFDD18` | long | Saved `d1` decode scratch/state |
| `$FFDD1C (nemesis_incremental_xor_row)` | long | XOR previous-row accumulator (`d2`) |
| `$FFDD20 (nemesis_incremental_bit_buffer)` | long | Payload bit buffer (`d5`, low word significant) |
| `$FFDD24 (nemesis_incremental_bits_remaining)` | long | Number of valid bits (`d6`, low word significant) |
| `$FFDD28 (nemesis_incremental_tiles_remaining)` | word | Tiles remaining |
| `$FFDD2A (nemesis_incremental_tile_budget)` | word | Per-call tile budget, reloaded to 5 |

## Enigma

### Identification and entry points

The unlabeled routine at `$82D6 (enigmadec)` is the standard Enigma tilemap decompressor. Its operation families, inline tile attributes, variable-width tile index, and `$7F`-family terminator distinguish it from the adjacent Nemesis code.

There are two entries:

| Reference | Behavior |
| --- | --- |
| `$82D2 (enigmadec_with_plane_header)` | Copies two longs (eight bytes) from `(a0)+` to `(a1)+`, then falls through |
| `$82D6 (enigmadec)` | Decodes the Enigma header/payload from `a0` to 16-bit words at `a1`; `d0` is the base tile word |

The eight copied bytes are not part of Enigma itself. Callers using `$82D2 (enigmadec_with_plane_header)`, notably `$A82A (load_enigma_map_bundle)`, preserve a plane-layout header before decoding the following tile words. Direct callers use `$82D6 (enigmadec)` and begin immediately at the Enigma header.

### Header

```text
byte 0: number of bits in an inline tile index
byte 1: which tile-attribute bits may be supplied inline
word 2: initial incrementing tile word, plus caller's d0 base
word 4: common tile word, plus caller's d0 base
then:   MSB-first payload
```

The second byte is shifted left three at `$0082E2`. `$008394-$0083DE` tests its five significant flags and conditionally consumes payload bits for tile-word bits `$8000`, `$4000`, `$2000`, `$1000`, and `$0800` (priority, palette selection, vertical flip, horizontal flip in VDP tile attributes). It then consumes the declared number of index bits and adds the caller's base tile word.

### Commands

Each command includes a four-bit count; the actual run length is `count + 1`.

| Prefix | Handler | Meaning |
|---|---:|---|
| `00` | `$008320` | Emit the incrementing header word, advancing it after each output |
| `01` | `$00832A` | Repeat the common header word |
| `100` | `$008332` | Decode one inline word and repeat it |
| `101` | `$00833E` | Decode one inline word, then increment it per output |
| `110` | `$00834C` | Decode one inline word, then decrement it per output |
| `111` | `$00835A` | Decode an inline word for every output |
| `111` + count `$F` | `$00837C` | End stream and align `a0` to an even address |

The command dispatch table is the eight-word branch table at `$00836C`. The first two entries both target `$008320`, and the next two target `$00832A`, because the short command form is six bits while the inline forms are seven bits.

### Pseudocode

```text
function enigma_decode(src, dst, base_tile):
    index_bits = read_u8(src)
    attribute_mask = read_u8(src)
    incrementing = read_be16(src) + base_tile
    common = read_be16(src) + base_tile
    bits = msb_bitstream(src)

    loop:
        if bits.read(1) == 0:
            opcode = bits.read(1)          // 0 or 1
        else:
            opcode = 4 + bits.read(2)      // 4 through 7
        count = bits.read(4)

        if opcode == 7 and count == 15:
            return align_even(bits.source_cursor)

        run = count + 1
        execute incrementing/common/repeat/increment/decrement/list
```

### Consumers and validated streams

`$A82A (load_enigma_map_bundle)` is the Enigma analogue of the Nemesis bundle loader. A nonzero ID selects a ten-byte record at `$00A85A`: destination pointer, source pointer, and base tile word. It calls `$82D2 (enigmadec_with_plane_header)`, retaining the leading eight-byte plane header.

Representative ROM-validated streams are:

| Source | Entry | Base | Compressed bytes consumed | Decoded words | Context |
|---:|---:|---:|---:|---:|---|
| `$07228E` | `$82D2 (enigmadec_with_plane_header)` | `$0001` | 20 | 52 | Headered UI/plane map |
| `$06F51C` | `$82D2 (enigmadec_with_plane_header)` | `$06BF` | 54 | 160 | Headered UI/plane map |
| `$01F596` | `$82D2 (enigmadec_with_plane_header)` | `$2001` | 64 | 154 | Headered ending map |
| `$065976` | `$82D6 (enigmadec)` | `$0000` | 814 | 768 | Level-specific animated/map data |
| `$012842` | `$82D6 (enigmadec)` | `$0000` | 70 | 60 | Small RAM table |

The compressed-byte count includes the six-byte Enigma header and, for `$82D2 (enigmadec_with_plane_header)`, the preceding eight-byte plane header; it is rounded to the even source cursor returned by the assembly.

During `$19848 (load_level_graphics_maps_and_camera)` level setup, two Enigma streams selected from the per-level table at `$05F5B8` decode to `$FF4000` and `$FF4800`. These are later referenced by the level-plane structures at `$FFE000 (primary_camera)` and `$FFE100 (secondary_camera)`, making Enigma central to level tilemap construction rather than merely a menu codec.

## Kosinski

### Entry and descriptor ordering

`$85A2 (kosinskidec)` takes:

- `a0`: compressed source;
- `a1`: byte destination.

It returns with both advanced to the end of the consumed stream and output. Unlike the Nemesis entries, it does not save working registers.

Each 16-bit descriptor is stored little-endian in the ROM and consumed least-significant bit first. `$85A2-$85AC` makes this explicit: the first source byte is written to `1(sp)`, the second to `(sp)`, and the resulting big-endian 68000 word therefore has the first byte as its low byte. A descriptor reload happens immediately after its sixteenth bit is consumed, before any following token data is read.

### Tokens

The first descriptor bit selects a literal or a match:

```text
1: literal byte follows
0: match; read a second descriptor bit
```

Short match (`00`):

- two more descriptor bits encode a length of 2 through 5;
- one following byte is sign-extended to a displacement of -256 through -1.

Long match (`01`):

- two following bytes encode a signed 13-bit backward displacement;
- the low three bits of the second byte normally encode a length of 3 through 9;
- if those three bits are zero, a third extension byte follows.

The long displacement reconstructed at `$860C-$861E` is equivalent to:

```text
offset = sign_extend_16($E000 | ((second_byte & $F8) << 5) | first_byte)
```

The extension byte has three special cases:

| Extension | Meaning |
|---:|---|
| `$00` | End stream and return |
| `$01` | No-op token; resume descriptor decoding without output |
| `$02-$FF` | Copy `extension + 1` bytes |

All matches copy forward one byte at a time from `a1 + negative_offset`. Overlap is intentional and produces repeated runs. There is no independent uncompressed-size header.

### Pseudocode

```text
function kosinski_decode(src, dst):
    descriptor = read_le16(src)
    descriptor_bits = 16

    loop:
        if next_lsb_bit():
            dst.write(read_u8(src))
            continue

        if next_lsb_bit() == 0:
            length = (next_lsb_bit() << 1 | next_lsb_bit()) + 2
            offset = sign_extend_8(read_u8(src))
        else:
            lo = read_u8(src)
            hi = read_u8(src)
            offset = sign_extend_16($E000 | (hi << 5) | lo)
            length_code = hi & 7
            if length_code != 0:
                length = length_code + 2
            else:
                extension = read_u8(src)
                if extension == 0: return
                if extension == 1: continue
                length = extension + 1

        repeat length times:
            dst.write(dst.current[offset])
```

### Representative consumers

| Call site | Source | Destination | Validated compressed -> output | Interpretation |
|---:|---:|---:|---:|---|
| `$0016E4` | `$071C6C` | `$FF8000 (decompression_scratch_buffer)` | 656 -> 2,248 bytes | Character-select UI/map data |
| `$00880E` | `$0389A0` | `$FF7000` | 514 -> 1,568 bytes | Ending/demo shared art |
| `$008854` | `$01F596` | `$FF8000 (decompression_scratch_buffer)` | 374 -> 2,248 bytes | Bad-ending map data |
| `$010648` | `$795A2 (z80_dac_driver_kosinski)` | `$FF7000` | 7,581 -> 7,936 bytes | Z80 DAC/PCM driver image |
| `$010864` | `$01CAEC` | `$FF7000` | 611 -> 8,192 bytes | Attract/demo input data |

`$1061C (load_z80_dac_driver)` deserves a size caveat. The Kosinski stream expands to exactly `$1F00` (7,936) bytes, but `$010656` copies only `dbf d2` with `d2=$1EC6`, i.e. `$1EC7` (7,879) bytes, into Z80 RAM `$0000`. The final 57 decompressed bytes are not copied. The loader separately writes sample-bank bytes to Z80 `$1FF8-$1FFB`, leaving the intervening mailbox/workspace region under explicit runtime control.

`$B748 (load_kosinski_story_asset)` selects fourteen Kosinski ending/story assets through eight-byte `(source,destination)` records at `$00B768`. Validated output sizes range from 512 bytes (`$039B80`) to 11,616 bytes (`$037360`), and destinations range across work RAM (`$FF0000`, `$FF0620`, `$FF0BE0`, and others). The caller can then upload art, retain maps, or build scene data without changing the codec.

### Level setup at `$19848 (load_level_graphics_maps_and_camera)`

The per-level table at `$05F5B8` mixes codecs in a fixed sequence:

```text
two records:  VDP command long + Kosinski source long
two records:  Enigma source long (destinations are $FF4000 and $FF4800)
one record:   Kosinski source long (destination $FFA000)
then:         palette and plane configuration
```

The first two Kosinski streams are decoded through `$FF8000 (decompression_scratch_buffer)` and copied to the VDP. The Enigma streams form the two main level-map datasets. The final Kosinski stream populates `$FFA000` and is subsequently used by the scrolling/plane machinery.

ROM-validated source sizes show why more than one codec is useful:

| Level | Kosinski art 1 | Kosinski art 2 | Enigma map 1 | Enigma map 2 | Kosinski `$FFA000` |
|---:|---:|---:|---:|---:|---:|
| 0 | 2,791 -> 5,056 | 5,716 -> 12,224 | 90 B -> 228 W | 1,134 B -> 920 W | 70 -> 5,040 |
| 1 | 3,401 -> 5,952 | 548 -> 1,120 | 558 B -> 528 W | 132 B -> 132 W | 70 -> 5,040 |
| 2 | 1,593 -> 3,424 | 2,850 -> 4,896 | 364 B -> 304 W | 588 B -> 480 W | 70 -> 5,040 |
| 3 | 2,336 -> 4,896 | 1,154 -> 2,176 | 542 B -> 484 W | 182 B -> 192 W | 191 -> 5,040 |
| 4 | 1,626 -> 4,000 | 1,909 -> 3,200 | 556 B -> 488 W | 210 B -> 220 W | 70 -> 5,040 |
| 5 | 1,952 -> 5,056 | 1,099 -> 2,336 | 712 B -> 544 W | 428 B -> 316 W | 341 -> 5,040 |
| 6 | 1,068 -> 2,496 | 1,767 -> 4,896 | 254 B -> 228 W | 958 B -> 796 W | 153 -> 1,680 |
| 7 | 3,469 -> 6,848 | 289 -> 704 | 974 B -> 856 W | 124 B -> 124 W | 70 -> 5,040 |

For Kosinski columns both sides are bytes. For Enigma columns the left side is compressed bytes consumed and the right side is decoded 16-bit words.

The fixed upload at `$0198AA` is deliberately independent of the Kosinski
terminator. `d7=$0096` gives 151 DBF iterations and
`$10496 (vdp_copy_long_38)` writes 38 longwords per iteration, so each pass
uploads exactly 22,952 bytes (`$59A8`) from `$FF8000
(decompression_scratch_buffer)`. The decoded prefixes in the table above range
from 704 to 12,224 bytes, and this routine does not clear the scratch suffix.
Bytes after the terminator are therefore retained scratch contents, not codec
padding. Only the decoded prefix is semantically valid; the oversized transfer
is a fixed VRAM-region refresh whose unused tail must not be interpreted as
part of the compressed asset. The second art command begins at a per-level VRAM
address chosen to follow the first asset's useful tile range.

`$0199C6` performs a further level-dependent Kosinski decode into `$FF8000 (decompression_scratch_buffer)`, then copies selected 16-byte chunks into the large plane buffers at `$FF0000`/`$FF2000`. This is the bridge from compressed block definitions to the runtime level layout assembled for scrolling.

## Format comparison and invariants

| Property | Nemesis | Enigma | Kosinski |
|---|---|---|---|
| Bit order | MSB-first | MSB-first | LSB-first descriptors |
| Output granularity | Nibbles grouped into 32-bit tile rows | 16-bit tile words | Bytes |
| Termination | Header tile count | Explicit `111/$F` command | Extended long match `$00` |
| Back-references | No | No | Yes, overlapping permitted |
| Stream-local table | Prefix decode table | Two seed words + attribute mask | No |
| Direct-to-VRAM path | Yes | No in these entry points | No |
| Incremental decoder | Yes | No | No |

Practical invariants for a native reimplementation or data extractor are:

- Nemesis output is exactly `(header & $7FFF) * 32` bytes.
- Nemesis bit 15 changes reconstruction, not the output length.
- Enigma output length must be discovered by decoding to its terminator.
- Kosinski output length must be discovered by decoding to its terminator.
- Kosinski descriptor refill is interleaved with token bytes: after consuming descriptor bit 16, the next descriptor word is fetched immediately, before the token's literal or match bytes.
- Enigma returns with the source aligned to an even address.
- None of the original routines protects the destination from overflow.

## Confidence and open questions

All code labels listed in the analysis-data ledger below are now confirmed at
100% for their stated entry-point contracts. In particular, the producer at
`$8454 (queue_nemesis_art_cues)` and the resumable consumers at `$84BA/$8510`
agree on the six-byte queue format, while `$19848 (load_level_graphics_maps_and_camera)`
has a complete per-level table walk through both codecs and both calls to the
camera-plane initializer. The broader content labels in the medium-confidence
section remain separate visual/asset-identification questions.

### High confidence (95-100%)

- Identification of Nemesis, Enigma, and Kosinski.
- Their entry addresses and register contracts.
- All token formats and termination rules described above.
- Nemesis XOR mode, header/output-size relationship, and lookup-table construction.
- Enigma command families, attribute-bit reconstruction, and even alignment.
- Kosinski little-endian/LSB-first descriptor ordering and match lengths.
- The incremental Nemesis five-tile VBlank budget.
- The fixed eight-record incremental art queue and its unchecked producer contract.
- Nemesis repeat tokens can continue across a 32-bit tile-row boundary.
- ROM-derived compressed and output sizes in the tables.
- The `$0198AA` level-art upload is always `$59A8` bytes and may copy a stale
  scratch-buffer suffix after the decoded prefix.
- Correction of `$8192 (nemesisdec_vram)` from `$018192` to `$8192 (nemesisdec_vram)`.

### Medium confidence (75-90%)

- Broad content classifications such as UI map, level map, and ending art, inferred from destinations and immediate consumers.
- `$01CAEC` as attract/demo input. It expands to 8 KiB in the demo-only branch and matches the separate demo input cursor mechanism, but the pointer initialization chain deserves its own focused trace.
- `$010E42` as shared HUD/status graphics; the transformations are clear, while exact named screen regions remain to be mapped.

### Open questions

1. Which exact VRAM tile ranges correspond to every ID in the Nemesis table at `$00A662`?
2. Which data fields in `$FFDD14` and `$FFDD18` merit semantic names beyond saved decoder scratch state?

## Analysis-data update ledger

These findings were integrated into the shared CSV files. Where another manuscript
covered the same address, the final CSV uses a consolidated name and description.

### `labels.csv`

Correction:

```csv
00008192, nemesisdec_vram, "100% - Nemesis decompressor targeting the VDP data port; a0=stream, VDP command preselected; header bit15 enables XOR-row reconstruction"
```

The obsolete `00018192` row was replaced rather than duplicated.

New entries:

```csv
00008280, nemesis_build_decode_table, "100% - Expands the Nemesis stream prefix-code header into the 256-entry direct lookup table at $FFF600"
000082D2, enigmadec_with_plane_header, "100% - Copies an 8-byte plane header from (a0)+ to (a1)+, then falls through to the Enigma tilemap decoder"
000082D6, enigmadec, "100% - Enigma tilemap decompressor; a0=stream, a1=word destination, d0=base tile; returns source even-aligned"
00008454, queue_nemesis_art_cues, "100% - Resolves an art-set index through the table at $8672 and appends 6-byte Nemesis source/VRAM-destination records at $FFDCD0"
000084BA, begin_incremental_nemesis_decode, "100% - Initializes the queued Nemesis stream, lookup table, XOR writer and resumable state for VBlank art upload"
00008510, continue_incremental_nemesis_decode, "100% - Resumes queued Nemesis-to-VRAM decoding for at most five tiles (160 bytes) per VBlank and advances/completes the queue"
0000A63A, load_nemesis_art_bundle, "100% - Processes four packed art IDs; selects VDP command/source records from $A662 and Nemesis-decompresses each directly to VRAM"
0000A82A, load_enigma_map_bundle, "100% - Processes four packed map IDs; selects destination/source/base records at $A85A and decodes headered Enigma maps"
0000B748, load_kosinski_story_asset, "100% - Selects a Kosinski source and RAM destination from the 8-byte record table at $B768 and decompresses it"
00019848, load_level_graphics_maps_and_camera, "100% - Loads the per-level mixed-codec graphics/maps package, builds plane data, and initializes both camera structures"
```

Suggested corrections to existing comments:

```csv
000081A4, nemesisdec_ram, "100% - Nemesis decompressor targeting (a4)+ RAM; a0=stream; header bit15 enables XOR-row reconstruction; builds lookup table at $FFF600"
000085A2, kosinskidec, "100% - Kosinski byte decompressor; little-endian LSB-first descriptor words, literal/short/long backrefs, extended long token 0=end and 1=no-op"
```

### `addresses.csv`

Suggested corrections:

```csv
FF8000, decompression_scratch_buffer, "100% - General decompression scratch buffer used by Nemesis, Enigma and Kosinski (not Nemesis-only)"
FFDCD0, art_array_cue, "100% - L - Head of 6-byte incremental Nemesis art queue records (source L + VRAM destination W); source field becomes current stream cursor while active"
```

New entries:

```csv
FFDCD4, nemesis_art_vram_destination, "100% - W - Current queued incremental Nemesis VRAM byte destination"
FFDD10, nemesis_incremental_writer, "100% - L - Saved normal/XOR Nemesis output-writer address for incremental VBlank decoding"
FFDD1C, nemesis_incremental_xor_row, "100% - L - Saved previous decoded row used by incremental Nemesis XOR reconstruction"
FFDD20, nemesis_incremental_bit_buffer, "100% - L - Saved Nemesis payload bit buffer; low word is significant"
FFDD24, nemesis_incremental_bits_remaining, "100% - L - Saved count of valid Nemesis payload bits; low word is significant"
FFDD28, nemesis_incremental_tiles_remaining, "100% - W - Number of tiles left in the active queued Nemesis stream"
FFDD2A, nemesis_incremental_tile_budget, "100% - W - Per-VBlank tile budget for incremental Nemesis decode; reloaded to 5"
FFF600, nemesis_decode_table, "100% - 512-byte, 256-entry direct prefix lookup table built from each Nemesis stream header"
```
