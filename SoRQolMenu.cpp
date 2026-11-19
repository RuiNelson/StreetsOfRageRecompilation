#include "SoRQolMenu.hpp"

#include "SoRHighScores.hpp"
#include "SoRInitialsKeyboard.hpp"
#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"
#include "SoRPlatform.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace SoRQolMenu {
namespace {

using namespace SoRMenuCanvas;
using SoRQolConfig::QolConfig;

constexpr int kTitleRow    = 3;
constexpr int kLabelColumn = 4;
constexpr int kValueColumn = 30;
constexpr int kHelpRow     = 22;
constexpr int kPages       = 2;

constexpr int kPlayersRow       = 19;
constexpr int kRoundRow         = 21;
constexpr int kCheatValueColumn = 18;

constexpr m_long kCheatFlag    = 0xFFFFFA7Bu;
constexpr m_long kLevel        = 0xFFFFFF02u;
constexpr m_long kLivesSetting = 0xFFFFFFCAu;
constexpr m_word kDefaultLives = 1; // the ROM's power-on value: 3 lives

enum class Row { RollCameraEarlier, FriendGrabKeepsWeapon, EnemyDamageCircles, SaveHighScores, InitialsP1, InitialsP2, CheatEnabled, Page, Save, Cancel };

std::vector<Row> rows(int page) {
    if (page == 0)
        return {Row::RollCameraEarlier, Row::FriendGrabKeepsWeapon, Row::EnemyDamageCircles, Row::CheatEnabled, Row::Page, Row::Save, Row::Cancel};
    return {Row::SaveHighScores, Row::InitialsP1, Row::InitialsP2, Row::Page, Row::Save, Row::Cancel};
}

int screenRow(Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
        case Row::SaveHighScores:
            return 7;
        case Row::FriendGrabKeepsWeapon:
        case Row::InitialsP1:
            return 9;
        case Row::EnemyDamageCircles:
        case Row::InitialsP2:
            return 11;
        case Row::CheatEnabled:
            return 13;
        case Row::Page:
            return 16;
        case Row::Save:
            return 18;
        case Row::Cancel:
            return 20;
    }
    return 0;
}

std::string_view label(Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
            return "ROLL CAMERA EARLIER";
        case Row::FriendGrabKeepsWeapon:
            return "FRIEND GRAB KEEPS WEAPON";
        case Row::EnemyDamageCircles:
            return "ENEMY DAMAGE CIRCLES";
        case Row::SaveHighScores:
            return "SAVE HIGH SCORES";
        case Row::InitialsP1:
            return "PLAYER 1 INITIALS";
        case Row::InitialsP2:
            return "PLAYER 2 INITIALS";
        case Row::CheatEnabled:
            return "CHEAT ENABLED";
        case Row::Page:
            return "";
        case Row::Save:
            return "SAVE AND APPLY";
        case Row::Cancel:
            return "CANCEL";
    }
    return "";
}

struct Menu {
    bool      isOpen  = false;
    int       page    = 0;
    int       cursor  = 0;
    bool      swallow = false; // ignore what is held when the menu opens
    QolConfig pending;
    SoRInitialsKeyboard::Keyboard keyboard;
    int       keyboardPlayer = 0;
    bool      typing = false;
    bool      askingSecret = false; // the message box of CHEAT ENABLED

    Repeat up, down, left, right, confirm, back;
    Shown  shown;
};

Menu &menu() {
    static Menu instance;
    return instance;
}

std::string &initialsOf(QolConfig &config, int player) {
    return player == 0 ? config.initialsP1 : config.initialsP2;
}

// Left, Right or confirm on a row that holds a value. False on the others.
bool changeValue(QolConfig &config, Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
            config.rollCameraEarlier = !config.rollCameraEarlier;
            return true;
        case Row::FriendGrabKeepsWeapon:
            config.friendGrabKeepsWeapon = !config.friendGrabKeepsWeapon;
            return true;
        case Row::EnemyDamageCircles:
            config.enemyDamageCircles = !config.enemyDamageCircles;
            return true;
        case Row::SaveHighScores:
            config.persistHighScores = !config.persistHighScores;
            return true;
        case Row::CheatEnabled:
            config.cheatEnabled = !config.cheatEnabled;
            return true;
        case Row::InitialsP1:
        case Row::InitialsP2:
        case Row::Page:
        case Row::Save:
        case Row::Cancel:
            break;
    }
    return false;
}

