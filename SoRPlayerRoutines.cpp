#include "SoRCpuOps.hpp"

using namespace sorops;

// ---------------------------------------------------------------------------
// The player-object routines the hand-written ports call: the action change ($2DE6 / $2EE8 and what they
// run), the box cache ($4104 / $4140 / $4192). Each is the ROM routine as it is, registers and condition
// codes included, so that a manual caller never reaches into generated code.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kFrameTimingTable    = 0x00003A30u; // player_frame_timing_table (word offsets to rows)
constexpr m_long kActionStateMask     = 0x0000394Eu; // byte per action/2: the +$58 bits an action keeps
constexpr m_long kActionAnimationRow  = 0x00002F2Cu; // byte per action: the animation set entry
constexpr m_long kActionSpeedTables   = 0x00003664u; // long per character (+$50): the action speed rows
constexpr m_long kWeaponStateJumpBase = 0x00002E5Au; // word offsets, $60 - 8
constexpr m_long kPlayerShapeTable    = 0x0001ABA8u;
constexpr m_long kLaneShapeTable      = 0x0001AB8Eu;
constexpr m_long kP1Object            = 0xFFFFB800u;

} // namespace

// ---------------------------------------------------------------------------
// $0035D0: jmp init_object_animation_frame
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_0035d0(m_long /*entry_*/) {
    traceEnter(0x000035D0u);
    init_object_animation_frame();
}

// ---------------------------------------------------------------------------
// $003A14 (player_load_frame_timer): +$0D = byte +$0A of the row the animation set (+$0C) selects.
// ---------------------------------------------------------------------------
void StreetsOfRage::player_load_frame_timer(m_long /*entry_*/) {
    traceEnter(0x00003A14u);
    auto &c   = cpu();
    auto &mem = memory();

    c.d[0] = mem.readByte(c.a[0] + 0x0Cu);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(0), 16)));
    c.a[2] = kFrameTimingTable;
    c.a[2] += sext16(mem.readWord(c.a[2] + sext16(c.dw(0))));
    c.setDb(0, mem.readByte(c.a[0] + 0x0Au));
    const m_byte value = mem.readByte(c.a[2] + sext16(c.dw(0)));
    mem.writeByte(c.a[0] + 0x0Du, value);
    tst(c, value, 8);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $003936 (player_action_change_mask_state_bits): +$51 = 1, and +$58 keeps only the bits the action's
