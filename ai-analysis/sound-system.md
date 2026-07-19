# Streets of Rage Sound System

**Manuscript:** reverse-engineering notes from the recompilation project  
**Scope:** music, sound effects (SFX), and PCM/DAC playback in *Streets of Rage* (Mega Drive)  
**Primary sources:** `code-analysis/addresses.csv`, `code-analysis/labels.csv`, `output/sor.asm`, ROM `rom/SOR.bin`, and `MegaDriveEnvironment` host emulation  

Symbol names below match the analysis CSVs (100% confidence entries unless noted). This document describes how the game produces audio: who owns the sequencer, how the 68000 and Z80 cooperate, how game code requests sounds, and how channel data is stored and advanced each frame.

---

## 1. Executive summary

*Streets of Rage* does **not** run a classic Z80-only SMPS-style music driver for the whole mix. The division of labour is:

| CPU | Role |
|-----|------|
| **68000** | Full music + SFX sequencer; writes **YM2612 (FM)** and **SN76489 (PSG)** registers directly |
| **Z80** | **DAC / PCM only** — streams 8-bit samples through YM2612 channel 6 (DAC path) |

Game logic never programs the chips itself for ordinary events. It posts **sound IDs** into a small work-RAM queue (`play_se`…). Once per **VBlank**, `sound_engine` drains that queue, updates channel state machines, and talks to hardware (and to the Z80 mailbox for PCM).

```
  Gameplay / UI / level flow
           │
           │  write sound ID → play_se / play_se_1 / play_se_2  ($FFF00A–$C)
           │  optional: stop_music / command $E0
           ▼
  ┌────────────────────────────────────────┐
  │  sound_engine  ($72914)                │
  │  (from vblank_handler $19D16)          │
  │                                        │
  │  • sound_process_queue                 │
  │  • sound_fade_music                    │
  │  • FM / PSG / DAC channel ticks        │
  │  • YM2612 + PSG port writes            │
  └──────────────┬─────────────────────────┘
                 │
     FM / PSG ───┼── z80_busreq + ym2612_* / sn76489_psg
                 │
     PCM ────────┼── z80_dac_command ($A01FFF)
                 ▼
  ┌────────────────────────────────────────┐
  │  Z80 DAC driver                        │
  │  (z80_dac_driver_kosinski @ $795A2)    │
  │  • bank via z80_bank_register          │
  │  • YM2612 DAC enable $2B / sample data │
  └────────────────────────────────────────┘
```

---

## 2. Hardware map (Mega Drive)

From `code-analysis/addresses.csv` (I/O used by the sound path):

| Address | Symbol | Notes |
|---------|--------|--------|
| `$A04000` | `ym2612_a0` | YM2612 part 0 address |
| `$A04001` | `ym2612_d0` | YM2612 part 0 data |
| `$A04002` | `ym2612_a1` | YM2612 part 1 address |
| `$A04003` | `ym2612_d1` | YM2612 part 1 data |
| `$A06000` | `z80_bank_register` | 9-bit bank into 68000 address space |
| `$A07F11` | `sn76489_psg_z80` | PSG via Z80 window (rarely used by this path) |
| `$A11100` | `z80_busreq` | `$0100` = request bus |
| `$A11200` | `z80_reset` | `$0000` held in reset; `$0100` run |
| `$A00000`–`$A01FFF` | (Z80 RAM window) | Driver image + mailboxes |
| `$C00011` (+ mirrors) | `sn76489_psg` | SN76489 PSG tone / noise / volume |

### 2.1 Z80 DAC mailboxes (68000 view of Z80 RAM `$1Fxx`)

| Address | Symbol | Notes |
|---------|--------|--------|
| `$A01FF6` | `z80_dac_status` | Polled by `sound_update_dac_channel` before posting a sample |
| `$A01FF8` | `z80_sample_bank_lo` | First of four bank/pointer bytes seeded `00 80 07 80` at load (`$1FF8`–`$1FFB`) |
| `$A01FFD` | `z80_dac_busy` | Bit 7 set while Z80 owns YM2612 DAC; `sound_ym2612_acquire` waits for clear |
| `$A01FFF` | `z80_dac_command` | Command mailbox; 68000 writes sample/command (`≥$81` plays) |

### 2.2 68000 sound workspace (work RAM)

