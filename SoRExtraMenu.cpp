#include "SoRExtraMenu.hpp"

#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"
#include "SoRSurvival.hpp"

#include <array>
#include <optional>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace SoRExtraMenu {
namespace {

using namespace SoRMenuCanvas;

constexpr int kTitleRow    = 3;
constexpr int kLabelColumn = 4;
constexpr int kValueColumn = 22;
constexpr int kHelpRow     = 22;
constexpr int kRounds      = 8;

constexpr m_long kP2PadMissing = 0xFFFFFC0Au;

enum class Page { Extra, Survival, Result };
enum class Row { Survival, Back, Players, Round, Start, Ok };

std::vector<Row> rows(Page page) {
    if (page == Page::Extra)
        return {Row::Survival, Row::Back};
    if (page == Page::Result)
        return {Row::Ok};
    return {Row::Players, Row::Round, Row::Start, Row::Back};
}

int screenRow(Page page, Row row) {
    switch (row) {
        case Row::Survival:
        case Row::Players:
            return 7;
        case Row::Round:
            return 9;
        case Row::Start:
            return 11;
        case Row::Back:
            return page == Page::Extra ? 9 : 13;
        case Row::Ok:
            return 18;
    }
    return 0;
}

struct Menu {
    bool requested = false;
    bool isOpen    = false;
    bool start     = false; // START was chosen: the game begins after the fade
    bool swallow   = false; // ignore what is held when the menu opens
    Page page      = Page::Extra;
    int  cursor    = 0;
    int  players   = 1;
    int  round     = 0;

    std::optional<SoRSurvival::Result> result; // shown once, when the game comes back here

    Repeat up, down, left, right, confirm, back;
    Shown  shown;
};

Menu &menu() {
    static Menu instance;
    return instance;
}

bool twoPlayersPossible(SystemMemory &memory) {
    return memory.readByte(kP2PadMissing) == 0;
}

std::string padded(unsigned value) {
    char text[16];
    std::snprintf(text, sizeof text, "%04u", value > 9999 ? 9999u : value);
    return text;
}

std::array<std::string_view, 2> help(const Menu &m, Row row) {
    switch (row) {
        case Row::Survival:
            return {"ENEMIES KEEP COMING UNTIL YOU HAVE", "NO LIVES LEFT. HOW MANY WILL FALL?"};
        case Row::Players:
            if (m.players == 2)
                return {"TWO PLAYERS, 3 LIVES EACH. THE LAST", "BLOW ON AN ENEMY WINS THE POINT."};
            return {"ONE PLAYER, 3 LIVES AND NO CONTINUE.", "EACH ENEMY BEATEN IS A POINT."};
        case Row::Round:
            return {"THE PLACE YOU FIGHT IN. THE GAME IS", "THE SAME IN EVERY ROUND."};
        case Row::Start:
            return {"CHOOSE YOUR CHARACTER AND FIGHT.", ""};
        case Row::Ok:
            return {"BACK TO SURVIVAL MODE.", ""};
        case Row::Back:
            if (m.page == Page::Extra)
                return {"BACK TO THE TITLE MENU.", ""};
            return {"BACK TO EXTRA.", ""};
    }
    return {};
}

void draw(const Menu &m, Canvas &canvas) {
    const std::vector<Row> list = rows(m.page);
    canvas.text(kLabelColumn, kTitleRow, m.page == Page::Extra ? "EXTRA" : "SURVIVAL MODE", true);
    if (m.page == Page::Result && m.result) {
        // In white like the records: nothing on it is a choice but OK.
        const SoRSurvival::Result &r = *m.result;
        canvas.text(kLabelColumn, 6, "GAME OVER", true);
        canvas.text(kLabelColumn, 8, "ENEMIES BEATEN", true);
        if (r.players == 2) {
            canvas.text(kLabelColumn + 2, 10, "PLAYER 1", true);
            canvas.text(kValueColumn, 10, padded(r.points[0]), true);
            canvas.text(kLabelColumn + 2, 11, "PLAYER 2", true);
            canvas.text(kValueColumn, 11, padded(r.points[1]), true);
        }
        canvas.text(kLabelColumn + 2, 12, "TOTAL", true);
        canvas.text(kValueColumn, 12, padded(r.total), true);
        canvas.text(kLabelColumn, 14, r.players == 2 ? "RECORD, 2 PLAYERS" : "RECORD, 1 PLAYER", true);
        canvas.text(kValueColumn, 14, padded(r.record), true);
        if (r.newRecord)
            canvas.text(kLabelColumn, 16, "NEW RECORD", true);
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Row  row      = list[i];
        const bool selected = static_cast<int>(i) == m.cursor;
        const int  y        = screenRow(m.page, row);
        switch (row) {
            case Row::Survival:
                canvas.text(kLabelColumn, y, "SURVIVAL MODE", selected);
                break;
            case Row::Back:
                canvas.text(kLabelColumn, y, "BACK", selected);
                break;
            case Row::Players:
                canvas.text(kLabelColumn, y, "PLAYERS", selected);
                canvas.text(kValueColumn, y, m.players == 2 ? "2 PLAYERS" : "1 PLAYER", selected);
                break;
            case Row::Round:
                canvas.text(kLabelColumn, y, "ROUND", selected);
                canvas.text(kValueColumn, y, "ROUND " + std::to_string(m.round + 1), selected);
                break;
            case Row::Start:
                canvas.text(kLabelColumn, y, "START", selected);
                break;
            case Row::Ok:
                canvas.text(kLabelColumn, y, "OK", selected);
                break;
        }
    }
    if (m.page == Page::Survival) {
        const SoRSurvival::Records &best = SoRSurvival::records();
        // In white, like the help lines: nothing to select, and the screen's grey is too dark to read.
        canvas.text(kLabelColumn, 16, "RECORD", true);
        canvas.text(kLabelColumn, 18, "1 PLAYER", true);
        canvas.text(kValueColumn, 18, padded(best.onePlayer), true);
        canvas.text(kLabelColumn, 20, "2 PLAYERS", true);
        canvas.text(kValueColumn, 20, padded(best.twoPlayers), true);
    }
    const auto lines = help(m, rows(m.page)[static_cast<std::size_t>(m.cursor)]);
    // Two lines in white, as in the other menus: this screen's grey is too dark for a sentence.
    canvas.text(kLabelColumn, kHelpRow + (m.page == Page::Survival ? 2 : 0), lines[0], true);
    canvas.text(kLabelColumn, kHelpRow + 1 + (m.page == Page::Survival ? 2 : 0), lines[1], true);
}

void goTo(Menu &m, Page page, Row row) {
    m.page = page;
    const std::vector<Row> list = rows(page);
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i] == row)
            m.cursor = static_cast<int>(i);
}

} // namespace