std::array<std::string, 2> help(const QolConfig &config, Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
            if (config.rollCameraEarlier)
                return {"THE CAMERA ROLLS WITH YOU FURTHER", "BACK, SO YOU SEE MORE OF THE STREET."};
            return {"THE CAMERA OF THE ORIGINAL GAME: IT", "ROLLS PAST THE MIDDLE OF THE SCREEN."};
        case Row::FriendGrabKeepsWeapon:
            if (config.friendGrabKeepsWeapon)
                return {"A FRIEND WHO GRABS YOU DOES NOT MAKE", "YOU DROP YOUR WEAPON. ENEMIES DO."};
            return {"AS IN THE ORIGINAL GAME: ANY GRAB", "MAKES YOU DROP YOUR WEAPON."};
        case Row::EnemyDamageCircles:
            if (config.enemyDamageCircles)
                return {"A CIRCLE OVER A HURT ENEMY FILLS UP", "WITH DAMAGE. FULL: THE ENEMY IS DEAD."};
            return {"AS IN THE ORIGINAL GAME: ONLY THE", "BOSSES SHOW THEIR HEALTH."};
        case Row::SaveHighScores:
            if (config.persistHighScores)
                return {"THE HIGH-SCORES ARE KEPT BETWEEN", "GAME RUNS."};
            return {"YOU'LL LOSE HIGH-SCORES WHEN YOU", "QUIT THE GAME."};
        case Row::InitialsP1:
            if (!config.initialsP1.empty())
                return {"PLAYER 1 DOES NOT TYPE A NAME ON A", "HIGH SCORE: THESE ARE ENTERED."};
            return {"AS IN THE ORIGINAL GAME: PLAYER 1", "TYPES A NAME ON A HIGH SCORE."};
        case Row::InitialsP2:
            if (!config.initialsP2.empty())
                return {"PLAYER 2 DOES NOT TYPE A NAME ON A", "HIGH SCORE. THE AI IS ALWAYS \"AI\"."};
            return {"AS IN THE ORIGINAL GAME: PLAYER 2", "TYPES A NAME. THE AI IS ALWAYS \"AI\"."};
        case Row::CheatEnabled:
            if (config.cheatEnabled)
                return {"OPTIONS ALWAYS LETS YOU CHOOSE THE", "LIVES AND THE STARTING ROUND."};
            return {"AS IN THE ORIGINAL GAME: THOSE ROWS", "NEED A SECRET BUTTON COMBINATION."};
        case Row::Page:
            return {"LEFT AND RIGHT CHANGE THE PAGE.", ""};
        case Row::Save:
            return {"USES THESE SETTINGS NOW, AND KEEPS", "THEM FOR NEXT TIME."};
        case Row::Cancel:
            return {"LEAVES WITHOUT CHANGING ANYTHING.", ""};
    }
    return {};
}

std::string_view onOff(bool value) {
    return value ? "ON" : "OFF";
}

void drawPage(const Menu &m, Canvas &canvas) {
    canvas.text(kLabelColumn, kTitleRow, "QUALITY-OF-LIFE", true);
    const std::vector<Row> list = rows(m.page);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Row  row      = list[i];
        const bool selected = static_cast<int>(i) == m.cursor;
        const int  y        = screenRow(row);
        canvas.text(kLabelColumn, y, label(row), selected);
        switch (row) {
            case Row::RollCameraEarlier:
                canvas.text(kValueColumn, y, onOff(m.pending.rollCameraEarlier), selected);
                break;
            case Row::FriendGrabKeepsWeapon:
                canvas.text(kValueColumn, y, onOff(m.pending.friendGrabKeepsWeapon), selected);
                break;
            case Row::EnemyDamageCircles:
                canvas.text(kValueColumn, y, onOff(m.pending.enemyDamageCircles), selected);
                break;
            case Row::SaveHighScores:
                canvas.text(kValueColumn, y, onOff(m.pending.persistHighScores), selected);
                break;
            case Row::InitialsP1:
                canvas.text(kValueColumn, y, m.pending.initialsP1.empty() ? "NOT SET" : m.pending.initialsP1, selected);
                break;
            case Row::InitialsP2:
                canvas.text(kValueColumn, y, m.pending.initialsP2.empty() ? "NOT SET" : m.pending.initialsP2, selected);
                break;
            case Row::CheatEnabled:
                canvas.text(kValueColumn, y, onOff(m.pending.cheatEnabled), selected);
                break;
            case Row::Page:
                canvas.text(kLabelColumn, y, "PAGE " + std::to_string(m.page + 1) + " OF " + std::to_string(kPages), selected);
                if (m.page > 0)
                    canvas.text(kValueColumn - 2, y, "PREV", selected);
                if (m.page < kPages - 1)
                    canvas.text(kValueColumn + 4, y, "NEXT", selected);
                break;
            case Row::Save:
            case Row::Cancel:
                break;
        }
    }
    const auto lines = help(m.pending, list[static_cast<std::size_t>(m.cursor)]);
    // White, as in the VIDEO menu: this screen's grey is too dark to read a sentence in.
    canvas.text(kLabelColumn, kHelpRow, lines[0], true);
    canvas.text(kLabelColumn, kHelpRow + 1, lines[1], true);
}