// byte of the table at $394E allows.
// ---------------------------------------------------------------------------
void StreetsOfRage::player_action_change_mask_state_bits(m_long /*entry_*/) {
    traceEnter(0x00003936u);
    auto &c   = cpu();
    auto &mem = memory();

    mem.writeByte(c.a[0] + 0x51u, 1);
    c.d[7] = mem.readByte(c.a[0] + 0x30u);
    c.setDw(7, static_cast<m_word>(lsrValue(c, c.dw(7), 1, 16)));
    c.setDb(7, mem.readByte(kActionStateMask + sext16(c.dw(7))));
    const m_byte result = static_cast<m_byte>(mem.readByte(c.a[0] + 0x58u) & c.db(7));
    mem.writeByte(c.a[0] + 0x58u, result);
    tst(c, result, 8);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $003614: the action's start speeds. +$1C and +$20 get the signed bytes of the row for the action
// (character +$50, action +$30 without its facing bit), 16.16 divided by 16 and negated when facing left,
// unless +$58 bit 2 says one was set by hand (the bit is consumed).
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_003614(m_long /*entry_*/) {
    traceEnter(0x00003614u);
    auto &c   = cpu();
    auto &mem = memory();

    c.d[1] = mem.readByte(c.a[0] + 0x50u);
    c.setDw(1, static_cast<m_word>(lslValue(c, c.dw(1), 2, 16)));
    c.a[1] = mem.readLong(kActionSpeedTables + sext16(c.dw(1)));
    c.setDb(1, mem.readByte(c.a[0] + 0x30u));
    c.setDb(1, static_cast<m_byte>(c.db(1) & 0xFEu));
    tst(c, c.db(1), 8);

    // moveq #0,d2; move.b (row),d2; ext.w d2; swap.w d2; asr.l #4,d2
    const auto speed = [&](m_long offset) {
        c.d[2] = mem.readByte(c.a[1] + offset + sext16(c.dw(1)));
        c.setDw(2, sext8to16(c.db(2)));
        c.d[2] = (c.d[2] << 16) | (c.d[2] >> 16);
        c.d[2] = asrValue(c, c.d[2], 4, 32);
    };

    if (!clearBitMem(c, mem, c.a[0] + 0x58u, 2)) {
        speed(0);
        if (mem.readByte(c.a[0] + 0x30u) & 1u)
            c.d[2] = negValue(c, c.d[2], 32);
        mem.writeLong(c.a[0] + 0x1Cu, c.d[2]);
        tst(c, c.d[2], 32);
    }
    if (!clearBitMem(c, mem, c.a[0] + 0x58u, 2)) {
        speed(1);
        mem.writeLong(c.a[0] + 0x20u, c.d[2]);
        tst(c, c.d[2], 32);
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $002E32: while a player's action is $28 or $30 (word +$08, low bits masked) and holds a weapon state
// (+$60 = 8..12), a jump table at $2E5A picks a no-op ($2E64) or "+4 to the animation word" ($2E66).
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_002e32(m_long /*entry_*/) {
    traceEnter(0x00002E32u);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDw(1, mem.readWord(c.a[0] + 0x08u));
    c.setDb(1, static_cast<m_byte>(c.db(1) & 0xFCu));
    tst(c, c.db(1), 8);
    cmpValue(c, c.dw(1), 0x28u, 16);
    if (!c.flagZ()) {
        cmpValue(c, c.dw(1), 0x30u, 16);
        if (!c.flagZ()) {
            c.ssp += 4;
            return;
        }
    }
    c.setDb(1, mem.readByte(c.a[0] + 0x60u));
    tst(c, c.db(1), 8);
    if (c.db(1) == 0) {
        c.ssp += 4;
        return;
    }
    c.setDb(1, static_cast<m_byte>(subValue(c, c.db(1), 8, 8)));
    c.setDw(1, static_cast<m_word>(addValue(c, c.dw(1), c.dw(1), 16)));
    c.setDw(1, mem.readWord(kWeaponStateJumpBase + sext16(c.dw(1))));
    tst(c, c.dw(1), 16);
    const m_long target = kWeaponStateJumpBase + sext16(c.dw(1));
    if (target == 0x00002E64u) {
        c.ssp += 4;
    } else if (target == 0x00002E66u) {
        const m_word animation = static_cast<m_word>(addValue(c, mem.readWord(c.a[0] + 0x08u), 4, 16));
        mem.writeWord(c.a[0] + 0x08u, animation);
        c.ssp += 4;
    } else {
        unhandledDispatch(target);
    }
}

// ---------------------------------------------------------------------------
// $002DE6 / $002EE8: change the player's action to d0 plus its facing bit. A different action is stored in +$30 and gets its animation (the row of the table at
// $2F2C), frame timer, start speeds and state-bit mask; +$58 bit 1 copies the saved animation state
// back. $2DE6 then runs the weapon state of $2E32; $2EE8 just returns.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_002de6(m_long /*entry_*/) {
    traceEnter(0x00002DE6u);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDb(1, mem.readByte(c.a[0] + 0x30u));
    c.setDb(1, static_cast<m_byte>(c.db(1) & 1u));
    tst(c, c.db(1), 8);
    c.setDb(0, static_cast<m_byte>(addValue(c, c.db(0), c.db(1), 8)));

    c.setDb(1, mem.readByte(c.a[0] + 0x30u));
    c.setDb(1, static_cast<m_byte>(c.db(1) ^ c.db(0)));
    tst(c, c.db(1), 8);
    if (c.db(1) != 0) {
        mem.writeByte(c.a[0] + 0x30u, c.db(0));
        c.a[1] = kActionAnimationRow;
        c.setDb(0, mem.readByte(c.a[1] + sext16(c.dw(0))));
        tst(c, c.db(0), 8);
        c.a[1] = mem.readLong(c.a[0] + 0x04u);
        SOR_CALL(sub_0035d0, 0x00002E0Cu, 0x000035D0u);
        SOR_CALL(player_load_frame_timer, 0x00002E10u, 0x00003A14u);
        SOR_CALL(sub_003614, 0x00002E14u, 0x00003614u);
        SOR_CALL(player_action_change_mask_state_bits, 0x00002E18u, 0x00003936u);
        if (clearBitMem(c, mem, c.a[0] + 0x58u, 1)) {
            const m_byte frame = mem.readByte(c.a[0] + 0x5Au);
            mem.writeByte(c.a[0] + 0x0Au, frame);
            tst(c, frame, 8);
            const m_byte timer = mem.readByte(c.a[0] + 0x5Bu);
            mem.writeByte(c.a[0] + 0x0Du, timer);
            tst(c, timer, 8);
        }
    }
    SOR_CALL(sub_002e32, 0x00002E30u, 0x00002E32u);
    c.ssp += 4;
}

void StreetsOfRage::sub_002ee8(m_long /*entry_*/) {
    traceEnter(0x00002EE8u);
    auto &c   = cpu();
    auto &mem = memory();

    c.setDb(1, mem.readByte(c.a[0] + 0x30u));
    c.setDb(1, static_cast<m_byte>(c.db(1) & 1u));
    tst(c, c.db(1), 8);
    c.setDb(0, static_cast<m_byte>(addValue(c, c.db(0), c.db(1), 8)));

    c.setDb(1, mem.readByte(c.a[0] + 0x30u));
    c.setDb(1, static_cast<m_byte>(c.db(1) ^ c.db(0)));
    tst(c, c.db(1), 8);
    if (c.db(1) != 0) {
        mem.writeByte(c.a[0] + 0x30u, c.db(0));
        c.setDb(0, mem.readByte(kActionAnimationRow + sext16(c.dw(0))));
        tst(c, c.db(0), 8);
        c.a[1] = mem.readLong(c.a[0] + 0x04u);
        SOR_CALL(sub_0035d0, 0x00002F0Au, 0x000035D0u);
        SOR_CALL(player_load_frame_timer, 0x00002F0Eu, 0x00003A14u);
        SOR_CALL(sub_003614, 0x00002F12u, 0x00003614u);
        SOR_CALL(player_action_change_mask_state_bits, 0x00002F16u, 0x00003936u);
        if (clearBitMem(c, mem, c.a[0] + 0x58u, 1)) {
            const m_byte frame = mem.readByte(c.a[0] + 0x5Au);
            mem.writeByte(c.a[0] + 0x0Au, frame);
            tst(c, frame, 8);
            const m_byte timer = mem.readByte(c.a[0] + 0x5Bu);
            mem.writeByte(c.a[0] + 0x0Du, timer);
            tst(c, timer, 8);
        }
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $004192: writes six words at (a1)+ for the shape record d1.w (five bytes at a5; the lane pair comes from
// the table at a6 by the record's third byte): X, lane and height, each the object's coordinate plus the
// record's offset, then plus its width.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_004192(m_long /*entry_*/) {
    traceEnter(0x00004192u);
    auto &c   = cpu();
    auto &mem = memory();

    const auto put = [&](m_word value) {
        mem.writeWord(c.a[1], value);
        c.a[1] += 2;
        tst(c, value, 16);
    };
    const auto step = [&](m_long table, m_long offset, int target) {
        c.setDb(target, mem.readByte(table + offset));
        c.setDw(target, sext8to16(c.db(target)));
    };

    c.setDw(0, c.dw(1));
    c.setDw(1, static_cast<m_word>(lslValue(c, c.dw(1), 2, 16)));
    c.setDw(1, static_cast<m_word>(addValue(c, c.dw(1), c.dw(0), 16)));
    const m_long record = sext16(c.dw(1));

    c.setDw(0, mem.readWord(c.a[0] + 0x10u));
    step(c.a[5], record + 0, 2);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(2), 16)));
    put(c.dw(0));
    step(c.a[5], record + 1, 2);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(2), 16)));
    put(c.dw(0));

    c.setDw(0, mem.readWord(c.a[0] + 0x14u));
    c.d[2] = mem.readByte(c.a[5] + record + 2);
    c.setDw(2, static_cast<m_word>(addValue(c, c.dw(2), c.dw(2), 16)));
    step(c.a[6], sext16(c.dw(2)) + 0, 3);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(3), 16)));
    put(c.dw(0));
    step(c.a[6], sext16(c.dw(2)) + 1, 3);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(3), 16)));
    put(c.dw(0));

    c.setDw(0, mem.readWord(c.a[0] + 0x18u));
    step(c.a[5], record + 3, 2);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(2), 16)));
    put(c.dw(0));
    step(c.a[5], record + 4, 2);
    c.setDw(0, static_cast<m_word>(addValue(c, c.dw(0), c.dw(2), 16)));
    put(c.dw(0));
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $01040A (memfill_long_12): three longs of d1 at (a1)+
// ---------------------------------------------------------------------------
void StreetsOfRage::memfill_long_12(m_long /*entry_*/) {
    traceEnter(0x0001040Au);
    auto &c = cpu();
    for (int i = 0; i < 3; ++i) {
        memory().writeLong(c.a[1], c.d[1]);
        c.a[1] += 4;
    }
    tst(c, c.d[1], 32);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $004140 (player_cache_boxes): the attack box (+$64) and body box (+$70... the two six-word records at
// a1) from the shape records of the object's attack (+$02) and body (+$03) shapes. P1 and an empty shape
// write zeros, anyone else a 16-by-16 stub for the attack record.
// ---------------------------------------------------------------------------
void StreetsOfRage::player_cache_boxes(m_long /*entry_*/) {
    traceEnter(0x00004140u);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[1] = c.a[0] + 0x64u;
    c.a[5] = kPlayerShapeTable;
    c.a[6] = kLaneShapeTable;
    c.d[1] = mem.readByte(c.a[0] + 0x02u);
    tst(c, c.d[1], 8);
    if (c.d[1] == 0) {
        cmpValue(c, c.a[0], sext16(static_cast<m_word>(kP1Object)), 32);
        if (!c.flagZ())
            c.d[1] = 0x00100010u;
        SOR_CALL(memfill_long_12, 0x0000416Au, 0x0001040Au);
    } else {
        SOR_CALL(sub_004192, 0x0000416Eu, 0x00004192u);
    }

    c.d[1] = 0;
    cmpValue(c, c.a[0], sext16(static_cast<m_word>(kP1Object)), 32);
    if (!c.flagZ())
        c.d[1] = 0x00100010u;
    if (!testBitMem(c, mem, c.a[0] + 0x4Bu, 1)) {
        c.d[1] = mem.readByte(c.a[0] + 0x03u);
        tst(c, c.d[1], 8);
        if (c.d[1] != 0) {
            sub_004192();
            return;
        }
    }
    memfill_long_12();
}

// ---------------------------------------------------------------------------
// $004104: a fresh start for the player's combat fields, then the box cache above.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_004104(m_long /*entry_*/) {
    traceEnter(0x00004104u);
    auto &c   = cpu();
    auto &mem = memory();
    const m_long o = c.a[0];

    mem.writeByte(o + 0x34u, 0);
    mem.writeWord(o + 0x02u, 0);
    mem.writeLong(o + 0x40u, 0);
    mem.writeLong(o + 0x44u, 0);
    mem.writeByte(o + 0x48u, 0);
    mem.writeByte(o + 0x4Au, 0);
    mem.writeLong(o + 0x4Cu, 0);
    mem.writeByte(o + 0x58u, 0);
    mem.writeLong(o + 0x5Au, 0);
    mem.writeByte(o + 0x61u, 0);
    mem.writeWord(o + 0x62u, 0);
    mem.writeLong(o + 0x7Cu, 0);
    const m_byte flags = static_cast<m_byte>(mem.readByte(o + 0x4Bu) & 0xF3u);
    mem.writeByte(o + 0x4Bu, flags);
    const m_byte mask = static_cast<m_byte>(mem.readByte(o + 0x59u) & 0x38u);
    mem.writeByte(o + 0x59u, mask);
    tst(c, mask, 8);
    player_cache_boxes();
}
