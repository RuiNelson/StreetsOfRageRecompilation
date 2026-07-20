#include "SorRuntime.hpp"
#include "SorCheats.hpp"
#include "Logger.hpp"

#include <random>

namespace {

constexpr m_long kGameState        = 0xFFFFFF00u;
constexpr m_long kLevel            = 0xFFFFFF02u;
constexpr m_long kWave             = 0xFFFFFF04u;
constexpr m_long kP1Object         = 0xFFFFB800u;
constexpr m_long kP2Object         = 0xFFFFB880u;
constexpr m_long kP1Lives          = 0xFFFFFF20u;
constexpr m_long kP1SpecialAttacks = 0xFFFFFF21u;
constexpr m_long kObjectTable      = 0xFFFFB900u;
constexpr m_word kLevelIntroState  = 0x0028u;
// Even values are init states; the loop then advances to the update mode (+2).
constexpr m_word kEndingBadInit    = 0x001Cu; // init_ending_bad
constexpr m_word kEndingGoodInit   = 0x0024u; // init_ending_good
constexpr int    kLevelCount       = 8;
constexpr int    kObjectSlotCount  = 32;
constexpr m_long kObjectSlotSize   = 0x80u;

constexpr m_long kObjectPrimaryStateOffset = 0x30u;
constexpr m_long kObjectHealthOffset       = 0x32u;
constexpr m_byte kFirstWeaponType           = 0x08u;
constexpr m_byte kLastWeaponType            = 0x0Cu;

constexpr bool isOrdinaryEnemy(m_byte type) {
    return type >= 0x20u && type <= 0x2Au;
}

constexpr bool isBespokeBoss(m_byte type) {
    return type == 0x30u || type == 0x35u; // Abadede or Mr. X
}

constexpr bool isSharedFrameworkBoss(m_byte type) {
    return type >= 0x55u && type <= 0x58u;
}

constexpr bool isWeapon(m_byte type) {
    return type >= kFirstWeaponType && type <= kLastWeaponType;
}

static_assert(isOrdinaryEnemy(0x20u) && isOrdinaryEnemy(0x2Au));
static_assert(!isOrdinaryEnemy(0x1Fu) && !isOrdinaryEnemy(0x2Bu));
static_assert(isBespokeBoss(0x30u) && isBespokeBoss(0x35u));
static_assert(isSharedFrameworkBoss(0x55u) && isSharedFrameworkBoss(0x58u));
static_assert(isWeapon(0x08u) && isWeapon(0x0Cu));
static_assert(!isWeapon(0x07u) && !isWeapon(0x0Du));

int levelFromTopRowNumber(SDL_Keycode key) {
    switch (key) {
        case SDLK_1:
            return 0;
        case SDLK_2:
            return 1;
        case SDLK_3:
            return 2;
        case SDLK_4:
            return 3;
        case SDLK_5:
            return 4;
        case SDLK_6:
            return 5;
        case SDLK_7:
            return 6;
        case SDLK_8:
            return 7;
        default:
            return -1;
    }
}

void incrementByte(SystemMemory &memory, m_long address, const char *label) {
    const m_byte before = memory.readByte(address);
    const m_byte after  = before == 0xFFu ? before : static_cast<m_byte>(before + 1u);
    memory.writeByte(address, after);
    Logger::log("[cheat] %s: %u -> %u", label, static_cast<unsigned>(before), static_cast<unsigned>(after));
}

m_word activePlayerObject(SystemMemory &memory) {
    if (memory.readByte(kP1Object) == 1u)
        return static_cast<m_word>(kP1Object);
    if (memory.readByte(kP2Object) == 1u)
        return static_cast<m_word>(kP2Object);
    return static_cast<m_word>(kP1Object);
}

int killInstantiatedEnemies(SystemMemory &memory) {
    const m_word attacker = activePlayerObject(memory);
    int killed = 0;

    for (int slot = 0; slot < kObjectSlotCount; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * kObjectSlotSize;
        const m_byte type = memory.readByte(object);

        if (isOrdinaryEnemy(type)) {
            // Match the cartridge's forced-death sweep: enter the airborne/death
            // reaction with negative health and retain a player for score credit.
            memory.writeByte(object + 0x37u, memory.readByte(object + 0x37u) | 0x02u);
            memory.writeWord(object + kObjectHealthOffset, 0xFFFFu);
            memory.writeWord(object + kObjectPrimaryStateOffset, 0x0300u);
            memory.writeWord(object + 0x3Eu, attacker);
            ++killed;
            continue;
        }

        if (isBespokeBoss(type)) {
            // The shared Abadede/Mr. X collision path selects state $0E on a
            // lethal hit. Clear its collision substate as that path does.
            memory.writeWord(object + kObjectHealthOffset, 0u);
            memory.writeByte(object + 0x5Bu, 0u);
            memory.writeByte(object + kObjectPrimaryStateOffset, 0x0Eu);
            ++killed;
            continue;
        }

        if (isSharedFrameworkBoss(type)) {
            // Feed an unavoidable lethal pending hit through the normal shared
            // boss damage path so pairing, score, HUD, and cleanup still run.
            memory.writeWord(object + kObjectHealthOffset, 0u);
            memory.writeByte(object + 0x6Cu, 1u);
            memory.writeByte(object + 0x6Du, 0u);
            memory.writeWord(object + 0x70u, attacker);
            ++killed;
        }
    }

    return killed;
}

m_long findFreeObjectSlot(SystemMemory &memory) {
    for (int slot = 0; slot < kObjectSlotCount; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * kObjectSlotSize;
        if (memory.readByte(object) == 0u)
            return object;
    }
    return 0u;
}

void clearObjectSlot(SystemMemory &memory, m_long object) {
    for (m_long offset = 0; offset < kObjectSlotSize; offset += 4u)
        memory.writeLong(object + offset, 0u);
}

m_byte randomWeaponType() {
    static std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<unsigned> distribution(kFirstWeaponType, kLastWeaponType);
    return static_cast<m_byte>(distribution(generator));
}

m_byte spawnRandomWeaponOnGround(SystemMemory &memory, m_long player, int xOffset) {
    if (memory.readByte(player) != 1u)
        return 0u;

    const m_long weapon = findFreeObjectSlot(memory);
    if (weapon == 0u)
        return 0u;

    const m_byte newType = randomWeaponType();
    clearObjectSlot(memory, weapon);

    memory.writeByte(weapon, newType);
    memory.copyLong(player + 0x10u, weapon + 0x10u);
    memory.copyLong(player + 0x14u, weapon + 0x14u);
    memory.writeWord(weapon + 0x10u,
                     static_cast<m_word>(memory.readWord(weapon + 0x10u) + xOffset));
    memory.writeLong(weapon + 0x18u, 0u);
    return newType;
}

const char *weaponName(m_byte type) {
    switch (type) {
        case 0x08u: return "knife";
        case 0x09u: return "bottle";
        case 0x0Au: return "baseball bat";
        case 0x0Bu: return "steel pipe";
        case 0x0Cu: return "pepper spray";
        default: return "unavailable";
    }
}

} // namespace

