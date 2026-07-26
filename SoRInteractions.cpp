#include "SoRCheats.hpp"
#include "Sor.hpp"

#include <cstdint>

namespace {

constexpr m_long kObjectTable          = 0xFFFFB900u;
constexpr m_long kObjectSlotSize       = 0x80u;
constexpr int    kInteractionScanSlots = 0x44;

constexpr m_long kStopMusic           = 0xFFFFF002u;
constexpr m_long kPlaySe              = 0xFFFFF00Au;
constexpr m_long kPauseTextFlag       = 0xFFFFFA46u;
constexpr m_long kP1StartButtonBuffer = 0xFFFFFA47u;
constexpr m_long kP2StartButtonBuffer = 0xFFFFFA48u;
constexpr m_long kFadeOutFlag         = 0xFFFFFA71u;
constexpr m_long kPaletteFadeCounter  = 0xFFFFFB0Cu;
constexpr m_long kP1ButtonPress       = 0xFFFFFC05u;
constexpr m_long kP2ButtonPress       = 0xFFFFFC09u;
constexpr m_long kPlayerMode          = 0xFFFFFF18u;
constexpr m_long kDemoMode            = 0xFFFFFF2Au;

constexpr m_long kObjType        = 0x00u;
constexpr m_long kObjX           = 0x10u;
constexpr m_long kObjY           = 0x14u;
constexpr m_long kObjZ           = 0x18u;
constexpr m_long kObjState       = 0x30u;
constexpr m_long kObjHeldPointer = 0x5Eu;
constexpr m_long kObjHeldType    = 0x60u;

constexpr m_long kPlayerInputPressed  = 0x55u;
constexpr m_long kPlayerFlags58       = 0x58u;
constexpr m_long kPlayerFlags59       = 0x59u;
constexpr m_long kPlayerSavedFrame    = 0x5Au;
constexpr m_long kPlayerSavedDuration = 0x5Bu;
constexpr m_long kPlayerTimer         = 0x5Cu;
constexpr m_long kPlayerComboState    = 0x5Du;

constexpr m_long kTargetInteraction = 0x51u;
constexpr m_long kTargetOwner       = 0x52u;
constexpr m_long kWeaponUseCounter  = 0x50u;
constexpr m_long kBottleBrokenFlag  = 0x54u;

constexpr m_byte kButtonAttack = 0x10u;
constexpr m_byte kButtonStart  = 0x80u;

constexpr bool bit(m_byte value, unsigned index) {
    return (value & static_cast<m_byte>(1u << index)) != 0;
}

void setBit(SystemMemory &memory, m_long address, unsigned index) {
    memory.writeByte(address, static_cast<m_byte>(memory.readByte(address) | (1u << index)));
}

bool clearBit(SystemMemory &memory, m_long address, unsigned index) {
    const m_byte before = memory.readByte(address);
    memory.writeByte(address, static_cast<m_byte>(before & ~(1u << index)));
    return bit(before, index);
}

void setResultD7(CPU68K &cpu, bool handled) {
    cpu.d[7] = handled ? 0xFFFFFFFFu : 0u;
    cpu.setNZClearVC(cpu.d[7], 0x80000000u);
}

bool call68k(CPU68K &cpu, SystemMemory &memory, auto &&fn, m_long retPc) {
    const m_long spBefore = cpu.ssp;
    cpu.ssp -= 4;
    memory.writeLong(cpu.ssp, retPc);
    fn();
    return (cpu.ssp & 0x00FFFFFFu) <= (spBefore & 0x00FFFFFFu);
}

m_word addWord(m_word value, m_word delta) {
    return static_cast<m_word>(value + delta);
}

bool isConsumablePickup(m_byte type) {
    return type == 0x47u || type == 0x4Bu || type == 0x4Cu || type == 0x4Fu || type == 0x3Fu || type == 0x40u;
}

m_long addressFromWord(m_word address) {
    return static_cast<m_long>(static_cast<std::int16_t>(address));
}

bool isEligibleWeapon(SystemMemory &memory, m_long object) {
    const m_byte type = memory.readByte(object + kObjType);
    return type >= 0x08u && type < 0x0Du && memory.readByte(object + kTargetInteraction) == 0u &&
           memory.readByte(object + kWeaponUseCounter) < 3u;
}

bool objectInsidePickupBox(SystemMemory &memory, m_long player, m_long object) {
    const auto sx = [](m_word value) {
        return static_cast<std::int16_t>(value);
    };

    const m_word minX = addWord(memory.readWord(player + kObjX), 0xFFECu);
    const m_word maxX = addWord(minX, 0x0028u);
    const m_word minY = addWord(memory.readWord(player + kObjY), 0xFFF0u);
    const m_word maxY = addWord(minY, 0x0020u);
    const m_word minZ = addWord(memory.readWord(player + kObjZ), 0xFFF8u);
    const m_word maxZ = addWord(minZ, 0x0008u);

    const m_word x = memory.readWord(object + kObjX);
    const m_word y = memory.readWord(object + kObjY);
    const m_word z = memory.readWord(object + kObjZ);

    return sx(minX) <= sx(x) && sx(x) <= sx(maxX) && sx(minY) <= sx(y) && sx(y) <= sx(maxY) && sx(minZ) <= sx(z) &&
           sx(z) <= sx(maxZ);
}

m_long findPickupTarget(SystemMemory &memory, m_long player) {
    if ((memory.readByte(player + kObjState) & 0xFEu) == 0x28u)
        return 0u;

    for (int slot = 0; slot < kInteractionScanSlots; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * kObjectSlotSize;
        if (!objectInsidePickupBox(memory, player, object))
            continue;

        if (isEligibleWeapon(memory, object) || isConsumablePickup(memory.readByte(object + kObjType)))
            return object;
    }

    return 0u;
}

bool reservePickupTarget(SystemMemory &memory, m_long player, m_long target) {
    if (target == 0u)
        return false;

    if (isEligibleWeapon(memory, target)) {
        if (memory.readByte(player + kObjHeldType) != 0u) {
            const m_long oldWeapon = addressFromWord(memory.readWord(player + kObjHeldPointer));
            memory.writeByte(oldWeapon + kTargetInteraction, 0u);
        }

        memory.writeByte(player + kObjHeldType, memory.readByte(target + kObjType));
        memory.writeWord(player + kObjHeldPointer, static_cast<m_word>(target & 0xFFFFu));
    }

    memory.writeWord(target + kTargetOwner, static_cast<m_word>(player & 0xFFFFu));
    memory.writeByte(target + kTargetInteraction, 1u);
    return true;
}

bool hasNearbyObjectInFront(SystemMemory &memory, m_long player) {
    const m_word playerX    = memory.readWord(player + kObjX);
    const m_word minY       = addWord(memory.readWord(player + kObjY), 0xFFF4u);
    const m_word maxY       = addWord(minY, 0x0018u);
    const bool   facingLeft = bit(memory.readByte(player + kObjState), 0);

    for (int slot = 0; slot < 32; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * kObjectSlotSize;
        const m_byte type   = memory.readByte(object + kObjType);
        if (type == 0u || type == 0x16u)
            continue;

        const m_word objectX  = memory.readWord(object + kObjX);
        m_word       distance = static_cast<m_word>(objectX - playerX);
        if (objectX >= playerX) {
            if (facingLeft)
                continue;
        } else {
            if (!facingLeft)
                continue;
            distance = static_cast<m_word>(0u - distance);
        }

        if (distance >= 0x0090u)
            continue;

        const m_word objectY = memory.readWord(object + kObjY);
        if (objectY >= minY && objectY < maxY)
            return true;
    }

    return false;
}

void return68k(CPU68K &cpu) {
    cpu.ssp += 4;
}

} // namespace

