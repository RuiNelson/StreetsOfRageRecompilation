#include "SorCheats.hpp"

#include <atomic>

namespace SorCheats {
namespace {

std::atomic_bool p1PunchPowerEnabled_{false};
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

void requestFreePoliceCall(m_long objectAddress) {
    freePoliceCaller_.store(objectAddress, std::memory_order_release);
}

bool consumeFreePoliceCall(m_long objectAddress) {
    return freePoliceCaller_.compare_exchange_strong(objectAddress, 0u, std::memory_order_acq_rel);
}

} // namespace SorCheats
