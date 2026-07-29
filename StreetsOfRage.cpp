#include "Sor.hpp"
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
constexpr m_word kEndingBadInit    = 0x001Cu; // init_ending_bad
constexpr m_word kEndingGoodInit   = 0x0024u; // init_ending_good
constexpr int    kLevelCount       = 8;
constexpr int    kObjectSlotCount  = 32;
constexpr m_long kObjectSlotSize   = 0x80u;

constexpr m_long kObjectPrimaryStateOffset = 0x30u;
constexpr m_long kObjectHealthOffset       = 0x32u;

constexpr bool isOrdinaryEnemy(m_byte type) {
    return type >= 0x20u && type <= 0x2Au;
}

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

int killInstantiatedEnemies(SystemMemory &memory) {
    const m_long activePlayer = activePlayerObject(memory);
    const m_word attacker = static_cast<m_word>(activePlayer != 0u ? activePlayer : kP1Object);
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

void StreetsOfRage::handleOptionHotkey(OptionHotkeyCode keyCode) {
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
            const bool enabled = !SoRCheats::p1PunchPowerEnabled();
            SoRCheats::setP1PunchPowerEnabled(enabled);
            Logger::log("[cheat] P1 punch power x%u: %s",
                        static_cast<unsigned>(SoRCheats::kPunchPowerMultiplier),
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
            const m_long player = activePlayerObject(memory());
            if (player == 0u) {
                Logger::log("[cheat] free police call unavailable: no active player");
                return;
            }
            SoRCheats::requestFreePoliceCall(player);
            Logger::log("[cheat] free police call requested for P%d", player == kP1Object ? 1 : 2);
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
