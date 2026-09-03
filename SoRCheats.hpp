#pragma once

#include "data_types.hpp"

namespace SoRCheats {

constexpr m_long kP1Object             = 0x00FFB800u;
constexpr m_byte kPunchPowerMultiplier = 12u;
constexpr m_byte kMaximumAttackDamage  = 0x0Fu;

void setP1PunchPowerEnabled(bool enabled);
bool p1PunchPowerEnabled();
void setAltControlsEnabled(bool enabled);
bool altControlsEnabled();
void updateAltAttackButton(int playerIndex, bool held);
bool altAttackButtonPressedForObject(m_long objectAddress);
void updateAltPickupButton(int playerIndex, bool held);
bool altPickupButtonPressedForObject(m_long objectAddress);
void clearAltPickupButtons();
void requestFreePoliceCall(m_long objectAddress);
bool consumeFreePoliceCall(m_long objectAddress);

// Debug hotkeys that change emulated RAM, deferred to the CPU thread.
//
// `handleOptionHotkey()` runs on the **main** thread and `run()` on the CPU
// thread (MegaDriveEnvironment/README.md, "The important threading rules").
// Writing object-table bytes from the hotkey handler therefore races the
// game's own object update: the family-kill cheats clear a live enemy's type
// and state while the CPU may be part-way through the loop that walks it, and
// a run that loses that race resets the console.
//
// Measured: four of sixteen scored Souther runs never reached the boss.
// `boss_fight.py`'s off-target heartbeat caught all of them cycling through
// 'Sega logo' -> 'Story intro' -> 'Hi-scores' -> attract demo, which is a
// **reset**, not a game over -- a game over lands on the continue screen. The
// only thing running in every one of them was `debug_scenario.py`'s family
// sweep, twice a second for the whole walk through round 2.
//
// `requestFreePoliceCall` above already had the right shape: record on the
// main thread, consume on the CPU thread inside a manual function. These give
// the rest of the hotkeys the same treatment, drained by
// `applyPendingSoRCheats` at the game's own frame boundary.
enum PendingCheat : unsigned {
    kCheatNone       = 0u,
    kCheatAddLife    = 1u << 0,
    kCheatAddSpecial = 1u << 1,
    kCheatKillAll    = 1u << 2,
    kCheatKillGarcia = 1u << 3,
    kCheatKillHakuRo = 1u << 4,
    kCheatKillSignal = 1u << 5,
    kCheatKillJack   = 1u << 6,
    kCheatKillNora   = 1u << 7,
};

/// Record one or more pending cheats (main thread).
void requestCheats(unsigned bits);
/// Take every pending cheat and clear the set (CPU thread).
unsigned consumeCheats();

/// Record a pending level jump, 0-based, or -1 for none (main thread).
void requestLevelJump(int level);
/// Take the pending level jump, or -1 when there is none (CPU thread).
int consumeLevelJump();

constexpr m_byte adjustP1PunchDamage(m_long objectAddress, m_byte damage, bool enabled) {
    if (!enabled || (objectAddress & 0x00FFFFFFu) != kP1Object)
        return damage;

    const unsigned boosted = static_cast<unsigned>(damage) * kPunchPowerMultiplier;
    return static_cast<m_byte>(boosted > kMaximumAttackDamage ? kMaximumAttackDamage : boosted);
}

} // namespace SoRCheats

class SystemMemory;

/// Apply every cheat request recorded since the last call.
///
/// **CPU thread only**, and only at a point where the game is not part-way
/// through its own object update -- `SoRManualFunctions.cpp`'s vblank waits
/// are exactly that point: the game has finished its frame and is waiting for
/// the interrupt. Defined next to the cheat helpers in `StreetsOfRage.cpp`.
void applyPendingSoRCheats(SystemMemory &memory);
