#include "Sor.hpp"

#include <cstdint>

// Native reimplementations of the labelled sound-driver entry points.
// Stack protocol matches generated CALL/RETURN_68K: each JSR target ends with
// cpu().ssp += 4. Nested calls to other recompiled/manual entry points push a
// dummy return address first. Tail transfers (bra/jmp to another entry) call
// the target with its entry address and return without an extra pop.
//
// Deep sequence helpers that are still generated (sub_072xxx / sub_073xxx) are
// invoked the same way. IRQs may lag for these short paths; busy-waits rely on
// wall-clock master cycles / the Z80 thread.

namespace {

constexpr m_long kZ80Busreq     = 0x00A11100u;
constexpr m_long kZ80Reset      = 0x00A11200u;
constexpr m_long kZ80Ram        = 0x00A00000u;
constexpr m_long kYm2612A0      = 0x00A04000u;
constexpr m_long kPsg           = 0x00C00011u;
constexpr m_long kZ80DacStatus  = 0x00A01FF6u;
constexpr m_long kZ80DacBusy    = 0x00A01FFDu;
constexpr m_long kZ80DacCommand = 0x00A01FFFu;
constexpr m_long kZ80SampleBank = 0x00A01FF8u;

constexpr m_long kPlaySe0       = 0x00FFF00Au;
constexpr m_long kStopMusic     = 0x00FFF002u;
constexpr m_long kStopMusicTmr  = 0x00FFF003u;
constexpr m_long kSoundPhase    = 0x00FFF006u;
constexpr m_long kSoundGate     = 0x00FFF007u;
constexpr m_long kSoundDacFlag  = 0x00FFF008u;
constexpr m_long kSoundPending  = 0x00FFF009u;
constexpr m_long kSoundPriority = 0x00FFF000u;
constexpr m_long kSoundWork     = 0x00FFF000u;

constexpr m_long kMusicFm0      = 0x00FFF040u;
constexpr m_long kMusicDac      = 0x00FFF130u;
constexpr m_long kMusicPsg0     = 0x00FFF160u;
constexpr m_long kSfxFm0        = 0x00FFF1F0u;
constexpr m_long kSfxPsg0       = 0x00FFF2B0u;
constexpr m_long kSfxFmExtra    = 0x00FFF340u;

constexpr m_long kPriorityTable = 0x00072824u;
constexpr m_long kLevelMusicTbl = 0x00011B44u;
constexpr m_long kZ80DriverRom  = 0x000795A2u;
constexpr m_long kKosinskiDest  = 0x00FF7000u;
constexpr m_long kIdLookup106D6 = 0x000106D6u;

constexpr m_long kChannelSize   = 0x30;
constexpr m_long kChSeqPtr      = 0x04;
constexpr m_long kChDuration    = 0x0E;
constexpr m_long kChDurationCmp = 0x0D;
constexpr m_long kChSample      = 0x10;
constexpr m_long kChSampleBak   = 0x11;
constexpr m_long kChAtten       = 0x09;
constexpr m_long kChHwIndex     = 0x01;

constexpr m_byte kChActive      = 0x80;
constexpr m_long kDriverBytes   = 0x1EC7; // dbf count $1EC6 → $1EC7 copies

bool channelActive(SystemMemory &mem, m_long channel) {
    return (mem.readByte(channel) & kChActive) != 0;
}

} // namespace

// ── Stack helpers (usable only from Sor members) ─────────────────────────────

#define SOR_RTS()                                                                                  \
    do {                                                                                           \
        cpu().ssp += 4;                                                                            \
        return;                                                                                    \
    } while (0)

#define SOR_JSR(call_expr)                                                                         \
    do {                                                                                           \
        cpu().ssp -= 4;                                                                            \
        memory().writeLong(cpu().ssp, 0);                                                          \
        call_expr;                                                                                 \
    } while (0)

#define SOR_TAIL(call_expr)                                                                        \
    do {                                                                                           \
        call_expr;                                                                                 \
        return;                                                                                    \
    } while (0)