| Address | Symbol | Notes |
|---------|--------|--------|
| `$FFF000` | `sound_active_priority` | Highest priority accepted this frame (from `sound_priority_table`) |
| `$FFF002` | `stop_music` | BGM fade counter; `$E0` sets `$10`; `0` when idle |
| `$FFF003` | `stop_music_timer` | Sub-frame timer for fade (reload 4) |
| `$FFF006` | `sound_update_phase` | `0` = music section, `$80` = SFX section, `1` = after SFX |
| `$FFF007` | `sound_engine_gate` | Non-zero → pause/special path in `sound_engine` (bit 7 sticky) |
| `$FFF008` | `sound_dac_busy_mirror` | Flag around DAC/sample channel updates |
| `$FFF009` | `sound_pending_id` | Winning ID from the queue this frame (bit 7 while pending / idle `$80`) |
| `$FFF00A` | `play_se` | Queue slot 0 — write ID with bit 7 set |
| `$FFF00B` | `play_se_1` | Queue slot 1 |
| `$FFF00C` | `play_se_2` | Queue slot 2 |
| `$FFF014` | `sound_music_voice_bank` | Absolute pointer to current music FM voice bank |
| `$FFF018` | `sound_sfx_header_ptr` | Scratch pointer when starting an SFX header |

### 2.3 Channel blocks (`$30` bytes each)

Bit 7 of `+0` = active; `+4` = sequence pointer; `+$E` = duration (see labels for engine walk order).

| Address | Symbol | Kind |
|---------|--------|------|
| `$FFF040` | `sound_music_fm0` | Music FM 0 |
| `$FFF070` | `sound_music_fm1` | Music FM 1 |
| `$FFF0A0` | `sound_music_fm2` | Music FM 2 |
| `$FFF0D0` | `sound_music_fm3` | Music FM 3 |
| `$FFF100` | `sound_music_fm4` | Music FM 4 |
| `$FFF130` | `sound_music_dac` | Music DAC / sample lane → `z80_dac_command` |
| `$FFF160` | `sound_music_psg0` | Music PSG 0 |
| `$FFF190` | `sound_music_psg1` | Music PSG 1 |
| `$FFF1C0` | `sound_music_psg2` | Music PSG 2 |
| `$FFF1F0` | `sound_sfx_fm0` | SFX FM 0 (may steal music FM) |
| `$FFF220` | `sound_sfx_fm1` | SFX FM 1 |
| `$FFF250` | `sound_sfx_fm2` | SFX FM / extra routing |
| `$FFF2B0` | `sound_sfx_psg0` | SFX PSG 0 |
| `$FFF2E0` | `sound_sfx_psg1` | SFX PSG 1 |
| `$FFF340` | `sound_sfx_fm_extra` | Extra FM SFX (ticked after main SFX pass) |

### 2.4 Audio ROM tables / driver blob

| Address | Symbol | Notes |
|---------|--------|--------|
| `$011B44` | `level_music_table` | Per-level BGM IDs for levels 0–7 |
| `$072824` | `sound_priority_table` | Priority for IDs from `$81` (index = ID−`$81`) |
| `$07288C` | `music_pointer_table` | Absolute music header pointers for IDs `$81+` |
| `$0732D2` | `fm_note_period_table` | 12 YM2612 semitone period words |
| `$073A64` | `psg_note_period_table` | PSG period words |
| `$073B56` | `sfx_pointer_table` | Absolute SFX header pointers for IDs `$A0`–`$CF` |
| `$0795A2` | `z80_dac_driver_kosinski` | Kosinski-compressed Z80 DAC driver |

---

## 3. Bootstrapping the Z80 DAC driver

### 3.1 `load_z80_dac_driver` ($1061C)

From `labels.csv`. Called from early init / level start paths (including `init_levelstart` at `$106EA`). Sequence:

1. Assert **`z80_reset`** and **`z80_busreq`**; spin until the bus is granted.
2. Kosinski-decompress **`z80_dac_driver_kosinski`** (`$795A2`) into work RAM `$FF7000` via **`kosinskidec`** (`$85A2`).
3. Copy **~$1EC7** bytes into Z80 RAM `$0000`.
4. Seed **`z80_sample_bank_lo`…** (`$1FF8`–`$1FFB`) with `00 80 07 80`.
5. Pulse RESET low then high; release BUSREQ so the Z80 begins at `$0000`.

### 3.2 Z80 driver behaviour (mailbox DAC player)

Compact PCM player, not a multi-channel sequencer:

