#include "SoRCpuOps.hpp"

using namespace sorops;

// ---------------------------------------------------------------------------
// The routines of the "bespoke" bosses (Abadede and kin: types that move with a pair object at +$50 and a
// target at +$5C) that the hand-written ports call: the target's signs and range ($12A4E, $12A78), the
// contact reaction ($154E0 and the four it dispatches to), the bounce ($1566E), the pair's follow ($1569C)
// and its animation ($1572A, $1573C, $1577C). Each is the ROM routine as it is, registers and condition
// codes included, so that a manual caller never reaches into generated code.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kReactionJumpBase  = 0x00015504u; // word offsets by contact result - 1
constexpr m_long kReactionNextState = 0x000155B2u; // byte per +$7D of the attacker, -1 for none
constexpr m_long kPairOffsets       = 0x00035054u; // two words (X, height) per pair pose
constexpr m_word kHitSound          = 0xFFB2u;

m_long swapWords(m_long value) {
    return (value << 16) | (value >> 16);
}

} // namespace

// ---------------------------------------------------------------------------
// $012A4E (bespoke_boss_target_signs): d7 = the lane sign in the high word and the X sign in the low word
// of the target (+$5C) relative to the object: 1 for ahead or level, $FFFF for behind.
// ---------------------------------------------------------------------------
void StreetsOfRage::bespoke_boss_target_signs(m_long /*entry_*/) {
    traceEnter(0x00012A4Eu);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[1] = sext16(mem.readWord(c.a[0] + 0x5Cu));
    c.d[7] = 0x00010001u;
    c.d[0] = subValue(c, mem.readLong(c.a[1] + 0x14u), mem.readLong(c.a[0] + 0x14u), 32);
    if (c.flagN()) {
        c.setDw(7, 0xFFFFu);
        tst(c, 0xFFFFu, 16);
        c.d[7] = swapWords(c.d[7]);
        tst(c, c.d[7], 32);
    }
    c.d[0] = subValue(c, mem.readLong(c.a[1] + 0x10u), mem.readLong(c.a[0] + 0x10u), 32);
    if (c.flagN()) {
        c.setDw(7, 0xFFFFu);
        tst(c, 0xFFFFu, 16);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $012A78 (bespoke_boss_range_test): d0 = the lane gate (high word) and the X gate (low word); d7 bit 0 =
// the target (+$5C) is outside the lane gate, bit 1 = outside the X gate; d0 ends with the X distance.
// ---------------------------------------------------------------------------
void StreetsOfRage::bespoke_boss_range_test(m_long /*entry_*/) {
    traceEnter(0x00012A78u);
    auto &c   = cpu();
    auto &mem = memory();

    c.d[7] = 0;
    tst(c, 0, 32);
    c.setDw(1, c.dw(0));
    tst(c, c.dw(1), 16);
    c.d[0] = swapWords(c.d[0]);
    tst(c, c.d[0], 32);
    c.a[1] = sext16(mem.readWord(c.a[0] + 0x5Cu));

    c.setDw(2, mem.readWord(c.a[1] + 0x14u));
    c.setDw(2, static_cast<m_word>(subValue(c, c.dw(2), mem.readWord(c.a[0] + 0x14u), 16)));
    if (c.flagN())
        c.setDw(2, static_cast<m_word>(negValue(c, c.dw(2), 16)));
    cmpValue(c, c.dw(2), c.dw(0), 16);
    if (!c.flagN()) {
        c.d[7] = 1;
        tst(c, 1, 32);
    }
    c.setDw(0, c.dw(2));
    tst(c, c.dw(2), 16);
    c.d[0] = swapWords(c.d[0]);
    tst(c, c.d[0], 32);

    c.setDw(2, mem.readWord(c.a[1] + 0x10u));
    c.setDw(2, static_cast<m_word>(subValue(c, c.dw(2), mem.readWord(c.a[0] + 0x10u), 16)));
    if (c.flagN())
        c.setDw(2, static_cast<m_word>(negValue(c, c.dw(2), 16)));
    cmpValue(c, c.dw(2), c.dw(1), 16);
    if (!c.flagN())
        c.setDb(7, static_cast<m_byte>(addValue(c, c.db(7), 2u, 8)));
    c.setDw(0, c.dw(2));
    tst(c, c.dw(2), 16);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $0156E0: the object's facing (+$09 bit 1) and its pair's (+$50) follow the direction of the X velocity
// (+$1C): set when it is negative.
// ---------------------------------------------------------------------------
void StreetsOfRage::abadede_sync_facing_from_velocity(m_long /*entry_*/) {
    traceEnter(0x000156E0u);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[1] = sext16(mem.readWord(c.a[0] + 0x50u));
    clearBitMem(c, mem, c.a[0] + 0x09u, 1);
    clearBitMem(c, mem, c.a[1] + 0x09u, 1);
    const m_word velocity = mem.readWord(c.a[0] + 0x1Cu);
    tst(c, velocity, 16);
    if (c.flagN()) {
        setBitMem(c, mem, c.a[0] + 0x09u, 1);
        setBitMem(c, mem, c.a[1] + 0x09u, 1);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $01566E: bounce toward the target: each velocity (+$1C X, +$20 lane) is negated when its sign differs from
// the target's side ($12A4E); a flipped X also turns the facing ($156E0).
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_01566e(m_long /*entry_*/) {
    traceEnter(0x0001566Eu);
    auto &c   = cpu();
    auto &mem = memory();

    SOR_CALL(bespoke_boss_target_signs, 0x00015672u, 0x00012A4Eu);
    c.setDw(6, c.dw(7));
    tst(c, c.dw(6), 16);
    c.d[7] = swapWords(c.d[7]);
    tst(c, c.d[7], 32);
    c.setDw(0, mem.readWord(c.a[0] + 0x1Cu));
    tst(c, c.dw(0), 16);
    c.setDw(1, mem.readWord(c.a[0] + 0x20u));
    tst(c, c.dw(1), 16);
    c.setDw(0, static_cast<m_word>(c.dw(0) ^ c.dw(6)));
    tst(c, c.dw(0), 16);
    c.setFlag(CPU68K::FlagZ, (c.dw(0) & 0x8000u) == 0);
    if (c.dw(0) & 0x8000u) {
        mem.writeLong(c.a[0] + 0x1Cu, negValue(c, mem.readLong(c.a[0] + 0x1Cu), 32));
        SOR_CALL(abadede_sync_facing_from_velocity, 0x0001568Eu, 0x000156E0u);
    }
    c.setDw(1, static_cast<m_word>(c.dw(1) ^ c.dw(7)));
    tst(c, c.dw(1), 16);
    c.setFlag(CPU68K::FlagZ, (c.dw(1) & 0x8000u) == 0);
    if (c.dw(1) & 0x8000u)
        mem.writeLong(c.a[0] + 0x20u, negValue(c, mem.readLong(c.a[0] + 0x20u), 32));
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $01569C: the pair object (+$50) takes the object's position, offset by pose d0 of the table at $35054
// (X, then height).
// ---------------------------------------------------------------------------
void StreetsOfRage::abadede_sync_linked_sprite(m_long /*entry_*/) {
    traceEnter(0x0001569Cu);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[1] = sext16(mem.readWord(c.a[0] + 0x50u));
    mem.writeLong(c.a[1] + 0x10u, mem.readLong(c.a[0] + 0x10u));
    tst(c, mem.readLong(c.a[0] + 0x10u), 32);
    mem.writeLong(c.a[1] + 0x14u, mem.readLong(c.a[0] + 0x14u));
    tst(c, mem.readLong(c.a[0] + 0x14u), 32);
    mem.writeLong(c.a[1] + 0x18u, mem.readLong(c.a[0] + 0x18u));
    tst(c, mem.readLong(c.a[0] + 0x18u), 32);
    c.a[2] = kPairOffsets;
    c.setDw(0, static_cast<m_word>(lslValue(c, c.dw(0), 2, 16)));
    c.setDw(1, mem.readWord(c.a[2] + sext16(c.dw(0))));
    tst(c, c.dw(1), 16);
    mem.writeWord(c.a[1] + 0x10u, static_cast<m_word>(addValue(c, mem.readWord(c.a[1] + 0x10u), c.dw(1), 16)));
    c.setDw(1, mem.readWord(c.a[2] + 2u + sext16(c.dw(0))));
    tst(c, c.dw(1), 16);
    mem.writeWord(c.a[1] + 0x18u, static_cast<m_word>(addValue(c, mem.readWord(c.a[1] + 0x18u), c.dw(1), 16)));
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $01572A: animation set a1 at timer d1 for the object, then its facing ($156E0).
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_01572a(m_long /*entry_*/) {
    traceEnter(0x0001572Au);
    auto &c   = cpu();
    auto &mem = memory();

    mem.writeWord(c.a[0] + 0x0Eu, c.dw(1));
    tst(c, c.dw(1), 16);
    c.a[1] = mem.readLong(c.a[0] + 0x04u);
    tst(c, c.a[1], 32);
    SOR_CALL(init_object_animation_frame, 0x00015738u, 0x0000B1A2u);
    abadede_sync_facing_from_velocity();
}

// ---------------------------------------------------------------------------
// $01573C: start animation 6 of the object (timer $225C), then the pair (+$50) on animation 2 with the
// flag bits of a following piece, state 1, and the pair placed ($1569C with d0 = 0).
// ---------------------------------------------------------------------------
void StreetsOfRage::abadede_init_linked_sprite_pose(m_long /*entry_*/) {
    traceEnter(0x0001573Cu);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDw(1, 0x225Cu);
    tst(c, 0x225Cu, 16);
    c.d[0] = 6;
    tst(c, 6, 32);
    SOR_CALL(sub_01572a, 0x00015746u, 0x0001572Au);
    c.ssp -= 4;
    mem.writeLong(c.ssp, c.a[0]);
    c.a[0] = sext16(mem.readWord(c.a[0] + 0x50u));
    mem.writeByte(c.a[0] + 0x01u, static_cast<m_byte>(mem.readByte(c.a[0] + 0x01u) & 0xFEu));
    mem.writeByte(c.a[0] + 0x01u, static_cast<m_byte>(mem.readByte(c.a[0] + 0x01u) | 0x0Cu));
    c.d[0] = 2;
    tst(c, 2, 32);
    mem.writeWord(c.a[0] + 0x0Eu, 0x225Cu);
    tst(c, 0x225Cu, 16);
    c.a[1] = mem.readLong(c.a[0] + 0x04u);
    tst(c, c.a[1], 32);
    SOR_CALL(init_object_animation_frame, 0x0001576Cu, 0x0000B1A2u);
    mem.writeByte(c.a[0] + 0x30u, 1);
    tst(c, 1, 8);
    c.a[0] = mem.readLong(c.ssp);
    c.ssp += 4;
    c.d[0] = 0;
    tst(c, 0, 32);
    abadede_sync_linked_sprite();
}

// ---------------------------------------------------------------------------
// $01577C: the pair's animation start above, both velocities $6.0000, then the bounce.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_01577c(m_long /*entry_*/) {
    traceEnter(0x0001577Cu);
    auto &c   = cpu();
    auto &mem = memory();

    SOR_CALL(abadede_init_linked_sprite_pose, 0x00015780u, 0x0001573Cu);
    mem.writeLong(c.a[0] + 0x20u, 0x00060000u);
    tst(c, 0x00060000u, 32);
    mem.writeLong(c.a[0] + 0x1Cu, 0x00060000u);
    tst(c, 0x00060000u, 32);
    sub_01566e();
}

// ---------------------------------------------------------------------------
// $015604: the object's recoil: +$5E = $3000, height velocity -8, X velocity $3.8000, mirrored when the
// target (+$5C) faces left (+$09 bit 1).
// ---------------------------------------------------------------------------
void StreetsOfRage::abadede_start_knockback(m_long /*entry_*/) {
    traceEnter(0x00015604u);
    auto &c   = cpu();
    auto &mem = memory();

    mem.writeLong(c.a[0] + 0x5Eu, 0x00003000u);
    tst(c, 0x00003000u, 32);
    mem.writeLong(c.a[0] + 0x24u, 0xFFF80000u);
    tst(c, 0xFFF80000u, 32);
    mem.writeLong(c.a[0] + 0x1Cu, 0x00038000u);
    tst(c, 0x00038000u, 32);
    c.a[1] = sext16(mem.readWord(c.a[0] + 0x5Cu));
    if (testBitMem(c, mem, c.a[1] + 0x09u, 1)) {
        mem.writeLong(c.a[0] + 0x1Cu, negValue(c, mem.readLong(c.a[0] + 0x1Cu), 32));
        mem.writeLong(c.a[0] + 0x5Eu, negValue(c, mem.readLong(c.a[0] + 0x5Eu), 32));
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $015632: d0 taken from health (+$32); at zero or below the recoil starts in state 6 ($15604).
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_015632(m_long /*entry_*/) {
    traceEnter(0x00015632u);
    auto &c   = cpu();
    auto &mem = memory();

    mem.writeWord(c.a[0] + 0x32u, static_cast<m_word>(subValue(c, mem.readWord(c.a[0] + 0x32u), c.dw(0), 16)));
    const bool greater = !c.flagZ() && (c.flagN() == c.flagV());
    if (greater) {
        c.ssp += 4;
        return;
    }
    mem.writeByte(c.a[0] + 0x5Bu, 0);
    tst(c, 0, 8);
    mem.writeByte(c.a[0] + 0x30u, 6);
    tst(c, 6, 8);
    abadede_start_knockback();
}

// ---------------------------------------------------------------------------
// $01550E: the reaction to a weak contact. State 3 and state $0B clear the attacker's grab (+$7C; state 3
// also d7), state 7 asks it to hold (+$7D = 1), anything else knocks the object to state 8 and the
// attacker to grab 1 / follow-up 2.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_01550e(m_long /*entry_*/) {
    traceEnter(0x0001550Eu);
    auto &c   = cpu();
    auto &mem = memory();

    const m_byte state = mem.readByte(c.a[0] + 0x30u);
    cmpValue(c, state, 3u, 8);
    if (state == 3u) {
        c.d[7] = 0;
        tst(c, 0, 32);
        mem.writeByte(c.a[1] + 0x7Cu, 0);
        tst(c, 0, 8);
    } else {
        cmpValue(c, state, 0x0Bu, 8);
        if (state == 0x0Bu) {
            mem.writeByte(c.a[1] + 0x7Cu, 0);
            tst(c, 0, 8);
        } else {
            cmpValue(c, state, 7u, 8);
            if (state == 7u) {
                mem.writeByte(c.a[1] + 0x7Du, 1);
                tst(c, 1, 8);
            } else {
                clearBitMem(c, mem, c.a[0] + 0x37u, 2);
                mem.writeByte(c.a[0] + 0x5Bu, 0);
                mem.writeByte(c.a[0] + 0x30u, 8);
                mem.writeByte(c.a[1] + 0x7Cu, 1);
                mem.writeByte(c.a[1] + 0x7Du, 2);
                tst(c, 2, 8);
            }
        }
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $015560: the reaction to a hard contact: the hit sound, state 4 (state 6 and the recoil when +$37 bit 0
// was set), then the attacker's damage (+$34) from health through $15632.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_015560(m_long /*entry_*/) {
    traceEnter(0x00015560u);
    auto &c   = cpu();
    auto &mem = memory();

    c.ssp -= 4;
    mem.writeLong(c.ssp, c.d[7]);
    c.setDw(7, kHitSound);
    tst(c, kHitSound, 16);
    SOR_CALL(queue_sound_id, 0x0001556Eu, 0x0001069Eu);
    c.d[7] = mem.readLong(c.ssp);
    c.ssp += 4;
    mem.writeByte(c.a[0] + 0x30u, 4);
    tst(c, 4, 8);
    if (clearBitMem(c, mem, c.a[0] + 0x37u, 0)) {
        mem.writeByte(c.a[0] + 0x5Bu, 0);
        tst(c, 0, 8);
        mem.writeByte(c.a[0] + 0x30u, 6);
        tst(c, 6, 8);
        SOR_CALL(abadede_start_knockback, 0x00015590u, 0x00015604u);
    }
    c.d[0] = mem.readByte(c.a[1] + 0x34u);
    tst(c, c.db(0), 8);
    sub_015632();
}

// ---------------------------------------------------------------------------
// $01559A: the object's next state (+$30) from the table at $155B2 by the attacker's follow-up (+$7D), unless
// the table says -1.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_01559a(m_long /*entry_*/) {
    traceEnter(0x0001559Au);
    auto &c   = cpu();
    auto &mem = memory();

    c.d[0] = 0;
    tst(c, 0, 32);
    c.setDb(0, mem.readByte(c.a[1] + 0x7Du));
    tst(c, c.db(0), 8);
    c.setDb(0, mem.readByte(kReactionNextState + sext16(c.dw(0))));
    tst(c, c.db(0), 8);
    if (!c.flagN()) {
        mem.writeByte(c.a[0] + 0x30u, c.db(0));
        tst(c, c.db(0), 8);
        mem.writeByte(c.a[0] + 0x5Bu, 0);
        tst(c, 0, 8);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $0155BE: the reaction to a strong contact: the hit sound, the object's damage (the attacker's +$34, 0 for
// a type above $0C, 1 for a $0C) from health, state 6 and the recoil; a type $0C attacker gives state $0F
// instead.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_0155be(m_long /*entry_*/) {
    traceEnter(0x000155BEu);
    auto &c   = cpu();
    auto &mem = memory();

    c.ssp -= 4;
    mem.writeLong(c.ssp, c.d[7]);
    c.setDw(7, kHitSound);
    tst(c, kHitSound, 16);
    SOR_CALL(queue_sound_id, 0x000155CCu, 0x0001069Eu);
    c.d[7] = mem.readLong(c.ssp);
    c.ssp += 4;
    clearBitMem(c, mem, c.a[0] + 0x37u, 1);
    mem.writeByte(c.a[0] + 0x5Bu, 0);
    tst(c, 0, 8);
    c.d[0] = 1;
    tst(c, 1, 32);
    const m_byte type = mem.readByte(c.a[1]);
    cmpValue(c, type, 0x0Cu, 8);
    if (type == 0x0Cu) {
        mem.writeByte(c.a[0] + 0x30u, 0x0F);
        tst(c, 0x0F, 8);
        c.ssp += 4;
        return;
    }
    if (!(c.flagZ() == false && c.flagN() == c.flagV())) { // not greater (signed)
        c.d[0] = 0;
        tst(c, 0, 32);
        c.setDb(0, mem.readByte(c.a[1] + 0x34u));
        tst(c, c.db(0), 8);
    }
    mem.writeWord(c.a[0] + 0x32u, static_cast<m_word>(subValue(c, mem.readWord(c.a[0] + 0x32u), c.dw(0), 16)));
    mem.writeByte(c.a[0] + 0x30u, 6);
    tst(c, 6, 8);
    abadede_start_knockback();
}

// ---------------------------------------------------------------------------
// $0154E0 (abadede_contact_reaction): a contact on the object ($AA22); on a contact the reaction for result d7 (1-5)
// is run: $1550E, $15560, $1559A, $155BE twice. An attacker of type 1 also becomes the target (+$5C).
// ---------------------------------------------------------------------------
void StreetsOfRage::abadede_contact_reaction(m_long /*entry_*/) {
    traceEnter(0x000154E0u);
    auto &c   = cpu();
    auto &mem = memory();

    SOR_CALL(resolve_object_contact, 0x000154E6u, 0x0000AA22u);
    if (c.flagZ()) {
        c.ssp += 4;
        return;
    }
    cmpValue(c, mem.readByte(c.a[1]), 1u, 8);
    if (c.flagZ())
        mem.writeWord(c.a[0] + 0x5Cu, static_cast<m_word>(c.a[1]));
    c.setDw(5, c.dw(7));
    tst(c, c.dw(5), 16);
    c.setDb(5, static_cast<m_byte>(subValue(c, c.db(5), 1u, 8)));
    c.setDw(5, static_cast<m_word>(addValue(c, c.dw(5), c.dw(5), 16)));
    c.setDw(5, mem.readWord(kReactionJumpBase + sext16(c.dw(5))));
    const m_long target = kReactionJumpBase + sext16(c.dw(5));
    switch (target) {
    case 0x0001550Eu:
        sub_01550e();
        break;
    case 0x00015560u:
        sub_015560();
        break;
    case 0x0001559Au:
        sub_01559a();
        break;
    case 0x000155BEu:
        sub_0155be();
        break;
    default:
        unhandledDispatch(target);
        break;
    }
}
