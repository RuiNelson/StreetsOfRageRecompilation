#include "SoRCpuOps.hpp"
#include "SoRQolConfig.hpp"

using namespace sorops;

// ---------------------------------------------------------------------------
// Small object routines the hand-written ports call: the floor probe ($AD2A / $AD30), the object
// velocity step ($B20E), the box tests between two objects ($AB24 / $ABF8), the animation setup
// ($B1A2 and what leads to it), the free-slot scan ($E46) and the two trampolines to queue_sound_id
// ($6ACA / $35D6). Each is the ROM routine as it is, registers and condition codes
// included, so that a manual caller never reaches into generated code.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kBlockmapStride             = 0xFFFFE02Eu; // primary_plane_blockmap_stride (W)
constexpr m_long kCollisionClassMap          = 0x00FFA000u; // level_collision_class_map
constexpr m_long kCollisionAttributeTablePtr = 0xFFFFFC34u; // collision_class_attribute_table_ptr (L)
constexpr m_long kCollisionHeightTablePtr    = 0xFFFFFC38u; // collision_class_height_table_ptr (L)
constexpr m_long kShapeTable                 = 0x0001A68Eu; // 5-byte shape records of the ordinary objects
constexpr m_long kPlayerShapeTable           = 0x0001ABA8u; // player_shape_table, also used by type $58
constexpr m_long kLaneShapeTable             = 0x0001AB8Eu; // byte pairs the shape record's lane index selects
constexpr m_byte kTwinsType                  = 0x58u;

constexpr m_long kObjX       = 0x10u;
constexpr m_long kObjLane    = 0x14u;
constexpr m_long kObjHeight  = 0x18u;
constexpr m_long kObjVelX    = 0x1Cu;
constexpr m_long kObjVelLane = 0x20u;
constexpr m_long kObjVelZ    = 0x24u;

