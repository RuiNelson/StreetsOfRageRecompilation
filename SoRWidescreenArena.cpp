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
constexpr m_long kSpriteLinkIndex     = 0xFFFFFA02u;
constexpr m_long kP1Object            = 0xFFFFB800u;
constexpr m_long kP2Object            = 0xFFFFB880u;
constexpr m_long kObjectTable         = 0xFFFFB900u;

constexpr m_long kSpawnCursor        = 0xFFFFFC14u;
constexpr m_long kSpawnXBase        = 0xFFFFFB12u;
constexpr m_long kDifficultyCopy    = 0xFFFFFB5Bu;
constexpr m_long kPlayerModeCopy    = 0xFFFFFF38u;
constexpr m_long kClampStateTable  = 0x000045D4u;
constexpr m_long kTableTargetToggle = 0xFFFFFA17u;

m_long signExtendWord(m_long value) {
    return static_cast<m_long>(static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFFu)));
}

// The wave objects that walk into the screen: the ordinary enemies ($20-$2A) and the bosses. Everything else a
// wave column creates is scenery or a prop placed on the background (round 1's shutters $54, round 2's
// posters and pipes $13/$14, the breakables), which must stay where the ROM puts it.
bool walksIntoView(m_byte type) {
    if (type >= 0x20u && type <= 0x2Au)
        return true;
    return type == 0x30u || (type >= 0x55u && type <= 0x58u);
}

// An enemy that waits behind a shop front. Its personality (+$40 low nibble) picks its first state through the
// type's own table, and the state handler $DDE6 creates the shutter ($54) at the enemy's X: round 1's green
// shutters. Moving such an enemy moves its shutter off the background's doorway. Two types reach $DDE6:
// Garcia $22/$32 (personality table $DDDA, six entries, state table $DD80) and Nora $26 (personality table
// $F0F6, three entries, state table $10362).
constexpr m_long kShutterStateHandler = 0x0000DDE6u;

bool waitsBehindShutter(SystemMemory &mem, m_byte type, m_byte personality) {
    m_long personalities = 0;
    m_long states        = 0;
    m_byte count         = 0;
    if (type == 0x22u || type == 0x32u) {
        personalities = 0x0000DDDAu;
        states        = 0x0000DD80u;
        count         = 6;
    } else if (type == 0x26u) {
        personalities = 0x0000F0F6u;
        states        = 0x00010362u;
        count         = 3;
    } else {
        return false;
    }
    const m_byte index = static_cast<m_byte>(personality & 0x0Fu);
    if (index >= count)
        return false;
    const m_byte state = mem.readByte(personalities + index * 2u);
    return mem.readWord(states + state * 2u) == kShutterStateHandler;
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

void StreetsOfRage::object_on_screen_margin_test(m_long /*entry_*/) {
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

void StreetsOfRage::resolve_enemy_player_contact(m_long /*entry_*/) {
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

void StreetsOfRage::resolve_enemy_registered_attacker_contact(m_long /*entry_*/) {
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
void StreetsOfRage::abadede_state7_run_update(m_long /*entry_*/) {
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
void StreetsOfRage::jack_retreat_to_screen_edge(m_long /*entry_*/) {
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

// ---------------------------------------------------------------------------
// $00A59C — is this scripted object ready to be activated by the screen?
//
// d0 = 1, and the object stops waiting (+$01 bit 0 and +$40 bit 7 cleared), once it is on screen (`$97E6`,
// widened above); an object with +$40 bit 7 set instead waits for the camera's left edge to pass it. Round 8
// scrolls the other way: there the two are swapped, and the unforced test is the camera's right edge,
// cam+$144, which widescreen moves out to cam+$164.
// ---------------------------------------------------------------------------
void StreetsOfRage::ordinary_enemy_wait_until_on_screen(m_long /*entry_*/) {
    traceEnter(0x0000A59Cu);

    auto &mem = memory();
    const m_word grow   = vdp().widescreen() ? kArenaGrow : 0u;
    const m_long object = cpu().a[0];

    mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) | 0x01u));
    SOR_CALL(sub_00a56c, 0x0000A5A6u, 0x0000A56Cu);

    const auto finish = [this](m_word result) {
        cpu().d[0] = result;
        cpu().setNZClearVC(result, 0x80000000u);
        cpu().ssp += 4;
    };
    const auto activate = [&]() {
        mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) & ~0x01u));
        mem.writeByte(object + 0x40, static_cast<m_byte>(mem.readByte(object + 0x40) & ~0x80u));
        finish(1);
    };

    const m_word camX   = mem.readWord(kCamX);
    const m_word x      = mem.readWord(object + 0x10);
    const bool   round8 = mem.readWord(kLevel) == 7u;
    const bool   forced = (mem.readByte(object + 0x40) & 0x80u) != 0;

    if (round8 && !forced) {
        if (static_cast<std::int16_t>(camX + 0x0144u + grow) < static_cast<std::int16_t>(x))
            activate();
        else
            finish(0);
        return;
    }
    if (!round8 && forced) {
        if (camX < x)
            finish(0);
        else
            activate();
        return;
    }

    SOR_CALL(object_on_screen_margin_test, 0x0000A5BCu, 0x000097E6u);
    if (cpu().flagZ())
        finish(0);
    else
        activate();
}