// ── YM2612 / PSG hardware ────────────────────────────────────────────────────

void Sor::sound_ym2612_acquire(m_long /*entry_*/) {
    traceEnter(0x00073298u);
    auto &mem = memory();

    for (;;) {
        mem.writeWord(kZ80Busreq, 0x0100);
        while ((mem.readByte(kZ80Busreq) & 1u) != 0) {
        }

        if ((mem.readByte(kZ80DacBusy) & 0x80u) != 0) {
            mem.writeWord(kZ80Busreq, 0x0000);
            continue;
        }

        while ((mem.readByte(kYm2612A0) & 0x80u) != 0) {
        }
        break;
    }
    SOR_RTS();
}

void Sor::sound_ym2612_write(m_long entry_) {
    traceEnter(0x00073206u);
    auto &mem = memory();

    // Mid-entry $73234: adjust reg by channel hw index (part 1 path) then write part 1.
    if (entry_ == 0x00073234u) {
        m_byte hw = static_cast<m_byte>(mem.readByte(cpu().a[3] + kChHwIndex) & ~0x04u);
        cpu().setDb(0, static_cast<m_byte>(cpu().db(0) + hw));
        SOR_TAIL(sound_ym2612_write_part1());
    }

    const m_byte reg  = cpu().db(0);
    const m_byte data = cpu().db(1);
    while ((mem.readByte(kYm2612A0) & 0x80u) != 0) {
    }
    mem.writeByte(kYm2612A0, reg);
    while ((mem.readByte(kYm2612A0) & 0x80u) != 0) {
    }
    mem.writeByte(kYm2612A0 + 1, data);
    SOR_RTS();
}

void Sor::sound_ym2612_write_part1(m_long /*entry_*/) {
    traceEnter(0x0007323Eu);
    auto &mem = memory();

    const m_byte reg  = cpu().db(0);
    const m_byte data = cpu().db(1);
    while ((mem.readByte(kYm2612A0) & 0x80u) != 0) {
    }
    mem.writeByte(kYm2612A0 + 2, reg);
    while ((mem.readByte(kYm2612A0) & 0x80u) != 0) {
    }
    mem.writeByte(kYm2612A0 + 3, data);
    SOR_RTS();
}

void Sor::sound_ym2612_write_bus(m_long /*entry_*/) {
    traceEnter(0x00073286u);
    SOR_JSR(sound_ym2612_acquire());
    SOR_JSR(sound_ym2612_write());
    memory().writeWord(kZ80Busreq, 0x0000);
    SOR_RTS();
}

void Sor::sound_ym2612_write_channel(m_long /*entry_*/) {
    traceEnter(0x000731FAu);
    auto &mem = memory();
    const m_byte hw = mem.readByte(cpu().a[3] + kChHwIndex);

    if ((hw & 0x04u) != 0) {
        // Part 1: clear bit 2 of hw index and add into reg, then write part 1.
        SOR_TAIL(sound_ym2612_write(0x00073234u));
    }

    cpu().setDb(0, static_cast<m_byte>(cpu().db(0) + hw));
    SOR_TAIL(sound_ym2612_write());
}

void Sor::sound_psg_silence(m_long /*entry_*/) {
    traceEnter(0x000731A2u);
    auto &mem = memory();
    mem.writeByte(kPsg, 0x9F);
    mem.writeByte(kPsg, 0xBF);
    mem.writeByte(kPsg, 0xDF);
    mem.writeByte(kPsg, 0xFF);
    SOR_RTS();
}

// ── Public game API ──────────────────────────────────────────────────────────

void Sor::queue_sound_id(m_long entry_) {
    traceEnter(0x0001069Eu);
    auto &mem = memory();

    // Alternate entry: map object+$50 through a tiny ROM table into d7, then queue.
    if (entry_ == 0x000106CAu) {
        const m_byte idx = mem.readByte(cpu().a[0] + 0x50);
        cpu().setDb(7, mem.readByte(kIdLookup106D6 + idx));
    }

    const m_byte id = cpu().db(7);

    // Already queued?
    for (int i = 0; i < 3; ++i) {
        if (mem.readByte(kPlaySe0 + i) == id) {
            SOR_RTS();
        }
    }
    // First free slot.
    for (int i = 0; i < 3; ++i) {
        if (mem.readByte(kPlaySe0 + i) == 0) {
            mem.writeByte(kPlaySe0 + i, id);
            break;
        }
    }
    SOR_RTS();
}

