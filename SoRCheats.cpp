#include "SoRCheats.hpp"

#include <atomic>

namespace SoRCheats {
namespace {

std::atomic_bool    p1PunchPowerEnabled_{false};
std::atomic_bool    altControlsEnabled_{false};
std::atomic_bool    altAttackHeld_[2]    = {false, false};
std::atomic_bool    altAttackPressed_[2] = {false, false};
std::atomic_bool    altPickupHeld_[2]    = {false, false};
std::atomic_bool    altPickupPressed_[2] = {false, false};
std::atomic<m_long> freePoliceCaller_{0};

static_assert(adjustP1PunchDamage(kP1Object, 1u, true) == 12u);
static_assert(adjustP1PunchDamage(kP1Object, 2u, true) == kMaximumAttackDamage);
static_assert(adjustP1PunchDamage(0x00FFB880u, 2u, true) == 2u);
static_assert(adjustP1PunchDamage(kP1Object, 2u, false) == 2u);

} // namespace

void setP1PunchPowerEnabled(bool enabled) {
    p1PunchPowerEnabled_.store(enabled, std::memory_order_release);
}

bool p1PunchPowerEnabled() {
    return p1PunchPowerEnabled_.load(std::memory_order_acquire);
}

void setAltControlsEnabled(bool enabled) {
    altControlsEnabled_.store(enabled, std::memory_order_release);
}

bool altControlsEnabled() {
    return altControlsEnabled_.load(std::memory_order_acquire);
}

void updateAltAttackButton(int playerIndex, bool held) {
    if (playerIndex < 0 || playerIndex >= 2)
        return;

    const bool wasHeld = altAttackHeld_[playerIndex].exchange(held, std::memory_order_acq_rel);
    altAttackPressed_[playerIndex].store(held && !wasHeld, std::memory_order_release);
}

bool altAttackButtonPressedForObject(m_long objectAddress) {
    const m_long normalized = objectAddress & 0x00FFFFFFu;
    if (normalized == kP1Object)
        return altAttackPressed_[0].load(std::memory_order_acquire);
    if (normalized == 0x00FFB880u)
        return altAttackPressed_[1].load(std::memory_order_acquire);
    return false;
}

void updateAltPickupButton(int playerIndex, bool held) {
    if (playerIndex < 0 || playerIndex >= 2)
        return;

    const bool wasHeld = altPickupHeld_[playerIndex].exchange(held, std::memory_order_acq_rel);
    altPickupPressed_[playerIndex].store(held && !wasHeld, std::memory_order_release);
}

bool altPickupButtonPressedForObject(m_long objectAddress) {
    const m_long normalized = objectAddress & 0x00FFFFFFu;
    if (normalized == kP1Object)
        return altPickupPressed_[0].load(std::memory_order_acquire);
    if (normalized == 0x00FFB880u)
        return altPickupPressed_[1].load(std::memory_order_acquire);
    return false;
}

void clearAltPickupButtons() {
    for (int i = 0; i < 2; ++i) {
        altAttackHeld_[i].store(true, std::memory_order_release);
        altAttackPressed_[i].store(false, std::memory_order_release);
        altPickupHeld_[i].store(true, std::memory_order_release);
        altPickupPressed_[i].store(false, std::memory_order_release);
    }
}

void requestFreePoliceCall(m_long objectAddress) {
    freePoliceCaller_.store(objectAddress, std::memory_order_release);
}

bool consumeFreePoliceCall(m_long objectAddress) {
    return freePoliceCaller_.compare_exchange_strong(objectAddress, 0u, std::memory_order_acq_rel);
}

} // namespace SoRCheats