void SorRuntime::handleOptionHotkey(OptionHotkeyCode keyCode) {
    if (keyCode.source != OptionHotkeyCode::Source::Keyboard)
        return;

    switch (keyCode.keyboardKey) {
        case SDLK_L:
            incrementByte(memory(), kP1Lives, "P1 lives");
            return;
        case SDLK_S:
            incrementByte(memory(), kP1SpecialAttacks, "P1 special attacks");
            return;
        case SDLK_P: {
            const bool enabled = !SorCheats::p1PunchPowerEnabled();
            SorCheats::setP1PunchPowerEnabled(enabled);
            Logger::log("[cheat] P1 punch power x%u: %s",
                        static_cast<unsigned>(SorCheats::kPunchPowerMultiplier),
                        enabled ? "on" : "off");
            return;
        }
        case SDLK_K: {
            const int killed = killInstantiatedEnemies(memory());
            Logger::log("[cheat] killed %d instantiated enem%s",
                        killed,
                        killed == 1 ? "y" : "ies");
            return;
        }
        case SDLK_W: {
            const m_byte p1Weapon = spawnRandomWeaponOnGround(memory(), kP1Object, 16);
            const m_byte p2Weapon = spawnRandomWeaponOnGround(memory(), kP2Object, -16);
            Logger::log("[cheat] spawned random weapons: P1=%s, P2=%s",
                        weaponName(p1Weapon),
                        weaponName(p2Weapon));
            return;
        }
        case SDLK_G:
            // Alt+G — jump to good ending init (game_state $24).
            memory().writeWord(kGameState, kEndingGoodInit);
            Logger::log("[cheat] starting good ending (game_state=$%04X)",
                        static_cast<unsigned>(kEndingGoodInit));
            return;
        case SDLK_B:
            // Alt+B — jump to bad ending init (game_state $1C).
            memory().writeWord(kGameState, kEndingBadInit);
            Logger::log("[cheat] starting bad ending (game_state=$%04X)",
                        static_cast<unsigned>(kEndingBadInit));
            return;
        default:
            break;
    }

    const int level = levelFromTopRowNumber(keyCode.keyboardKey);
    if (level < 0)
        return;

    memory().writeWord(kLevel, static_cast<m_word>(level));
    memory().writeWord(kWave, 0);
    memory().writeWord(kGameState, kLevelIntroState);
    Logger::log("[cheat] loading level %d of %d", level + 1, kLevelCount);
}