void Sor::play_level_music(m_long /*entry_*/) {
    traceEnter(0x00011B12u);
    auto &mem = memory();

    mem.writeWord(0xFFFFFB3Eu, 0x0003);

    // half_damage / special flags select alternate IDs $87 / $90.
    if (mem.readByte(0xFFFFFA43u) != 0) {
        mem.writeByte(kPlaySe0, 0x87);
        SOR_RTS();
    }

    const m_word level = mem.readWord(0xFFFFFF02u);
    if ((mem.readByte(0xFFFFFA05u) & 0x40u) != 0) {
        m_byte id = 0x90;
        if (level != 7)
            id = 0x87;
        mem.writeByte(kPlaySe0, id);
        SOR_RTS();
    }

    mem.writeByte(kPlaySe0, mem.readByte(kLevelMusicTbl + level));
    SOR_RTS();
}

void Sor::load_z80_dac_driver(m_long /*entry_*/) {
    traceEnter(0x0001061Cu);
    auto &mem = memory();

    mem.writeWord(kZ80Reset, 0x0100);
    mem.writeWord(kZ80Busreq, 0x0100);
    while ((mem.readByte(kZ80Busreq) & 1u) != 0) {
    }

    cpu().a[0] = kZ80DriverRom;
    cpu().a[1] = kKosinskiDest;
    SOR_JSR(kosinskidec());

    m_long dst = kZ80Ram;
    m_long src = kKosinskiDest;
    for (m_long i = 0; i < kDriverBytes; ++i)
        mem.writeByte(dst++, mem.readByte(src++));

    mem.writeByte(kZ80SampleBank + 0, 0x00);
    mem.writeByte(kZ80SampleBank + 1, 0x80);
    mem.writeByte(kZ80SampleBank + 2, 0x07);
    mem.writeByte(kZ80SampleBank + 3, 0x80);

    mem.writeWord(kZ80Reset, 0x0000);
    // Original ror.b #8,d0 is only a short delay between reset edges.
    mem.writeWord(kZ80Reset, 0x0100);
    mem.writeWord(kZ80Busreq, 0x0000);
    SOR_RTS();
}

// ── Engine frame ─────────────────────────────────────────────────────────────

void Sor::sound_engine(m_long /*entry_*/) {
    traceEnter(0x00072914u);
    auto &mem = memory();

    const m_byte gate = mem.readByte(kSoundGate);
    if (gate != 0) {
        if (static_cast<std::int8_t>(gate) < 0) {
            mem.writeByte(kSoundGate, 0);
            SOR_RTS();
        }

        // Positive gate: key-off FM slots, silence PSG, then fall into normal tick.
        SOR_JSR(sound_ym2612_acquire());
        for (m_byte ch = 0; ch < 3; ++ch) {
            cpu().setDb(0, 0x28);
            cpu().setDb(1, ch);
            SOR_JSR(sound_ym2612_write());
            cpu().setDb(0, 0x28);
            cpu().setDb(1, static_cast<m_byte>(ch + 4));
            SOR_JSR(sound_ym2612_write());
        }
        mem.writeWord(kZ80Busreq, 0x0000);
        SOR_JSR(sound_psg_silence());
    }

    if (mem.readByte(kStopMusic) == 0)
        SOR_JSR(sound_process_queue());
    SOR_JSR(sound_fade_music());

    if (mem.readByte(kSoundGate) == 0) {
        mem.writeByte(kSoundPhase, 0);

        m_long ch = kMusicFm0;
        for (int i = 0; i < 5; ++i, ch += kChannelSize) {
            if (channelActive(mem, ch)) {
                cpu().a[3] = ch;
                SOR_JSR(sound_update_fm_channel());
            }
        }

        if (channelActive(mem, kMusicDac)) {
            cpu().a[3] = kMusicDac;
            SOR_JSR(sound_update_dac_channel());
        }

        ch = kMusicPsg0;
        for (int i = 0; i < 3; ++i, ch += kChannelSize) {
            if (channelActive(mem, ch)) {
                cpu().a[3] = ch;
                SOR_JSR(sound_update_psg_channel());
            }
        }
    }

    mem.writeByte(kSoundPhase, 0x80);

    m_long ch = kSfxFm0;
    for (int i = 0; i < 2; ++i, ch += kChannelSize) {
        if (channelActive(mem, ch)) {
            cpu().a[3] = ch;
            SOR_JSR(sound_update_fm_channel());
        }
    }

    ch = kSfxPsg0;
    for (int i = 0; i < 2; ++i, ch += kChannelSize) {
        if (channelActive(mem, ch)) {
            cpu().a[3] = ch;
            SOR_JSR(sound_update_psg_channel());
        }
    }

    mem.writeByte(kSoundPhase, 0x01);
    if (channelActive(mem, kSfxFmExtra)) {
        cpu().a[3] = kSfxFmExtra;
        SOR_JSR(sound_update_fm_channel());
    }

    SOR_RTS();
}