// ---------------------------------------------------------------------------
// $007546 — round 8's thrown office table, state 0: wait hidden, then arm
//
// The table sits at its spawn point until the camera edge passes it: cam-$20 for variants 0-2 (they come from
// the left), cam+$160 for the others. Armed, it takes a launch speed from the per-variant table at $75DC and
// the lane and height of the player it targets. Both edges are outside the screen, so widescreen moves them
// out by $20.
// ---------------------------------------------------------------------------
void StreetsOfRage::round8_table_state0_wait_and_arm(m_long /*entry_*/) {
    traceEnter(0x00007546u);

    auto &mem = memory();
    const m_word grow   = vdp().widescreen() ? kArenaGrow : 0u;
    const m_long object = cpu().a[0];

    mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) | 0x01u));
    const m_word camX    = mem.readWord(kCamX);
    const m_byte variant = mem.readByte(object + 0x40);
    const m_word edge    = variant >= 3u ? static_cast<m_word>(camX + 0x0160u + grow)
                                         : static_cast<m_word>(camX - 0x0020u - grow);
    cpu().setDw(0, edge);
    if (!(edge < mem.readWord(object + 0x10))) {
        cpu().ssp += 4;
        return;
    }

    mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) & 0xFEu));
    cpu().d[0] = mem.readByte(object + 0x40);
    if (cpu().d[0] != 0u) {
        mem.writeByte(object + 0x34, 0x03u);
        cpu().d[7] = 0xFFFFFFCBu;
        SOR_CALL(sub_006aca, 0x00007582u, 0x00006ACAu);
        const m_long tableEntry = 0x000075DCu + 2u * (cpu().dw(0) & 0xFFFFu);
        mem.writeWord(object + 0x1C, mem.readWord(tableEntry));

        cpu().a[1] = kP1Object;
        cpu().a[2] = kP2Object;
        const m_byte previous = mem.readByte(kTableTargetToggle);
        mem.writeByte(kTableTargetToggle, static_cast<m_byte>(previous ^ 0x01u));
        if ((previous & 0x01u) != 0) {
            cpu().a[2] = kP1Object;
            cpu().a[1] = kP2Object;
        }
        if (mem.readByte(cpu().a[1]) == 0)
            cpu().a[1] = signExtendWord(cpu().a[2]);
        mem.writeWord(object + 0x14, mem.readWord(cpu().a[1] + 0x14));
        mem.writeWord(object + 0x18, mem.readWord(cpu().a[1] + 0x18));
    }

    mem.writeByte(object + 0x3A, static_cast<m_byte>(mem.readByte(object + 0x3A) | 0x01u));
    mem.writeWord(object + 0x18, 0x00A8u);
    cpu().a[1] = 0x0006ED72u;
    SOR_CALL(sub_006abc, 0x000075C8u, 0x00006ABCu);
    if (mem.readWord(object + 0x1C) != 0) {
        sub_0095ce();
        return;
    }
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $0040A6 — a player carried off the right edge during a police special
//
// While the police special runs, a player who ends up beyond cam+$160 (thrown off the screen) is put back
// down: the landing lane is looked up, the hold on the ground is cleared and the action restarts. The edge
// is the screen's own, so widescreen moves it to cam+$180. The flags it returns matter more than its effect: see the
// comment on the exits below.
// ---------------------------------------------------------------------------
void StreetsOfRage::police_special_player_update_gate(m_long /*entry_*/) {
    traceEnter(0x000040A6u);

    auto &mem = memory();
    const m_word grow   = vdp().widescreen() ? kArenaGrow : 0u;
    const m_long player = cpu().a[0];

    // The caller (update_player_object) skips the rest of the player's update on `bne` after this routine, so
    // the Z flag at each exit is part of its result: while a police special runs, every exit but the last
    // leaves Z clear and the player's input is ignored; with no special running, Z is set.
    if (mem.readByte(kPoliceSpecialActive) == 0) {
        cpu().setNZClearVC(0, 0x80u);
        cpu().ssp += 4;
        return;
    }
    const m_long verticalVelocity = mem.readLong(player + 0x24);
    if (verticalVelocity != 0) {
        cpu().setNZClearVC(verticalVelocity, 0x80000000u);
        cpu().ssp += 4;
        return;
    }
    if ((mem.readByte(player + 0x59) & 0x02u) != 0) {
        cpu().setFlagZ(false);
        cpu().ssp += 4;
        return;
    }
    const m_word x    = mem.readWord(player + 0x10);
    const m_word edge = static_cast<m_word>(mem.readWord(kCamX) + 0x0160u + grow);
    const m_word diff = static_cast<m_word>(x - edge);
    const bool   carry    = x < edge;
    const bool   overflow = (((x ^ edge) & (x ^ diff)) & 0x8000u) != 0;
    cpu().setNZVC(diff, 0x8000u, overflow, carry);
    if (carry) {
        cpu().ssp += 4;
        return;
    }

    SOR_CALL(sub_00ad2a, 0x000040B2u, 0x0000AD2Au);
    mem.writeWord(player + 0x18, cpu().dw(6));
    SOR_CALL(sub_00b228, 0x000040BAu, 0x0000B228u);
    mem.writeByte(player + 0x59, static_cast<m_byte>(mem.readByte(player + 0x59) & 0xFDu));

    const m_byte action = static_cast<m_byte>(mem.readByte(player + 0x30) & 0xFEu);
    if (action >= 4u) {
        cpu().d[0] = 2;
        if (mem.readByte(player + 0x60) == 0) {
            SOR_CALL(sub_002ee8, 0x000040FCu, 0x00002EE8u);
        } else {
            cpu().d[0] = 0x30;
            SOR_CALL(sub_002de6, 0x000040F8u, 0x00002DE6u);
        }
        SOR_CALL(sub_004104, 0x00004100u, 0x00004104u);
    }
    cpu().d[7] = 0xFFFFFFFFu;
    cpu().setNZClearVC(cpu().d[7], 0x80000000u);
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $00B0FE — cull_sprite_piece_x
//
// A mapped sprite piece (d0 = its biased hardware X) is kept only within [$60, $1E0); otherwise the SAT record
// reserved for it is rolled back (link index - 1, a2 - 6) and d5 = -1 (Z clear). $60 is where a piece up to 32
// pixels wide is wholly left of a 320-pixel screen (hardware left edge $80). Widescreen's screen starts at
// $60, so a piece starting up to 32 pixels further left is still partly visible: the lower bound moves to $40,
// or sprites vanish while still showing at the far left edge. The upper bound, $1E0, is already widescreen's
// right edge.
// ---------------------------------------------------------------------------
void StreetsOfRage::cull_sprite_piece_x(m_long /*entry_*/) {
    traceEnter(0x0000B0FEu);

    const m_word lower = vdp().widescreen() ? 0x0040u : 0x0060u;
    const m_word x     = cpu().dw(0);

    if (x >= lower && x < 0x01E0u) {
        cpu().d[5] = 0;
        cpu().setNZClearVC(0, 0x80000000u);
    } else {
        memory().writeByte(kSpriteLinkIndex, static_cast<m_byte>(memory().readByte(kSpriteLinkIndex) - 1u));
        cpu().a[2] -= 6;
        cpu().d[5] = 0xFFFFFFFFu;
        cpu().setNZClearVC(0xFFFFFFFFu, 0x80000000u);
    }
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $000CC0 — spawn_object_batch
//
// Spawns one column of a wave from the ELC stream: a signed word count - 1, then six-byte records
// (type, +$40, +$41, +$49, X low byte, +$15), the X high byte coming from spawn_x_base. X is a world
// coordinate, so where the wave lands depends on the camera. An object is dormant until the screen test
// ($97E6, widened) accepts it, so one created just past the screen's right edge waits, unseen, until the
// camera brings it in. In widescreen that edge moved out by $20, and an object created within the new
// visible band would be accepted at once and appear abruptly, with the enemy standing inside the screen. So
// under --widescreen an enemy or boss created beyond the original screen edge is placed $20 further out
// (the same distance past the widened edge as it was past the original), and likewise on the left. Scenery
// and props keep their X (walksIntoView), and so does an enemy waiting behind a shop front
// (waitsBehindShutter), whose shutter is created at its X: they belong to the background, and moving them
// showed round 1's shutters and round 2's posters and pipes 32 pixels off their place in it. Objects
// gated by the camera keep their X: they wake when the camera passes it, and the camera's lock at the
// wave gate is the same in both modes. Which objects those are depends on the round ($A59C): +$40 bit 7
// set in rounds 1-7, bit 7 clear in round 8, which scrolls left.
// ---------------------------------------------------------------------------
void StreetsOfRage::spawn_object_batch(m_long /*entry_*/) {
    traceEnter(0x00000CC0u);

    auto &mem = memory();
    const m_word grow = vdp().widescreen() ? kArenaGrow : 0u;

    m_long record = mem.readLong(kSpawnCursor);
    std::int16_t remaining = static_cast<std::int16_t>(mem.readWord(record));
    record += 2;

    if (remaining >= 0) {
        for (;; --remaining) {
            SOR_CALL(find_free_object_slot, 0x00000CCEu, 0x00000E46u);
            if (!cpu().flagZ()) {
                // No free slot: the ROM restarts the console here.
                init();
                return;
            }

            const m_long object = cpu().a[1];
            m_byte       type   = mem.readByte(record);
            record += 1;
            const m_byte difficulty = static_cast<m_byte>(mem.readByte(record + 4) & 0x03u);

            bool skip = difficulty > mem.readByte(kDifficultyCopy);
            if (!skip && (type & 0x80u) != 0 && mem.readByte(kPlayerModeCopy) != 3u)
                skip = true;
            type = static_cast<m_byte>(type & 0x7Fu);

            if (skip) {
                record += 5;
            } else {
                mem.writeByte(object, type);
                mem.writeByte(object + 0x40, mem.readByte(record++));
                mem.writeByte(object + 0x41, mem.readByte(record++));
                mem.writeByte(object + 0x49, mem.readByte(record++));
                m_word x = static_cast<m_word>((mem.readWord(kSpawnXBase) & 0xFF00u) | mem.readByte(record++));
                // A camera-gated object waits for the camera to pass its X ($A59C), not for the screen test,
                // and the camera stops at the wave's lock. Moving it out by $20 can put it beyond the lock,
                // where it never wakes (and the next section's art check, $884, resets the console on it).
                // That is +$40 bit 7 set (cam_x passing it), except in round 8, where it is bit 7 clear
                // (cam_x+$144 passing it): round 8's last wave left a HakuRo asleep at the lock that way.
                const m_byte personality = mem.readByte(object + 0x40);
                const bool   forced      = (personality & 0x80u) != 0;
                const bool   cameraGated = mem.readWord(kLevel) == 7u ? !forced : forced;
                if (grow != 0 && walksIntoView(type) && !cameraGated
                    && !waitsBehindShutter(mem, type, personality)) {
                    const std::int16_t rel = static_cast<std::int16_t>(x - mem.readWord(kCamX));
                    if (rel >= 0x0140)
                        x = static_cast<m_word>(x + grow);
                    else if (rel < 0)
                        x = static_cast<m_word>(x - grow);
                }
                mem.writeWord(object + 0x10, x);
                mem.writeByte(object + 0x15, mem.readByte(record++));
                cpu().a[0] = signExtendWord(object);

                const m_word level = mem.readWord(kLevel);
                if (level == 4u)
                    mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) | 0x10u));
                cpu().setDw(6, 0x0088u);
                if (level != 6u) {
                    const m_long saved = cpu().d[2];
                    SOR_CALL(sub_00ad2a, 0x00000D36u, 0x0000AD2Au);
                    cpu().d[2] = saved;
                }
                mem.writeWord(object + 0x18, cpu().dw(6));

                const m_byte slotType = static_cast<m_byte>(mem.readByte(object) - 0x20u);
                if (slotType < 0x0Bu && (slotType & 0x80u) == 0) {
                    mem.writeByte(object + 1, static_cast<m_byte>(mem.readByte(object + 1) | 0x08u));
                    mem.writeWord(kActiveProgression, static_cast<m_word>(mem.readWord(kActiveProgression) + 1u));
                }
            }
            if (remaining == 0)
                break;
        }
    }

    mem.writeLong(kSpawnCursor, record);
    cpu().a[2] = record;
    cpu().ssp += 4;
}