m_long signExtendWord(m_word value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

// ext.w after move.b: the sign-extended byte as a word.
m_word signExtendByte(m_byte value) {
    return static_cast<m_word>(static_cast<std::int16_t>(static_cast<std::int8_t>(value)));
}

bool lessSigned(m_word left, m_word right) {
    return static_cast<std::int16_t>(left) < static_cast<std::int16_t>(right);
}

// moveq: N and Z from the value, V and C clear, X untouched.
void setMoveqFlags(CPU68K &cpu, m_long value) {
    cpu.setNZClearVC(value, 0x80000000u);
}

// add.w / sub.w to a data register's low word, with X as the ROM leaves it.
void addWordToD(CPU68K &cpu, int reg, m_word value) {
    const m_long sum = static_cast<m_long>(cpu.dw(reg)) + value;
    cpu.setDw(reg, static_cast<m_word>(sum));
    cpu.setFlag(CPU68K::FlagX, (sum & 0x10000u) != 0);
}

void subWordFromD(CPU68K &cpu, int reg, m_word value) {
    const m_long diff = static_cast<m_long>(cpu.dw(reg)) - value;
    cpu.setDw(reg, static_cast<m_word>(diff));
    cpu.setFlag(CPU68K::FlagX, (diff & 0x10000u) != 0);
}

// d5.w = the signed byte at (an)+ , as `move.b (an)+, d5` and `ext.w d5`.
void loadSignedByteD5(CPU68K &cpu, SystemMemory &mem, int addressReg, bool postIncrement) {
    cpu.setDw(5, signExtendByte(mem.readByte(cpu.a[addressReg])));
    if (postIncrement)
        cpu.a[addressReg] += 1;
}

// $00AB88: one axis of $AB24. d1.w is the first object's coordinate, a3 points at its (offset, width)
// signed bytes, a2 at the upper bound word of the other object, whose lower bound is the word before it.
// d7 = 1 for an overlap, 0 for a miss, and Z says which.
void shapeAxisBetweenBounds(CPU68K &cpu, SystemMemory &mem) {
    bool hit = false;
    do {
        loadSignedByteD5(cpu, mem, 3, true);
        addWordToD(cpu, 1, cpu.dw(5));
        if (lessSigned(mem.readWord(cpu.a[2]), cpu.dw(1)))
            break;
        loadSignedByteD5(cpu, mem, 3, true);
        addWordToD(cpu, 1, cpu.dw(5));
        cpu.a[2] -= 2;
        if (lessSigned(cpu.dw(1), mem.readWord(cpu.a[2])))
            break;
        hit = true;
    } while (false);
    cpu.d[7] = hit ? 1u : 0u;
    setMoveqFlags(cpu, cpu.d[7]);
}

// $00AC78: one axis of $ABF8. d0.w and d1.w are the two objects' coordinates, a2 and a3 point at their
// (offset, width) signed bytes. d7 = 1 for an overlap, 0 for a miss.
void shapeAxisBetweenPair(CPU68K &cpu, SystemMemory &mem) {
    bool hit = false;
    do {
        loadSignedByteD5(cpu, mem, 2, true);
        addWordToD(cpu, 0, cpu.dw(5));
        loadSignedByteD5(cpu, mem, 2, false);
        addWordToD(cpu, 0, cpu.dw(5));
        loadSignedByteD5(cpu, mem, 3, true);
        addWordToD(cpu, 1, cpu.dw(5));
        if (lessSigned(cpu.dw(0), cpu.dw(1)))
            break;
        loadSignedByteD5(cpu, mem, 2, true);
        subWordFromD(cpu, 0, cpu.dw(5));
        loadSignedByteD5(cpu, mem, 3, true);
        addWordToD(cpu, 1, cpu.dw(5));
        if (lessSigned(cpu.dw(1), cpu.dw(0)))
            break;
        hit = true;
    } while (false);
    cpu.d[7] = hit ? 1u : 0u;
    setMoveqFlags(cpu, cpu.d[7]);
}

// The body of $AD30; $AD2A zeroes d1-d3 first.
void probeFloor(CPU68K &cpu, SystemMemory &mem) {
    const m_long object = cpu.a[0];

    const auto addClamped = [&](int reg, m_long field) {
        const m_word sum = static_cast<m_word>(cpu.dw(reg) + mem.readWord(object + field));
        if (sum & 0x8000u)
            cpu.d[reg] = 0;
        else
            cpu.setDw(reg, sum);
    };
    addClamped(1, kObjX);
    addClamped(3, kObjHeight);
    addClamped(2, kObjLane);

    cpu.setDw(4, cpu.dw(1));
    cpu.setDw(1, static_cast<m_word>(cpu.dw(1) >> 4));
    cpu.setDw(2, static_cast<m_word>(cpu.dw(2) >> 3));
    const std::int32_t row = static_cast<std::int32_t>(static_cast<std::int16_t>(cpu.dw(2)))
                           * static_cast<std::int32_t>(static_cast<std::int16_t>(mem.readWord(kBlockmapStride)));
    cpu.d[2] = static_cast<m_long>(row);
    cpu.setDw(2, static_cast<m_word>(cpu.dw(2) + cpu.dw(1)));

    cpu.a[6] = kCollisionClassMap;
    cpu.setDb(7, mem.readByte(cpu.a[6] + signExtendWord(cpu.dw(2))));
    cpu.setDb(4, static_cast<m_byte>(cpu.db(4) & 0x0Fu));
    if (cpu.db(4) < 8u)
        cpu.setDb(7, static_cast<m_byte>(cpu.db(7) >> 4));
    cpu.setDw(7, static_cast<m_word>(cpu.dw(7) & 0x000Fu));
    cpu.setDw(6, cpu.dw(7));

    cpu.a[6] = mem.readLong(kCollisionAttributeTablePtr);
    cpu.setDb(7, mem.readByte(cpu.a[6] + signExtendWord(cpu.dw(7))));
    cpu.setDw(6, static_cast<m_word>(cpu.dw(6) * 2u));
    cpu.a[6] = mem.readLong(kCollisionHeightTablePtr);
    cpu.setDw(6, mem.readWord(cpu.a[6] + signExtendWord(cpu.dw(6))));

    // add.w d6,d6 of a class (0-15) leaves X clear; nothing after it touches X.
    cpu.setFlag(CPU68K::FlagX, false);
    if (cpu.dw(3) < cpu.dw(6)) {
        cpu.d[7] = 0;
        setMoveqFlags(cpu, 0);
    } else {
        cpu.setNZClearVC(cpu.db(7), 0x80u);
    }
}

// add.l (velocity), (position) with the flags of the ROM's `add.l d0, mem`.
void addVelocity(CPU68K &cpu, SystemMemory &mem, m_long positionField, m_long velocityField) {
    const m_long object   = cpu.a[0];
    const m_long velocity = mem.readLong(object + velocityField);
    const m_long before   = mem.readLong(object + positionField);
    const m_long after    = before + velocity;
    cpu.d[0]              = velocity;
    mem.writeLong(object + positionField, after);
    const bool carry    = after < before;
    const bool overflow = ((before ^ after) & (velocity ^ after) & 0x80000000u) != 0;
    cpu.setNZVCX(after, 0x80000000u, overflow, carry, carry);
}

} // namespace

