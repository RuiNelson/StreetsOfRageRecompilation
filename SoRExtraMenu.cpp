#include "SoRExtraMenu.hpp"

#include "SoRInitialsKeyboard.hpp"
#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"
#include "SoRGta6.hpp"
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
constexpr int kElevator    = 6; // round 7 is not offered (user, 2026-11-19: it gave trouble)

constexpr m_long kP2PadMissing = 0xFFFFFC0Au;

enum class Page { Extra, Survival, Initials, Result };
enum class Row { Survival, Gta6, Back, Players, Round, Start, Ok };

std::vector<Row> rows(Page page) {
    if (page == Page::Extra)
        return {Row::Survival, Row::Gta6, Row::Back};
    if (page == Page::Result)
        return {Row::Ok};
    if (page == Page::Initials)
        return {Row::Start}; // the keyboard has its own cursor
    return {Row::Players, Row::Round, Row::Start, Row::Back};
}

int screenRow(Page page, Row row) {
    switch (row) {
        case Row::Survival:
        case Row::Players:
            return 7;
        case Row::Gta6:
        case Row::Round:
            return 9;
        case Row::Start:
            return 11;
        case Row::Back:
            return page == Page::Extra ? 11 : 13;
        case Row::Ok:
            return 20;
    }
    return 0;
}

struct Menu {
    bool requested = false;
    bool isOpen    = false;
    bool start     = false; // START was chosen: the game begins after the fade
    bool gta6      = false; // GTA 6 was chosen: that game begins after the fade
    bool swallow   = false; // ignore what is held when the menu opens
    Page page      = Page::Extra;
    int  cursor    = 0;
    int  players   = 1;
    int  round     = 0;

