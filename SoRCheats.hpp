#pragma once

#include "data_types.hpp"

class SystemMemory;

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

/// Alt/Option+T: after Round 5 is playable, skip corridors until Onihime/Yasha.
void requestTwinsBossWarp();
bool twinsBossWarpRequested();
void clearTwinsBossWarpRequest();
/// CPU-thread tick (game_infinite_loop). Idle when no warp is armed.
void tickTwinsBossWarp(SystemMemory &memory);

constexpr m_byte adjustP1PunchDamage(m_long objectAddress, m_byte damage, bool enabled) {
    if (!enabled || (objectAddress & 0x00FFFFFFu) != kP1Object)
        return damage;

    const unsigned boosted = static_cast<unsigned>(damage) * kPunchPowerMultiplier;
    return static_cast<m_byte>(boosted > kMaximumAttackDamage ? kMaximumAttackDamage : boosted);
}

} // namespace SoRCheats