void Sor::sound_process_queue(m_long /*entry_*/) {
    traceEnter(0x00072D08u);
    auto &mem = memory();

    // Pick the highest-priority ID among the three play_se slots (must have bit7).
    m_byte bestPrio = mem.readByte(kSoundPriority);
    m_byte bestId   = 0;
    bool   haveNew  = false;

    for (int i = 0; i < 3; ++i) {
        const m_byte id = mem.readByte(kPlaySe0 + i);
        mem.writeByte(kPlaySe0 + i, 0);
        if ((id & 0x80u) == 0)
            continue;
        if (id < 0x81u)
            continue;
        const m_byte prio = mem.readByte(kPriorityTable + (id - 0x81u));
        if (prio < bestPrio)
            continue;
        bestPrio = prio;
        bestId   = id;
        haveNew  = true;
    }

    if ((bestPrio & 0x80u) != 0)
        bestPrio = 0;
    mem.writeByte(kSoundPriority, bestPrio);

    if (haveNew)
        mem.writeByte(kSoundPending, bestId);

    // No pending command (bit7 clear) → full reset + idle mark ($72F6E).
    // After a normal frame, pending is $80 so empty queues fall through and
    // the music start path rejects $80 without resetting.
    if ((mem.readByte(kSoundPending) & 0x80u) == 0) {
        SOR_TAIL(sub_072da0(0x00072F6Eu));
    }

    const m_byte id = mem.readByte(kSoundPending);
    cpu().setDb(0, id);

    if (id < 0xA0u) {
        SOR_TAIL(sub_072da0(0x00072EA0u)); // start music ($81–$9F)
    }
    if (id < 0xD0u) {
        SOR_TAIL(sub_072da0(0x00072DCEu)); // start SFX ($A0–$CF)
    }
    if (id < 0xE0u) {
        SOR_TAIL(sub_072da0(0x00072DA4u)); // direct PCM ($D0–$DF)
    }
    if (id < 0xE4u) {
        // $E0–$E3 special jump table at $72D90. Only $E0 (fade) is used in practice.
        if (static_cast<m_byte>(id - 0xE0u) == 0) {
            SOR_TAIL(sub_072fe4(0x0007302Eu));
        }
    }

    SOR_TAIL(sub_072da0(0x00072F6Eu));
}

