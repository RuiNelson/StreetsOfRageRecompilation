#include "SoRCpuOps.hpp"

using namespace sorops;

// ---------------------------------------------------------------------------
// The enemy-object routines the hand-written ports call: the type test ($9350), the walk toward a point
// ($9604 and the vector-to-velocity conversion it uses), the attacker lookup ($AA34, $AA22, $A9BA) and a few
// small helpers. Each is the ROM routine as it is, registers and condition codes included, so that a
// manual caller never reaches into generated code.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kDifficulty         = 0xFFFFFFC6u; // difficulty_ (W)
constexpr m_long kPlayerMode         = 0xFFFFFF18u; // player_mode_ (B)
constexpr m_long kCamX               = 0xFFFFE002u; // cam_x_ (W, the integer part)
constexpr m_long kCameraY            = 0xFFFFE00Eu; // camera_y_ (L; the integer part is the word at its address)
constexpr m_long kRegisteredAttackers = 0xFFFFFB22u; // a count word, then up to five object pointers
constexpr m_long kP1Object           = 0xFFFFB800u;
constexpr m_long kP2Object           = 0xFFFFB880u;
constexpr m_long kPaletteFadeCounter = 0xFFFFFB0Cu;
constexpr m_long kPaletteDirty       = 0xFFFFFA01u;
constexpr m_long kPalette            = 0xFFFFF400u;
constexpr m_long kTargetPalette      = 0xFFFFF480u;
constexpr m_long kFadeSteps          = 0x000105B8u; // three words per fade phase
constexpr m_long kDirectionTable     = 0x0002705Eu; // two words (cos, sin of the vector's slope) per step
constexpr m_long kNonordinaryTypes   = 0x0000936Eu; // a byte per type from $20
constexpr m_long kAttackerReactions  = 0x0000A9C8u; // word offsets, by contact result

m_long swapWords(m_long value) {
    return (value << 16) | (value >> 16);
}

} // namespace

