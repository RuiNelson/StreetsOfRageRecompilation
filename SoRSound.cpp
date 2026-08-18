#include "SoR.hpp"
#include "Logger.hpp"

#include <cstdint>
#include <thread>

// Small sound-side helpers reimplemented as native C++.
// The per-VBlank engine, remaining YM write helpers, and channel sequencers
// stay generated until the full native port is proven in-game.

namespace {

constexpr m_long kPlaySe0       = 0x00FFF00Au;
constexpr m_long kLevelMusicTbl = 0x00011B44u;
constexpr m_long kIdLookup106D6 = 0x000106D6u;
constexpr m_long kHalfDamage    = 0xFFFFFA43u;

constexpr m_long kZ80Busreq  = 0x00A11100u;
constexpr m_long kZ80DacBusy = 0x00A01FFDu;
constexpr m_long kYm2612A0   = 0x00A04000u;

} // namespace

// ---------------------------------------------------------------------------
// $01069E — queue a sound ID (d7) into the 3-slot play_se queue at $FFF00A
// Alternate entry $0106CA: map object+$50 through ROM table $106D6 into d7 first.
// ---------------------------------------------------------------------------
void StreetsOfRage::queue_sound_id(m_long entry_) {
    traceEnter(0x0001069Eu);
    auto &mem = memory();

    if (entry_ == 0x000106CAu) {
        const m_byte idx = mem.readByte(cpu().a[0] + 0x50);
        cpu().setDb(7, mem.readByte(kIdLookup106D6 + idx));
    }

    const m_byte id = cpu().db(7);
    // Logger::log("[sound] queue_sound_id $%02X", static_cast<unsigned>(id));

    for (int i = 0; i < 3; ++i) {
        if (mem.readByte(kPlaySe0 + i) == id) {
            goto out;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (mem.readByte(kPlaySe0 + i) == 0) {
            mem.writeByte(kPlaySe0 + i, id);
            break;
        }
    }

    out: cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $011B12 — post the current level's BGM (or alternate IDs under flags)
// ---------------------------------------------------------------------------
void StreetsOfRage::play_level_music(m_long /*entry_*/) {
    traceEnter(0x00011B12u);
    auto &mem = memory();

    mem.writeWord(0xFFFFFB3Eu, 0x0003);

    if (mem.readByte(kHalfDamage) != 0) {
        mem.writeByte(kPlaySe0, 0x87);
        cpu().ssp += 4;
        return;
    }

    const m_word level = mem.readWord(0xFFFFFF02u);
    if ((mem.readByte(0xFFFFFA05u) & 0x40u) != 0) {
        mem.writeByte(kPlaySe0, level == 7 ? 0x90 : 0x87);
        cpu().ssp += 4;
        return;
    }

    mem.writeByte(kPlaySe0, mem.readByte(kLevelMusicTbl + level));
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $073298 — take the Z80 bus and wait until the YM2612 is free to program
//
// ROM: BUSREQ, spin until ACK, if $A01FFD bit7 (DAC busy) release + retry,
// then spin on YM status bit 7. Callers write FM registers and then release
// the bus. D2 receives the last YM status byte (bit 7 clear on the RTS path).
// ---------------------------------------------------------------------------
void StreetsOfRage::sound_ym2612_acquire(m_long /*entry_*/) {
    traceEnter(0x00073298u);

    // Called from sound_engine during VBlank: waitForInterrupt() would deadlock
    // on the raised IPL. Do not sleep here either. Voices pulse $A01FFD for a
    // few Z80 instructions per sample; a 50 µs pause after release lets the
    // Z80 thread start a long catch-up slice, and the next BUSREQ then stalls
    // the 68K (visible hitch). The ROM retries after three NOPs so it reclaims
    // the bus before that happens. yield() is only for the ACK/YM polls.
    const auto waitForHardware = [this] {
        if (irqLevel() > cpu().interruptMask())
            serviceIRQ();
        else
            std::this_thread::yield();
        return !shouldQuit();
    };

    while (!shouldQuit()) {
        memory().writeWord(kZ80Busreq, 0x0100u);
        memory().waitForByteValue(kZ80Busreq, 0, waitForHardware);
        if (shouldQuit())
            break;

        if ((memory().readByte(kZ80DacBusy) & 0x80u) != 0) {
            // Drop the bus so the host release path can run a Z80 slice, then
            // hammer BUSREQ immediately like the ROM's three-NOP retry.
            memory().writeWord(kZ80Busreq, 0);
            continue;
        }

        for (;;) {
            const m_byte status = memory().readByte(kYm2612A0);
            cpu().setDb(2, status);
            cpu().setFlag(CPU68K::FlagN, (status & 0x80u) != 0);
            cpu().setFlag(CPU68K::FlagV, false);
            cpu().setFlag(CPU68K::FlagC, false);
            cpu().setFlag(CPU68K::FlagZ, (status & 0x80u) == 0);
            if ((status & 0x80u) == 0 || !waitForHardware())
                break;
        }
        break;
    }

    cpu().ssp += 4;
}
