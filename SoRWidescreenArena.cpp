#include "SoR.hpp"

#include <cstdint>

// ---------------------------------------------------------------------------
// Widescreen arena
//
// The game logic was written for a 320-pixel screen: the players are clamped to it, enemies
// walk in from just outside it, and many combat and AI tests ask whether an object is "on
// screen". In `--widescreen` (VDP Mode 48) sprites reach 32 pixels further out on each
// side, so each of those edges moves out by kArenaGrow. Every routine here is a hand-written
// port of a ROM routine that does exactly what the ROM does when the flag is off.
//
// Object X is a world coordinate; `x - cam_x` is its position in sprite space, where the
// screen spans [0, $140) normally and [-$20, $160) in widescreen. Object +$28 holds the
// biased screen X (`x - cam_x + $80`), so its screen spans [$80, $1C0) normally and
// [$60, $1E0) in widescreen.
// ---------------------------------------------------------------------------

namespace {

constexpr m_word kArenaGrow = 0x0020u;

constexpr m_long kCamX                = 0xFFFFE002u;
constexpr m_long kCameraXMax          = 0xFFFFE01Au;
constexpr m_long kCameraXMin          = 0xFFFFE01Eu;
constexpr m_long kGameState           = 0xFFFFFF00u;
constexpr m_long kLevel               = 0xFFFFFF02u;
constexpr m_long kPoliceSpecialActive = 0xFFFFFA1Au;
constexpr m_long kEndOfLevelFlag      = 0xFFFFFA73u;
constexpr m_long kLevelSpawnFlowFlags = 0xFFFFFA05u;
constexpr m_long kStatusBitfield1     = 0xFFFFFB14u;
constexpr m_long kActiveProgression   = 0xFFFFFB1Eu;
constexpr m_long kP1Object            = 0xFFFFB800u;
constexpr m_long kP2Object            = 0xFFFFB880u;
constexpr m_long kObjectTable         = 0xFFFFB900u;

constexpr m_long kClampStateTable = 0x000045D4u;

m_long signExtendWord(m_long value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFFu)));
}

} // namespace

