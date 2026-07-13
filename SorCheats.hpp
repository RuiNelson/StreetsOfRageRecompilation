#pragma once

#include "data_types.hpp"

namespace SorCheats {

constexpr m_long kP1Object             = 0x00FFB800u;
constexpr m_byte kPunchPowerMultiplier = 4u;
constexpr m_byte kMaximumAttackDamage  = 0x0Fu;

void setP1PunchPowerEnabled(bool enabled);
bool p1PunchPowerEnabled();

constexpr m_byte adjustP1PunchDamage(m_long objectAddress, m_byte damage, bool enabled) {
    if (!enabled || (objectAddress & 0x00FFFFFFu) != kP1Object)
        return damage;

    const unsigned boosted = static_cast<unsigned>(damage) * kPunchPowerMultiplier;
    return static_cast<m_byte>(boosted > kMaximumAttackDamage ? kMaximumAttackDamage : boosted);
}

} // namespace SorCheats