// Turning CHEAT ENABLED on takes the original game's kind of secret: Special+Punch+Jump+Right, all from one player.
constexpr SoRInput::ActionMask kSecret = SoRControlsConfig::actionBit(SoRControlsConfig::Action::Special) |
                                         SoRControlsConfig::actionBit(SoRControlsConfig::Action::Punch) |
                                         SoRControlsConfig::actionBit(SoRControlsConfig::Action::Jump) |
                                         SoRControlsConfig::actionBit(SoRControlsConfig::Action::Right);

bool secretHeld(SoRInput::ActionMask remote) {
    const auto holds = [](SoRInput::ActionMask mask) { return (mask & kSecret) == kSecret; };
    return holds(remote) || holds(SoRInput::heldActions(0)) || holds(SoRInput::heldActions(1));
}

void drawSecretBox(Canvas &canvas) {
    constexpr int kTop = 9, kBottom = 15;
    const std::string blank(34, ' '), rule(34, '-');
    for (int y = kTop; y <= kBottom; ++y)
        canvas.text(kLabelColumn, y, y == kTop || y == kBottom ? rule : blank, true);
    canvas.text(kLabelColumn + 2, kTop + 2, "PRESS THE SECRET COMBINATION,", true);
    canvas.text(kLabelColumn + 2, kTop + 4, SoRPlatform::consoleMenus() ? "OR PRESS OPTIONS." : "OR PRESS START.", true);
}

void openKeyboard(Menu &m, int player) {
    m.keyboard       = SoRInitialsKeyboard::open(std::string("PLAYER ") + (player == 0 ? "1" : "2") + " INITIALS",
                                                 initialsOf(m.pending, player));
    m.keyboardPlayer = player;
    m.typing         = true;
}

} // namespace

void drawOptionsRow(SystemMemory &memory, bool highlighted) {
    const int row = 17; // free in the ROM's layout: VIDEO is at 15
    drawText(memory, kLabelColumn, row, "QUALITY-OF-LIFE", highlighted);
}

void drawExitRow(SystemMemory &memory, bool highlighted) {
    // The ROM's bundles $0A and $10 put it at 18, against the row above.
    const int row = memory.readByte(kCheatFlag) != 0 ? 23 : 19;
    drawText(memory, kLabelColumn, row, "EXIT", highlighted);
}

void drawPlayersLabel(SystemMemory &memory, bool highlighted) {
    drawText(memory, kLabelColumn, kPlayersRow, "PLAYERS", highlighted);
}

void drawRoundLabel(SystemMemory &memory, bool highlighted) {
    drawText(memory, kLabelColumn, kRoundRow, "ROUND NO.", highlighted);
}

void drawPlayersDigit(SystemMemory &memory, m_word tile) {
    memory.writeLong(kVdpControl, vramWriteCommand(cellAddress(kCheatValueColumn, kPlayersRow)));
    memory.writeWord(kVdpData, tile);
}

void drawRoundDigit(SystemMemory &memory, m_word tile) {
    memory.writeLong(kVdpControl, vramWriteCommand(cellAddress(kCheatValueColumn, kRoundRow)));
    memory.writeWord(kVdpData, tile);
}

void open() {
    Menu &m   = menu();
    m         = Menu{};
    m.isOpen  = true;
    m.swallow = true;
    m.pending = SoRQolConfig::current();
}

bool isOpen() {
    return menu().isOpen;
}

