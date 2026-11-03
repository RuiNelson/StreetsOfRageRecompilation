#include "SoRWidescreenArena.hpp"

#include "SoRCpuOps.hpp"

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
constexpr m_long kBlockmapStride      = 0xFFFFE02Eu;
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


// ---------------------------------------------------------------------------
// $0043AA — clamp_players_to_gameplay_bounds
//
// Keeps each active player inside the camera window (X) and the lane range (Y) during gameplay. The X
// window is [cam+$20, cam+$120]; widescreen opens it to [cam, cam+$140], so the players can reach the
// 32-pixel margin of the wider screen on both sides. Also owns the inner entry $43D2.
//
// The right edge never passes the collision map's last pixel (stride * 16 - 1). At a round's end the camera
// stops with the map's edge at the 320-pixel screen's edge (round 5: cam $13C0, map $1500 wide), so cam+$140
// is the first X off the map; the floor probe ($AD30) has no width check and reads the next row's first
// column there, which on round 5's bottom lane is class 0: an invisible hole.
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
                const m_word stride = mem.readWord(kBlockmapStride);
                if (grow != 0 && stride != 0) {
                    const std::int16_t mapEnd = static_cast<std::int16_t>(stride * 16u - 1u);
                    if (mapEnd < edge)
                        edge = mapEnd;
                }
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
        SOR_CALL(probe_floor_at_object, 0x00000CA2u, 0x0000AD2Au);
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
        test_shape_box_overlap();
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
            SOR_CALL(test_attacker_shape_box_overlap, 0x0000ABB6u, 0x0000ABF8u);
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
    SOR_CALL(test_attacker_shape_box_overlap, 0x0000ABDCu, 0x0000ABF8u);
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

    SOR_CALL(abadede_contact_reaction, 0x00014C3Eu, 0x000154E0u);
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

    SOR_CALL(integrate_object_velocity, 0x00014C54u, 0x0000B20Eu);
    cpu().d[0] = 0x01;
    SOR_CALL(abadede_sync_linked_sprite, 0x00014C5Cu, 0x0001569Cu);

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
    SOR_CALL(abadede_steer_velocity_toward_target, 0x00014C80u, 0x0001566Eu);
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
        SOR_CALL(enemy_set_action_facing_target, 0x0000F4B4u, 0x000096C0u);
        mem.writeByte(enemy + 1, static_cast<m_byte>(mem.readByte(enemy + 1) | 0x04u));
        mem.writeByte(enemy + 0x50, 0x08u);
    }

    SOR_CALL(ordinary_enemy_contact_dispatch, 0x0000F4C4u, 0x0000A9BAu);
    if (cpu().flagN()) {
        cpu().ssp += 4;
        return;
    }
    SOR_CALL(jack_abort_retreat_if_target_near, 0x0000F4CAu, 0x0000F482u);
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

        SOR_CALL(rng_draw, 0x0000F4EEu, 0x000104D8u);
        mem.writeWord(enemy + 0x62, mem.readByte(0x0000F53Cu + (cpu().dw(0) & 0x07u)));
        SOR_CALL(rng_draw, 0x0000F502u, 0x000104D8u);
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
    SOR_CALL(object_update_screen_position, 0x0000A5A6u, 0x0000A56Cu);

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
        // At a wave's lock (max - min < 4, as $6A6 tests it) the camera scrolls no further, so the widened edge
        // would leave an object between it and the ROM's edge asleep for good, and the next section's art check
        // ($884) resets the console on it: round 8's lock at cam_x $1100 with a Garcia at $1260 (cam+$160) in 2P.
        // There the ROM's edge is used.
        const bool   locked = static_cast<m_word>(mem.readWord(kCameraXMax) - mem.readWord(kCameraXMin)) < 4u;
        const m_word edge   = static_cast<m_word>(camX + 0x0144u + (locked ? 0u : grow));
        if (static_cast<std::int16_t>(edge) < static_cast<std::int16_t>(x))
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
        SOR_CALL(queue_object_sound_id, 0x00007582u, 0x00006ACAu);
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
    SOR_CALL(object_start_animation_with_flag_bit6, 0x000075C8u, 0x00006ABCu);
    if (mem.readWord(object + 0x1C) != 0) {
        register_enemy_attacker();
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

    SOR_CALL(probe_floor_at_object, 0x000040B2u, 0x0000AD2Au);
    mem.writeWord(player + 0x18, cpu().dw(6));
    SOR_CALL(object_clear_velocity, 0x000040BAu, 0x0000B228u);
    mem.writeByte(player + 0x59, static_cast<m_byte>(mem.readByte(player + 0x59) & 0xFDu));

    const m_byte action = static_cast<m_byte>(mem.readByte(player + 0x30) & 0xFEu);
    if (action >= 4u) {
        cpu().d[0] = 2;
        if (mem.readByte(player + 0x60) == 0) {
            SOR_CALL(player_set_action_faced_restore_frame, 0x000040FCu, 0x00002EE8u);
        } else {
            cpu().d[0] = 0x30;
            SOR_CALL(player_set_action_faced_weapon_adjust, 0x000040F8u, 0x00002DE6u);
        }
        SOR_CALL(player_clear_combat_fields, 0x00004100u, 0x00004104u);
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
                    SOR_CALL(probe_floor_at_object, 0x00000D36u, 0x0000AD2Au);
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

// ---------------------------------------------------------------------------
// The "ROUND n" banner (level intro substates 0, 2 and 6)
//
// The banner is eleven hardware sprites at the head of the sprite buffer $FFDA10, copied there at round start
// from the ROM template $1079A: five on the left ("ROUND"), the round number in the middle, five on the right.
// Substate 2 ($11A76) slides the halves in by 4 pixels a frame and drops the number by 4, until the last right
// sprite is below $178 (35 frames); substate 6 ($11AC4) slides them out by 6 and lifts the number by 6, until
// that sprite reaches $200 (24 frames). The template's halves sit just outside the 320-pixel sprite window
// ([$80, $1C0) in raw X), and so do the positions they slide out to, but inside widescreen's [$60, $1E0): the
// letters showed in the side margins during the fade-in, and again after sliding out. Under --widescreen the
// halves start and end kArenaGrow further out and cover the extra distance in the same number of frames; the
// number's Y, the frame counts and the substate changes are the ROM's, so the intro's timing is unchanged. The
// step is read from the number sprite's Y, which moves the same in both modes.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kBannerSprites  = 0xFFFFDA10u;
constexpr m_long kBannerTemplate = 0x0001079Au;
constexpr m_long kIntroSubstate  = 0xFFFFFB48u;
constexpr m_long kIntroTimer     = 0xFFFFFB4Au;
constexpr int    kBannerHalf     = 5; // sprites 0-4 left, 5 the number, 6-10 right

m_long bannerSprite(int index) { return kBannerSprites + static_cast<m_long>(index) * 8u; }
m_long bannerTemplate(int index) { return kBannerTemplate + static_cast<m_long>(index) * 8u; }

// Frames the ROM takes to slide the banner in ($11A76's stop test) and out ($11AC4's).
int bannerSlideInFrames(SystemMemory &mem) {
    int    frames = 0;
    m_word x      = mem.readWord(bannerTemplate(2 * kBannerHalf) + 6);
    do {
        x = static_cast<m_word>(x - 4u);
        ++frames;
    } while (x >= 0x0178u);
    return frames;
}

int bannerSlideOutFrames(SystemMemory &mem) {
    int    frames = 0;
    m_word x      = static_cast<m_word>(mem.readWord(bannerTemplate(2 * kBannerHalf) + 6) - 4u * bannerSlideInFrames(mem));
    do {
        x = static_cast<m_word>(x + 6u);
        ++frames;
    } while (x < 0x0200u);
    return frames;
}

// A sprite whose 9-bit X is 0 is not drawn but masks the sprites after it on its lines: the template's last right
// sprite ($200) is one. It keeps the ROM's X so the mask stays.
m_word widenBannerX(m_word x, int extra) {
    return (x & 0x01FFu) == 0 ? x : static_cast<m_word>(x + extra);
}

// Writes the halves' X for a banner `travel` pixels in from the template, each half `extra` further out.
void placeBannerHalves(SystemMemory &mem, int travel, int extra) {
    for (int i = 0; i < kBannerHalf; ++i) {
        const m_word left  = static_cast<m_word>(mem.readWord(bannerTemplate(i) + 6) + travel);
        const m_word right = static_cast<m_word>(mem.readWord(bannerTemplate(kBannerHalf + 1 + i) + 6) - travel);
        mem.writeWord(bannerSprite(i) + 6, widenBannerX(left, -extra));
        mem.writeWord(bannerSprite(kBannerHalf + 1 + i) + 6, widenBannerX(right, extra));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// $011A68 — level_intro_wait_fade_in
//
// Intro substate 0: fades palette colours 0-16 in ($105CE) and advances when the fade counter runs out. The
// banner waits at its template position meanwhile; widescreen moves its halves out of the side margins.
// ---------------------------------------------------------------------------
void StreetsOfRage::level_intro_wait_fade_in(m_long /*entry_*/) {
    traceEnter(0x00011A68u);

    if (vdp().widescreen())
        placeBannerHalves(memory(), 0, kArenaGrow);

    cpu().d[6] = 0x10u;
    SOR_CALL(palette_fade_step_to_target, 0x00011A6Eu, 0x000105CEu);
    if (cpu().eq()) {
        const m_word substate = memory().readWord(kIntroSubstate);
        const m_word next     = static_cast<m_word>(substate + 2u);
        memory().writeWord(kIntroSubstate, next);
        const bool carry = next < substate;
        cpu().setNZVCX(next, 0x8000u, ((~(substate ^ 2u) & (substate ^ next)) & 0x8000u) != 0, carry, carry);
    }
    cpu().ssp += 4;
}

// ---------------------------------------------------------------------------
// $011A76 — level_intro_banner_slide_in
//
// Intro substate 2: left half +4, number Y +4, right half -4; once the last right sprite is below $178, waits
// $60 frames (substate 4) and jumps to play_level_music.
// ---------------------------------------------------------------------------
void StreetsOfRage::level_intro_banner_slide_in(m_long /*entry_*/) {
    traceEnter(0x00011A76u);

    auto      &mem = memory();
    const bool wide = vdp().widescreen();

    const m_long number = bannerSprite(kBannerHalf);
    mem.writeWord(number, static_cast<m_word>(mem.readWord(number) + 4u));
    m_word last = 0;
    if (wide) {
        const int frames = bannerSlideInFrames(mem);
        const int step   = (mem.readWord(number) - mem.readWord(bannerTemplate(kBannerHalf))) / 4;
        placeBannerHalves(mem, 4 * step, kArenaGrow * (frames - step) / frames);
        last = static_cast<m_word>(mem.readWord(bannerTemplate(2 * kBannerHalf) + 6) - 4 * step);
    } else {
        for (int i = 0; i < kBannerHalf; ++i) {
            const m_long left  = bannerSprite(i) + 6;
            const m_long right = bannerSprite(kBannerHalf + 1 + i) + 6;
            mem.writeWord(left, static_cast<m_word>(mem.readWord(left) + 4u));
            mem.writeWord(right, static_cast<m_word>(mem.readWord(right) - 4u));
        }
        last = mem.readWord(bannerSprite(2 * kBannerHalf) + 6);
    }
    cpu().a[1] = bannerSprite(2 * kBannerHalf + 1);
    cpu().setDw(7, 0xFFFFu);
    cpu().setFlag(CPU68K::FlagX, static_cast<m_word>(last + 4u) >= 4u);

    const m_word diff = static_cast<m_word>(last - 0x0178u);
    cpu().setNZVC(diff, 0x8000u, (((last ^ 0x0178u) & (last ^ diff)) & 0x8000u) != 0, last < 0x0178u);
    if (last >= 0x0178u) {
        cpu().ssp += 4;
        return;
    }
    mem.writeWord(kIntroSubstate, static_cast<m_word>(mem.readWord(kIntroSubstate) + 2u));
    cpu().setFlag(CPU68K::FlagX, false);
    mem.writeWord(kIntroTimer, 0x0060u);
    cpu().setNZClearVC(0x60u, 0x8000u);
    play_level_music(0x00011B12u);
}

// ---------------------------------------------------------------------------
// $011AC4 — level_intro_banner_slide_out
//
// Intro substate 6: left half -6, number Y -6, right half +6; once the last right sprite reaches $200, waits
// $30 frames (substate 8) before level_intro_finish.
// ---------------------------------------------------------------------------
void StreetsOfRage::level_intro_banner_slide_out(m_long /*entry_*/) {
    traceEnter(0x00011AC4u);

    auto      &mem = memory();
    const bool wide = vdp().widescreen();

    const m_long number = bannerSprite(kBannerHalf);
    mem.writeWord(number, static_cast<m_word>(mem.readWord(number) - 6u));
    m_word last = 0;
    if (wide) {
        const int in     = bannerSlideInFrames(mem);
        const int frames = bannerSlideOutFrames(mem);
        const int top    = mem.readWord(bannerTemplate(kBannerHalf)) + 4 * in;
        const int step   = (top - mem.readWord(number)) / 6;
        placeBannerHalves(mem, 4 * in - 6 * step, kArenaGrow * step / frames);
        last = static_cast<m_word>(mem.readWord(bannerTemplate(2 * kBannerHalf) + 6) - 4 * in + 6 * step);
    } else {
        for (int i = 0; i < kBannerHalf; ++i) {
            const m_long left  = bannerSprite(i) + 6;
            const m_long right = bannerSprite(kBannerHalf + 1 + i) + 6;
            mem.writeWord(left, static_cast<m_word>(mem.readWord(left) - 6u));
            mem.writeWord(right, static_cast<m_word>(mem.readWord(right) + 6u));
        }
        last = mem.readWord(bannerSprite(2 * kBannerHalf) + 6);
    }
    cpu().a[1] = bannerSprite(2 * kBannerHalf + 1);
    cpu().setDw(7, 0xFFFFu);
    cpu().setFlag(CPU68K::FlagX, last < 6u);

    const m_word diff = static_cast<m_word>(last - 0x0200u);
    cpu().setNZVC(diff, 0x8000u, (((last ^ 0x0200u) & (last ^ diff)) & 0x8000u) != 0, last < 0x0200u);
    if (last >= 0x0200u) {
        mem.writeWord(kIntroSubstate, static_cast<m_word>(mem.readWord(kIntroSubstate) + 2u));
        cpu().setFlag(CPU68K::FlagX, false);
        mem.writeWord(kIntroTimer, 0x0030u);
        cpu().setNZClearVC(0x30u, 0x8000u);
    }
    cpu().ssp += 4;
}

namespace {

constexpr m_long kPrimaryCamera = 0xFFFFE000u;

// Widescreen's Plane A past the map's right end (world X x >= the map's pixel width): the pixel column (within the
// map) to draw there instead, or -1 to keep the ROM's wrap. Round 5 (level 4) repeats the last block, 2 tiles
// twice. Round 8 (level 7) continues the corridor's wall, whose pillars repeat every 18 blocks: the map ends on a
// pillar, so the margin gets the blocks that follow the previous one.
int wideMarginColumn(m_word level, int x, int width) {
    if (x < width)
        return -1;
    if (level == 4u)
        return width - 0x10;
    if (level == 7u) {
        while (x >= width)
            x -= 0x120;
        return x;
    }
    return -1;
}

// Widescreen's Plane A left of the map's drawn part: round 8 (level 7) ends with the camera at $80, and the throne
// room is drawn from X $80 (block 8) on, the blocks before it empty: the left margin was a hole onto Plane B's city.
// The room is symmetric about the screen's centre, X $120, from the statues' bases (blocks $B-$C and $17-$18) inwards,
// but the ROM's left wall has one tile more between the base and the wall lamp than the right one. So every block
// left of the left base (X < $B0) is drawn as the mirror (H-flipped) of the block at $230 - X: the margin is filled
// and the walls match (user). Returns that block's pixel column, or -1.
int wideMirrorColumn(m_word level, int x) {
    if (level == 7u && x >= 0 && x < 0xB0)
        return 0x230 - (x & ~0x0F);
    return -1;
}

// A metatile's top (or bottom) long, its two name-table words swapped and H-flipped.
m_long mirrorMetatileLong(m_long pair) {
    return ((pair << 16) | (pair >> 16)) ^ 0x08000800u;
}

} // namespace

// ---------------------------------------------------------------------------
// $01953A — build_tilemap_column_update
//
// Queues at a4 one column of the strip a6 from block map cell a5 (d2 the VDP command): [command, 2h+1, then per block
// its top and bottom longs], stepping a5 by the stride. In widescreen, Plane A's column comes from wideMirrorColumn
// when it has one (its X is a5's column in the block map at a1+$2A).
// ---------------------------------------------------------------------------
void StreetsOfRage::build_tilemap_column_update(m_long /*entry_*/) {
    traceEnter(0x0001953Au);
    auto &cpu = this->cpu();
    auto &mem = memory();
    mem.writeWord(cpu.a[4], cpu.dw(2));
    cpu.a[4] += 2;
    cpu.setDw(2, mem.readWord(cpu.a[6]));
    if (mem.readWord(cpu.a[1] + 0x42u) == 2u)
        cpu.d[2] = 0x0Fu;
    cpu.setDw(0, static_cast<m_word>(cpu.dw(2) * 2u + 1u));
    mem.writeWord(cpu.a[4], cpu.dw(0));
    cpu.a[4] += 2;
    cpu.a[6] = mem.readLong(cpu.a[1] + 0x32u);
    cpu.setDw(1, mem.readWord(cpu.a[1] + 0x2Eu));

    m_long mirrorOffset = 0;
    if (vdp().widescreen() && cpu.a[1] == kPrimaryCamera && cpu.dw(1) != 0) {
        const m_long cell   = (cpu.a[5] - mem.readLong(cpu.a[1] + 0x2Au)) & 0x00FFFFFFu;
        const int    x      = static_cast<int>(cell % cpu.dw(1)) * 0x10;
        const int    column = wideMirrorColumn(mem.readWord(kLevel), x);
        if (column >= 0)
            mirrorOffset = static_cast<m_long>((column - x) >> 4);
    }
    do {
        cpu.d[0] = static_cast<m_long>(mem.readByte(cpu.a[5] + mirrorOffset)) << 3;
        m_long top    = mem.readLong(cpu.a[6] + cpu.d[0]);
        m_long bottom = mem.readLong(cpu.a[6] + cpu.d[0] + 4u);
        if (mirrorOffset != 0) {
            top    = mirrorMetatileLong(top);
            bottom = mirrorMetatileLong(bottom);
        }
        mem.writeLong(cpu.a[4], top);
        mem.writeLong(cpu.a[4] + 4u, bottom);
        cpu.a[4] += 8;
        cpu.a[5] += signExtendWord(cpu.dw(1));
        cpu.setDw(2, static_cast<m_word>(cpu.dw(2) - 1u));
    } while (cpu.dw(2) != 0xFFFFu);
    cpu.ssp += 4;
}

// ---------------------------------------------------------------------------
// $019656 — resolve_blockmap_and_vdp_address
//
// World (d0 x, d1 y) of a strip a6 of the plane a1, with the strip's moduli (d4 width, d5 height) and first block
// row d6: a5 = the block map cell (a2 + ((y mod H) / 16 + row) * stride + (x mod W) / 16), d2 = the wrapped
// name-table write command (Plane A at a1 = $E000, Plane B otherwise). The map wraps, so the column past its
// right edge is the map's first. Round 5 ends, and round 8 starts, with the map's edge at the 320-pixel screen's
// edge, so widescreen's right margin showed the start of the map (round 5: a wooden wall with a porthole; round 8:
// nothing, a hole onto Plane B's city). There Plane A columns past the map's end are wideMarginColumn's (user).
// ---------------------------------------------------------------------------
void StreetsOfRage::resolve_blockmap_and_vdp_address(m_long /*entry_*/) {
    traceEnter(0x00019656u);

    auto        &cpu = this->cpu();
    auto        &mem = memory();
    const m_word x   = cpu.dw(0);
    const m_word y   = cpu.dw(1);
    const auto   divs = [&cpu](int reg, m_word value, m_word divisor) {
        const std::int32_t dividend  = static_cast<std::int16_t>(value);
        const std::int32_t by        = static_cast<std::int16_t>(divisor);
        const m_word       quotient  = static_cast<m_word>(dividend / by);
        const m_word       remainder = static_cast<m_word>(dividend % by);
        cpu.d[reg]                   = (static_cast<m_long>(remainder) << 16) | quotient;
        cpu.d[reg]                   = (cpu.d[reg] << 16) | (cpu.d[reg] >> 16); // swap
    };
    cpu.setDw(2, x);
    cpu.setDw(3, y);
    divs(0, x, cpu.dw(4));
    divs(1, y, cpu.dw(5));

    if (vdp().widescreen() && cpu.a[1] == kPrimaryCamera) {
        const int column = wideMarginColumn(mem.readWord(kLevel), static_cast<std::int16_t>(x),
                                            static_cast<std::int16_t>(cpu.dw(4)));
        if (column >= 0)
            cpu.setDw(0, static_cast<m_word>(column));
    }

    cpu.a[5] = cpu.a[2];
    cpu.setDw(1, static_cast<m_word>((cpu.dw(1) >> 4) + cpu.dw(6)));
    cpu.d[1] = static_cast<m_long>(cpu.dw(1)) * mem.readWord(cpu.a[1] + 0x2Eu);
    cpu.setDw(0, static_cast<m_word>((cpu.dw(0) >> 4) + cpu.dw(1)));
    cpu.a[5] += signExtendWord(cpu.dw(0));

    cpu.setDw(6, static_cast<m_word>(cpu.dw(6) << 4));
    m_word command = static_cast<m_word>((((cpu.dw(3) + cpu.dw(6)) & 0x00FFu) << 4) & 0xFF00u);
    cpu.setDw(3, command);
    command = static_cast<m_word>((((x & 0x01FFu) >> 2) + command) & 0x0FFCu);
    command |= cpu.a[1] == kPrimaryCamera ? 0x4000u : 0x6000u;
    cpu.setDw(2, command);
    cpu.setNZClearVC(command, 0x8000u);
    cpu.ssp += 4;
}

// ---------------------------------------------------------------------------
// $019AF4 — initialize_visible_tilemap_plane
//
// The initial draw of one plane (a1 its camera, a2 its block map, a3 its row queue), inside the display-off load:
// for each block row on screen, a 32-block row from the camera's X - $60 is queued and flushed at once through
// $196A4 (dispatch_background_vblank_upload), inlined here (a reimplemented routine calls only reimplemented
// routines). The ROM reads the 32 blocks in one run, so a row past the map's right end reads the next row's first
// blocks: in widescreen those of Plane A come from wideMarginColumn, like the streamed columns ($19656). Round 8
// starts with the map's end at the 320-pixel screen's edge, and that row is the only draw of its margin. Blocks with
// a wideMirrorColumn are drawn mirrored, like the streamed columns ($1953A).
// ---------------------------------------------------------------------------
void StreetsOfRage::initialize_visible_tilemap_plane(m_long /*entry_*/) {
    traceEnter(0x00019AF4u);
    constexpr m_long kVdpControl          = 0x00C00004u;
    constexpr m_long kVdpData             = 0x00C00000u;
    constexpr m_long kZ80BusRequest       = 0x00A11100u;
    constexpr m_long kDmaCommandScratch   = 0xFFFFFC00u;
    constexpr m_long kSecondaryCamera     = 0xFFFFE100u;
    constexpr m_long kPrimaryRowQueue     = 0xFFFFEA00u;
    constexpr m_long kSecondaryRowQueue   = 0xFFFFEB20u;
    constexpr m_long kPrimaryColumnQueue  = 0xFFFFEC40u;
    constexpr m_long kSecondaryColumnQueue = 0xFFFFECE0u;
    constexpr m_long kUploadJumpTable     = 0x000196B2u;

    auto      &cpu  = this->cpu();
    auto      &mem  = memory();
    const bool wide = vdp().widescreen();

    // $196CA / $19758: one horizontal-scroll DMA (registers $8F, $93-$97, then the command through the scratch).
    const auto dmaHscroll = [&](m_word step, m_long length, m_word sourceMid, m_long command) {
        mem.writeWord(kZ80BusRequest, 0x0100u);
        cpu.a[6] = kVdpControl;
        mem.writeWord(kVdpControl, step);
        mem.writeLong(kVdpControl, length);
        mem.writeWord(kVdpControl, 0x9500u);
        mem.writeWord(kVdpControl, sourceMid);
        mem.writeWord(kVdpControl, 0x977Fu);
        mem.writeLong(kDmaCommandScratch, command);
        mem.writeWord(kVdpControl, mem.readWord(kDmaCommandScratch));
        mem.writeWord(kVdpControl, mem.readWord(kDmaCommandScratch + 2u));
        mem.writeWord(kVdpControl, 0x8F02u);
        mem.writeWord(kZ80BusRequest, 0x0000u);
    };
    // $19800 flush_one_tilemap_update_queue: a2 the camera, a3 its row queue, a4 its column queue.
    const auto flushOne = [&] {
        traceEnter(0x00019800u);
        const m_long control = cpu.a[0];
        const m_long data    = cpu.a[1];
        for (;;) {
            cpu.setDw(0, mem.readWord(cpu.a[4]));
            if (cpu.dw(0) == 0)
                break;
            mem.writeWord(cpu.a[4], 0);
            cpu.a[4] += 2;
            cpu.setDw(7, mem.readWord(cpu.a[4]));
            cpu.a[4] += 2;
            do {
                mem.writeWord(control, cpu.dw(0));
                mem.writeWord(control, cpu.dw(4));
                mem.writeLong(data, mem.readLong(cpu.a[4]));
                cpu.a[4] += 4;
                cpu.setDw(0, static_cast<m_word>((cpu.dw(0) + cpu.dw(2)) & cpu.dw(3)));
                cpu.setDw(7, static_cast<m_word>(cpu.dw(7) - 1u));
            } while (cpu.dw(7) != 0xFFFFu);
        }
        for (;;) {
            cpu.setDw(0, mem.readWord(cpu.a[3]));
            if (cpu.dw(0) == 0)
                break;
            mem.writeWord(cpu.a[3], 0);
            cpu.a[3] += 2;
            cpu.setDw(7, mem.readWord(cpu.a[3]));
            cpu.a[3] += 2;
            if (cpu.dw(7) == 0)
                break;
            cpu.a[4] = cpu.a[3] + 0x80u;
            cpu.setDw(1, cpu.dw(0));
            do {
                mem.writeWord(control, cpu.dw(1));
                mem.writeWord(control, cpu.dw(4));
                mem.writeLong(data, mem.readLong(cpu.a[3]));
                cpu.a[3] += 4;
                cpu.setDw(6, static_cast<m_word>(cpu.dw(1) + cpu.dw(2)));
                mem.writeWord(control, cpu.dw(6));
                mem.writeWord(control, cpu.dw(4));
                mem.writeLong(data, mem.readLong(cpu.a[4]));
                cpu.a[4] += 4;
                cpu.setDw(1, static_cast<m_word>((cpu.dw(1) + 4u) & cpu.dw(5)));
                cpu.setDw(7, static_cast<m_word>(cpu.dw(7) - 1u));
            } while (cpu.dw(7) != 0xFFFFu);
            cpu.a[3] = cpu.a[4];
        }
    };
    // $196A4 dispatch_background_vblank_upload, by the primary camera's state, with a0/a1 the VDP ports.
    const auto upload = [&](m_long retPc) {
        logCallFromReturn(retPc, 0x000196A4u);
        traceEnter(0x000196A4u);
        cpu.setDw(0, static_cast<m_word>(mem.readWord(kPrimaryCamera) * 2u));
        cpu.setDw(0, mem.readWord(kUploadJumpTable + signExtendWord(cpu.dw(0))));
        m_long target = kUploadJumpTable + signExtendWord(cpu.dw(0));
        traceEnter(target);
        if (target == 0x000196CAu) {
            if (mem.readWord(kLevel) == 2u) {
                target = 0x00019758u;
            } else {
                dmaHscroll(0x8F20u, 0x93209400u, 0x96A8u, 0x74000083u);
                dmaHscroll(0x8F20u, 0x93209400u, 0x96AAu, 0x74020083u);
                target = 0x000197D8u;
            }
        }
        if (target == 0x00019758u) {
            dmaHscroll(0x8F04u, 0x93009401u, 0x96ADu, 0x74000083u);
            dmaHscroll(0x8F04u, 0x93009401u, 0x96ACu, 0x74020083u);
            target = 0x000197D8u;
        }
        cpu.setDw(2, 0x0080u);
        cpu.setDw(3, 0xEFFFu);
        cpu.d[4] = 3;
        cpu.setDw(5, 0xFF7Fu);
        cpu.a[2] = kPrimaryCamera;
        cpu.a[3] = kPrimaryRowQueue;
        cpu.a[4] = kPrimaryColumnQueue;
        logCallFromReturn(0x000197F4u, 0x00019800u);
        flushOne();
        cpu.a[2] = kSecondaryCamera;
        cpu.a[3] = kSecondaryRowQueue;
        cpu.a[4] = kSecondaryColumnQueue;
        flushOne();
    };
    // Queue one 32-block row (top longs, the bottom longs $80 further on) from a5, the block at world X x.
    const auto queueRow = [&](int x, int width) {
        const bool margin = wide && cpu.a[1] == kPrimaryCamera;
        const m_word level = mem.readWord(kLevel);
        mem.writeWord(cpu.a[3], cpu.dw(2));
        cpu.a[3] += 2;
        cpu.a[6] = mem.readLong(cpu.a[1] + 0x32u);
        cpu.d[1] = 0x1Fu;
        mem.writeWord(cpu.a[3], cpu.dw(1));
        cpu.a[3] += 2;
        for (int i = 0; i < 32; ++i, x += 0x10) {
            m_long    block  = cpu.a[5];
            const int mirror = margin ? wideMirrorColumn(level, x) : -1;
            const int column = mirror >= 0 ? mirror : margin ? wideMarginColumn(level, x, width) : -1;
            if (column >= 0)
                block = cpu.a[5] + static_cast<m_long>((column - x) >> 4);
            ++cpu.a[5];
            cpu.d[0] = static_cast<m_long>(mem.readByte(block)) << 3;
            m_long top    = mem.readLong(cpu.a[6] + cpu.d[0]);
            m_long bottom = mem.readLong(cpu.a[6] + cpu.d[0] + 4u);
            if (mirror >= 0) {
                top    = mirrorMetatileLong(top);
                bottom = mirrorMetatileLong(bottom);
            }
            mem.writeLong(cpu.a[3], top);
            cpu.a[3] += 4;
            mem.writeLong(cpu.a[3] + 0x7Cu, bottom);
        }
        cpu.setDw(1, 0xFFFFu);
    };
    const auto resolve = [&](m_long retPc) {
        logCallFromReturn(retPc, 0x00019656u);
        cpu.ssp -= 4;
        mem.writeLong(cpu.ssp, retPc);
        resolve_blockmap_and_vdp_address();
    };

    const m_long camera = cpu.a[1];
    const m_long y      = mem.readLong(camera + 0x0Eu);
    cpu.d[6]            = y;
    if (mem.readWord(camera + 0x42u) != 2u) {
        cpu.setDw(2, mem.readWord(camera + 0x44u));
        cpu.a[0]         = camera;
        m_word stripsLeft = cpu.dw(2);
        m_long strip      = camera;
        for (;;) {
            cpu.setDw(3, mem.readWord(strip + 0x46u));
            m_word rowsLeft = cpu.dw(3);
            for (;;) {
                const m_long saved[7] = {cpu.d[2], cpu.d[3], cpu.d[6], cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3]};
                const int x = static_cast<std::int16_t>(static_cast<m_word>(mem.readWord(strip + 0x50u) - 0x60u));
                cpu.setDw(0, static_cast<m_word>(x));
                cpu.setDw(1, mem.readWord(camera + 0x0Eu));
                cpu.setDw(4, static_cast<m_word>(mem.readWord(camera + 0x2Eu) << 4));
                cpu.setDw(5, 0x0100u);
                cpu.d[6] = 0;
                resolve(0x00019B2Au);
                queueRow(x, static_cast<std::int16_t>(cpu.dw(4)));
                cpu.a[0] = kVdpControl;
                cpu.a[1] = kVdpData;
                upload(0x00019B58u);
                cpu.d[2] = saved[0];
                cpu.d[3] = saved[1];
                cpu.d[6] = saved[2];
                cpu.a[0] = saved[3];
                cpu.a[1] = saved[4];
                cpu.a[2] = saved[5];
                cpu.a[3] = saved[6];
                mem.writeWord(camera + 0x0Eu, static_cast<m_word>(mem.readWord(camera + 0x0Eu) + 0x10u));
                if (rowsLeft-- == 0)
                    break;
            }
            cpu.setDw(3, 0xFFFFu);
            strip += 0x12u;
            cpu.a[0] = strip;
            if (stripsLeft-- == 0)
                break;
        }
        cpu.setDw(2, 0xFFFFu);
    } else {
        // Layout mode 2 (round 7): 16 rows over the 1280 x 1536 grid, from the camera's own X.
        for (m_word rowsLeft = 0x0Fu;; --rowsLeft) {
            cpu.setDw(2, rowsLeft);
            const m_long saved[5] = {cpu.d[2], cpu.d[6], cpu.a[1], cpu.a[2], cpu.a[3]};
            cpu.a[4] = cpu.a[3];
            const int x = static_cast<std::int16_t>(static_cast<m_word>(mem.readWord(camera + 0x02u) - 0x60u));
            cpu.setDw(0, static_cast<m_word>(x));
            cpu.setDw(1, mem.readWord(camera + 0x0Eu));
            cpu.d[6] = 0;
            cpu.setDw(4, 0x0500u);
            cpu.setDw(5, 0x0600u);
            resolve(0x00019B96u);
            queueRow(x, 0x0500);
            cpu.a[0] = kVdpControl;
            cpu.a[1] = kVdpData;
            upload(0x00019BC4u);
            cpu.d[2] = saved[0];
            cpu.d[6] = saved[1];
            cpu.a[1] = saved[2];
            cpu.a[2] = saved[3];
            cpu.a[3] = saved[4];
            mem.writeWord(camera + 0x0Eu, static_cast<m_word>(mem.readWord(camera + 0x0Eu) + 0x10u));
            if (rowsLeft == 0)
                break;
        }
        cpu.setDw(2, 0xFFFFu);
    }
    mem.writeLong(camera + 0x0Eu, cpu.d[6]);
    cpu.setNZClearVC(cpu.d[6], 0x80000000u);
    cpu.ssp += 4;
}

// ---------------------------------------------------------------------------
// Round 3's third palm top
//
// Round 3's palm trunks near the street are Plane A and their tops are sprites: type-$4E objects that $E98
// creates with each section's waves, each two 2x4 pieces of tile $122 eight pixels left of X. The layout
// repeats every $600 (palms at $288, $2B8, $318, ...). The boss section creates only $E88 and $EB8, and at the
// boss (camera $DC0) widescreen shows the next trunk, $F18, in the right margin with no top (user). The SAT gets that palm's records again, $60 further right, at the
// end of the list where the palms are (the farthest bucket). Nothing but the SAT buffer and its counters
// changes, so the game is unaffected.
// ---------------------------------------------------------------------------

namespace {

constexpr m_long kSatBuffer          = 0xFFFFDA00u;
constexpr m_long kSatSlotsRemaining  = 0xFFFFFA03u;
constexpr m_long kSatWritePtr        = 0xFFFFFB04u;
constexpr m_word kPalmX              = 0x0EB8u; // the boss arena's right palm (camera $DC0)
constexpr m_word kThirdPalmShift     = 0x0060u;
constexpr m_word kPalmTile           = 0x0122u;
constexpr m_word kPalmPieceLeft      = 0x0008u;
constexpr m_word kSpriteWindowRight  = 0x01E0u; // biased X past the widescreen's right edge

} // namespace

void SoRWidescreenArena::appendRound3PalmTop(SystemMemory &memory) {
    if (memory.readWord(kLevel) != 2u)
        return;
    const m_long end    = 0xFFFF0000u | memory.readWord(kSatWritePtr);
    const m_word pieceX = static_cast<m_word>(kPalmX + 0x80u - memory.readWord(kCamX) - kPalmPieceLeft);
    const m_word thirdX = static_cast<m_word>(pieceX + kThirdPalmShift);
    if (static_cast<std::int16_t>(thirdX) >= static_cast<std::int16_t>(kSpriteWindowRight))
        return;

    m_long write = end;
    for (m_long record = kSatBuffer; record < end; record += 8u) {
        if ((memory.readWord(record + 4u) & 0x07FFu) != kPalmTile || memory.readWord(record + 6u) != pieceX)
            continue;
        const m_byte slots = memory.readByte(kSatSlotsRemaining);
        if (slots == 0)
            break;
        const auto link = static_cast<m_byte>(memory.readByte(kSpriteLinkIndex) + 1u);
        memory.writeByte(kSpriteLinkIndex, link);
        memory.writeByte(kSatSlotsRemaining, static_cast<m_byte>(slots - 1u));
        memory.writeWord(write, memory.readWord(record));
        memory.writeByte(write + 2u, memory.readByte(record + 2u));
        memory.writeByte(write + 3u, link);
        memory.writeWord(write + 4u, memory.readWord(record + 4u));
        memory.writeWord(write + 6u, thirdX);
        write += 8u;
    }
    if (write == end)
        return;
    memory.writeLong(write, 0);
    memory.writeWord(kSatWritePtr, static_cast<m_word>(write));
}
