#include "Sor.hpp"

#include <cstdint>
#include <cstdio>
#include <print>

// Small sound-side helpers reimplemented as native C++.
// The per-VBlank engine, YM bus protocol, and channel sequencers remain in
// generated code until the full native port is proven in-game.

namespace {

constexpr m_long kPlaySe0       = 0x00FFF00Au;
constexpr m_long kLevelMusicTbl = 0x00011B44u;
constexpr m_long kIdLookup106D6 = 0x000106D6u;
constexpr m_long kHalfDamage    = 0xFFFFFA43u;

} // namespace

// ---------------------------------------------------------------------------
// $01069E — queue a sound ID (d7) into the 3-slot play_se queue at $FFF00A
// Alternate entry $0106CA: map object+$50 through ROM table $106D6 into d7 first.
// ---------------------------------------------------------------------------
void Sor::queue_sound_id(m_long entry_) {
    traceEnter(0x0001069Eu);
    auto &mem = memory();

    if (entry_ == 0x000106CAu) {
        const m_byte idx = mem.readByte(cpu().a[0] + 0x50);
        cpu().setDb(7, mem.readByte(kIdLookup106D6 + idx));
    }

    const m_byte id = cpu().db(7);
    std::printf("[sound] queue_sound_id $%02X\n", static_cast<unsigned>(id));

    for (int i = 0; i < 3; ++i) {
        if (mem.readByte(kPlaySe0 + i) == id) {
            cpu().ssp += 4;
            return;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (mem.readByte(kPlaySe0 + i) == 0) {
            mem.writeByte(kPlaySe0 + i, id);
            break;
        }
    }
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $011B12 — post the current level's BGM (or alternate IDs under flags)
// ---------------------------------------------------------------------------
void Sor::play_level_music(m_long /*entry_*/) {
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