// ---------------------------------------------------------------------------
// $00AD2A (probe_floor_at_object) / $00AD30 (probe_floor_at_object_offset)
//
// Collision class of the floor under object a0 (plus d1 X, d2 lane, d3 height; negative sums clamp to 0):
// d7 = the class's attribute byte when d3 is not under the class's height, else 0, with Z for it.
// d6 is left with the class height.
// ---------------------------------------------------------------------------
void StreetsOfRage::probe_floor_at_object(m_long /*entry_*/) {
    traceEnter(0x0000AD2Au);
    cpu().d[1] = 0;
    cpu().d[2] = 0;
    cpu().d[3] = 0;
    probeFloor(cpu(), memory());
    cpu().ssp += 4;
}

void StreetsOfRage::probe_floor_at_object_offset(m_long /*entry_*/) {
    traceEnter(0x0000AD30u);
    probeFloor(cpu(), memory());
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $00B20E (integrate_object_velocity)
//
// X += +$1C, lane += +$20, then height += +$24. The entry $B21E does the height alone. d0 ends with
// the last velocity and the flags are those of the last add.l.
// ---------------------------------------------------------------------------
void StreetsOfRage::integrate_object_velocity(m_long entry_) {
    if (entry_ == 0x0000B21Eu) {
        traceEnter(0x0000B21Eu);
    } else {
        traceEnter(0x0000B20Eu);
        addVelocity(cpu(), memory(), kObjX, kObjVelX);
        addVelocity(cpu(), memory(), kObjLane, kObjVelLane);
    }
    addVelocity(cpu(), memory(), kObjHeight, kObjVelZ);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $00AB24 (test_shape_box_overlap)
//
// Box overlap of objects a0 and a1 for the shape id in d1.w, a2 pointing at the word after a1's box
// pair (a1+$66): X, then lane, then height, each from a five-byte shape record. Z set when they miss.
// ---------------------------------------------------------------------------
void StreetsOfRage::test_shape_box_overlap(m_long /*entry_*/) {
    traceEnter(0x0000AB24u);
    auto &cpu = this->cpu();
    auto &mem = memory();

    cpu.a[5] = mem.readByte(cpu.a[0]) == kTwinsType ? kPlayerShapeTable : kShapeTable;
    cpu.setDw(0, cpu.dw(1));
    cpu.setDw(1, static_cast<m_word>(cpu.dw(1) * 5u));
    cpu.a[3] = cpu.a[5] + signExtendWord(cpu.dw(1));
    cpu.d[6] = 0;
    cpu.setDw(1, mem.readWord(cpu.a[0] + kObjX));
    if (!lessSigned(cpu.dw(1), mem.readWord(cpu.a[1] + kObjX)))
        cpu.d[6] = 2;

    shapeAxisBetweenBounds(cpu, mem);
    if (cpu.flagZ()) {
        cpu.ssp += 4;
        return;
    }

    cpu.a[2] += 6;
    cpu.setDw(1, mem.readWord(cpu.a[0] + kObjLane));
    cpu.d[0] = mem.readByte(cpu.a[3]);
    cpu.a[3] += 1;
    const m_long heightRecord = cpu.a[3];
    cpu.setDw(0, static_cast<m_word>(cpu.dw(0) * 2u));
    cpu.a[3] = kLaneShapeTable + signExtendWord(cpu.dw(0));
    shapeAxisBetweenBounds(cpu, mem);
    cpu.a[3] = heightRecord;

    cpu.setNZClearVC(cpu.db(7), 0x80u);
    if (cpu.db(7) == 0) {
        cpu.ssp += 4;
        return;
    }

    cpu.a[2] += 6;
    cpu.setDw(1, mem.readWord(cpu.a[0] + kObjHeight));
    shapeAxisBetweenBounds(cpu, mem);
    cpu.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00ABF8 (test_attacker_shape_box_overlap)
//
// Box overlap of objects a0 (shape id d1.w) and a1 (shape id d0.w): X, lane, height. d7 = 1 and Z clear
// for an overlap, else Z set.
// ---------------------------------------------------------------------------
void StreetsOfRage::test_attacker_shape_box_overlap(m_long /*entry_*/) {
    traceEnter(0x0000ABF8u);
    auto &cpu = this->cpu();
    auto &mem = memory();

    cpu.a[5] = kShapeTable;
    cpu.setDw(2, cpu.dw(0));
    cpu.setDw(0, static_cast<m_word>(cpu.dw(0) * 5u));
    cpu.a[2] = cpu.a[5] + signExtendWord(cpu.dw(0));
    if (mem.readByte(cpu.a[0]) == kTwinsType)
        cpu.a[5] = kPlayerShapeTable;
    cpu.setDw(2, cpu.dw(1));
    cpu.setDw(1, static_cast<m_word>(cpu.dw(1) * 5u));
    cpu.a[3] = cpu.a[5] + signExtendWord(cpu.dw(1));

    cpu.setDw(0, mem.readWord(cpu.a[1] + kObjX));
    cpu.setDw(1, mem.readWord(cpu.a[0] + kObjX));
    cpu.d[6] = 0;
    if (!lessSigned(cpu.dw(1), cpu.dw(0)))
        cpu.d[6] = 2;

    shapeAxisBetweenPair(cpu, mem);
    if (cpu.flagZ()) {
        cpu.ssp += 4;
        return;
    }

    cpu.d[0] = 0;
    cpu.d[1] = 0;
    cpu.d[1] = mem.readByte(cpu.a[2]);
    cpu.a[2] += 1;
    cpu.d[0] = mem.readByte(cpu.a[3]);
    cpu.a[3] += 1;
    const m_long savedA2 = cpu.a[2];
    const m_long savedA3 = cpu.a[3];
    cpu.a[5]             = kLaneShapeTable;
    cpu.setDw(0, static_cast<m_word>(cpu.dw(0) * 2u));
    cpu.setDw(1, static_cast<m_word>(cpu.dw(1) * 2u));
    cpu.a[3] = cpu.a[5] + signExtendWord(cpu.dw(0));
    cpu.a[2] = cpu.a[5] + signExtendWord(cpu.dw(1));
    cpu.setDw(0, mem.readWord(cpu.a[1] + kObjLane));
    cpu.setDw(1, mem.readWord(cpu.a[0] + kObjLane));
    shapeAxisBetweenPair(cpu, mem);
    cpu.a[2] = savedA2;
    cpu.a[3] = savedA3;

    cpu.setNZClearVC(cpu.db(7), 0x80u);
    if (cpu.db(7) == 0) {
        cpu.ssp += 4;
        return;
    }

    cpu.setDw(0, mem.readWord(cpu.a[1] + kObjHeight));
    cpu.setDw(1, mem.readWord(cpu.a[0] + kObjHeight));
    shapeAxisBetweenPair(cpu, mem);
    if (!cpu.flagZ()) {
        cpu.d[7] = 1;
        setMoveqFlags(cpu, 1);
    }
    cpu.ssp += 4;
}

// ---------------------------------------------------------------------------
// $006ACA (queue_object_sound_id) and $0035D6: `jmp queue_sound_id`, so the sound id in d7 is queued
// and queue_sound_id's rts returns to the caller.
// ---------------------------------------------------------------------------
void StreetsOfRage::queue_object_sound_id(m_long /*entry_*/) {
    traceEnter(0x00006ACAu);
    queue_sound_id();
}

void StreetsOfRage::player_queue_sound_trampoline(m_long /*entry_*/) {
    traceEnter(0x000035D6u);
    queue_sound_id();
}

// ---------------------------------------------------------------------------
// $000E46 (find_free_object_slot): a1 = the first object slot from $FFB900 whose type byte is 0 (the 32nd
// when none is), Z set when one was found.
// ---------------------------------------------------------------------------
void StreetsOfRage::find_free_object_slot(m_long /*entry_*/) {
    traceEnter(0x00000E46u);
    auto &c   = cpu();
    auto &mem = memory();

    c.a[1] = 0xFFFFB900u;
    c.d[3] = 0x1Fu;
    for (;;) {
        const m_byte type = mem.readByte(c.a[1]);
        tst(c, type, 8);
        if (type == 0)
            break;
        c.a[1] += 0x80u;
        c.setDw(3, static_cast<m_word>(c.dw(3) - 1u));
        if (c.dw(3) == 0xFFFFu)
            break;
    }
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00B1A2 (init_object_animation_frame)
//
// Starts animation d0 (a word offset into the set at a1): +$08 = d0, +$0A = 0, +$0C and +$0D = the
// frame's first byte, and +$02 / +$03 = the two bytes after the frame header (three when the header's
// word has bit 15 set). a1 ends past them.
// ---------------------------------------------------------------------------
void StreetsOfRage::init_object_animation_frame(m_long /*entry_*/) {
    traceEnter(0x0000B1A2u);
    auto &c   = cpu();
    auto &mem = memory();

    mem.writeWord(c.a[0] + 0x08u, c.dw(0));
    c.d[1] = 0;
    mem.writeByte(c.a[0] + 0x0Au, 0);
    c.a[1] += sext16(mem.readWord(c.a[1] + sext16(c.dw(0))));
    c.a[1] += 1;
    const m_byte frame = mem.readByte(c.a[1]);
    mem.writeByte(c.a[0] + 0x0Cu, frame);
    mem.writeByte(c.a[0] + 0x0Du, frame);
    c.a[1] -= 1;
    c.setDw(0, mem.readWord(c.a[1] + 2u));
    const bool wide = (c.d[0] & 0x8000u) != 0;
    c.d[0] &= ~0x8000u;
    c.a[1] = c.a[1] + 1u + sext16(c.dw(0));
    if (wide)
        c.a[1] += 2;
    const m_byte first = mem.readByte(c.a[1]);
    c.a[1] += 1;
    mem.writeByte(c.a[0] + 0x02u, first);
    const m_byte second = mem.readByte(c.a[1]);
    c.a[1] += 1;
    mem.writeByte(c.a[0] + 0x03u, second);
    tst(c, second, 8);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $00B196 (init_object_animation_state_and_advance): +$30 += 1, +$04 = a1, +$0E = d1, then the frame start above.
// ---------------------------------------------------------------------------
void StreetsOfRage::init_object_animation_state_and_advance(m_long /*entry_*/) {
    traceEnter(0x0000B196u);
    auto &c   = cpu();
    auto &mem = memory();

    const m_byte state = static_cast<m_byte>(addValue(c, mem.readByte(c.a[0] + 0x30u), 1, 8));
    mem.writeByte(c.a[0] + 0x30u, state);
    mem.writeLong(c.a[0] + 0x04u, c.a[1]);
    mem.writeWord(c.a[0] + 0x0Eu, c.dw(1));
    init_object_animation_frame();
}

// ---------------------------------------------------------------------------
// $00B1D6: animation 0 of the set at a1 (d1 = timer word), state advanced.
// ---------------------------------------------------------------------------
void StreetsOfRage::object_init_animation_from_zero(m_long /*entry_*/) {
    traceEnter(0x0000B1D6u);
    cpu().d[0] = 0;
    tst(cpu(), 0, 32);
    init_object_animation_state_and_advance();
}

// ---------------------------------------------------------------------------
// $006ABC: +$01 bit 6 set, d1 = 0, then $B1D6. Entered at $6AC2 without the bit, at $6AC4 without d1.
// ---------------------------------------------------------------------------
void StreetsOfRage::object_start_animation_with_flag_bit6(m_long entry_) {
    auto &c   = cpu();
    auto &mem = memory();

    if (entry_ != 0x00006AC2u && entry_ != 0x00006AC4u) {
        traceEnter(0x00006ABCu);
        const m_byte flags = static_cast<m_byte>(mem.readByte(c.a[0] + 0x01u) | 0x40u);
        mem.writeByte(c.a[0] + 0x01u, flags);
        tst(c, flags, 8);
    }
    if (entry_ != 0x00006AC4u) {
        if (entry_ == 0x00006AC2u)
            traceEnter(0x00006AC2u);
        c.d[1] = 0;
        tst(c, 0, 32);
    } else {
        traceEnter(0x00006AC4u);
    }
    object_init_animation_from_zero();
}

// ---------------------------------------------------------------------------
// $00B228: the object's three velocities (+$1C, +$20, +$24) cleared, d7 = 0.
// ---------------------------------------------------------------------------
void StreetsOfRage::object_clear_velocity(m_long /*entry_*/) {
    traceEnter(0x0000B228u);
    auto &c   = cpu();
    auto &mem = memory();

    c.d[7] = 0;
    mem.writeLong(c.a[0] + kObjVelX, 0);
    mem.writeLong(c.a[0] + kObjVelLane, 0);
    mem.writeLong(c.a[0] + kObjVelZ, 0);
    tst(c, 0, 32);
    c.ssp += 4;
}

// ---------------------------------------------------------------------------
// $003504 (player_drop_carried_weapon), and what a player's action change runs: $003A14, $003614, $003936.
//
// The ROM's routines, except under `--qol grab`. A grab reaches the drop through +$7D 2 or 3 ($34E6, $34EA),
// and the holder in +$7E tells a friend from an enemy: a friend's keeps the weapon, hidden by $5E2E while the
// player is held. The ROM's way out of a hold is always action $02, as it never has a weapon there, so
// $003936 turns that into the armed $30 the other exits use (`$2DE6`).
// ---------------------------------------------------------------------------
namespace {

constexpr m_byte kPlayerType = 0x01u;

bool grabbedByFriend(SystemMemory &mem, m_long player) {
    if (mem.readByte(player + 0x7Cu) != 1u || static_cast<std::int16_t>(mem.readWord(player + 0x32u)) < 0)
        return false;
    const m_byte reaction = mem.readByte(player + 0x7Du);
    if (reaction != 2u && reaction != 3u)
        return false;
    const m_word holder = mem.readWord(player + 0x7Eu);
    return holder != 0u && mem.readByte(sext16(holder)) == kPlayerType;
}

} // namespace

void StreetsOfRage::player_drop_carried_weapon(m_long /*entry_*/) {
    traceEnter(0x00003504u);
    auto &c   = cpu();
    auto &mem = memory();

    const m_byte weapon = mem.readByte(c.a[0] + 0x60u);
    c.setDb(1, weapon);
    tst(c, weapon, 8);
    if (weapon != 0u && !(SoRQolConfig::current().friendGrabKeepsWeapon && grabbedByFriend(mem, c.a[0]))) {
        mem.writeByte(c.a[0] + 0x60u, 0);
        c.a[1] = sext16(mem.readWord(c.a[0] + 0x5Eu));
        mem.writeByte(c.a[1] + 0x51u, 2);
        mem.writeWord(c.a[0] + 0x5Eu, 0);
        tst(c, 0, 16);
    }
    c.ssp += 4;
}

void StreetsOfRage::player_load_frame_timer(m_long /*entry_*/) {
    traceEnter(0x00003A14u);
    auto &c   = cpu();
    auto &mem = memory();

    constexpr m_long kPlayerFrameTimingTable = 0x00003A30u;
    c.d[0] = static_cast<m_long>(mem.readByte(c.a[0] + 0x0Cu)) * 2u;
    c.a[2] = kPlayerFrameTimingTable + sext16(mem.readWord(kPlayerFrameTimingTable + c.d[0]));
    c.setDb(0, mem.readByte(c.a[0] + 0x0Au));
    const m_byte timer = mem.readByte(c.a[2] + sext16(c.dw(0)));
    mem.writeByte(c.a[0] + 0x0Du, timer);
    tst(c, timer, 8);
    c.ssp += 4;
}

void StreetsOfRage::player_load_action_velocity(m_long /*entry_*/) {
    traceEnter(0x00003614u);
    auto &c   = cpu();
    auto &mem = memory();

    constexpr m_long kPlayerActionVelocityTablePtrs = 0x00003664u;
    c.d[1] = static_cast<m_long>(mem.readByte(c.a[0] + 0x50u)) << 2;
    c.a[1] = mem.readLong(kPlayerActionVelocityTablePtrs + c.d[1]);
    const m_byte action = mem.readByte(c.a[0] + 0x30u);
    c.setDb(1, static_cast<m_byte>(action & 0xFEu));
    const auto loadVelocity = [&](m_long offset) {
        return static_cast<m_long>(static_cast<std::int32_t>(sext8(mem.readByte(c.a[1] + sext16(c.dw(1)) + offset)) << 16) >> 4);
    };
    if (!clearBitMem(c, mem, c.a[0] + 0x58u, 2)) {
        c.d[2] = loadVelocity(0);
        if ((action & 0x01u) != 0u)
            c.d[2] = negValue(c, c.d[2], 32);
        mem.writeLong(c.a[0] + 0x1Cu, c.d[2]);
    }
    if (!clearBitMem(c, mem, c.a[0] + 0x58u, 2)) {
        c.d[2] = loadVelocity(1);
        mem.writeLong(c.a[0] + 0x20u, c.d[2]);
    }
    c.ssp += 4;
}

void StreetsOfRage::player_action_change_mask_state_bits(m_long /*entry_*/) {
    traceEnter(0x00003936u);
    auto &c   = cpu();
    auto &mem = memory();

    constexpr m_long kActionStateMaskTable = 0x0000394Eu;
    const auto maskStateBits = [&] {
        c.d[7] = mem.readByte(c.a[0] + 0x30u) >> 1;
        c.setDb(7, mem.readByte(kActionStateMaskTable + c.d[7]));
        const m_byte masked = static_cast<m_byte>(mem.readByte(c.a[0] + 0x58u) & c.db(7));
        mem.writeByte(c.a[0] + 0x58u, masked);
        tst(c, masked, 8);
    };

    mem.writeByte(c.a[0] + 0x51u, 1);
    maskStateBits();

    const m_byte weapon = mem.readByte(c.a[0] + 0x60u);
    const m_byte action = mem.readByte(c.a[0] + 0x30u);
    if (SoRQolConfig::current().friendGrabKeepsWeapon && weapon != 0u && (action & 0xFEu) == 0x02u) {
        constexpr m_long kPlayerActionAnimationTable = 0x00002F2Cu;
        const auto armed = static_cast<m_byte>(0x30u | (action & 0x01u));
        mem.writeByte(c.a[0] + 0x30u, armed);
        c.setDw(0, mem.readByte(kPlayerActionAnimationTable + armed));
        c.a[1] = mem.readLong(c.a[0] + 0x04u);
        SOR_CALL(init_object_animation_frame, 0x00002E08u, 0x00B1A2u);
        SOR_CALL(player_load_frame_timer, 0x00002E0Cu, 0x003A14u);
        SOR_CALL(player_load_action_velocity, 0x00002E10u, 0x003614u);
        maskStateBits();
        // $2E32: the bat and the pipe (types $0A, $0B) hold the next animation of the family.
        if (weapon == 0x0Au || weapon == 0x0Bu)
            mem.writeWord(c.a[0] + 0x08u, static_cast<m_word>(mem.readWord(c.a[0] + 0x08u) + 4u));
    }
    c.ssp += 4;
}
