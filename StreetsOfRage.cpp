#include "SoR.hpp"
#include "SoRCheats.hpp"
#include "Logger.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

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
constexpr int    kLevelCount       = 8;
constexpr int    kObjectSlotCount  = 32;
constexpr m_long kObjectSlotSize   = 0x80u;

constexpr m_long kObjectPrimaryStateOffset = 0x30u;
constexpr m_long kObjectHealthOffset       = 0x32u;

constexpr bool isOrdinaryEnemy(m_byte type) {
    return type >= 0x20u && type <= 0x2Au;
}

// Ordinary-enemy families, by the object type at +$00. The same split the
// analysis manuscripts and the autoplay observer use, so a cheat named after
// an enemy kills exactly what the rest of the workspace calls by that name.
constexpr bool isGarcia(m_byte type) {
    return type >= 0x20u && type <= 0x23u; // includes the $23 "strong" palette
}

constexpr bool isSignal(m_byte type) {
    return type == 0x24u;
}

constexpr bool isHakuRo(m_byte type) {
    return type == 0x25u || type == 0x2Au; // the ninja, in both its variants
}

constexpr bool isNora(m_byte type) {
    return type == 0x26u;
}

constexpr bool isJack(m_byte type) {
    return type == 0x27u; // $28 is his thrown axe, an object of its own
}

static_assert(isGarcia(0x20u) && isGarcia(0x23u) && !isGarcia(0x24u));
static_assert(isSignal(0x24u) && !isSignal(0x25u));
static_assert(isHakuRo(0x25u) && isHakuRo(0x2Au) && !isHakuRo(0x26u));
static_assert(isNora(0x26u) && !isNora(0x27u));
static_assert(isJack(0x27u) && !isJack(0x28u));

constexpr bool isBespokeBoss(m_byte type) {
    return type == 0x30u || type == 0x35u; // Abadede or Mr. X
}

constexpr bool isSharedFrameworkBoss(m_byte type) {
    return type >= 0x55u && type <= 0x58u;
}

static_assert(isOrdinaryEnemy(0x20u) && isOrdinaryEnemy(0x2Au));
static_assert(!isOrdinaryEnemy(0x1Fu) && !isOrdinaryEnemy(0x2Bu));
static_assert(isBespokeBoss(0x30u) && isBespokeBoss(0x35u));
static_assert(isSharedFrameworkBoss(0x55u) && isSharedFrameworkBoss(0x58u));

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

m_long activePlayerObject(SystemMemory &memory) {
    if (memory.readByte(kP1Object) == 1u)
        return kP1Object;
    if (memory.readByte(kP2Object) == 1u)
        return kP2Object;
    return 0u;
}

// True once this enemy's health word has crossed the cartridge's own lethal
// boundary. The ROM's ordinary-enemy checks are signed, so $8000-$FFFF is
// already dead while the object still occupies its slot playing out the death
// reaction. Killing such an object again restarts that reaction from the top:
// held down (or swept repeatedly by an automated harness), the same corpses
// are re-killed every time and never finish dying.
bool isAlreadyDying(SystemMemory &memory, m_long object) {
    return memory.readWord(object + kObjectHealthOffset) >= 0x8000u;
}

// An object slot whose primary state is still zero is **mid-spawn**, not a
// live enemy: a wave's slots are populated before `$937A` runs, so for one
// frame they hold a complete, visible, uninitialised entity -- type byte
// already written, everything else not yet. autoplay's observer had to learn
// the same thing from the read side (`world_map._is_dormant_combatant`, which
// records five of them appearing for a single tick at state $00 with zero
// health and zero velocity, and the AI punching at the nearest).
//
// Writing a death into one of those is what reset the console. The family
// sweep runs twice a second for a whole level, so it lands on that one-frame
// window every so often -- measured at four runs in sixteen -- and a trace
// caught the transition exactly: the actor walking normally at full health
// with four lives, reaching x=2268, which is precisely where round 2's wave 2
// spawns, and the very next poll reading level 0, 'Sega logo', lives 0.
bool isStillSpawning(SystemMemory &memory, m_long object) {
    return memory.readWord(object + kObjectPrimaryStateOffset) == 0u;
}

