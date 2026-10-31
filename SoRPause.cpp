#include "SoRPause.hpp"

#include "SoRMenuCanvas.hpp"
#include "SoRRemoteInput.hpp"
#include "SoR.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace {

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
constexpr m_long kP1Continues         = 0xFFFFFF1Au;
constexpr m_long kP2Continues         = 0xFFFFFF1Cu;
constexpr m_long kP1Lives             = 0xFFFFFF20u;
constexpr m_long kP2Lives             = 0xFFFFFF23u;
constexpr m_long kP1Object            = 0xFFFFB800u;
constexpr m_long kP2Object            = 0xFFFFB880u;

constexpr m_long kObjectHealth = 0x32u;

constexpr m_byte kButtonUp    = 0x01u;
constexpr m_byte kButtonDown  = 0x02u;
constexpr m_byte kButtonFace  = 0x70u;
constexpr m_byte kButtonStart = 0x80u;

constexpr m_byte kSoundMove  = 0xB9u; // the menus' cursor sound
constexpr m_byte kSoundPick  = 0xBAu; // the menus' confirm sound
constexpr m_byte kSoundPause = 0xBDu; // the jingle, whose sequence freezes or releases the music

constexpr m_byte kObjectPlayer   = 0x01u;
constexpr m_byte kObjectContinue = 0x0Fu;

enum class Screen { Main, Confirm };

struct Menu {
    bool   open   = false;
    Screen screen = Screen::Main;
    int    cursor = 0; // Main: RESUME, FORFEIT. Confirm: NO, YES.
    int    asking = 0; // Confirm: the player answering
    int    yes    = 0; // Confirm: the players who said YES so far, as player_mode's bits
};

// What is on screen, for the render thread. Everything else is the CPU thread's.
std::mutex menuMutex;
Menu       shownMenu;

Menu menu;

// A player who forfeited stays forfeited until the game has them on the
// continue object: the ROM has paths where a player at zero health keeps the
// life (a fall takes none; the life may already be gone when they say yes).
std::array<bool, 2> forfeiting{};

using Glyph = std::array<m_byte, 64>;
std::array<Glyph, 0x30> font{};
bool                    fontKept = false;

void publish() {
    const std::lock_guard lock(menuMutex);
    shownMenu = menu;
}

int nextToAsk(m_byte playerMode, int after) {
    // With the AI on P2 there is one person to ask, and their answer is for both.
    if (SoRRemoteInput::aiAssist())
        return after < 0 && (playerMode & 1u) != 0 ? 0 : -1;
    for (int player = after + 1; player < 2; ++player)
        if ((playerMode & (1u << player)) != 0)
            return player;
    return -1;
}

enum class Outcome { Stay, Resume };

// One paused frame. @p presses are the players' new presses (those not in the game: 0).
Outcome menuFrame(SystemMemory &memory, const std::array<m_byte, 2> &presses, m_byte playerMode) {
    const auto play = [&](m_byte id) { memory.writeByte(kPlaySe, id); };
    const auto bothPresses = static_cast<m_byte>(presses[0] | presses[1]);

    if (menu.screen == Screen::Main) {
        if ((bothPresses & kButtonStart) != 0)
            return Outcome::Resume;
        if ((bothPresses & (kButtonUp | kButtonDown)) != 0) {
            menu.cursor = 1 - menu.cursor;
            play(kSoundMove);
        } else if ((bothPresses & kButtonFace) != 0) {
            if (menu.cursor == 0)
                return Outcome::Resume;
            const int first = nextToAsk(playerMode, -1);
            if (first >= 0) {
                play(kSoundPick);
                menu.screen = Screen::Confirm;
                menu.asking = first;
                menu.cursor = 0;
                menu.yes    = 0;
            }
        }
        return Outcome::Stay;
    }

    // Only the player asked answers for themself. Anyone's Start is a NO, so a
    // question nobody can answer (an AI at the other seat) never holds the game.
    const m_byte press  = presses[static_cast<std::size_t>(menu.asking)];
    bool         answer = (bothPresses & kButtonStart) != 0;
    bool         isYes  = false;
    if (!answer) {
        if ((press & (kButtonUp | kButtonDown)) != 0) {
            menu.cursor = 1 - menu.cursor;
            play(kSoundMove);
        } else if ((press & kButtonFace) != 0) {
            answer = true;
            isYes  = menu.cursor == 1;
        }
    }
    if (!answer)
        return Outcome::Stay;

    play(kSoundPick);
    if (isYes)
        menu.yes |= SoRRemoteInput::aiAssist() ? static_cast<int>(playerMode) : 1 << menu.asking;
    if (const int next = nextToAsk(playerMode, menu.asking); next >= 0) {
        menu.asking = next;
        menu.cursor = 0;
        return Outcome::Stay;
    }
    if (menu.yes != 0)
        return Outcome::Resume;
    menu.screen = Screen::Main;
    menu.cursor = 1;
    return Outcome::Stay;
}

