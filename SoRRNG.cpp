#include "SoR.hpp"

#include <cstdint>

// The game's random number generator, reimplemented as native C++.
// The state is the long at $FFFF40 (work RAM, zeroed at boot), as the ROM keeps it; it is read and
// written there on every draw, never held in C++.

namespace {

constexpr m_long kRngState = 0xFFFFFF40u;
constexpr m_long kRngSeed  = 0x2A6D365Au; // the state a zero long is replaced by

} // namespace

// ---------------------------------------------------------------------------
// $0104D8 — draw: advance the state at $FFFF40 and return the draw in d0.w
//
// state = (state == 0 ? $2A6D365A : state); s = state * 41 (mod 2^32), the ROM's asl/add pairs;
// the new state is (lo(s) + hi(s)) << 16 | lo(s) (all 16-bit sums, wrapping), whose upper word is
// also returned in d0.w. d0's high word keeps the old state's, and d1 is saved and restored.
// Condition codes as left by the ROM: X is add.w's carry, N/Z come from the restored d1,
// V and C are clear.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_0104d8(m_long /*entry_*/) {
    traceEnter(0x000104D8u);
    auto &mem = memory();

    m_long state = mem.readLong(kRngState);
    if (state == 0)
        state = kRngSeed;

    const m_long scaled = state * 41u; // d1 = ((state << 2) + state) << 3, plus state
    const m_word low    = static_cast<m_word>(scaled);
    const m_long sum    = static_cast<m_long>(low) + static_cast<m_long>(scaled >> 16);
    const m_word draw   = static_cast<m_word>(sum);

    mem.writeLong(kRngState, (static_cast<m_long>(draw) << 16) | low);

    cpu().d[0] = (state & 0xFFFF0000u) | draw;
    cpu().setFlag(CPU68K::FlagX, (sum & 0x10000u) != 0);
    cpu().setNZClearVC(cpu().d[1], 0x80000000u);
    cpu().ssp += 4;
}