// ---------------------------------------------------------------------------
// $009350 (is_nonordinary_enemy_type): d0 = type; d0 = 0 for an ordinary enemy (types $20-$2A the table
// at $936E marks 0), else 1.
// ---------------------------------------------------------------------------
void StreetsOfRage::is_nonordinary_enemy_type(m_long /*entry_*/) {
    traceEnter(0x00009350u);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDw(0, static_cast<m_word>(c.dw(0) & 0x00FFu));
    tst(c, c.dw(0), 16);
    c.setDb(0, static_cast<m_byte>(subValue(c, c.db(0), 0x20u, 8)));
    bool ordinary = false;
    if (!c.flagN()) {
        cmpValue(c, c.db(0), 0x0Bu, 8);
        if (c.flagC()) { // inside the table
            c.setDb(0, mem.readByte(kNonordinaryTypes + sext16(c.dw(0))));
            tst(c, c.db(0), 8);
            ordinary = c.db(0) == 0;
        }
    }
    c.d[0] = ordinary ? 0u : 1u;
    tst(c, c.d[0], 32);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $0096C0: face the target (+$42) and start animation d0: bit 1 of d0 = the target is to the left, the
// word goes to +$08, the animation set (+$04) starts it. Z = the object faces right (+$09 bit 1 clear).
// Entered at $96C4 with a5 already the target.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_0096c0(m_long entry_) {
    auto &c   = cpu();
    auto &mem = memory();

    if (entry_ != 0x000096C4u) {
        traceEnter(0x000096C0u);
        c.a[5] = sext16(mem.readWord(c.a[0] + 0x42u));
    } else {
        traceEnter(0x000096C4u);
    }
    c.setDw(6, mem.readWord(c.a[5] + 0x10u));
    cmpValue(c, c.dw(6), mem.readWord(c.a[0] + 0x10u), 16);
    if (lessSigned16(mem.readWord(c.a[0] + 0x10u), c.dw(6)))
        c.d[0] &= ~2u;
    else
        c.d[0] |= 2u;
    mem.writeWord(c.a[0] + 0x08u, c.dw(0));
    tst(c, c.dw(0), 16);
    c.a[1] = mem.readLong(c.a[0] + 0x04u);
    SOR_CALL(init_object_animation_frame, 0x000096E4u, 0x0000B1A2u);
    testBitMem(c, mem, c.a[0] + 0x09u, 1);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00982C (ordinary_enemy_vector_to_velocity): the target vector (d0 - d4 in X, d1 - d5 in lane) and the
// speed d6 become a velocity pair in d0 (X) and d1 (lane), 16.16. Below Normal difficulty a speed of $300
// or more is cut by $100.
// ---------------------------------------------------------------------------
void StreetsOfRage::ordinary_enemy_vector_to_velocity(m_long /*entry_*/) {
    traceEnter(0x0000982Cu);
    auto &c   = cpu();
    auto &mem = memory();

    const m_word difficulty = mem.readWord(kDifficulty);
    tst(c, difficulty, 16);
    if (difficulty == 0) {
        cmpValue(c, c.dw(6), 0x0300u, 16);
        if (!c.flagC())
            c.setDw(6, static_cast<m_word>(subValue(c, c.dw(6), 0x0100u, 16)));
    }
    SOR_CALL(sub_009842, 0x0000983Eu, 0x00009842u);
    sub_009896();
}

// ---------------------------------------------------------------------------
// $009842: the vector's octant and slope. d2 bits: 0 = X was negative, 1 = lane was negative, 2 = the
// lane is the longer side; d0 and d1 end as the absolute components (d0 the longer), d1 the slope index
// 0-$3F: (shorter << 6) / longer, clamped.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_009842(m_long /*entry_*/) {
    traceEnter(0x00009842u);
    auto &c = cpu();

    c.d[2] = 0;
    tst(c, 0, 32);
    c.setDw(3, c.dw(0));
    c.setDw(3, static_cast<m_word>(c.dw(3) ^ c.dw(4)));
    tst(c, c.dw(3), 16);
    c.setDw(0, static_cast<m_word>(subValue(c, c.dw(0), c.dw(4), 16)));
    const bool borrow = c.flagC();
    c.setDw(3, static_cast<m_word>(c.dw(3) & 0x8000u));
    tst(c, c.dw(3), 16);
    const bool signsDiffer = c.dw(3) != 0;
    if (borrow != signsDiffer) {
        c.d[2] |= 1u;
        c.setDw(0, static_cast<m_word>(negValue(c, c.dw(0), 16)));
    }

    c.setDw(1, static_cast<m_word>(subValue(c, c.dw(1), c.dw(5), 16)));
    if (c.flagN()) {
        tst(c, c.dw(5), 16);
        if (!c.flagN()) {
            c.d[2] |= 2u;
            c.setDw(1, static_cast<m_word>(negValue(c, c.dw(1), 16)));
        }
    }

    cmpValue(c, c.dw(1), c.dw(0), 16);
    if (!c.flagC()) {
        c.d[2] |= 4u;
        const m_long saved = c.d[0];
        c.d[0]             = c.d[1];
        c.d[1]             = saved;
    }

    c.d[1] = sext16(c.dw(1));
    tst(c, c.d[1], 32);
    c.d[1] = lslValue(c, c.d[1], 6, 32);
    tst(c, c.dw(0), 16);
    if (c.dw(0) != 0) {
        const m_long dividend = c.d[1];
        const m_long divisor  = c.dw(0);
        const m_long quotient = dividend / divisor;
        if (quotient <= 0xFFFFu) {
            c.d[1] = ((dividend % divisor) << 16) | quotient;
            tst(c, quotient, 16);
        } else {
            c.setCCRFlags(CPU68K::FlagN | CPU68K::FlagZ | CPU68K::FlagV | CPU68K::FlagC, CPU68K::FlagN | CPU68K::FlagV);
        }
    } else {
        c.d[1] = 0;
        tst(c, 0, 32);
    }
    c.setDw(1, static_cast<m_word>(c.dw(1) & 0x00FFu));
    tst(c, c.dw(1), 16);
    cmpValue(c, c.db(1), 0x40u, 8);
    if (!c.flagC()) {
        c.setDb(1, 0x3Fu);
        tst(c, 0x3Fu, 8);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $009896: the slope index in d1 and the speed in d6 become the two velocity components: the table at
// $2705E gives the pair (cos, sin) as fractions, scaled by d6, swapped when the lane was the longer side
// (d2 bit 2) and negated by d2 bits 1 and 0. d0 = X, d1 = lane, as 16.16 divided by 256.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_009896(m_long /*entry_*/) {
    traceEnter(0x00009896u);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[2] = kDirectionTable;
    c.setDw(1, static_cast<m_word>(c.dw(1) & 0x00FFu));
    tst(c, c.dw(1), 16);
    c.setDw(1, static_cast<m_word>(addValue(c, c.dw(1), c.dw(1), 16)));
    c.setDw(1, static_cast<m_word>(addValue(c, c.dw(1), c.dw(1), 16)));
    if (c.dw(1) == 0) {
        c.setDw(0, 0);
        tst(c, 0, 16);
        c.setDw(1, c.dw(6));
        tst(c, c.dw(6), 16);
    } else {
        c.a[2] += sext16(c.dw(1));
        c.setDw(0, mem.readWord(c.a[2]));
        c.a[2] += 2;
        c.setDw(1, mem.readWord(c.a[2]));
        c.d[0] = static_cast<m_long>(c.dw(0)) * c.dw(6);
        tst(c, c.d[0], 32);
        c.d[0] = swapWords(c.d[0]);
        tst(c, c.d[0], 32);
        c.d[1] = static_cast<m_long>(c.dw(1)) * c.dw(6);
        tst(c, c.d[1], 32);
        c.d[1] = swapWords(c.d[1]);
        tst(c, c.d[1], 32);
    }
    if (c.d[2] & 4u) {
        const m_long saved = c.d[0];
        c.d[0]             = c.d[1];
        c.d[1]             = saved;
    }
    if (c.d[2] & 2u)
        c.setDw(0, static_cast<m_word>(negValue(c, c.dw(0), 16)));
    c.d[0] = swapWords(c.d[0]);
    c.setDw(0, 0);
    c.d[0] = asrValue(c, c.d[0], 8, 32);
    if (c.d[2] & 1u)
        c.setDw(1, static_cast<m_word>(negValue(c, c.dw(1), 16)));
    c.d[1] = swapWords(c.d[1]);
    c.setDw(1, 0);
    c.d[1] = asrValue(c, c.d[1], 8, 32);
    const m_long saved = c.d[0];
    c.d[0]             = c.d[1];
    c.d[1]             = saved;
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $009604 (ordinary_enemy_approach_point): move toward the point in +$60 (X) and +$62 (lane) at the speed in
// +$64: d7 = 1 when within 4 of it on both axes, else the velocities +$1C / +$20 are set for it and d7 = 0.
// ---------------------------------------------------------------------------
void StreetsOfRage::ordinary_enemy_approach_point(m_long /*entry_*/) {
    traceEnter(0x00009604u);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDw(0, mem.readWord(c.a[0] + 0x60u));
    c.setDw(1, mem.readWord(c.a[0] + 0x62u));
    c.setDw(2, c.dw(0));
    c.setDw(3, c.dw(1));
    c.setDw(4, mem.readWord(c.a[0] + 0x10u));
    c.setDw(5, mem.readWord(c.a[0] + 0x14u));

    bool arrived = false;
    c.setDw(2, static_cast<m_word>(subValue(c, c.dw(2), c.dw(4), 16)));
    if (c.flagN())
        c.setDw(2, static_cast<m_word>(negValue(c, c.dw(2), 16)));
    cmpValue(c, c.dw(2), 4u, 16);
    if (c.flagC()) {
        c.setDw(3, static_cast<m_word>(subValue(c, c.dw(3), c.dw(5), 16)));
        if (c.flagN())
            c.setDw(3, static_cast<m_word>(negValue(c, c.dw(3), 16)));
        cmpValue(c, c.dw(3), 4u, 16);
        arrived = c.flagC();
    }
    if (arrived) {
        c.d[7] = 1;
        tst(c, 1, 32);
        c.ssp += 4;
        return;
    }
    c.setDw(6, mem.readWord(c.a[0] + 0x64u));
    SOR_CALL(ordinary_enemy_vector_to_velocity, 0x00009638u, 0x0000982Cu);
    mem.writeLong(c.a[0] + 0x1Cu, c.d[0]);
    tst(c, c.d[0], 32);
    mem.writeLong(c.a[0] + 0x20u, c.d[1]);
    tst(c, c.d[1], 32);
    c.d[7] = 0;
    tst(c, 0, 32);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00A56C: the object's screen position (+$28 = X + $80 - camera X, +$2C = height + lane/2 + $80 - camera
// Y) unless it is flagged (+$01 bit 1) to keep its own.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_00a56c(m_long /*entry_*/) {
    traceEnter(0x0000A56Cu);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDw(2, mem.readWord(c.a[0] + 0x10u));
    c.setDw(3, mem.readWord(c.a[0] + 0x14u));
    c.setDw(3, static_cast<m_word>(asrValue(c, c.dw(3), 1, 16)));
    c.setDw(3, static_cast<m_word>(addValue(c, c.dw(3), mem.readWord(c.a[0] + 0x18u), 16)));
    if (!testBitMem(c, mem, c.a[0] + 0x01u, 1)) {
        c.setDw(2, static_cast<m_word>(addValue(c, c.dw(2), 0x80u, 16)));
        c.setDw(3, static_cast<m_word>(addValue(c, c.dw(3), 0x80u, 16)));
        c.setDw(2, static_cast<m_word>(subValue(c, c.dw(2), mem.readWord(kCamX), 16)));
        c.setDw(3, static_cast<m_word>(subValue(c, c.dw(3), mem.readWord(kCameraY), 16)));
        mem.writeWord(c.a[0] + 0x28u, c.dw(2));
        tst(c, c.dw(2), 16);
        mem.writeWord(c.a[0] + 0x2Cu, c.dw(3));
        tst(c, c.dw(3), 16);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00AA34: which object attacks a0 now? The registered attackers (a count word and up to five pointers at
// $FFFB22) are tried through $ABA4, then each active player (not in +$59 bit 1, not hit already in +$7C
// bit 0) through $AAA0. d7 = the contact result and a1 = the attacker; d7 = 0 and Z for none.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_00aa34(m_long /*entry_*/) {
    traceEnter(0x0000AA34u);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[4] = kRegisteredAttackers;
    c.setDw(3, mem.readWord(c.a[4]));
    c.a[4] += 2;
    tst(c, c.dw(3), 16);
    if (c.dw(3) != 0) {
        c.d[4] = 4;
        for (;;) {
            c.setDw(0, mem.readWord(c.a[4]));
            c.a[4] += 2;
            tst(c, c.dw(0), 16);
            if (c.dw(0) != 0) {
                c.a[1] = sext16(c.dw(0));
                cmpValue(c, c.a[0], sext16(static_cast<m_word>(c.a[1])), 32);
                if (!c.flagZ()) {
                    SOR_CALL(resolve_enemy_registered_attacker_contact, 0x0000AA4Cu, 0x0000ABA4u);
                    if (!c.flagZ())
                        goto hit;
                }
                c.setDb(3, static_cast<m_byte>(subValue(c, c.db(3), 1u, 8)));
                if (c.flagZ())
                    break;
            }
            c.setDw(4, static_cast<m_word>(c.dw(4) - 1u));
            if (c.dw(4) == 0xFFFFu)
                break;
        }
    }

    {
        const m_byte mode = mem.readByte(kPlayerMode);
        c.setFlag(CPU68K::FlagZ, (mode & 1u) == 0);
        if (mode & 1u) {
            c.a[1] = kP1Object;
            if (!testBitMem(c, mem, c.a[1] + 0x59u, 1)) {
                if (testBitMem(c, mem, c.a[1] + 0x7Cu, 0))
                    goto none;
                SOR_CALL(resolve_enemy_player_contact, 0x0000AA76u, 0x0000AAA0u);
                if (!c.flagZ())
                    goto hit;
            }
        }
        c.setFlag(CPU68K::FlagZ, (mode & 2u) == 0);
        if (!(mode & 2u))
            goto none;
        c.a[1] = kP2Object;
        if (testBitMem(c, mem, c.a[1] + 0x59u, 1))
            goto none;
        if (testBitMem(c, mem, c.a[1] + 0x7Cu, 0))
            goto none;
        SOR_CALL(resolve_enemy_player_contact, 0x0000AA96u, 0x0000AAA0u);
        if (c.flagZ()) {
            c.ssp += 4;
            return;
        }
    }
hit:
    tst(c, c.db(7), 8);
    c.ssp += 4;
    return;
none:
    c.d[7] = 0;
    tst(c, 0, 32);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00AA22: $AA34, and on a contact the pair is linked: +$3E of the object = the attacker, +$7E of the
// attacker = the object, +$7C of the attacker = the contact result.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_00aa22(m_long /*entry_*/) {
    traceEnter(0x0000AA22u);
    auto &c   = cpu();
    auto &mem = memory();

    SOR_CALL(sub_00aa34, 0x0000AA24u, 0x0000AA34u);
    if (!c.flagZ()) {
        mem.writeWord(c.a[0] + 0x3Eu, static_cast<m_word>(c.a[1]));
        tst(c, static_cast<m_word>(c.a[1]), 16);
        mem.writeWord(c.a[1] + 0x7Eu, static_cast<m_word>(c.a[0]));
        tst(c, static_cast<m_word>(c.a[0]), 16);
        mem.writeByte(c.a[1] + 0x7Cu, c.db(7));
        tst(c, c.db(7), 8);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00A9BA: $AA22, then the reaction to the contact result d7 (1 grabbed, 2 struck, 3 hit hard, 4 and 5 hit
// by a weapon or a body): the object's state (+$30) is set, health (+$32) taken, and d7 = -1 (1 for a
// grab, whose attacker's +$7D is cleared and moved to d5).
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_00a9ba(m_long /*entry_*/) {
    traceEnter(0x0000A9BAu);
    auto &c   = cpu();
    auto &mem = memory();

    SOR_CALL(sub_00aa22, 0x0000A9BCu, 0x0000AA22u);
    if (c.flagZ()) {
        c.ssp += 4;
        return;
    }
    c.setDw(7, static_cast<m_word>(addValue(c, c.dw(7), c.dw(7), 16)));
    c.setDw(7, mem.readWord(kAttackerReactions + sext16(c.dw(7))));
    const m_long target = kAttackerReactions + sext16(c.dw(7));
    const auto setState = [&](m_word state) {
        mem.writeWord(c.a[0] + 0x30u, state);
        tst(c, state, 16);
    };
    switch (target) {
    case 0x0000AA20u:
        c.ssp += 4;
        return;
    case 0x0000AA14u:
        c.setDb(5, mem.readByte(c.a[1] + 0x7Du));
        tst(c, c.db(5), 8);
        mem.writeByte(c.a[1] + 0x7Du, 0);
        tst(c, 0, 8);
        c.d[7] = 1;
        tst(c, 1, 32);
        c.ssp += 4;
        return;
    case 0x0000AA02u:
        setState(0x0200u);
        break;
    case 0x0000A9D4u:
        setState(0x0500u);
        break;
    case 0x0000A9DCu: {
        cmpValue(c, mem.readByte(c.a[1]), 0x0Cu, 8);
        if (c.flagZ()) {
            setState(0x0400u);
            break;
        }
        setState(0x0300u);
        c.d[0] = 0;
        tst(c, 0, 32);
        cmpValue(c, c.a[1], sext16(0xC900u), 32);
        if (c.flagC()) {
            mem.writeWord(c.a[0] + 0x32u, static_cast<m_word>(subValue(c, mem.readWord(c.a[0] + 0x32u), 2u, 16)));
        } else {
            c.setDb(0, mem.readByte(c.a[1] + 0x34u));
            tst(c, c.db(0), 8);
            mem.writeWord(c.a[0] + 0x32u, static_cast<m_word>(subValue(c, mem.readWord(c.a[0] + 0x32u), c.dw(0), 16)));
        }
        break;
    }
    default:
        unhandledDispatch(target);
        return;
    }
    c.d[7] = 0xFFFFFFFFu;
    tst(c, c.d[7], 32);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00F482: d7 = -1 and the object's state (+$30) = $0700 when its target (+$42) is within $40 of it in X,
// else d7 = 0.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_00f482(m_long /*entry_*/) {
    traceEnter(0x0000F482u);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[1] = sext16(mem.readWord(c.a[0] + 0x42u));
    c.setDw(0, mem.readWord(c.a[1] + 0x10u));
    c.setDw(0, static_cast<m_word>(subValue(c, c.dw(0), mem.readWord(c.a[0] + 0x10u), 16)));
    if (c.flagN())
        c.setDw(0, static_cast<m_word>(negValue(c, c.dw(0), 16)));
    cmpValue(c, c.dw(0), 0x40u, 16);
    if (c.flagC()) {
        mem.writeWord(c.a[0] + 0x30u, 0x0700u);
        tst(c, 0x0700u, 16);
        c.d[7] = 0xFFFFFFFFu;
        tst(c, c.d[7], 32);
    } else {
        c.d[7] = 0;
        tst(c, 0, 32);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $0105CE: one step of the palette fade (phase = the counter at $FFFB0C, low two bits): a step table of
// three words at $105B8 per phase; when the first is not negative every colour of the active palette moves
// toward the target palette by the step (d6 + 1 colours). The counter is decremented.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_0105ce(m_long /*entry_*/) {
    traceEnter(0x000105CEu);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDw(7, mem.readWord(kPaletteFadeCounter));
    tst(c, c.dw(7), 16);
    c.setDb(7, static_cast<m_byte>(c.db(7) & 3u));
    tst(c, c.db(7), 8);
    c.d[7] = static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(c.dw(7))) * 6);
    tst(c, c.d[7], 32);
    const m_long step = kFadeSteps + sext16(c.dw(7));
    c.setDw(0, mem.readWord(step));
    tst(c, c.dw(0), 16);
    if (!c.flagN()) {
        c.a[1] = kPalette;
        c.a[2] = kTargetPalette;
        for (;;) {
            c.setDw(4, mem.readWord(c.a[2]));
            c.setDw(3, mem.readWord(c.a[1]));
            c.setDw(5, c.dw(3));
            c.setDw(3, static_cast<m_word>(c.dw(3) & mem.readWord(step + 2u)));
            c.setDw(4, static_cast<m_word>(c.dw(4) & mem.readWord(step + 2u)));
            c.setDw(3, static_cast<m_word>(addValue(c, c.dw(3), mem.readWord(step), 16)));
            cmpValue(c, c.dw(4), c.dw(3), 16);
            if (!c.flagC()) {
                c.setDw(5, static_cast<m_word>(c.dw(5) & mem.readWord(step + 4u)));
                c.setDw(3, static_cast<m_word>(c.dw(3) | c.dw(5)));
                mem.writeWord(c.a[1], c.dw(3));
            }
            c.a[1] += 2;
            c.a[2] += 2;
            c.setDw(6, static_cast<m_word>(c.dw(6) - 1u));
            if (c.dw(6) == 0xFFFFu)
                break;
        }
        mem.writeByte(kPaletteDirty, 1);
    }
    mem.writeWord(kPaletteFadeCounter, static_cast<m_word>(subValue(c, mem.readWord(kPaletteFadeCounter), 1u, 16)));
    c.ssp += 4;
}