// Native colours: the framebuffer's are 0-7.
constexpr m_byte kSelected   = 7;
constexpr m_byte kUnselected = 4;

void drawText(Framebuffer &framebuffer, int x, int y, std::string_view text, m_byte level) {
    for (const char c : text) {
        const auto tile = SoRMenuCanvas::glyph(c);
        if (tile && *tile < font.size()) {
            const Glyph &glyph = font[*tile];
            for (int row = 0; row < 8; ++row) {
                for (int column = 0; column < 8; ++column) {
                    // The font's colour 1 is the letter, $C its shadow, 2 the screen's background.
                    const m_byte pixel = glyph[static_cast<std::size_t>(row * 8 + column)];
                    if (pixel == 1)
                        framebuffer.setPixel(x + column, y + row, level, level, level);
                    else if (pixel == 0x0C)
                        framebuffer.setPixel(x + column, y + row, 0, 0, 0);
                }
            }
        }
        x += 8;
    }
}

void darken(Framebuffer &framebuffer, int left, int top, int right, int bottom) {
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            m_byte b = 0;
            m_byte g = 0;
            m_byte r = 0;
            framebuffer.getPixel(x, y, b, g, r);
            framebuffer.setPixel(x, y, b >> 2, g >> 2, r >> 2);
        }
    }
}

} // namespace

namespace SoRPause {

void onModeMenu(std::span<const m_byte> vram) {
    for (std::size_t tile = 0; tile < font.size(); ++tile) {
        for (std::size_t byte = 0; byte < 32; ++byte) {
            const m_byte packed = vram[tile * 32 + byte];
            font[tile][byte * 2]     = static_cast<m_byte>(packed >> 4);
            font[tile][byte * 2 + 1] = static_cast<m_byte>(packed & 0x0Fu);
        }
    }
    fontKept   = true;
    forfeiting = {};
    menu       = Menu{};
    publish();
}

void drawOverlay(SystemMemory &memory, Framebuffer &framebuffer, int width, int height) {
    // The flag, not the menu alone: a cold restart clears it under the menu.
    if (memory.readByte(kPauseTextFlag) == 0 || !fontKept)
        return;
    Menu shown;
    {
        const std::lock_guard lock(menuMutex);
        shown = shownMenu;
    }
    if (!shown.open)
        return;

    // Below the ROM's word PAUSE, on lines 88-103 of the 224-line picture; widescreen hides the first 8.
    constexpr int kFirstLine = 112;
    constexpr int kLineStep  = 12;
    const int     top        = kFirstLine - (height < 224 ? 8 : 0);
    const int     centre     = width / 2;

    const std::string question = "ARE YOU SURE PLAYER " + std::to_string(shown.asking + 1) + "?";
    const bool        confirm  = shown.screen == Screen::Confirm;
    const int         lines    = confirm ? 3 : 2;
    const int         halfWide = confirm ? static_cast<int>(question.size()) * 4 + 8 : 48;

    darken(framebuffer,
           std::max(0, centre - halfWide),
           top - 6,
           std::min(width, centre + halfWide),
           std::min(height, top + lines * kLineStep + 2));

    int line = top;
    if (confirm) {
        drawText(framebuffer, centre - static_cast<int>(question.size()) * 4, line, question, kSelected);
        line += kLineStep;
    }
    const std::array<std::string_view, 2> entries =
        confirm ? std::array<std::string_view, 2>{"NO", "YES"} : std::array<std::string_view, 2>{"RESUME", "FORFEIT"};
    const int entryColumn = centre - 28;
    for (int i = 0; i < 2; ++i) {
        const bool selected = i == shown.cursor;
        if (selected)
            drawText(framebuffer, entryColumn - 12, line, ">", kSelected);
        drawText(framebuffer, entryColumn, line, entries[static_cast<std::size_t>(i)], selected ? kSelected : kUnselected);
        line += kLineStep;
    }
}

} // namespace SoRPause