void request() {
    menu().requested = true;
}

void showResult(const SoRSurvival::Result &result) {
    Menu &m     = menu();
    m.requested = true;
    m.result    = result;
}

bool resultPending() {
    const Menu &m = menu();
    return m.requested && m.result.has_value();
}

bool openIfRequested(SystemMemory &memory) {
    Menu &m = menu();
    if (!m.requested)
        return false;
    const int players = SoRSurvival::lastPlayers();
    const int round   = SoRSurvival::lastRound();
    const auto result = m.result;
    m                 = Menu{};
    m.result          = result;
    if (m.result)
        m.page = Page::Result;
    m.isOpen          = true;
    m.swallow         = true;
    m.players         = players == 2 && twoPlayersPossible(memory) ? 2 : 1;
    m.round           = round;
    Canvas canvas;
    draw(m, canvas);
    flush(memory, m.shown, canvas);
    return true;
}

bool isOpen() {
    return menu().isOpen;
}

Result frame(SystemMemory &memory, SoRControlsConfig::ActionMask remote) {
    Menu &m = menu();
    if (!m.isOpen)
        return Result::Back;

    const SoRInput::MenuButtons held = SoRInput::menuButtons(remote, true);
    const bool up      = m.up.step(held.up, true);
    const bool down    = m.down.step(held.down, true);
    const bool left    = m.left.step(held.left, true);
    const bool right   = m.right.step(held.right, true);
    const bool confirm = m.confirm.step(held.confirm, false);
    const bool back    = m.back.step(held.back, false);

    Result result = Result::Open;
    if (m.swallow) {
        m.swallow = false; // a key still held from opening is not a press
    } else {
        const std::vector<Row> list  = rows(m.page);
        const auto             count = static_cast<int>(list.size());
        const Row              row   = list[static_cast<std::size_t>(m.cursor)];
        const auto             step  = [&](int value, int size) { return (value + (left ? -1 : 1) + size) % size; };
        if (up || down) {
            m.cursor = (m.cursor + (up ? -1 : 1) + count) % count;
            playSound(memory, kSoundMove);
        } else if (m.page == Page::Result && (back || confirm)) {
            playSound(memory, kSoundPick);
            m.result.reset();
            goTo(m, Page::Survival, Row::Start);
        } else if (back || (confirm && row == Row::Back)) {
            playSound(memory, kSoundPick);
            if (m.page == Page::Survival)
                goTo(m, Page::Extra, Row::Survival);
            else
                result = Result::Back;
        } else if (row == Row::Players && (left || right || confirm)) {
            if (twoPlayersPossible(memory)) {
                m.players = m.players == 2 ? 1 : 2;
                playSound(memory, kSoundMove);
            }
        } else if (row == Row::Round && (left || right || confirm)) {
            m.round = confirm ? (m.round + 1) % kRounds : step(m.round, kRounds);
            playSound(memory, kSoundMove);
        } else if (confirm && row == Row::Survival) {
            playSound(memory, kSoundPick);
            goTo(m, Page::Survival, Row::Start);
        } else if (confirm && row == Row::Start) {
            playSound(memory, kSoundPick);
            m.start = true;
            result  = Result::Start;
        }
    }

    Canvas canvas;
    if (result != Result::Open) {
        flush(memory, m.shown, canvas); // the screen fades out empty
        m.isOpen = false;
        return result;
    }
    draw(m, canvas);
    flush(memory, m.shown, canvas);
    return result;
}

bool takeStart(SystemMemory &memory) {
    Menu &m = menu();
    if (!m.start)
        return false;
    m.start     = false;
    m.requested = false;
    SoRSurvival::begin(memory, m.players, m.round);
    return true;
}

} // namespace SoRExtraMenu
