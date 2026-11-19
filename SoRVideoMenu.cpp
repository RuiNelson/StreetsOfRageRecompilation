#include "SoRVideoMenu.hpp"

#include "SoRInput.hpp"
#include "SoRMenuCanvas.hpp"
#include "SoRPlatform.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SoRVideoMenu {
namespace {

using namespace SoRMenuCanvas;
using SoRVideoConfig::VideoConfig;

constexpr int kTitleRow    = 3;
constexpr int kLabelColumn = 4;
constexpr int kValueColumn = 22;
constexpr int kHelpRow     = 23;

enum class Row { FrameSync, Picture, Display, IntegerScaling, ScalingFilter, FrameGeneration, Reset, Save, Cancel };

// The rows this screen shows: no DISPLAY on a console, FRAME GENERATION only where the window can generate frames.
std::vector<Row> rowList(bool frameGeneration) {
    std::vector<Row> rows{Row::FrameSync, Row::Picture};
    if (!SoRPlatform::consoleMenus())
        rows.push_back(Row::Display);
    rows.insert(rows.end(), {Row::IntegerScaling, Row::ScalingFilter});
    if (frameGeneration)
        rows.push_back(Row::FrameGeneration);
    rows.insert(rows.end(), {Row::Reset, Row::Save, Row::Cancel});
    return rows;
}

// Every other screen row from 7, or from 5 when nine rows would reach the help lines.
int screenRow(const std::vector<Row> &rows, std::size_t index) {
    const int first = rows.size() > 8 ? 5 : 7;
    return first + 2 * static_cast<int>(index);
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
        case Row::ScalingFilter:
            return "SCALING FILTER";
        case Row::FrameGeneration:
            return "FRAME GENERATION";
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

std::string_view scalingFilterName(VDP::ScalingFilter filter) {
    switch (filter) {
        case VDP::Nearest:
            return "NEAREST";
        case VDP::Bilinear:
            return "BILINEAR";
        case VDP::MetalFX:
            return "METALFX";
        case VDP::Hybrid:
            break;
    }
    return "HYBRID";
}

struct Menu {
    bool        isOpen = false;
    int         cursor = 0;
    bool        swallow = false; // ignore what is held when the menu opens
    VideoConfig pending;
    int         refreshHz = 0;
    int         normalHz  = 60;
    bool        metalFX   = false; // the window can draw VDP::MetalFX
    std::vector<Row> rows;         // see rowList()

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

// The filters in menu order; METALFX only where the window can draw it.
void changeScalingFilter(VideoConfig &config, int step, bool metalFX) {
    constexpr std::array kFilters = {VDP::Nearest, VDP::Bilinear, VDP::Hybrid, VDP::MetalFX};
    const int            count    = static_cast<int>(kFilters.size()) - (metalFX ? 0 : 1);
    const auto           index    = static_cast<int>(std::ranges::find(kFilters, config.scalingFilter) - kFilters.begin());
    config.scalingFilter          = kFilters[static_cast<std::size_t>((std::min(index, count - 1) + step + count) % count)];
}

// Left, Right or confirm on a row that holds a value. False on the others.
bool changeValue(VideoConfig &config, Row row, int step, bool metalFX) {
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
        case Row::ScalingFilter:
            changeScalingFilter(config, step, metalFX);
            return true;
        case Row::FrameGeneration: {
            constexpr std::array kModes = {VDP::NoFrameGeneration, VDP::GenerateFrames, VDP::GenerateHoldPoses};
            const auto index = static_cast<int>(std::ranges::find(kModes, config.frameGeneration) - kModes.begin());
            config.frameGeneration = kModes[static_cast<std::size_t>((index + step + 3) % 3)];
            return true;
        }
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
        case Row::ScalingFilter:
            return std::string(scalingFilterName(config.scalingFilter));
        case Row::FrameGeneration:
            return config.frameGeneration == VDP::GenerateFrames      ? "ON"
                   : config.frameGeneration == VDP::GenerateHoldPoses ? "ON, NO POSE BLEND"
                                                                       : "OFF";
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
                return {"WHOLE MULTIPLES ONLY: EVEN PIXELS,", "WITH BORDERS AROUND THE PICTURE."};
            return {"THE PICTURE FILLS AS MUCH AS IT CAN.", ""};
        case Row::ScalingFilter:
            switch (m.pending.scalingFilter) {
                case VDP::Nearest:
                    return {"SHARP PIXELS. OFF WHOLE MULTIPLES,", "SOME ARE WIDER THAN OTHERS."};
                case VDP::Bilinear:
                    return {"SMOOTH, EVEN PIXELS, A LITTLE", "BLURRED."};
                case VDP::MetalFX:
                    return {"APPLE'S METALFX UPSCALER: SMOOTHED", "AND SHARPENED EDGES."};
                case VDP::Hybrid:
                    break;
            }
            return {"SHARP, EVEN PIXELS: WHOLE MULTIPLE,", "THEN SMOOTHED TO FIT."};
        case Row::FrameGeneration:
            if (m.pending.frameGeneration == VDP::NoFrameGeneration)
                return {"ONLY THE FRAMES THE GAME DRAWS.", ""};
            if (m.pending.frameSync != VDP::InternalTimer)
                return {"ONLY WITH VSYNC AUTO: NO EFFECT", "WITH THIS VSYNC."};
            if (m.pending.frameGeneration == VDP::GenerateHoldPoses)
                return {"A FRAME BETWEEN EACH TWO. A NEW", "POSE WAITS FOR THE REAL FRAME."};
            return {"A FRAME BETWEEN EACH TWO. A NEW", "POSE IS BLENDED WITH THE OLD ONE."};
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
    const std::vector<Row> &rows = m.rows;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row  row      = rows[i];
        const bool selected = static_cast<int>(i) == m.cursor;
        canvas.text(kLabelColumn, screenRow(rows, i), label(row), selected);
        canvas.text(kValueColumn, screenRow(rows, i), value(m.pending, row), selected);
    }
    const auto lines = help(m, rows[static_cast<std::size_t>(m.cursor)]);
    // White: this screen's grey is palette line 1's darkest text, too dark to read a sentence in.
    canvas.text(kLabelColumn, kHelpRow, lines[0], true);
    canvas.text(kLabelColumn, kHelpRow + 1, lines[1], true);
}

} // namespace

void drawOptionsRow(SystemMemory &memory, const VideoConfig &current, bool highlighted) {
    const int row = 15; // free in the ROM's layout: CONTROLS ends at 13, EXIT is at 18
    drawText(memory, kLabelColumn, row, "VIDEO", highlighted);
    std::string summary = current.widescreen ? "WIDESCREEN" : "ORIGINAL";
    if (!SoRPlatform::consoleMenus())
        summary += " " + std::string(displayName(current.fullscreen));
    drawText(memory, 18, row, summary, highlighted);
    // Back from a longer summary, as from WIDESCREEN FULLSCREEN to ORIGINAL WINDOW.
    drawText(memory, 18 + static_cast<int>(summary.size()), row, std::string(40 - 18 - summary.size(), ' '), highlighted);
}

void open(const VideoConfig &current, int refreshHz, int normalHz, bool metalFX, bool frameGeneration) {
    Menu &m     = menu();
    m           = Menu{};
    m.isOpen    = true;
    m.swallow   = true;
    m.pending   = current;
    m.refreshHz = refreshHz;
    m.normalHz  = normalHz;
    m.metalFX   = metalFX;
    m.rows      = rowList(frameGeneration);
    // What the window draws: a METALFX it cannot draw shows as HYBRID.
    if (!metalFX && m.pending.scalingFilter == VDP::MetalFX)
        m.pending.scalingFilter = VDP::Hybrid;
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

    const std::vector<Row>    &rows  = m.rows;
    const auto                 count = static_cast<int>(rows.size());
    const Row                  row   = rows[static_cast<std::size_t>(m.cursor)];
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
        if (changeValue(m.pending, row, right ? 1 : -1, m.metalFX))
            playSound(memory, kSoundMove);
    } else if (confirm) {
        playSound(memory, kSoundPick);
        switch (row) {
            case Row::Reset:
                m.pending            = VideoConfig{};
                m.pending.fullscreen = SoRPlatform::consoleMenus(); // a console has no window to go back to
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
                changeValue(m.pending, row, 1, m.metalFX);
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