// Put one ordinary enemy through the cartridge's own forced-death sweep.
// Split out of killInstantiatedEnemies so the per-family cheats below kill
// exactly the way the kill-everything cheat already does.
void killOrdinaryEnemy(SystemMemory &memory, m_long object, m_word attacker) {
    // Match the cartridge's forced-death sweep: enter the airborne/death
    // reaction with negative health and retain a player for score credit.
    memory.writeByte(object + 0x37u, memory.readByte(object + 0x37u) | 0x02u);
    memory.writeWord(object + kObjectHealthOffset, 0xFFFFu);
    memory.writeWord(object + kObjectPrimaryStateOffset, 0x0300u);
    memory.writeWord(object + 0x3Eu, attacker);
}

// Kill every instantiated ordinary enemy whose type ``matches``. Bosses are
// deliberately out of scope: they have their own lethal paths (see
// killInstantiatedEnemies) and no family cheat names one.
int killOrdinaryEnemiesMatching(SystemMemory &memory, bool (*matches)(m_byte)) {
    const m_long activePlayer = activePlayerObject(memory);
    const m_word attacker = static_cast<m_word>(activePlayer != 0u ? activePlayer : kP1Object);
    int killed = 0;

    for (int slot = 0; slot < kObjectSlotCount; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * kObjectSlotSize;
        const m_byte type = memory.readByte(object);

        if (!isOrdinaryEnemy(type) || !matches(type))
            continue;
        if (isAlreadyDying(memory, object) || isStillSpawning(memory, object))
            continue;

        killOrdinaryEnemy(memory, object, attacker);
        ++killed;
    }

    return killed;
}

void logFamilyKill(const char *family, int killed) {
    Logger::log("[cheat] killed %d %s%s", killed, family, killed == 1 ? "" : "s");
}