void StreetsOfRage::player_normal_attack_input(m_long entry_) {
    traceEnter(entry_);

    const m_long player        = cpu().a[0];
    const bool   alternative   = SoRCheats::alternativePickupRoutineEnabled();
    const m_byte pressed       = memory().readByte(player + kPlayerInputPressed);
    const bool   attackPressed = (pressed & kButtonAttack) != 0u;
    const bool   startPressed  = (pressed & kButtonStart) != 0u;
    const bool   comboPending  = clearBit(memory(), player + kPlayerFlags58, 5);

    if (comboPending) {
        setBit(memory(), player + kPlayerFlags58, 1);
        memory().writeByte(player + kPlayerSavedFrame, 1u);
        memory().writeByte(player + kPlayerSavedDuration, 2u);
    } else if (!attackPressed && !(alternative && startPressed)) {
        cpu().setFlag(CPU68K::FlagZ, true);
        return68k(cpu());
        return;
    }

    const bool tryPickup = alternative ? (startPressed && !attackPressed && !comboPending) : true;
    if (tryPickup) {
        if (!call68k(
                cpu(),
                memory(),
                [this] {
                    find_close_interaction_target();
                },
                0x3050u))
            return;
        if (cpu().ne()) {
            setResultD7(cpu(), true);
            return68k(cpu());
            return;
        }
    }

    if (!attackPressed && !comboPending) {
        setResultD7(cpu(), false);
        return68k(cpu());
        return;
    }

    if (clearBit(memory(), player + kPlayerFlags59, 0)) {
        clearBit(memory(), player + kPlayerFlags58, 1);
        m_word action = static_cast<m_word>((memory().readByte(player + kPlayerComboState) & 0xFEu) + 2u);
        cpu().setDw(0, action);
        if (static_cast<m_byte>(action) < 0x20u) {
            memory().writeByte(player + kPlayerComboState, static_cast<m_byte>(action));
            if (!call68k(
                    cpu(),
                    memory(),
                    [this] {
                        sub_001aae(0x2EE8u);
                    },
                    0x3080u))
                return;
            setResultD7(cpu(), true);
            return68k(cpu());
            return;
        }
    }

    memory().writeByte(player + kPlayerTimer, 0x10u);
    cpu().d[0] = 0x18u;
    memory().writeByte(player + kPlayerComboState, cpu().db(0));
    if (!call68k(
            cpu(),
            memory(),
            [this] {
                sub_001aae(0x2EE8u);
            },
            0x3080u))
        return;
    setResultD7(cpu(), true);
    return68k(cpu());
}