void Sor::sound_fade_music(m_long /*entry_*/) {
    traceEnter(0x00073066u);
    auto &mem = memory();

    if (mem.readByte(kStopMusic) == 0)
        SOR_RTS();

    m_byte tmr = mem.readByte(kStopMusicTmr);
    if (tmr != 0) {
        mem.writeByte(kStopMusicTmr, static_cast<m_byte>(tmr - 1));
        SOR_RTS();
    }

    m_byte fade = mem.readByte(kStopMusic);
    fade = static_cast<m_byte>(fade - 1);
    mem.writeByte(kStopMusic, fade);
    if (fade == 0) {
        SOR_TAIL(sound_reset_all());
    }

    mem.writeByte(kStopMusicTmr, 0x04);
    m_long ch = kMusicFm0;
    for (int i = 0; i < 5; ++i, ch += kChannelSize) {
        const m_byte atten = static_cast<m_byte>(mem.readByte(ch + kChAtten) + 1);
        mem.writeByte(ch + kChAtten, atten);
        cpu().a[3] = ch;
        cpu().setDb(3, atten);
        SOR_JSR(sub_0736da());
    }
    SOR_RTS();
}

void Sor::sound_reset_all(m_long /*entry_*/) {
    traceEnter(0x00072F8Au);
    auto &mem = memory();

    cpu().setDb(0, 0x2B);
    cpu().setDb(1, 0x80);
    SOR_JSR(sound_ym2612_write_bus());

    cpu().setDb(0, 0x27);
    cpu().setDb(1, 0x00);
    SOR_JSR(sound_ym2612_write_bus());

    SOR_JSR(sound_ym2612_acquire());
    cpu().setDb(0, 0xB4);
    cpu().setDb(1, 0xC0);
    for (int i = 0; i < 3; ++i) {
        SOR_JSR(sound_ym2612_write());
        SOR_JSR(sound_ym2612_write_part1());
        cpu().setDb(0, static_cast<m_byte>(cpu().db(0) + 1));
    }
    mem.writeWord(kZ80Busreq, 0x0000);

    // Clear $9C longs ($272 bytes) of sound workspace from $FFF000.
    m_long p = kSoundWork;
    for (int i = 0; i < 0x9C; ++i) {
        mem.writeLong(p, 0);
        p += 4;
    }

    SOR_JSR(sub_073142());
    SOR_JSR(sound_psg_silence());
    SOR_RTS();
}

// ── Per-channel ticks (native control flow; helpers still generated) ─────────

void Sor::sound_update_fm_channel(m_long /*entry_*/) {
    traceEnter(0x00072B08u);
    auto &mem = memory();
    const m_long ch = cpu().a[3];

    cpu().a[4] = mem.readLong(ch + kChSeqPtr);

    if ((mem.readByte(ch) & 0x40u) != 0) {
        SOR_TAIL(sub_0736da(0x00073762u));
    }

    m_byte dur = mem.readByte(ch + kChDuration);
    dur        = static_cast<m_byte>(dur - 1);
    mem.writeByte(ch + kChDuration, dur);

    if (dur == 0) {
        if (mem.readByte(cpu().a[4]) != 0xFD && (mem.readByte(ch) & 0x20u) == 0)
            mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) & ~0x02u));
        mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) & ~0x20u));
        mem.writeByte(kSoundDacFlag, 0);
        SOR_JSR(sub_072b5e());
        SOR_JSR(sub_0731ba());
        SOR_RTS();
    }

    if ((mem.readByte(ch) & 0x02u) != 0) {
        mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) & ~0x20u));
        SOR_JSR(sub_0731de());
        SOR_RTS();
    }

    SOR_JSR(sub_072bfe());
    SOR_JSR(sub_072c1a());
    SOR_JSR(sub_072c74());
    SOR_RTS();
}

void Sor::sound_update_psg_channel(m_long /*entry_*/) {
    traceEnter(0x0007388Cu);
    auto &mem = memory();
    const m_long ch = cpu().a[3];

    m_byte dur = mem.readByte(ch + kChDuration);
    dur        = static_cast<m_byte>(dur - 1);
    mem.writeByte(ch + kChDuration, dur);

    if (dur == 0) {
        mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) & ~0x20u));
        SOR_JSR(sub_0738d8());
        SOR_JSR(sub_0739b6());
        SOR_RTS();
    }

    if ((mem.readByte(ch) & 0x02u) == 0) {
        SOR_JSR(sub_0738bc());
        SOR_JSR(sub_0739ac());
        SOR_JSR(sub_072c1a());
        SOR_JSR(sub_073980());
    }
    SOR_RTS();
}

