#include "SoRVideoMenu.hpp"

#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace SoRVideoMenu {
namespace {

using namespace SoRMenuCanvas;
using SoRVideoConfig::VideoConfig;

constexpr int kTitleRow    = 3;
constexpr int kLabelColumn = 4;
constexpr int kValueColumn = 22;
constexpr int kHelpRow     = 22;

enum class Row { FrameSync, Picture, Display, IntegerScaling, Reset, Save, Cancel };
constexpr std::array kRowList = {
    Row::FrameSync, Row::Picture, Row::Display, Row::IntegerScaling, Row::Reset, Row::Save, Row::Cancel};

constexpr int screenRow(Row row) {
    switch (row) {
        case Row::FrameSync:
            return 7;
        case Row::Picture:
            return 9;
        case Row::Display:
            return 11;
        case Row::IntegerScaling:
            return 13;
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
        case Row::FrameSync:
            return "VSYNC";
        case Row::Picture:
            return "PICTURE";
        case Row::Display:
            return "DISPLAY";
        case Row::IntegerScaling:
            return "INTEGER SCALING";
        case Row::Reset:
            return "RESET TO DEFAULTS";
        case Row::Save:
            return "SAVE AND APPLY";
        case Row::Cancel:
            return "CANCEL";
    }
    return "";
}

std::string_view frameSyncName(VDP::Synchronization frameSync) {
    switch (frameSync) {
        case VDP::VSync:
            return "VSYNC";
        case VDP::VSync2:
            return "VSYNC HALF";
        case VDP::VSync3:
            return "VSYNC THIRD";
        case VDP::InternalTimer:
            break;
    }
    return "AUTO";
}

std::string_view pictureName(bool widescreen) {
    return widescreen ? "WIDESCREEN 16:9" : "ORIGINAL 4:3";
}

std::string_view displayName(bool fullscreen) {
    return fullscreen ? "FULLSCREEN" : "WINDOW";
}

struct Menu {
    bool        isOpen = false;
    int         cursor = 0;
    bool        swallow = false; // ignore what is held when the menu opens
    VideoConfig pending;
    int         refreshHz = 0;
    int         normalHz  = 60;

    std::optional<VideoConfig> chosen;