// Soft 68000 jsr into generated code: push the return PC, call, and leave this routine if the callee
// unwound past it (the same contract as CALL in generated code).
#define SOR_CALL(fn, retPc, target)                                              \
    do {                                                                         \
        const m_long spBefore_ = cpu().ssp;                                      \
        logCallFromReturn((retPc), (target));                                    \
        cpu().ssp -= 4;                                                          \
        memory().writeLong(cpu().ssp, (retPc));                                  \
        (fn)();                                                                  \
        if ((cpu().ssp & 0x00FFFFFFu) > (spBefore_ & 0x00FFFFFFu))               \
            return;                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// $0043AA — clamp_players_to_gameplay_bounds
//
// Keeps each active player inside the camera window (X) and the lane range (Y) during gameplay. The X
// window is [cam+$20, cam+$120]; widescreen opens it to [cam, cam+$140], so the players can reach the
// 32-pixel margin of the wider screen on both sides. Also owns the inner entry $43D2.
// ---------------------------------------------------------------------------
void StreetsOfRage::clamp_players_to_gameplay_bounds(m_long /*entry_*/) {
    traceEnter(0x000043AAu);

    auto &mem = memory();

    const m_word state = mem.readWord(kGameState);
    if (state < 0x0014u || state >= 0x0018u || mem.readByte(kPoliceSpecialActive) != 0) {
        cpu().ssp += 4;
        return;
    }

    const m_word grow = vdp().widescreen() ? kArenaGrow : 0u;

    for (const m_long player : {kP1Object, kP2Object}) {
        cpu().a[0] = player;
        if (mem.readByte(player) == 0)
            continue;

        const m_byte tableIndex = static_cast<m_byte>(mem.readByte(player + 0x30) >> 1);
        const m_byte keepX      = mem.readByte(kClampStateTable + tableIndex);
        if ((keepX & 0x80u) == 0 && mem.readByte(kEndOfLevelFlag) == 0) {
            const std::int16_t x    = static_cast<std::int16_t>(mem.readWord(player + 0x10));
            std::int16_t       edge = static_cast<std::int16_t>(mem.readWord(kCamX) + 0x20 - grow);
            if (x < edge) {
                mem.writeWord(player + 0x10, static_cast<m_word>(edge));
            } else {
                edge = static_cast<std::int16_t>(edge + 0x100 + 2 * grow);
                if (x > edge)
                    mem.writeWord(player + 0x10, static_cast<m_word>(edge));
            }
        }

        const std::int16_t lane      = static_cast<std::int16_t>(mem.readWord(player + 0x14));
        std::int16_t       laneLimit = 0x0002;
        if (lane < laneLimit) {
            mem.writeWord(player + 0x14, static_cast<m_word>(laneLimit));
        } else {
            laneLimit = mem.readWord(kLevel) == 6u ? 0x00A0 : 0x0070;
            if (lane > laneLimit)
                mem.writeWord(player + 0x14, static_cast<m_word>(laneLimit));
        }
    }

    cpu().a[1] = kClampStateTable;
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $000C60 — spawn_single_object
//
// Initialises one object from a six-byte script record at a2 into the slot a1. The side byte puts it just
// outside the right edge (cam+$148) or the left edge (cam-8); widescreen moves both out by $20 so enemies
// walk in from beyond the visible screen instead of appearing at its edge.
// ---------------------------------------------------------------------------
void StreetsOfRage::spawn_single_object(m_long /*entry_*/) {
    traceEnter(0x00000C60u);

    auto &mem = memory();

    mem.writeWord(kActiveProgression, static_cast<m_word>(mem.readWord(kActiveProgression) + 1u));

    const m_long object = cpu().a[1];
    m_long       record = cpu().a[2];

    mem.writeByte(object, static_cast<m_byte>(mem.readByte(record++) & 0x7Fu));
    mem.writeByte(object + 0x40, mem.readByte(record++));
    mem.writeByte(object + 0x41, mem.readByte(record++));
    mem.writeByte(object + 0x49, mem.readByte(record++));
    mem.writeByte(object + 0x15, mem.readByte(record++));
    cpu().a[0] = signExtendWord(object);
    cpu().a[2] = record;

    const m_word level = mem.readWord(kLevel);
    if (level == 4u)
        mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) | 0x10u));

    if (level == 6u) {
        cpu().setDw(6, 0x0088u);
    } else {
        SOR_CALL(sub_00ad2a, 0x00000CA2u, 0x0000AD2Au);
    }
    mem.writeWord(object + 0x18, cpu().dw(6));

    const m_word grow     = vdp().widescreen() ? kArenaGrow : 0u;
    const m_byte side     = mem.readByte(cpu().a[2]);
    cpu().a[2] += 1;
    const m_word camX = mem.readWord(kCamX);
    const m_word x    = side == 0 ? static_cast<m_word>(camX + 0x0148u + grow)
                                  : static_cast<m_word>(camX - 0x0008u - grow);
    cpu().setDw(0, x);
    mem.writeWord(object + 0x10, x);

    cpu().setNZClearVC(x, 0x8000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0097CE / $0097E6 — "is the object on screen" tests (d0 = 1 or 0, Z follows d0)
//
// Unsigned window on the biased screen X in object +$28: [$80, $1C0) and [$70, $1D0). Widescreen widens
// both by $20 on each side.
// ---------------------------------------------------------------------------
void StreetsOfRage::object_on_screen_test(m_long /*entry_*/) {
    traceEnter(0x000097CEu);

    const m_word grow   = vdp().widescreen() ? kArenaGrow : 0u;
    const m_word screenX = memory().readWord(cpu().a[0] + 0x28);
    const m_word result  = screenX >= 0x0080u - grow && screenX < 0x01C0u + grow ? 1u : 0u;

    cpu().d[0] = result;
    cpu().setNZClearVC(result, 0x80000000u);
    cpu().ssp += 4;
}

void StreetsOfRage::sub_0097e6(m_long /*entry_*/) {
    traceEnter(0x000097E6u);

    const m_word grow    = vdp().widescreen() ? kArenaGrow : 0u;
    const m_word screenX = memory().readWord(cpu().a[0] + 0x28);
    const m_word result  = screenX >= 0x0070u - grow && screenX < 0x01D0u + grow ? 1u : 0u;

    cpu().d[0] = result;
    cpu().setNZClearVC(result, 0x80000000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0006A6 — update_camera_scroll_if_needed
//
// When the camera is locked (max - min < 4) every ordinary enemy stranded beyond the off-screen edge
// is pushed back to it: the left edge (cam-$20) normally, the right edge (cam+$160) in round 8, which
// scrolls the other way. The edge is a margin outside the visible screen, so widescreen moves it out by
// $20 (left cam-$40, round 8 right cam+$180).
// ---------------------------------------------------------------------------
void StreetsOfRage::update_camera_scroll_if_needed(m_long /*entry_*/) {
    traceEnter(0x000006A6u);

    auto &mem = memory();

    const m_word span = static_cast<m_word>(mem.readWord(kCameraXMax) - mem.readWord(kCameraXMin));
    if (span >= 4u) {
        cpu().ssp += 4;
        return;
    }

    mem.writeByte(kLevelSpawnFlowFlags, static_cast<m_byte>(mem.readByte(kLevelSpawnFlowFlags) & ~0x02u));
    mem.writeWord(kStatusBitfield1, 0x0004u);

    const m_word grow   = vdp().widescreen() ? kArenaGrow : 0u;
    const bool   round8 = mem.readWord(kLevel) == 7u;
    std::int16_t edge   = static_cast<std::int16_t>(mem.readWord(kCamX) - 0x20);
    edge                = static_cast<std::int16_t>(round8 ? edge + 0x180 + grow : edge - grow);
    cpu().setDw(1, static_cast<m_word>(edge));

    cpu().a[1] = kObjectTable;
    for (int slot = 0; slot < 32; ++slot, cpu().a[1] += 0x80) {
        const m_long object = cpu().a[1];
        cpu().d[0]          = mem.readByte(object);
        if (cpu().d[0] == 0)
            continue;

        SOR_CALL(is_nonordinary_enemy_type, 0x000006EAu, 0x00009350u);
        if (!cpu().flagZ())
            continue;

        const std::int16_t x = static_cast<std::int16_t>(mem.readWord(object + 0x10));
        if (round8 ? edge > x : edge < x)
            continue;

        mem.writeWord(object + 0x10, static_cast<m_word>(edge));
        const m_word lane = mem.readWord(kLevel) == 3u ? 0x0020u : 0x0060u;
        mem.writeWord(object + 0x14, lane);
        const m_word state = mem.readWord(object + 8);
        if ((state & ~0x0002u) != 0x0004u)
            mem.writeWord(object + 0x30, 0x0100u);
    }
    cpu().setDw(7, 0xFFFFu);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $00AAA0 / $00ABA4 — can the attacker (a0) hit the target (a1)? Result in d7, flags from d7.
//
// A hit only counts while the attacked object is on screen: biased screen X (object +$28) inside
// [$78, $1C8). Widescreen widens that window by $20 on each side, so enemies in the extra bands can be
// hit and can hit back. The rectangle tests are the generated subroutines $AB24 / $ABF8.
// ---------------------------------------------------------------------------
namespace {

bool attackWindowContains(m_word screenX, m_word grow) {
    return screenX >= 0x0078u - grow && screenX < 0x01C8u + grow;
}

} // namespace

void StreetsOfRage::sub_00aaa0(m_long /*entry_*/) {
    traceEnter(0x0000AAA0u);

    auto &mem = memory();
    const m_word grow = vdp().widescreen() ? kArenaGrow : 0u;
    const auto   finish = [this](m_byte result) {
        cpu().d[7] = result;
        cpu().setNZClearVC(result, 0x80000000u);
        cpu().ssp += 4;
    };
    bool unwound = false;
    const auto rectangleOverlap = [&](m_long retPc) -> bool {
        const m_long spBefore = cpu().ssp;
        logCallFromReturn(retPc, 0x0000AB24u);
        cpu().ssp -= 4;
        memory().writeLong(cpu().ssp, retPc);
        sub_00ab24();
        unwound = (cpu().ssp & 0x00FFFFFFu) > (spBefore & 0x00FFFFFFu);
        return !cpu().flagZ();
    };

    cpu().a[2] = cpu().a[1] + 0x64;
    const bool firstZero = mem.readWord(cpu().a[2]) == 0;
    cpu().a[2] += 2;
    const m_byte attackShape = mem.readByte(cpu().a[0] + 3);
    if (!firstZero && attackShape != 0) {
        cpu().d[1] = attackShape;
        const bool overlaps = rectangleOverlap(0x0000AAB2u);
        if (unwound)
            return;
        if (!overlaps)
            goto secondary;
        if (mem.readByte(cpu().a[1] + 0x34) != 0) {
            if (!attackWindowContains(mem.readWord(cpu().a[0] + 0x28), grow)) {
                finish(0);
                return;
            }
            const m_byte reaction = mem.readByte(cpu().a[1] + 0x42);
            mem.writeByte(cpu().a[0] + 0x37, static_cast<m_byte>((mem.readByte(cpu().a[0] + 0x37) & 0xFEu) | reaction));
            finish(2);
            return;
        }
        if (mem.readWord(cpu().a[1] + 0x4C) != 0 || mem.readByte(cpu().a[1] + 0x7C) == 3u) {
            finish(0);
            return;
        }
        std::int16_t distance = static_cast<std::int16_t>(mem.readWord(cpu().a[1] + 0x18) - mem.readWord(cpu().a[0] + 0x18));
        if (distance < 0)
            distance = static_cast<std::int16_t>(-distance);
        finish(distance > 8 ? 0 : 3);
        return;
    }

secondary:
    {
        const m_byte defenseShape = mem.readByte(cpu().a[0] + 2);
        cpu().d[1]                = defenseShape;
        if (defenseShape == 0) {
            finish(0);
            return;
        }
        cpu().a[2] = cpu().a[1] + 0x70;
        const bool zero = mem.readWord(cpu().a[2]) == 0;
        cpu().a[2] += 2;
        const bool overlaps = !zero && rectangleOverlap(0x0000AB1Au);
        if (unwound)
            return;
        if (!overlaps || (mem.readByte(cpu().a[1] + 0x7C) & 1u) != 0) {
            finish(0);
            return;
        }
        finish(1);
    }
}

void StreetsOfRage::sub_00aba4(m_long /*entry_*/) {
    traceEnter(0x0000ABA4u);

    auto &mem = memory();
    const m_word grow = vdp().widescreen() ? kArenaGrow : 0u;
    const auto   finish = [this](m_byte result) {
        cpu().d[7] = result;
        cpu().setNZClearVC(result, 0x80000000u);
        cpu().ssp += 4;
    };

    cpu().d[0] = mem.readByte(cpu().a[1] + 2);
    if (cpu().d[0] != 0) {
        cpu().d[1] = mem.readByte(cpu().a[0] + 3);
        if (cpu().d[1] != 0) {
            SOR_CALL(sub_00abf8, 0x0000ABB6u, 0x0000ABF8u);
            if (!cpu().flagZ()) {
                finish(attackWindowContains(mem.readWord(cpu().a[0] + 0x28), grow) ? 5 : 0);
                return;
            }
        }
    }

    cpu().d[1] = mem.readByte(cpu().a[0] + 2);
    if (cpu().d[1] == 0) {
        finish(0);
        return;
    }
    cpu().d[0] = mem.readByte(cpu().a[1] + 3);
    if (cpu().d[0] == 0) {
        finish(0);
        return;
    }
    SOR_CALL(sub_00abf8, 0x0000ABDCu, 0x0000ABF8u);
    if (cpu().flagZ()) {
        // The rectangles miss: d7 and the flags are whatever $ABF8 left, exactly as in the ROM.
        cpu().ssp += 4;
        return;
    }
    finish(attackWindowContains(mem.readWord(cpu().a[0] + 0x28), grow) ? 4 : 0);
}

// ---------------------------------------------------------------------------
// $014C38 — charging boss: runs until it leaves the screen
//
// While charging the boss turns (state 2) once its biased screen X passes the screen edge: right at
// $1C0, left at $80. Widescreen moves both edges out by $20 so the charge ends at the edge of the wider
// screen.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_014c38(m_long /*entry_*/) {
    traceEnter(0x00014C38u);

    auto &mem = memory();
    const m_word grow = vdp().widescreen() ? kArenaGrow : 0u;
    const m_long boss = cpu().a[0];

    SOR_CALL(sub_0154e0, 0x00014C3Eu, 0x000154E0u);
    if (cpu().dw(7) != 0) {
        cpu().ssp += 4;
        return;
    }

    cpu().d[0] = 0x10;
    SOR_CALL(bespoke_boss_range_test, 0x00014C48u, 0x00012A78u);
    if ((cpu().d[7] & 0x02u) == 0) {
        mem.writeByte(boss + 1, static_cast<m_byte>(mem.readByte(boss + 1) & 0xFBu));
        mem.writeWord(boss + 0x0A, 0x0001u);
        mem.writeWord(boss + 0x54, 0x0002u);
        mem.writeByte(boss + 0x5B, 0x02u);
        cpu().a[1] = signExtendWord(mem.readWord(boss + 0x50));
        mem.writeByte(cpu().a[1] + 0x58, 0x01u);
        cpu().ssp += 4;
        return;
    }

    SOR_CALL(sub_00b20e, 0x00014C54u, 0x0000B20Eu);
    cpu().d[0] = 0x01;
    SOR_CALL(sub_01569c, 0x00014C5Cu, 0x0001569Cu);

    const std::int16_t screenX = static_cast<std::int16_t>(mem.readWord(boss + 0x28));
    bool               turn;
    if (static_cast<std::int16_t>(mem.readWord(boss + 0x1C)) < 0)
        turn = !(screenX > 0x0080 - static_cast<std::int16_t>(grow));
    else
        turn = screenX >= 0x01C0 + static_cast<std::int16_t>(grow);
    if (!turn) {
        cpu().ssp += 4;
        return;
    }

    SOR_CALL(sub_01577c, 0x00014C7Au, 0x0001577Cu);
    SOR_CALL(sub_01566e, 0x00014C80u, 0x0001566Eu);
    mem.writeWord(boss + 0x54, 0x0001u);
    mem.writeByte(boss + 0x5B, 0x00u);
    mem.writeByte(boss + 0x30, 0x02u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $00F4A6 — ordinary enemy walks to a spot at the side of the screen
//
// The spot is the player clamp's edge: cam+$120 on the right, cam+$20 on the left (the side is object
// +$9 bit 1). Widescreen uses the wider clamp, cam+$140 and cam, so the enemy stands at the margin of
// the wider screen. The tests and the walk itself are the generated subroutines.
// ---------------------------------------------------------------------------
void StreetsOfRage::sub_00f4a6(m_long /*entry_*/) {
    traceEnter(0x0000F4A6u);

    auto &mem = memory();
    const m_word grow  = vdp().widescreen() ? kArenaGrow : 0u;
    const m_long enemy = cpu().a[0];

    const m_byte flags = mem.readByte(enemy + 0x31);
    mem.writeByte(enemy + 0x31, static_cast<m_byte>(flags | 0x01u));
    if ((flags & 0x01u) == 0) {
        cpu().d[0] = 0;
        SOR_CALL(sub_0096c0, 0x0000F4B4u, 0x000096C0u);
        mem.writeByte(enemy + 1, static_cast<m_byte>(mem.readByte(enemy + 1) | 0x04u));
        mem.writeByte(enemy + 0x50, 0x08u);
    }

    SOR_CALL(sub_00a9ba, 0x0000F4C4u, 0x0000A9BAu);
    if (cpu().flagN()) {
        cpu().ssp += 4;
        return;
    }
    SOR_CALL(sub_00f482, 0x0000F4CAu, 0x0000F482u);
    if (cpu().flagN()) {
        cpu().ssp += 4;
        return;
    }

    const m_byte armed = mem.readByte(enemy + 0x31);
    mem.writeByte(enemy + 0x31, static_cast<m_byte>(armed | 0x08u));
    if ((armed & 0x08u) == 0) {
        const m_word edge = (mem.readByte(enemy + 9) & 0x02u) != 0 ? static_cast<m_word>(0x0120u + grow)
                                                                    : static_cast<m_word>(0x0020u - grow);
        mem.writeWord(enemy + 0x60, static_cast<m_word>(mem.readWord(kCamX) + edge));

        SOR_CALL(sub_0104d8, 0x0000F4EEu, 0x000104D8u);
        mem.writeWord(enemy + 0x62, mem.readByte(0x0000F53Cu + (cpu().dw(0) & 0x07u)));
        SOR_CALL(sub_0104d8, 0x0000F502u, 0x000104D8u);
        const m_long entry = 0x0000DD68u + 4u * (cpu().dw(0) & 0x03u);
        mem.writeWord(enemy + 0x64, mem.readWord(entry));
        mem.writeWord(enemy + 0x0C, mem.readWord(entry + 2));
    }

    SOR_CALL(ordinary_enemy_approach_point, 0x0000F51Cu + 6u, 0x00009604u);
    if (!cpu().flagZ()) {
        const m_byte remaining = static_cast<m_byte>(mem.readByte(enemy + 0x50) - 1u);
        mem.writeByte(enemy + 0x50, remaining);
        if (remaining == 0) {
            mem.writeWord(enemy + 0x30, 0x0E00u);
            cpu().ssp += 4;
            return;
        }
        mem.writeByte(enemy + 0x31, static_cast<m_byte>(mem.readByte(enemy + 0x31) & ~0x08u));
    }
    ordinary_enemy_move_with_collision();
}