    std::optional<SoRSurvival::Result> result; // shown once, when the game comes back here
    SoRInitialsKeyboard::Keyboard      keyboard; // START asks each player's initials first
    int                                typingPlayer = 0;

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

// Initials as the tables show them: three cells, "---" for none.
std::string shownInitials(const std::string &initials) {
    if (initials.empty())
        return "---";
    std::string text = initials;
    text.resize(3, ' ');
    return text;
}

void drawTable(Canvas &canvas, int row, const char *title, const std::vector<SoRSurvival::Entry> &table, int players,
               int marked) {
    canvas.text(kLabelColumn, row, title, true);
    for (std::size_t i = 0; i < SoRSurvival::kTopEntries; ++i) {
        const int y = row + 1 + static_cast<int>(i);
        std::string line = (static_cast<int>(i) == marked ? ">" : " ") + std::to_string(i + 1) + ". ";
        if (i < table.size()) {
            line += shownInitials(table[i].initials[0]);
            if (players == 2)
                line += " " + shownInitials(table[i].initials[1]);
            canvas.text(kValueColumn, y, padded(table[i].points), true);
        } else {
            line += players == 2 ? "--- ---" : "---";
        }
        canvas.text(kLabelColumn, y, line, true);
    }
}

std::array<std::string_view, 2> help(const Menu &m, Row row) {
    switch (row) {
        case Row::Survival:
            return {"ENEMIES KEEP COMING UNTIL YOU HAVE", "NO LIVES LEFT. HOW MANY WILL FALL?"};
        case Row::Gta6:
            return {"ONE PLAYER AS THE POLICE CAR: IT", "RUNS THEM OVER. NO HIGH SCORES."};
        case Row::Players:
            if (m.players == 2)
                return {"TWO PLAYERS, 3 LIVES EACH. THE LAST", "BLOW ON AN ENEMY WINS THE POINT."};
            return {"ONE PLAYER, 3 LIVES AND NO CONTINUE.", "EACH ENEMY BEATEN IS A POINT."};
        case Row::Round:
            return {"THE PLACE YOU FIGHT IN. THE GAME IS", "THE SAME IN EVERY ROUND."};
        case Row::Start:
            return {"ENTER YOUR INITIALS, CHOOSE YOUR", "CHARACTER AND FIGHT."};
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
    if (m.page == Page::Initials) {
        SoRInitialsKeyboard::draw(m.keyboard, canvas);
        return;
    }
    if (m.page == Page::Result && m.result) {
        // In white like the tables: nothing on it is a choice but OK.
        const SoRSurvival::Result &r = *m.result;
        canvas.text(kLabelColumn, 5, "GAME OVER", true);
        canvas.text(kLabelColumn, 7, "ENEMIES BEATEN", true);
        for (int player = 0; player < r.players; ++player) {
            canvas.text(kLabelColumn + 2, 8 + player, shownInitials(r.initials[player]), true);
            canvas.text(kValueColumn, 8 + player, padded(r.points[player]), true);
        }
        if (r.players == 2) {
            canvas.text(kLabelColumn + 2, 10, "TOTAL", true);
            canvas.text(kValueColumn, 10, padded(r.total), true);
        }
        const SoRSurvival::Records &best = SoRSurvival::records();
        drawTable(canvas, 12, r.players == 2 ? "RECORDS, 2 PLAYERS" : "RECORDS, 1 PLAYER",
                  r.players == 2 ? best.twoPlayers : best.onePlayer, r.players, r.place);
        static constexpr const char *kPlaces[] = {"NEW RECORD", "SECOND PLACE", "THIRD PLACE"};
        if (r.place >= 0 && r.place < 3)
            canvas.text(kLabelColumn, 17, kPlaces[r.place], true);
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Row  row      = list[i];
        const bool selected = static_cast<int>(i) == m.cursor;
        const int  y        = screenRow(m.page, row);
        switch (row) {
            case Row::Survival:
                canvas.text(kLabelColumn, y, "SURVIVAL MODE", selected);
                break;
            case Row::Gta6:
                canvas.text(kLabelColumn, y, "GTA 6", selected);
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
        // In white, like the help lines: nothing to select, and the screen's grey is too dark to read.
        const SoRSurvival::Records &best = SoRSurvival::records();
        drawTable(canvas, 15, "RECORDS, 1 PLAYER", best.onePlayer, 1, -1);
        drawTable(canvas, 19, "RECORDS, 2 PLAYERS", best.twoPlayers, 2, -1);
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
    m.round           = round == kElevator ? 0 : round;
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
    const auto typeInitials = [&](int player) {
        m.page         = Page::Initials;
        m.cursor       = 0;
        m.typingPlayer = player;
        m.keyboard     = SoRInitialsKeyboard::open(std::string("PLAYER ") + (player == 0 ? "1" : "2") + " INITIALS",
                                                   SoRSurvival::initials(player));
    };
    if (m.swallow) {
        m.swallow = false; // a key still held from opening is not a press
    } else if (m.page == Page::Initials) {
        // Before the game, each player's initials for the records, the ones kept last already typed (user).
        switch (SoRInitialsKeyboard::step(m.keyboard, memory, up, down, left, right, confirm, back)) {
            case SoRInitialsKeyboard::Result::Open:
                break;
            case SoRInitialsKeyboard::Result::Cancel:
                goTo(m, Page::Survival, Row::Start);
                break;
            case SoRInitialsKeyboard::Result::Ok:
                SoRSurvival::setInitials(m.typingPlayer, m.keyboard.text);
                if (m.typingPlayer == 0 && m.players == 2) {
                    typeInitials(1);
                } else {
                    m.start = true;
                    result  = Result::Start;
                }
                break;
        }
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
            if (m.round == kElevator)
                m.round = confirm ? (m.round + 1) % kRounds : step(m.round, kRounds);
            playSound(memory, kSoundMove);
        } else if (confirm && row == Row::Survival) {
            playSound(memory, kSoundPick);
            goTo(m, Page::Survival, Row::Start);
        } else if (confirm && row == Row::Gta6) {
            playSound(memory, kSoundPick);
            m.gta6 = true;
            result = Result::Start;
        } else if (confirm && row == Row::Start) {
            playSound(memory, kSoundPick);
            typeInitials(0);
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
    if (m.gta6) {
        m.gta6      = false;
        m.requested = false;
        SoRGta6::begin(memory);
        return true;
    }
    if (!m.start)
        return false;
    m.start     = false;
    m.requested = false;
    SoRSurvival::begin(memory, m.players, m.round);
    return true;
}

} // namespace SoRExtraMenu