| Z80 addr | Symbol (68000 map) | Role |
|----------|--------------------|------|
| `$1FF8`–`$1FFB` | `z80_sample_bank_lo`… | Sample bank / pointer setup |
| `$1FF6` | `z80_dac_status` | Status polled before posting |
| `$1FFD` bit 7 | `z80_dac_busy` | Busy while Z80 owns DAC |
| `$1FFF` | `z80_dac_command` | Command byte from 68000 |

Typical start: clear busy, program bank register `$6000` from `$1FF8`–`$1FFB`, loop on `$1FFF` — commands **`≥ $81`** enable DAC (YM reg `$2B = $80`), index sample with `SUB $81`, stream from banked ROM.

The host emulator (`MegaDriveEnvironment` Z80 core) models the 68000 driver hammering BUSREQ while waiting on `z80_dac_busy` — a real hardware handshake.

---

## 4. How game code requests sound

### 4.1 Fire-and-forget queue

Ordinary game code posts an ID with bit 7 set (`$80`–`$FF`):

```asm
move.b #$87, (play_se_).w     ; request ID $87
```

**`queue_sound_id`** (`$1069E`): ID in `d7`; scans `play_se` / `play_se_1` / `play_se_2`, skips if already present, else stores in the first free slot.  
Alternate entry `$106CA` maps `object+$50` through ROM table `$106D6` into `d7` first.

In the recompilation runtime this helper is hand-written (`SoRSound.cpp`) and may log via async `Logger`.

### 4.2 Level BGM: `play_level_music` ($11B12)

Posts BGM from **`level_music_table`** (`$11B44`), unless flags force alternate IDs such as `$87` / `$90`:

| Level | Sound ID |
|------:|----------|
| 0 | `$81` |
| 1 | `$8B` |
| 2 | `$84` |
| 3 | `$8C` |
| 4 | `$85` |
| 5 | `$89` |
| 6 | `$86` |
| 7 | `$88` |

Also hand-written in `SoRSound.cpp` in the recompilation project.

Other call sites (non-exhaustive):

| ID | Typical context |
|----|-----------------|
| `$E0` | Fade / stop current BGM (`sound_start_music_fade`) |
| `$90` | Sting / jingle on certain sequence ends |
| `$87` | Alternate BGM on some transitions |
| `$BD`, `$BF` | Event SFX |

---

## 5. The 68000 sound engine

### 5.1 When it runs

**`vblank_handler`** (`$19D16`) always invokes **`sound_engine`** before `RTE`. Channel timing is **frame-based**.

### 5.2 `sound_engine` ($72914)

Each frame (simplified):

1. **`sound_process_queue`** — unless `stop_music` blocks starts.
2. **`sound_fade_music`** — advance fade state.
3. Walk active channels (bit 7 of block base):

| Symbol base | Count | Kind | Tick |
|-------------|------:|------|------|
| `sound_music_fm0`…`fm4` | 5 | Music FM | `sound_update_fm_channel` |
| `sound_music_dac` | 1 | Music DAC | `sound_update_dac_channel` |
| `sound_music_psg0`…`psg2` | 3 | Music PSG | `sound_update_psg_channel` |
| `sound_sfx_fm0`… | 2+ | SFX FM | `sound_update_fm_channel` |
| `sound_sfx_psg0`… | 2 | SFX PSG | `sound_update_psg_channel` |
| `sound_sfx_fm_extra` | 1 | Extra FM SFX | `sound_update_fm_channel` |

`sound_update_phase` (`$FFF006`) separates music vs SFX ownership so SFX can steal hardware channels without the music tick fighting them.  
`sound_engine_gate` (`$FFF007`) forces the pause/special path (key-off / silence when positive; sticky clear when bit 7 set).

### 5.3 `sound_process_queue` ($72D08)

1. Read up to three queue bytes (`play_se`…).
2. Accept only values with **bit 7 set**.
3. Map `ID − $81` through **`sound_priority_table`**; keep highest priority in `sound_active_priority` / `sound_pending_id`.
4. Clear the queue slots.
5. Branch on ID range:

| ID range | Class | Behaviour |
|----------|-------|-----------|
| `$81`–`$9F` | **Music** | `music_pointer_table` → arm FM + PSG (+ DAC) channels |
| `$A0`–`$CF` | **SFX** | `sfx_pointer_table` → arm SFX channels; may steal music slots |
| `$D0`–`$DF` | **Direct PCM** | `(ID + $B6)` → `z80_dac_command` under BUSREQ |
| `$E0`–`$E3` | **Special** | `$E0` → `sound_start_music_fade` |
| else / idle | Reset / idle | `sound_reset_all` path or mark `sound_pending_id = $80` |