int killInstantiatedEnemies(SystemMemory &memory) {
    const m_long activePlayer = activePlayerObject(memory);
    const m_word attacker = static_cast<m_word>(activePlayer != 0u ? activePlayer : kP1Object);
    int killed = 0;

    for (int slot = 0; slot < kObjectSlotCount; ++slot) {
        const m_long object = kObjectTable + static_cast<m_long>(slot) * kObjectSlotSize;
        const m_byte type = memory.readByte(object);

        // Same one-frame spawn window the family sweep has to skip -- see
        // isStillSpawning. This is the K hotkey, pressed by hand rather than
        // twice a second, so it is far less likely to land on it; the guard is
        // here because the hazard is identical, not because it was measured
        // from this path.
        if (isStillSpawning(memory, object))
            continue;

        if (isOrdinaryEnemy(type)) {
            killOrdinaryEnemy(memory, object, attacker);
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

} // namespace

StreetsOfRage::~StreetsOfRage() {
    if (callLog_ != nullptr)
        std::fclose(callLog_);
}

void StreetsOfRage::setCallLog(const std::string &path) {
    if (callLog_ != nullptr) {
        std::fclose(callLog_);
        callLog_ = nullptr;
    }
    callLogPending_ = 0;

    callLog_ = std::fopen(path.c_str(), "wb");
    if (callLog_ == nullptr) {
        throw std::runtime_error("Cannot open call log '" + path + "': " + std::strerror(errno));
    }

    std::fputs("event,source,callsite,target\n", callLog_);
    std::fflush(callLog_);
}

void StreetsOfRage::logEntry(m_long entry) {
    if (callLog_ == nullptr)
        return;

    std::fprintf(callLog_,
                 "entry,%06X,,\n",
                 static_cast<unsigned>(entry & 0x00FFFFFFu));
    if (++callLogPending_ >= 4096u) {
        std::fflush(callLog_);
        callLogPending_ = 0;
    }
}

void StreetsOfRage::logCall(m_long source, m_long callsite, m_long target) {
    if (callLog_ == nullptr)
        return;

    std::fprintf(callLog_,
                 "call,%06X,%06X,%06X\n",
                 static_cast<unsigned>(source & 0x00FFFFFFu),
                 static_cast<unsigned>(callsite & 0x00FFFFFFu),
                 static_cast<unsigned>(target & 0x00FFFFFFu));
    if (++callLogPending_ >= 4096u) {
        std::fflush(callLog_);
        callLogPending_ = 0;
    }
}

// Every hotkey that changes emulated RAM only *records* what it wants here.
// This runs on the main thread; the game runs on the CPU thread, and writing
// object-table bytes from under it is what resets the console. See
// SoRCheats.hpp's note, and applyPendingSoRCheats below.
void StreetsOfRage::handleOptionHotkey(OptionHotkeyCode keyCode) {
    if (keyCode.source != OptionHotkeyCode::Source::Keyboard)
        return;

    switch (keyCode.keyboardKey) {
        case SDLK_L:
            SoRCheats::requestCheats(SoRCheats::kCheatAddLife);
            return;
        case SDLK_S:
            SoRCheats::requestCheats(SoRCheats::kCheatAddSpecial);
            return;
        case SDLK_P: {
            const bool enabled = !SoRCheats::p1PunchPowerEnabled();
            SoRCheats::setP1PunchPowerEnabled(enabled);
            Logger::log("[cheat] P1 punch power x%u: %s",
                        static_cast<unsigned>(SoRCheats::kPunchPowerMultiplier),
                        enabled ? "on" : "off");
            return;
        }
        case SDLK_K:
            SoRCheats::requestCheats(SoRCheats::kCheatKillAll);
            return;
        case SDLK_W: {
            const m_long player = activePlayerObject(memory());
            if (player == 0u) {
                Logger::log("[cheat] free police call unavailable: no active player");
                return;
            }
            SoRCheats::requestFreePoliceCall(player);
            Logger::log("[cheat] free police call requested for P%d", player == kP1Object ? 1 : 2);
            return;
        }
        // Per-family kill cheats. Each letter is the enemy's own initial
        // where that was free: G(arcia), N(inja, the Haku-Ro), J(ack). Signal
        // takes B and Nora takes U, since S already adds a special attack and
        // N is the ninja. G and B were the good/bad ending jumps until those
        // were dropped as no longer needed.
        case SDLK_G:
            SoRCheats::requestCheats(SoRCheats::kCheatKillGarcia);
            return;
        case SDLK_N:
            SoRCheats::requestCheats(SoRCheats::kCheatKillHakuRo);
            return;
        case SDLK_B:
            SoRCheats::requestCheats(SoRCheats::kCheatKillSignal);
            return;
        case SDLK_J:
            SoRCheats::requestCheats(SoRCheats::kCheatKillJack);
            return;
        case SDLK_U:
            SoRCheats::requestCheats(SoRCheats::kCheatKillNora);
            return;
        default:
            break;
    }

    const int level = levelFromTopRowNumber(keyCode.keyboardKey);
    if (level < 0)
        return;

    SoRCheats::requestLevelJump(level);
}

// The CPU-thread half. Called from the vblank waits in
// SoRManualFunctions.cpp, which is the game's own frame boundary: its logic
// for the frame is done and it is waiting for the interrupt, so no object
// update can be part-way through a record these writes are about to change.
void applyPendingSoRCheats(SystemMemory &memory) {
    const unsigned pending = SoRCheats::consumeCheats();
    if (pending != SoRCheats::kCheatNone) {
        if (pending & SoRCheats::kCheatAddLife)
            incrementByte(memory, kP1Lives, "P1 lives");
        if (pending & SoRCheats::kCheatAddSpecial)
            incrementByte(memory, kP1SpecialAttacks, "P1 special attacks");
        if (pending & SoRCheats::kCheatKillAll) {
            const int killed = killInstantiatedEnemies(memory);
            Logger::log("[cheat] killed %d instantiated enem%s",
                        killed,
                        killed == 1 ? "y" : "ies");
        }
        if (pending & SoRCheats::kCheatKillGarcia)
            logFamilyKill("Garcia", killOrdinaryEnemiesMatching(memory, isGarcia));
        if (pending & SoRCheats::kCheatKillHakuRo)
            logFamilyKill("Haku-Ro", killOrdinaryEnemiesMatching(memory, isHakuRo));
        if (pending & SoRCheats::kCheatKillSignal)
            logFamilyKill("Signal", killOrdinaryEnemiesMatching(memory, isSignal));
        if (pending & SoRCheats::kCheatKillJack)
            logFamilyKill("Jack", killOrdinaryEnemiesMatching(memory, isJack));
        if (pending & SoRCheats::kCheatKillNora)
            logFamilyKill("Nora", killOrdinaryEnemiesMatching(memory, isNora));
    }

    const int level = SoRCheats::consumeLevelJump();
    if (level < 0)
        return;
    memory.writeWord(kLevel, static_cast<m_word>(level));
    memory.writeWord(kWave, 0);
    memory.writeWord(kGameState, kLevelIntroState);
    Logger::log("[cheat] loading level %d of %d", level + 1, kLevelCount);
}

void StreetsOfRage::dumpUnhandledDispatchCpuState() {
    std::fprintf(stderr,
                 "[dispatch] SR=$%04X SSP=$%06X USP=$%06X PC=$%06X\n",
                 static_cast<unsigned>(cpu().status()),
                 static_cast<unsigned>(cpu().ssp & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().usp & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().pc & 0x00FFFFFFu));
    std::fprintf(stderr,
                 "[dispatch] D0=$%08X D1=$%08X D2=$%08X D3=$%08X "
                 "D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                 static_cast<unsigned>(cpu().d[0]),
                 static_cast<unsigned>(cpu().d[1]),
                 static_cast<unsigned>(cpu().d[2]),
                 static_cast<unsigned>(cpu().d[3]),
                 static_cast<unsigned>(cpu().d[4]),
                 static_cast<unsigned>(cpu().d[5]),
                 static_cast<unsigned>(cpu().d[6]),
                 static_cast<unsigned>(cpu().d[7]));
    std::fprintf(stderr,
                 "[dispatch] A0=$%06X A1=$%06X A2=$%06X A3=$%06X "
                 "A4=$%06X A5=$%06X A6=$%06X\n",
                 static_cast<unsigned>(cpu().a[0] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().a[1] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().a[2] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().a[3] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().a[4] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().a[5] & 0x00FFFFFFu),
                 static_cast<unsigned>(cpu().a[6] & 0x00FFFFFFu));

    const m_long a0 = cpu().a[0] & 0x00FFFFFFu;
    std::fprintf(stderr,
                 "[dispatch] object@A0 type=%02X flags=%02X state30=%02X next31=%02X "
                 "ptr4=%08X anim8=%04X timerE=%04X stack=%08X %08X %08X\n",
                 static_cast<unsigned>(memory().readByte(a0 + 0)),
                 static_cast<unsigned>(memory().readByte(a0 + 1)),
                 static_cast<unsigned>(memory().readByte(a0 + 0x30)),
                 static_cast<unsigned>(memory().readByte(a0 + 0x31)),
                 static_cast<unsigned>(memory().readLong(a0 + 4)),
                 static_cast<unsigned>(memory().readWord(a0 + 8)),
                 static_cast<unsigned>(memory().readWord(a0 + 0x0E)),
                 static_cast<unsigned>(memory().readLong(cpu().ssp)),
                 static_cast<unsigned>(memory().readLong(cpu().ssp + 4)),
                 static_cast<unsigned>(memory().readLong(cpu().ssp + 8)));
}