void StreetsOfRage::player_held_object_attack_input(m_long entry_) {
    traceEnter(entry_);

    const m_long player        = cpu().a[0];
    const m_byte pressed       = memory().readByte(player + kPlayerInputPressed);
    const bool   attackPressed = (pressed & kButtonAttack) != 0u;
    const bool   startPressed  = (pressed & kButtonStart) != 0u;
    const bool   alternative   = SoRCheats::alternativePickupRoutineEnabled();

    if (!attackPressed && !(alternative && startPressed)) {
        cpu().setFlag(CPU68K::FlagZ, true);
        return68k(cpu());
        return;
    }

    const bool tryPickup = alternative ? (startPressed && !attackPressed) : true;
    if (tryPickup) {
        if (!call68k(
                cpu(),
                memory(),
                [this] {
                    find_close_interaction_target();
                },
                0x3092u))
            return;
        if (cpu().ne()) {
            setResultD7(cpu(), true);
            return68k(cpu());
            return;
        }
    }

    if (!attackPressed) {
        setResultD7(cpu(), false);
        return68k(cpu());
        return;
    }

    const m_byte heldType = memory().readByte(player + kObjHeldType);
    cpu().setDb(1, heldType);
    cpu().d[0] = 0x48u;

    if (heldType == 0x0Au || heldType == 0x0Bu) {
        cpu().d[7] = 0xFFFFFFA7u;
        if (!call68k(
                cpu(),
                memory(),
                [this] {
                    sub_0026e2(0x35D6u);
                },
                0x3122u))
            return;
    } else {
        cpu().d[0] = 0x44u;
        if (heldType == 0x09u) {
            const m_long weapon = addressFromWord(memory().readWord(player + kObjHeldPointer));
            if (memory().readByte(weapon + kBottleBrokenFlag) != 0u)
                cpu().d[0] = 0x46u;
        } else if (heldType != 0x0Cu && hasNearbyObjectInFront(memory(), player)) {
            cpu().d[0] = 0x46u;
        }
    }

    if (!call68k(
            cpu(),
            memory(),
            [this] {
                sub_001aae(0x2DE6u);
            },
            0x3132u))
        return;
    setResultD7(cpu(), true);
    return68k(cpu());
}