Priorities: music ~**`$80`**; SFX roughly **`$2F`–`$7F`** (e.g. `$BD`/`$BF` at `$7F`); PCM **`$72`–`$78`**.

---

## 6. Music and SFX data formats

### 6.1 Music header

Pointed by **`music_pointer_table`**. Example layout (track `$87` @ `$75B3A`):

```
+0  word   relative offset to FM instrument / voice bank → sound_music_voice_bank
+2  byte   FM track count (commonly 6)
+3  byte   PSG track count (commonly 3)
+4  …      per FM track: word relative seq ptr, byte transpose
           per PSG track: word relative seq ptr, two meta bytes
```

Six “FM” tracks map onto five tonal FM slots plus **`sound_music_dac`**.

### 6.2 SFX header

Pointed by **`sfx_pointer_table`**. Routing tables at `$72E74` / `$72E88` choose which SFX channel block to initialise and which music channel to mark stolen.

### 6.3 Sequence stream

- **Byte &lt; `$F0`**: duration (bit 7 often rest / key-off); optional pitch byte follows.
- **Byte ≥ `$F0`**: command → **`sound_seq_command`** (`$732EA`), with separate FM vs PSG jump tables (`$73302` / `$73342`).

### 6.4 Pitch tables

| Chip | Symbol | Use |
|------|--------|-----|
| YM2612 | `fm_note_period_table` | Semitone periods → regs `$A0`/`$A4` + key-on `$28` |
| SN76489 | `psg_note_period_table` | Periods → latch writes to `sn76489_psg` |

PSG volume envelopes also use a pointer table near `$728D0`.

---

## 7. Hardware write paths (labelled routines)

| Symbol | Address | Role |
|--------|---------|------|
| `sound_ym2612_acquire` | `$73298` | BUSREQ, wait ACK, wait `z80_dac_busy` clear, wait YM ready |
| `sound_ym2612_write` | `$73206` | Part 0: reg → `ym2612_a0`, data → `ym2612_d0` |
| `sound_ym2612_write_part1` | `$7323E` | Part 1: reg → `ym2612_a1`, data → `ym2612_d1` |
| `sound_ym2612_write_channel` | `$731FA` | Add channel hw index; part 0 or part 1 |
| `sound_ym2612_write_bus` | `$73286` | acquire + write + release bus |
| `sound_psg_silence` | `$731A2` | `$9F $BF $DF $FF` on `sn76489_psg` |
| `sound_update_dac_channel` | `$72A4E` | DAC sequence; posts to `z80_dac_command` |
| `sound_update_fm_channel` | `$72B08` | FM sequence + pitch via `fm_note_period_table` |
| `sound_update_psg_channel` | `$7388C` | PSG sequence + pitch via `psg_note_period_table` |
| `sound_seq_command` | `$732EA` | `$F0+` command dispatcher |

Key-on / key-off use YM register **`$28`**. Direct PCM (`$D0`–`$DF`) bypasses the music DAC sequencer and posts straight to `z80_dac_command`.

---

## 8. Stop / fade music

| Symbol | Address | Role |
|--------|---------|------|
| `sound_start_music_fade` | `$7302E` | From command `$E0`: `stop_music=$10`, `stop_music_timer=4`; clear DAC/PSG music channels |
| `sound_fade_music` | `$73066` | Every 4 frames raise FM attenuation; when counter hits 0 → `sound_reset_all` |
| `sound_reset_all` | `$72F8A` | DAC/YM cleanup, zero `$FFF000` workspace, `sound_psg_silence` |

---

## 9. `sync_z80_1` / `sync_z80_2` (not sound mailboxes)

| Symbol | Address | Role |
|--------|---------|------|
| `sync_z80_1` | `$10502` | Write **1** to `$FFFA00`, enable IRQs (`sr=$2500`), spin until `vblank_handler` clears it |
| `sync_z80_2` | `$10514` | Same with **2** |

These are **VBlank wait** helpers, not Z80 sound mailboxes. `vblank_handler` consumes `$FFFA00` (scroll/DMA, often with BUSREQ), then runs `sound_engine`. Manual C++ bodies: `SorManualFunctions.cpp`.

---

