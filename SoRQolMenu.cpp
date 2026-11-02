#include "SoRQolMenu.hpp"

#include "SoRHighScores.hpp"
#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"

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

constexpr m_long kCheatFlag    = 0xFFFFFA7Bu;
constexpr m_long kLevel        = 0xFFFFFF02u;
constexpr m_long kLivesSetting = 0xFFFFFFCAu;
constexpr m_word kDefaultLives = 1; // the ROM's power-on value: 3 lives

enum class Row { RollCameraEarlier, FriendGrabKeepsWeapon, EnemyDamageCircles, SaveHighScores, InitialsP1, InitialsP2, CheatEnabled, Page, Save, Cancel };

std::vector<Row> rows(int page) {
    if (page == 0)
        return {Row::RollCameraEarlier, Row::FriendGrabKeepsWeapon, Row::EnemyDamageCircles, Row::SaveHighScores, Row::Page, Row::Save, Row::Cancel};
    return {Row::InitialsP1, Row::InitialsP2, Row::CheatEnabled, Row::Page, Row::Save, Row::Cancel};
}

int screenRow(Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
        case Row::InitialsP1:
            return 7;
        case Row::FriendGrabKeepsWeapon:
        case Row::InitialsP2:
            return 9;
        case Row::EnemyDamageCircles:
        case Row::CheatEnabled:
            return 11;
        case Row::SaveHighScores:
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

// The on-screen keyboard that sets a player's initials.
constexpr std::array<std::string_view, 3> kLetterRows = {"ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ"};
enum class Special { Space, Delete, Ok, Cancel };
constexpr std::array<Special, 4> kSpecials    = {Special::Space, Special::Delete, Special::Ok, Special::Cancel};
constexpr std::array<int, 4>     kSpecialCols = {8, 15, 21, 26};
constexpr int                    kKeyRows     = 4;
constexpr int                    kKeyColumn   = 8;
constexpr int                    kKeyStride   = 3;
constexpr int                    kFirstKeyRow = 9;
constexpr int                    kInitials    = 3;

std::string_view specialLabel(Special key) {
    switch (key) {
        case Special::Space:
            return "SPACE";
        case Special::Delete:
            return "DEL";
        case Special::Ok:
            return "OK";
        case Special::Cancel:
            return "CANCEL";
    }
    return "";
}

struct Keyboard {
    bool        isOpen = false;
    int         player = 0; // 0 or 1
    std::string text;
    int         keyRow = 0;
    int         keyCol = 0;
};

int keysInRow(int keyRow) {
    return keyRow < 3 ? static_cast<int>(kLetterRows[static_cast<std::size_t>(keyRow)].size()) : static_cast<int>(kSpecials.size());
}

struct Menu {
    bool      isOpen  = false;
    int       page    = 0;
    int       cursor  = 0;
    bool      swallow = false; // ignore what is held when the menu opens
    QolConfig pending;
    Keyboard  keyboard;

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

void drawKeyboard(const Keyboard &k, Canvas &canvas) {
    canvas.text(kLabelColumn, kTitleRow, std::string("PLAYER ") + (k.player == 0 ? "1" : "2") + " INITIALS", true);
    std::string shown = k.text;
    shown.resize(kInitials, '-');
    canvas.text(kKeyColumn, 6, "INITIALS: " + shown, true);

    for (int keyRow = 0; keyRow < kKeyRows; ++keyRow) {
        const int y = kFirstKeyRow + keyRow * 2;
        for (int col = 0; col < keysInRow(keyRow); ++col) {
            const bool selected = keyRow == k.keyRow && col == k.keyCol;
            if (keyRow < 3) {
                const char letter = kLetterRows[static_cast<std::size_t>(keyRow)][static_cast<std::size_t>(col)];
                canvas.text(kKeyColumn + col * kKeyStride, y, std::string(1, letter), selected);
            } else {
                canvas.text(kSpecialCols[static_cast<std::size_t>(col)], y, specialLabel(kSpecials[static_cast<std::size_t>(col)]), selected);
            }
        }
    }
    canvas.text(kLabelColumn, kHelpRow, "MOVE AND CONFIRM TO PICK A LETTER.", true);
    canvas.text(kLabelColumn, kHelpRow + 1, "BACK LEAVES WITHOUT CHANGES.", true);
}

void openKeyboard(Menu &m, int player) {
    m.keyboard        = Keyboard{};
    m.keyboard.isOpen = true;
    m.keyboard.player = player;
    m.keyboard.text   = initialsOf(m.pending, player);
}

// One frame of the keyboard. True when it closes.
bool keyboardFrame(Menu &m, SystemMemory &memory, bool up, bool down, bool left, bool right, bool confirm, bool back) {
    Keyboard &k = m.keyboard;
    if (back) {
        playSound(memory, kSoundPick);
        return true;
    }
    if (up || down) {
        k.keyRow = (k.keyRow + (up ? -1 : 1) + kKeyRows) % kKeyRows;
        k.keyCol = std::min(k.keyCol, keysInRow(k.keyRow) - 1);
        playSound(memory, kSoundMove);
    } else if (left || right) {
        const int count = keysInRow(k.keyRow);
        k.keyCol        = (k.keyCol + (left ? -1 : 1) + count) % count;
        playSound(memory, kSoundMove);
    } else if (confirm) {
        playSound(memory, kSoundPick);
        if (k.keyRow < 3) {
            if (static_cast<int>(k.text.size()) < kInitials)
                k.text += kLetterRows[static_cast<std::size_t>(k.keyRow)][static_cast<std::size_t>(k.keyCol)];
            return false;
        }
        switch (kSpecials[static_cast<std::size_t>(k.keyCol)]) {
            case Special::Space:
                if (static_cast<int>(k.text.size()) < kInitials)
                    k.text += ' ';
                break;
            case Special::Delete:
                if (!k.text.empty())
                    k.text.pop_back();
                break;
            case Special::Ok:
                initialsOf(m.pending, k.player) = SoRQolConfig::normalizeInitials(k.text);
                return true;
            case Special::Cancel:
                return true;
        }
    }
    return false;
}

} // namespace

void drawOptionsRow(SystemMemory &memory, bool highlighted) {
    const int row = 17; // free in the ROM's layout: VIDEO is at 15
    drawText(memory, kLabelColumn, row, "QUALITY-OF-LIFE", highlighted);
}

void drawExitRow(SystemMemory &memory, bool highlighted) {
    const int row = 19; // the ROM's bundles $0A and $10 put it at 18, against the row above
    drawText(memory, kLabelColumn, row, "EXIT", highlighted);
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
    const bool typing  = m.keyboard.isOpen;
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
        if (keyboardFrame(m, memory, up, down, left, right, confirm, back))
            m.keyboard.isOpen = false;
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
    if (m.keyboard.isOpen)
        drawKeyboard(m.keyboard, canvas);
    else
        drawPage(m, canvas);
    flush(memory, m.shown, canvas);
    return Result::Open;
}

} // namespace SoRQolMenu
