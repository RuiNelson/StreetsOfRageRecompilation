#include "SorRuntime.hpp"

#include <cstdio>

namespace {

constexpr m_long kGameState       = 0xFFFFFF00u;
constexpr m_long kLevel           = 0xFFFFFF02u;
constexpr m_long kWave            = 0xFFFFFF04u;
constexpr m_long kP1Lives         = 0xFFFFFF20u;
constexpr m_long kP1SpecialAttacks = 0xFFFFFF21u;
constexpr m_word kLevelIntroState = 0x0028u;
constexpr int    kLevelCount      = 8;

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
    std::fprintf(stderr, "[cheat] %s: %u -> %u\n", label, static_cast<unsigned>(before), static_cast<unsigned>(after));
    std::fflush(stderr);
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
        default:
            break;
    }

    const int level = levelFromTopRowNumber(keyCode.keyboardKey);
    if (level < 0)
        return;

    memory().writeWord(kLevel, static_cast<m_word>(level));
    memory().writeWord(kWave, 0);
    memory().writeWord(kGameState, kLevelIntroState);
    std::fprintf(stderr, "[cheat] loading level %d of %d\n", level + 1, kLevelCount);
    std::fflush(stderr);
}