## 10. Labelled sound code map (`labels.csv`)

| Address | Symbol |
|---------|--------|
| `$085A2` | `kosinskidec` |
| `$10502` | `sync_z80_1` |
| `$10514` | `sync_z80_2` |
| `$1061C` | `load_z80_dac_driver` |
| `$1069E` | `queue_sound_id` |
| `$11B12` | `play_level_music` |
| `$19D16` | `vblank_handler` |
| `$72914` | `sound_engine` |
| `$72A4E` | `sound_update_dac_channel` |
| `$72B08` | `sound_update_fm_channel` |
| `$72D08` | `sound_process_queue` |
| `$72F8A` | `sound_reset_all` |
| `$7302E` | `sound_start_music_fade` |
| `$73066` | `sound_fade_music` |
| `$731A2` | `sound_psg_silence` |
| `$731FA` | `sound_ym2612_write_channel` |
| `$73206` | `sound_ym2612_write` |
| `$7323E` | `sound_ym2612_write_part1` |
| `$73286` | `sound_ym2612_write_bus` |
| `$73298` | `sound_ym2612_acquire` |
| `$732EA` | `sound_seq_command` |
| `$7388C` | `sound_update_psg_channel` |

Deep sequence helpers remain largely unnamed `sub_072xxx` / `sub_073xxx` in generated code.

---

## 11. Recompilation project map

| Artifact | Location |
|----------|----------|
| Address symbols | `code-analysis/addresses.csv` |
| Code labels | `code-analysis/labels.csv` |
| Manual function list | `code-analysis/manual_functions.txt` |
| Disassembly | `output/sor.asm` |
| Generated 68000 recompile | `generated/Sor.cpp` / `Sor.hpp` |
| Manual VBlank + punch damage | `SorManualFunctions.cpp` |
| Manual `queue_sound_id` / `play_level_music` | `SoRSound.cpp` |
| Async host logger | `MegaDriveEnvironment` `util/Logger.hpp` |
| YM2612 + PSG host | `MegaDriveEnvironment` `system/sound/` |
| Z80 host + bank/BUSREQ | `MegaDriveEnvironment` `system/z80/` |
| Local ROM (not versioned) | `rom/SOR.bin` |

**Manual vs generated (sound-related):** full engine/channel ticks stay **generated** (a full native port of `sound_engine` previously softlocked the game). Only small helpers are hand-written today (`queue_sound_id`, `play_level_music`, plus non-sound manuals).

---

## 12. Design takeaways

1. **68000-centric mixer** — tempo, music, and most SFX are frame-driven on the main CPU; the Z80 is specialised for sample streaming.
2. **Single public API** — almost all audio is “write an ID to `play_se`”; music and SFX share one queue and `sound_priority_table`.
3. **Hardware arbitration** — `z80_busreq` plus `z80_dac_busy` / `z80_dac_status` keep FM writes and DAC streaming from stomping each other on the YM2612.
4. **Channel stealing** — SFX blocks sit in separate RAM and can suppress music lanes for the same physical FM/PSG channel.
5. **Fade, not only mute** — BGM stop is a multi-frame attenuation ramp (`stop_music` / `sound_fade_music`), then `sound_reset_all`.

---

## 13. Open follow-ups

1. **Name every ID** `$81`–`$CF` (and PCM `$D0`–`$DF`) from call sites and auditory testing.
2. **Document the full `$F0+` command set** in `sound_seq_command` (FM vs PSG tables).
3. **Extract the Z80 sample directory** (offsets/lengths after `SUB $81`) and map PCM IDs to game events.
4. **Instrument bank format** behind the music header’s voice-bank pointer / `sound_music_voice_bank`.
5. Safe, incremental **native port** of `sound_ym2612_*` / engine (with correct BUSREQ pacing) if desired.

---

## 14. One-line summary

***Streets of Rage* runs a custom 68000 frame sequencer (`sound_engine`) for FM and PSG music/SFX, uses the `play_se` work-RAM queue as the only game-facing API, and relegates the Z80 to a BUSREQ-guarded YM2612 DAC sample player driven by `z80_dac_command` / `z80_dac_busy` in the top of Z80 RAM.**

---

*Addresses and routine names synced with `code-analysis/addresses.csv` and `code-analysis/labels.csv`. Behaviours derived from disassembly and ROM inspection; confidence high for structure and I/O, lower for human-readable track/SFX names until call-site naming is completed.*