void Sor::sound_update_dac_channel(m_long /*entry_*/) {
    traceEnter(0x00072A4Eu);
    auto &mem = memory();
    const m_long ch = cpu().a[3];

    m_byte dur = mem.readByte(ch + kChDuration);
    dur        = static_cast<m_byte>(dur - 1);
    mem.writeByte(ch + kChDuration, dur);

    if (dur != 0) {
        cpu().a[4] = mem.readLong(ch + kChSeqPtr);
        if (dur == mem.readByte(ch + kChDurationCmp)) {
            // Tie / sample-swap path
            mem.writeByte(ch + kChSampleBak, mem.readByte(ch + kChSample));
            mem.writeByte(ch + kChSample, 0x85);
            mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) | 0x02u));
            goto post_sample;
        }
        mem.writeLong(ch + kChSeqPtr, cpu().a[4]);
        SOR_RTS();
    }

    mem.writeByte(kSoundDacFlag, 0x80);
    SOR_JSR(sound_ym2612_acquire());
    if (mem.readByte(kZ80DacStatus) != 0)
        mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) | 0x01u));
    mem.writeWord(kZ80Busreq, 0x0000);

    cpu().a[4] = mem.readLong(ch + kChSeqPtr);
    if ((mem.readByte(ch) & 0x02u) != 0) {
        mem.writeByte(ch + kChSample, mem.readByte(ch + kChSampleBak));
        mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) & ~0x02u));
    }

    for (;;) {
        m_byte b = mem.readByte(cpu().a[4]);
        cpu().a[4] += 1;
        cpu().setDb(5, b);
        if (b >= 0xF0u) {
            SOR_JSR(sound_seq_command());
            continue;
        }
        mem.writeByte(ch + kChDuration, static_cast<m_byte>(b & 0x7Fu));
        if (static_cast<std::int8_t>(b) < 0) {
            // Rest: duck to quiet sample $85, then post.
            mem.writeByte(ch + kChSampleBak, mem.readByte(ch + kChSample));
            mem.writeByte(ch + kChSample, 0x85);
            mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) | 0x02u));
            break;
        }
        if (b == 0) {
            SOR_TAIL(sub_0736da(0x00073730u));
        }
        // Sample id follows.
        b = mem.readByte(cpu().a[4]);
        cpu().a[4] += 1;
        cpu().setDb(5, b);
        mem.writeByte(ch + kChSample, b);
        break;
    }

post_sample:
    SOR_JSR(sound_ym2612_acquire());
    if (mem.readByte(kZ80DacStatus) == 0) {
        mem.writeByte(kZ80DacCommand, mem.readByte(ch + kChSample));
        mem.writeByte(ch, static_cast<m_byte>(mem.readByte(ch) & ~0x01u));
    }
    mem.writeWord(kZ80Busreq, 0x0000);
    SOR_TAIL(sub_072bc8());
}

void Sor::sound_seq_command(m_long /*entry_*/) {
    traceEnter(0x000732EAu);
    auto &mem = memory();

    // d5 is command byte >= $F0. Index = (d5 - $F0) * 4 into FM or PSG table.
    m_word index = static_cast<m_word>(static_cast<m_byte>(cpu().db(5) - 0xF0u));
    index        = static_cast<m_word>(index << 2);
    cpu().setDw(5, index);

    const m_byte hw = mem.readByte(cpu().a[3] + kChHwIndex);
    if (static_cast<std::int8_t>(hw) < 0) {
        // PSG command table
        SOR_TAIL(dispatch(0x00073342u + static_cast<std::int16_t>(index)));
    }
    // FM command table
    SOR_TAIL(dispatch(0x00073302u + static_cast<std::int16_t>(index)));
}

#undef SOR_RTS
#undef SOR_JSR
#undef SOR_TAIL