void StreetsOfRage::handle_pause_start_input(m_long entry_) {
    traceEnter(entry_);

    const auto finishWithPauseFlag = [this] {
        cpu().setNZClearVC(memory().readByte(kPauseTextFlag), 0x80u);
        cpu().ssp += 4;
    };

    if (memory().readByte(kStopMusic) != 0u) {
        finishWithPauseFlag();
        return;
    }

    const m_byte pauseFlag = memory().readByte(kPauseTextFlag);
    cpu().d[7]             = pauseFlag == 0u ? 3u : 0u;

    bool                  pauseRequested = false;
    const m_byte          playerMode     = memory().readByte(kPlayerMode);
    std::array<m_byte, 2> presses{};

    if ((playerMode & 0x01u) != 0u) {
        const m_byte buffered =
            static_cast<m_byte>(memory().readByte(kP1StartButtonBuffer) | memory().readByte(kP1ButtonPress));
        memory().writeByte(kP1StartButtonBuffer, buffered);
        presses[0] = buffered;
        if ((buffered & kButtonStart) != 0u)
            pauseRequested = true;
    }

    if ((pauseFlag != 0u || !pauseRequested) && (playerMode & 0x02u) != 0u) {
        const m_byte buffered =
            static_cast<m_byte>(memory().readByte(kP2StartButtonBuffer) | memory().readByte(kP2ButtonPress));
        memory().writeByte(kP2StartButtonBuffer, buffered);
        presses[1] = buffered;
        if ((buffered & kButtonStart) != 0u)
            pauseRequested = true;
    }

    // Kills a player the way the time-over does ($109DE): the ROM's own zero
    // health, on the ROM's own KO path, with one life and no continue left.
    const auto forfeit = [this](int player) {
        memory().writeWord(player == 0 ? kP1Continues : kP2Continues, 0u);
        memory().writeByte(player == 0 ? kP1Lives : kP2Lives, 1u);
        cpu().a[0] = player == 0 ? kP1Object : kP2Object;
        cpu().d[7] = 0xFFFFFFB0u; // -$50
        const m_long sp = cpu().ssp;
        logCallFromReturn(0x000109E8u, 0x00004E6Cu);
        cpu().ssp -= 4;
        memory().writeLong(cpu().ssp, 0x000109E8u);
        adjust_player_health();
        if ((cpu().ssp & 0x00FFFFFFu) > (sp & 0x00FFFFFFu))
            return false;
        logCallFromReturn(0x00001F1Au, 0x00004E14u);
        cpu().ssp -= 4;
        memory().writeLong(cpu().ssp, 0x00001F1Au);
        draw_player_lives_and_specials();
        return (cpu().ssp & 0x00FFFFFFu) <= (sp & 0x00FFFFFFu);
    };

    if (pauseFlag != 0u) {
        memory().writeByte(kP1StartButtonBuffer, 0u);
        memory().writeByte(kP2StartButtonBuffer, 0u);
        // The AI's input is not a person's answer.
        if (SoRRemoteInput::aiAssist())
            presses[1] = 0u;
        if (!menu.open) {
            menu      = Menu{};
            menu.open = true;
        }
        const Outcome outcome = menuFrame(memory(), presses, playerMode);
        if (outcome == Outcome::Resume) {
            const int yes = menu.yes;
            menu          = Menu{};
            publish();
            memory().writeByte(kPauseTextFlag, 0u);
            memory().writeByte(kPlaySe, kSoundPause);
            // The press that chose the entry stays in the menu: on this same
            // frame the players would read it as Special, Punch or Jump.
            memory().writeByte(kP1ButtonPress, 0u);
            memory().writeByte(kP2ButtonPress, 0u);
            for (int player = 0; player < 2; ++player) {
                if ((yes & (1 << player)) != 0) {
                    forfeiting[static_cast<std::size_t>(player)] = true;
                    if (!forfeit(player))
                        return;
                }
            }
        } else {
            publish();
        }
        finishWithPauseFlag();
        return;
    }

    for (int player = 0; player < 2; ++player) {
        const auto index = static_cast<std::size_t>(player);
        if (!forfeiting[index])
            continue;
        const m_long object = player == 0 ? kP1Object : kP2Object;
        const m_byte type   = memory().readByte(object);
        if (type == kObjectContinue)
            forfeiting[index] = false;
        else if (type == kObjectPlayer && memory().readWord(object + kObjectHealth) != 0u && !forfeit(player))
            return;
    }

    if (pauseRequested) {
        if (memory().readByte(kDemoMode) != 0u) {
            const m_byte fade = memory().readByte(kFadeOutFlag);
            if (fade != 0u) {
                cpu().setNZClearVC(fade, 0x80u);
                cpu().ssp += 4;
                return;
            }

            memory().writeByte(kDemoMode, static_cast<m_byte>(memory().readByte(kDemoMode) | 0x80u));
            memory().writeByte(kFadeOutFlag, 1u);
            memory().writeWord(kPaletteFadeCounter, 0x0040u);
            cpu().setNZClearVC(0x0040u, 0x8000u);
            cpu().ssp += 4;
            return;
        }

        memory().writeByte(kPauseTextFlag, 3u);
        memory().writeByte(kPlaySe, kSoundPause);
        menu      = Menu{};
        menu.open = true;
        publish();
    }

    memory().writeByte(kP1StartButtonBuffer, 0u);
    memory().writeByte(kP2StartButtonBuffer, 0u);
    finishWithPauseFlag();
}