Result frame(SystemMemory &memory, SoRControlsConfig::ActionMask remote) {
    Menu &m = menu();
    if (!m.isOpen)
        return Result::Closed;

    const SoRInput::MenuButtons held = SoRInput::menuButtons(remote, true);
    const bool typing  = m.typing;
    const bool up      = m.up.step(held.up, true);
    const bool down    = m.down.step(held.down, true);
    const bool left    = m.left.step(held.left, typing);
    const bool right   = m.right.step(held.right, typing);
    const bool confirm = m.confirm.step(held.confirm, false);
    const bool back    = m.back.step(held.back, false);

    bool close = false;
    if (m.swallow) {
        m.swallow = false; // a key still held from opening is not a press
    } else if (typing) {
        const auto result = SoRInitialsKeyboard::step(m.keyboard, memory, up, down, left, right, confirm, back);
        if (result == SoRInitialsKeyboard::Result::Ok)
            initialsOf(m.pending, m.keyboardPlayer) = SoRQolConfig::normalizeInitials(m.keyboard.text);
        if (result != SoRInitialsKeyboard::Result::Open)
            m.typing = false;
    } else if (m.askingSecret) {
        if (secretHeld(remote)) {
            m.pending.cheatEnabled = true;
            m.askingSecret         = false;
            playSound(memory, kSoundPick);
        } else if (back) {
            m.askingSecret = false;
            playSound(memory, kSoundPick);
        }
    } else {
        const std::vector<Row> list  = rows(m.page);
        const auto             count = static_cast<int>(list.size());
        const Row              row   = list[static_cast<std::size_t>(m.cursor)];
        const auto             goTo  = [&](int page) {
            m.page   = page;
            const std::vector<Row> next = rows(page);
            for (std::size_t i = 0; i < next.size(); ++i)
                if (next[i] == Row::Page)
                    m.cursor = static_cast<int>(i);
            playSound(memory, kSoundMove);
        };
        if (up || down) {
            m.cursor = (m.cursor + (up ? -1 : 1) + count) % count;
            playSound(memory, kSoundMove);
        } else if (back) {
            playSound(memory, kSoundPick);
            close = true;
        } else if (row == Row::Page && (left || right || confirm)) {
            if (left && m.page > 0)
                goTo(m.page - 1);
            else if (right && m.page < kPages - 1)
                goTo(m.page + 1);
            else if (confirm)
                goTo((m.page + 1) % kPages);
        } else if (row == Row::CheatEnabled && !m.pending.cheatEnabled && (left || right || confirm)) {
            m.askingSecret = true;
            playSound(memory, kSoundPick);
        } else if (left || right) {
            if (changeValue(m.pending, row))
                playSound(memory, kSoundMove);
        } else if (confirm) {
            playSound(memory, kSoundPick);
            switch (row) {
                case Row::InitialsP1:
                    openKeyboard(m, 0);
                    break;
                case Row::InitialsP2:
                    openKeyboard(m, 1);
                    break;
                case Row::Save:
                    if (!SoRQolConfig::save(m.pending, SoRQolConfig::defaultPath()))
                        std::fprintf(stderr, "Could not write %s\n", SoRQolConfig::defaultPath().string().c_str());
                    if (m.pending.persistHighScores && !SoRQolConfig::current().persistHighScores)
                        SoRHighScores::load(memory);
                    else if (!m.pending.persistHighScores)
                        SoRHighScores::erase();
                    // We are inside OPTIONS: the rows come and go when the screen is redrawn. Off, the choices go too.
                    if (m.pending.cheatEnabled != SoRQolConfig::current().cheatEnabled) {
                        memory.writeByte(kCheatFlag, m.pending.cheatEnabled ? 1 : 0);
                        if (!m.pending.cheatEnabled) {
                            memory.writeWord(kLevel, 0);
                            memory.writeWord(kLivesSetting, kDefaultLives);
                        }
                    }
                    SoRQolConfig::current() = m.pending;
                    close                   = true;
                    break;
                case Row::Cancel:
                    close = true;
                    break;
                default:
                    changeValue(m.pending, row);
                    break;
            }
        }
    }

    Canvas canvas;
    if (close) {
        flush(memory, m.shown, canvas); // clear plane A for the OPTIONS screen
        m.isOpen = false;
        return Result::Closed;
    }
    if (m.typing)
        SoRInitialsKeyboard::draw(m.keyboard, canvas);
    else
        drawPage(m, canvas);
    if (m.askingSecret)
        drawSecretBox(canvas);
    flush(memory, m.shown, canvas);
    return Result::Open;
}

} // namespace SoRQolMenu