    Repeat up, down, left, right, confirm, back;
    Shown  shown;
};

Menu &menu() {
    static Menu instance;
    return instance;
}

void changeFrameSync(VideoConfig &config, int step) {
    constexpr std::array kModes = {VDP::InternalTimer, VDP::VSync, VDP::VSync2, VDP::VSync3};
    const auto           count  = static_cast<int>(kModes.size());
    const auto           index  = static_cast<int>(std::ranges::find(kModes, config.frameSync) - kModes.begin());
    config.frameSync            = kModes[static_cast<std::size_t>((index + step + count) % count)];
}

// Left, Right or confirm on a row that holds a value. False on the others.
bool changeValue(VideoConfig &config, Row row, int step) {
    switch (row) {
        case Row::FrameSync:
            changeFrameSync(config, step);
            return true;
        case Row::Picture:
            config.widescreen = !config.widescreen;
            return true;
        case Row::Display:
            config.fullscreen = !config.fullscreen;
            return true;
        case Row::IntegerScaling:
            config.integerScaling = !config.integerScaling;
            return true;
        case Row::Reset:
        case Row::Save:
        case Row::Cancel:
            break;
    }
    return false;
}

std::string value(const VideoConfig &config, Row row) {
    switch (row) {
        case Row::FrameSync:
            return std::string(frameSyncName(config.frameSync));
        case Row::Picture:
            return std::string(pictureName(config.widescreen));
        case Row::Display:
            return std::string(displayName(config.fullscreen));
        case Row::IntegerScaling:
            return config.integerScaling ? "ON" : "OFF";
        case Row::Reset:
        case Row::Save:
        case Row::Cancel:
            break;
    }
    return "";
}

// The game's speed on this screen, in the words of someone choosing a mode.
std::array<std::string, 2> frameSyncHelp(const Menu &m) {
    if (m.pending.frameSync == VDP::InternalTimer)
        return {"THE GAME KEEPS ITS OWN SPEED ON ANY", "SCREEN. NO TEARING. RECOMMENDED."};
    if (m.refreshHz <= 0)
        return {"THE GAME'S SPEED FOLLOWS THE SCREEN.", ""};

    const int divisor = m.pending.frameSync == VDP::VSync2 ? 2 : m.pending.frameSync == VDP::VSync3 ? 3 : 1;
    const int fps     = m.refreshHz / divisor;
    std::array<std::string, 2> help{"YOUR SCREEN: " + std::to_string(m.refreshHz) + " HZ", ""};
    if (std::abs(fps - m.normalHz) <= 1)
        help[1] = "THE GAME RUNS AT NORMAL SPEED";
    else
        help[1] = std::string(fps > m.normalHz ? "TOO FAST" : "TOO SLOW") + ": THE GAME RUNS AT " + std::to_string(fps) + " FPS";
    return help;
}

std::array<std::string, 2> help(const Menu &m, Row row) {
    switch (row) {
        case Row::FrameSync:
            return frameSyncHelp(m);
        case Row::Picture:
            if (m.pending.widescreen)
                return {"A WIDER VIEW OF THE GAME, WITH MORE", "OF THE STREET ON THE SIDES."};
            return {"THE PICTURE OF THE ORIGINAL GAME,", "AS ON THE CONSOLE."};
        case Row::Display:
            if (m.pending.fullscreen)
                return {"THE GAME FILLS THE WHOLE SCREEN.", "CTRL F SWITCHES WHILE PLAYING."};
            return {"THE GAME RUNS IN A WINDOW.", "CTRL F SWITCHES WHILE PLAYING."};
        case Row::IntegerScaling:
            if (m.pending.integerScaling)
                return {"SHARP, EVEN PIXELS, WITH BORDERS", "AROUND THE PICTURE."};
            return {"THE PICTURE FILLS AS MUCH AS IT CAN,", "SOME PIXELS WIDER THAN OTHERS."};
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
    canvas.text(kLabelColumn, kTitleRow, "VIDEO", true);
    for (std::size_t i = 0; i < kRowList.size(); ++i) {
        const Row  row      = kRowList[i];
        const bool selected = static_cast<int>(i) == m.cursor;
        canvas.text(kLabelColumn, screenRow(row), label(row), selected);
        canvas.text(kValueColumn, screenRow(row), value(m.pending, row), selected);
    }
    const auto lines = help(m, kRowList[static_cast<std::size_t>(m.cursor)]);
    // White: this screen's grey is palette line 1's darkest text, too dark to read a sentence in.
    canvas.text(kLabelColumn, kHelpRow, lines[0], true);
    canvas.text(kLabelColumn, kHelpRow + 1, lines[1], true);
}

} // namespace

void drawOptionsRow(SystemMemory &memory, const VideoConfig &current, bool highlighted) {
    const int row = 15; // free in the ROM's layout: CONTROLS ends at 13, EXIT is at 18
    drawText(memory, kLabelColumn, row, "VIDEO", highlighted);
    const std::string summary = std::string(current.widescreen ? "WIDESCREEN" : "ORIGINAL") + " " +
                                std::string(displayName(current.fullscreen));
    drawText(memory, 18, row, summary, highlighted);
    // Back from a longer summary, as from WIDESCREEN FULLSCREEN to ORIGINAL WINDOW.
    drawText(memory, 18 + static_cast<int>(summary.size()), row, std::string(40 - 18 - summary.size(), ' '), highlighted);
}

void open(const VideoConfig &current, int refreshHz, int normalHz) {
    Menu &m     = menu();
    m           = Menu{};
    m.isOpen    = true;
    m.swallow   = true;
    m.pending   = current;
    m.refreshHz = refreshHz;
    m.normalHz  = normalHz;
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

    const auto count = static_cast<int>(kRowList.size());
    const Row  row   = kRowList[static_cast<std::size_t>(m.cursor)];
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
        if (changeValue(m.pending, row, right ? 1 : -1))
            playSound(memory, kSoundMove);
    } else if (confirm) {
        playSound(memory, kSoundPick);
        switch (row) {
            case Row::Reset:
                m.pending = VideoConfig{};
                break;
            case Row::Save:
                if (!SoRVideoConfig::save(m.pending, SoRVideoConfig::defaultPath()))
                    std::fprintf(stderr, "Could not write %s\n", SoRVideoConfig::defaultPath().string().c_str());
                m.chosen = m.pending;
                close    = true;
                break;
            case Row::Cancel:
                close = true;
                break;
            default:
                changeValue(m.pending, row, 1);
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

std::optional<VideoConfig> takeChosen() {
    Menu &m = menu();
    return std::exchange(m.chosen, std::nullopt);
}

} // namespace SoRVideoMenu
