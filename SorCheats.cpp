#include "SorCheats.hpp"

#include <atomic>

namespace SorCheats {
namespace {

std::atomic_bool p1PunchPowerEnabled_{false};

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

} // namespace SorCheats
