#include "SoRQolMenu.hpp"

#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace SoRQolMenu {
namespace {

using namespace SoRMenuCanvas;
using SoRQolConfig::QolConfig;

constexpr int kTitleRow    = 3;
constexpr int kLabelColumn = 4;
constexpr int kValueColumn = 25;
constexpr int kHelpRow     = 22;

enum class Row { RollCameraEarlier, Reset, Save, Cancel };
constexpr std::array kRows = {Row::RollCameraEarlier, Row::Reset, Row::Save, Row::Cancel};

int screenRow(Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
            return 7;
        case Row::Reset:
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
        case Row::Reset:
            return "RESET TO DEFAULTS";
        case Row::Save:
            return "SAVE AND APPLY";
        case Row::Cancel:
            return "CANCEL";
    }
    return "";
}

struct Menu {
    bool      isOpen  = false;
    int       cursor  = 0;
    bool      swallow = false; // ignore what is held when the menu opens
    QolConfig pending;

    Repeat up, down, left, right, confirm, back;
    Shown  shown;
};

Menu &menu() {
    static Menu instance;
    return instance;
}

// Left, Right or confirm on a row that holds a value. False on the others.
bool changeValue(QolConfig &config, Row row) {
    if (row != Row::RollCameraEarlier)
        return false;
    config.rollCameraEarlier = !config.rollCameraEarlier;
    return true;
}

std::array<std::string, 2> help(const QolConfig &config, Row row) {
    switch (row) {
        case Row::RollCameraEarlier:
            if (config.rollCameraEarlier)
                return {"THE CAMERA ROLLS WITH YOU FURTHER", "BACK, SO YOU SEE MORE OF THE STREET."};
            return {"THE CAMERA OF THE ORIGINAL GAME: IT", "ROLLS PAST THE MIDDLE OF THE SCREEN."};
        case Row::Reset:
            return {"PUTS BACK THE DEFAULTS ABOVE. THEY", "COUNT ONLY AFTER SAVE AND APPLY."};
        case Row::Save:
            return {"USES THESE SETTINGS NOW, AND KEEPS", "THEM FOR NEXT TIME."};
        case Row::Cancel:
            return {"LEAVES WITHOUT CHANGING ANYTHING.", ""};
    }
    return {};
}

void draw(const Menu &m, Canvas &canvas) {
    canvas.text(kLabelColumn, kTitleRow, "QUALITY-OF-LIFE", true);
    for (std::size_t i = 0; i < kRows.size(); ++i) {
        const Row  row      = kRows[i];
        const bool selected = static_cast<int>(i) == m.cursor;
        canvas.text(kLabelColumn, screenRow(row), label(row), selected);
        if (row == Row::RollCameraEarlier)
            canvas.text(kValueColumn, screenRow(row), m.pending.rollCameraEarlier ? "ON" : "OFF", selected);
    }
    const auto lines = help(m.pending, kRows[static_cast<std::size_t>(m.cursor)]);
    // White, as in the VIDEO menu: this screen's grey is too dark to read a sentence in.
    canvas.text(kLabelColumn, kHelpRow, lines[0], true);
    canvas.text(kLabelColumn, kHelpRow + 1, lines[1], true);
}

} // namespace

void drawOptionsRow(SystemMemory &memory, bool highlighted) {
    const int row = 16; // free in the ROM's layout: VIDEO is at 15, EXIT at 18
    drawText(memory, kLabelColumn, row, "QUALITY-OF-LIFE", highlighted);
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
    const bool up      = m.up.step(held.up, true);
    const bool down    = m.down.step(held.down, true);
    const bool left    = m.left.step(held.left, false);
    const bool right   = m.right.step(held.right, false);
    const bool confirm = m.confirm.step(held.confirm, false);
    const bool back    = m.back.step(held.back, false);

    const auto count = static_cast<int>(kRows.size());
    const Row  row   = kRows[static_cast<std::size_t>(m.cursor)];
    bool       close = false;
    if (m.swallow) {
        m.swallow = false; // a key still held from opening is not a press
    } else if (up || down) {
        m.cursor = (m.cursor + (up ? -1 : 1) + count) % count;
        playSound(memory, kSoundMove);
    } else if (back) {
        playSound(memory, kSoundPick);
        close = true;
    } else if (left || right) {
        if (changeValue(m.pending, row))
            playSound(memory, kSoundMove);
    } else if (confirm) {
        playSound(memory, kSoundPick);
        switch (row) {
            case Row::Reset:
                m.pending = QolConfig{};
                break;
            case Row::Save:
                if (!SoRQolConfig::save(m.pending, SoRQolConfig::defaultPath()))
                    std::fprintf(stderr, "Could not write %s\n", SoRQolConfig::defaultPath().string().c_str());
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

    Canvas canvas;
    if (close) {
        flush(memory, m.shown, canvas); // clear plane A for the OPTIONS screen
        m.isOpen = false;
        return Result::Closed;
    }
    draw(m, canvas);
    flush(memory, m.shown, canvas);
    return Result::Open;
}

} // namespace SoRQolMenu