void StreetsOfRage::find_close_interaction_target(m_long entry_) {
    traceEnter(entry_);

    const m_long player = cpu().a[0];
    const m_long target = findPickupTarget(memory(), player);
    if (target == 0u) {
        setResultD7(cpu(), false);
        return68k(cpu());
        return;
    }

    reservePickupTarget(memory(), player, target);
    cpu().d[0] = 0x28u;
    if (!call68k(
            cpu(),
            memory(),
            [this] {
                sub_001aae(0x2DE6u);
            },
            0x31EAu))
        return;

    setResultD7(cpu(), true);
    return68k(cpu());
}

void StreetsOfRage::handle_pause_start_input(m_long entry_) {
    traceEnter(entry_);

    const auto finishWithPauseFlag = [this] {
        const m_byte pause = memory().readByte(kPauseTextFlag);
        cpu().setNZClearVC(pause, 0x80u);
        return68k(cpu());
    };

    if (memory().readByte(kStopMusic) != 0u) {
        finishWithPauseFlag();
        return;
    }

    cpu().d[7] = memory().readByte(kPauseTextFlag) == 0u ? 3u : 0u;

    bool pauseRequested = false;
    const bool alternativePickupOnly =
        SoRCheats::alternativePickupRoutineEnabled() && memory().readByte(kDemoMode) == 0u;
    const m_byte playerMode = memory().readByte(kPlayerMode);

    if ((playerMode & 0x01u) != 0u) {
        const m_byte buffered =
            static_cast<m_byte>(memory().readByte(kP1StartButtonBuffer) | memory().readByte(kP1ButtonPress));
        memory().writeByte(kP1StartButtonBuffer, buffered);
        if ((buffered & kButtonStart) != 0u && !alternativePickupOnly)
            pauseRequested = true;
    }

    if (!pauseRequested && (playerMode & 0x02u) != 0u) {
        const m_byte buffered =
            static_cast<m_byte>(memory().readByte(kP2StartButtonBuffer) | memory().readByte(kP2ButtonPress));
        memory().writeByte(kP2StartButtonBuffer, buffered);
        if ((buffered & kButtonStart) != 0u && !alternativePickupOnly)
            pauseRequested = true;
    }

    if (pauseRequested) {
        if (memory().readByte(kDemoMode) != 0u) {
            const m_byte fade = memory().readByte(kFadeOutFlag);
            if (fade != 0u) {
                cpu().setNZClearVC(fade, 0x80u);
                return68k(cpu());
                return;
            }

            setBit(memory(), kDemoMode, 7);
            memory().writeByte(kFadeOutFlag, 1u);
            memory().writeWord(kPaletteFadeCounter, 0x0040u);
            cpu().setNZClearVC(0x0040u, 0x8000u);
            return68k(cpu());
            return;
        }

        memory().writeByte(kPauseTextFlag, cpu().db(7));
        memory().writeByte(kPlaySe, 0xBDu);
    }

    memory().writeByte(kP1StartButtonBuffer, 0u);
    memory().writeByte(kP2StartButtonBuffer, 0u);
    finishWithPauseFlag();
}
