#include "SoRControlsMenu.hpp"

#include "SoRInput.hpp"
#include "system/memory/SystemMemory.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace SoRControlsMenu {
namespace {

using SoRControlsConfig::Action;
using SoRControlsConfig::ControlsConfig;
using SoRControlsConfig::GamepadChoice;
using SoRControlsConfig::PadInput;
using SoRControlsConfig::Scheme;
using SoRControlsConfig::SchemeBindings;

constexpr m_long kVdpData    = 0x00C00000u;
constexpr m_long kVdpControl = 0x00C00004u;
constexpr m_long kPlaneA     = 0xC000u; // the OPTIONS screen's plane A (its $45A40003-style commands)
constexpr m_word kGreyText   = 0x2000u; // palette line 1; line 0 is the white of the selected entry
constexpr m_long kPlaySe     = 0xFFFFF00Au;
constexpr m_byte kSoundMove  = 0xB9u; // options_row_nav's cursor sound
constexpr m_byte kSoundPick  = 0xBAu; // the menus' confirm sound

constexpr int kColumns     = 64;
constexpr int kRows        = 28;
constexpr int kTitleRow    = 3;
constexpr int kFirstRow    = 7;
constexpr int kBackRow     = 19;
constexpr int kLabelColumn = 4;

constexpr int kRepeatDelay = 20;
constexpr int kRepeatRate  = 6;

// The menu font loaded for the title and OPTIONS screens: a text byte is the
// tile index (the ROM's own strings use these codes, e.g. "A:SPECIAL ATTACK"
// at $1C9FC).
std::optional<m_byte> glyph(char c) {
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return static_cast<m_byte>(c - 'A' + 0x01);
    if (c >= '0' && c <= '9')
        return static_cast<m_byte>(c - '0' + 0x1B);
    switch (c) {
        case ' ':
            return 0x00;
        case ':':
            return 0x26;
        case '>':
            return 0x27;
        case '.':
            return 0x29;
        case ',':
            return 0x2A;
        case '\'':
            return 0x2C;
        case '"':
            return 0x2D;
        case '?':
            return 0x2E;
        case '!':
            return 0x2F;
        default:
            return std::nullopt;
    }
}

m_long vramWriteCommand(m_long address) {
    return 0x40000000u | ((address & 0x3FFFu) << 16) | ((address >> 14) & 0x3u);
}

m_long cellAddress(int column, int row) {
    return kPlaneA + static_cast<m_long>(row * kColumns + column) * 2u;
}

struct Canvas {
    std::array<m_word, kColumns * kRows> cells{};

    void text(int column, int row, std::string_view value, bool highlighted) {
        if (row < 0 || row >= kRows)
            return;
        for (const char c : value) {
            if (column >= 0 && column < kColumns)
                cells[static_cast<std::size_t>(row * kColumns + column)] =
                    static_cast<m_word>(glyph(c).value_or(0x00) | (highlighted ? 0u : kGreyText));
            ++column;
        }
    }
};

std::string upper(std::string value) {
    for (char &c : value)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return value;
}

std::string fit(std::string value, std::size_t width) {
    value = upper(std::move(value));
    for (char &c : value)
        if (!glyph(c))
            c = ' ';
    if (value.size() > width)
        value.resize(width);
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    return value;
}

std::string_view schemeName(Scheme scheme) {
    return scheme == Scheme::Enhanced6 ? "ENHANCED 6" : "ORIGINAL 3";
}

std::string_view actionLabel(Action action) {
    switch (action) {
        case Action::Up:
            return "UP";
        case Action::Down:
            return "DOWN";
        case Action::Left:
            return "LEFT";
        case Action::Right:
            return "RIGHT";
        case Action::Start:
            return "START";
        case Action::Special:
            return "SPECIAL";
        case Action::Punch:
            return "PUNCH";
        case Action::Jump:
            return "JUMP";
        case Action::BackAttack:
            return "BACK ATTACK";
        case Action::Pickup:
            return "PICK ITEM";
        case Action::Throw:
            return "THROW KNIFE";
    }
    return "";
}

std::string keyName(SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN)
        return "NONE";
    const char *name = SDL_GetScancodeName(scancode);
    std::string text = upper(name ? name : "");
    const bool  drawable =
        !text.empty() && std::ranges::all_of(text, [](char c) { return glyph(c).has_value(); });
    return drawable ? text : "KEY " + std::to_string(static_cast<int>(scancode));
}

std::string padName(const PadInput &input) {
    switch (input.kind) {
        case PadInput::Kind::None:
            return "NONE";
        case PadInput::Kind::LeftTrigger:
            return "LT";
        case PadInput::Kind::RightTrigger:
            return "RT";
        case PadInput::Kind::Button:
            break;
    }
    switch (input.button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return "SOUTH";
        case SDL_GAMEPAD_BUTTON_EAST:
            return "EAST";
        case SDL_GAMEPAD_BUTTON_WEST:
            return "WEST";
        case SDL_GAMEPAD_BUTTON_NORTH:
            return "NORTH";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return "LB";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return "RB";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            return "L3";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            return "R3";
        default:
            return upper(SoRControlsConfig::padInputName(input));
    }
}

enum class Screen { Top, Player, Device, Bind, Deadzone };

enum class ItemId {
    Player1,
    Player2,
    Scheme,
    Device,
    BindGamepad,
    BindKeyboard,
    Deadzone,
    DeviceAuto,
    DevicePad,
    DeviceNone,
    Back,
};

struct Item {
    ItemId      id;
    std::string label;
    std::string value;
    bool        enabled = true;
    int         row     = 0;
    int         pad     = -1; // DevicePad: index into the gamepad list
};

struct Repeat {
    int held = 0;

    // A press, then repeats while held.
    bool step(bool down, bool repeats) {
        if (!down) {
            held = 0;
            return false;
        }
        ++held;
        return held == 1 || (repeats && held >= kRepeatDelay && (held - kRepeatDelay) % kRepeatRate == 0);
    }
};

struct Menu {
    bool   isOpen = false;
    Screen screen = Screen::Top;
    int    player = 0;
    int    cursor = 0;
    int    topCursor    = 0;
    int    playerCursor = 0;
    bool   swallow      = false; // ignore what is held when the menu opens

    Repeat up, down, left, right, confirm, back;

    // BIND
    SoRInput::CaptureDevice bindDevice = SoRInput::CaptureDevice::Keyboard;
    Scheme                  bindScheme = Scheme::Original3;
    std::vector<Action>     bindActions;
    std::size_t             bindStep = 0;
    SchemeBindings          pending;
    int                     refusedFrames = 0;

    Canvas shown;
    bool   shownValid = false;
};

Menu &menu() {
    static Menu instance;
    return instance;
}

void playSound(SystemMemory &memory, m_byte id) {
    memory.writeByte(kPlaySe, id);
}

std::string deviceChoiceText(const SoRControlsConfig::PlayerConfig &config) {
    switch (config.gamepad) {
        case GamepadChoice::None:
            return "NONE";
        case GamepadChoice::Device:
            return config.gamepadName.empty() ? "GAMEPAD" : config.gamepadName;
        case GamepadChoice::Auto:
            break;
    }
    return "AUTO";
}

std::vector<Item> items(const Menu &m) {
    const ControlsConfig config = SoRInput::config();
    std::vector<Item>    list;
    switch (m.screen) {
        case Screen::Top:
            list.push_back({ItemId::Player1, "PLAYER 1", std::string(schemeName(config.players[0].scheme))});
            list.push_back({ItemId::Player2, "PLAYER 2", std::string(schemeName(config.players[1].scheme))});
            break;
        case Screen::Player: {
            const auto &player = config.players[static_cast<std::size_t>(m.player)];
            const auto  pad    = SoRInput::assignedGamepad(m.player);
            const bool  hasKbd = SoRInput::keyboardAvailable();
            list.push_back({ItemId::Scheme, "SCHEME", std::string(schemeName(player.scheme))});
            list.push_back({ItemId::Device, "DEVICE", deviceChoiceText(player)});
            list.push_back({ItemId::BindGamepad, "BIND GAMEPAD", pad ? pad->name : "NO GAMEPAD", pad.has_value()});
            list.push_back({ItemId::BindKeyboard, "BIND KEYBOARD", hasKbd ? "" : "NO KEYBOARD", hasKbd});
            list.push_back({ItemId::Deadzone,
                            "JOYSTICK DEADZONE",
                            pad ? std::to_string(player.deadzonePercent) : "NO GAMEPAD",
                            pad.has_value()});
            break;
        }
        case Screen::Device: {
            const std::vector<SoRInput::GamepadInfo> pads = SoRInput::gamepads();
            list.push_back({ItemId::DeviceAuto, "AUTO", ""});
            for (std::size_t i = 0; i < pads.size() && i < 7; ++i)
                list.push_back({ItemId::DevicePad, pads[i].name, "", true, 0, static_cast<int>(i)});
            list.push_back({ItemId::DeviceNone, "NONE", ""});
            break;
        }
        case Screen::Bind:
        case Screen::Deadzone:
            return list;
    }
    for (std::size_t i = 0; i < list.size(); ++i)
        list[i].row = kFirstRow + static_cast<int>(i) * 2;
    if (m.screen != Screen::Device)
        list.push_back({ItemId::Back, "BACK", "", true, kBackRow});
    return list;
}

void moveCursor(Menu &m, const std::vector<Item> &list, int step) {
    if (list.empty())
        return;
    const int count = static_cast<int>(list.size());
    int       next  = m.cursor;
    for (int i = 0; i < count; ++i) {
        next = (next + step + count) % count;
        if (list[static_cast<std::size_t>(next)].enabled) {
            m.cursor = next;
            return;
        }
    }
}

void clampCursor(Menu &m, const std::vector<Item> &list) {
    if (list.empty()) {
        m.cursor = 0;
        return;
    }
    m.cursor = std::clamp(m.cursor, 0, static_cast<int>(list.size()) - 1);
    if (!list[static_cast<std::size_t>(m.cursor)].enabled)
        moveCursor(m, list, 1);
}

void goTo(Menu &m, Screen screen, int cursor) {
    m.screen = screen;
    m.cursor = cursor;
}

void startBind(Menu &m, SoRInput::CaptureDevice device) {
    if (!SoRInput::beginCapture(device, m.player))
        return;
    const ControlsConfig config = SoRInput::config();
    m.bindDevice                = device;
    m.bindScheme                = config.players[static_cast<std::size_t>(m.player)].scheme;
    m.pending                   = config.players[static_cast<std::size_t>(m.player)].schemeBindings(m.bindScheme);
    const auto actions          = device == SoRInput::CaptureDevice::Keyboard
                                      ? SoRControlsConfig::keyboardActions(m.bindScheme)
                                      : SoRControlsConfig::gamepadActions(m.bindScheme);
    m.bindActions.assign(actions.begin(), actions.end());
    m.bindStep      = 0;
    m.refusedFrames = 0;
    goTo(m, Screen::Bind, 0);
}

void finishBind(Menu &m, bool keep) {
    SoRInput::endCapture();
    if (keep) {
        ControlsConfig config = SoRInput::config();
        config.players[static_cast<std::size_t>(m.player)].schemeBindings(m.bindScheme) = m.pending;
        SoRInput::setConfig(config);
    }
    goTo(m, Screen::Player, m.playerCursor);
}

bool alreadyBound(const Menu &m, const SoRInput::Captured &captured) {
    for (std::size_t i = 0; i < m.bindStep; ++i) {
        const auto index = static_cast<std::size_t>(m.bindActions[i]);
        if (captured.status == SoRInput::Captured::Status::Key && m.pending.keys[index] == captured.key)
            return true;
        if (captured.status == SoRInput::Captured::Status::Pad && m.pending.pad[index] == captured.pad)
            return true;
    }
    return false;
}

void updateBind(Menu &m, SystemMemory &memory) {
    if (m.refusedFrames > 0)
        --m.refusedFrames;
    if (m.bindDevice == SoRInput::CaptureDevice::Gamepad && !SoRInput::assignedGamepad(m.player)) {
        finishBind(m, false); // unplugged
        return;
    }
    const SoRInput::Captured captured = SoRInput::takeCapture();
    switch (captured.status) {
        case SoRInput::Captured::Status::Pending:
            return;
        case SoRInput::Captured::Status::Cancelled:
            playSound(memory, kSoundMove);
            finishBind(m, false);
            return;
        case SoRInput::Captured::Status::Key:
        case SoRInput::Captured::Status::Pad:
            break;
    }
    if (alreadyBound(m, captured)) {
        m.refusedFrames = 60;
        return;
    }
    const auto index = static_cast<std::size_t>(m.bindActions[m.bindStep]);
    if (captured.status == SoRInput::Captured::Status::Key)
        m.pending.keys[index] = captured.key;
    else
        m.pending.pad[index] = captured.pad;
    m.refusedFrames = 0;
    playSound(memory, kSoundMove);
    if (++m.bindStep >= m.bindActions.size()) {
        playSound(memory, kSoundPick);
        finishBind(m, true);
    }
}

void changeDeadzone(Menu &m, int step) {
    ControlsConfig config  = SoRInput::config();
    int           &percent = config.players[static_cast<std::size_t>(m.player)].deadzonePercent;
    percent                = std::clamp(percent + step, 0, SoRControlsConfig::kMaximumDeadzonePercent);
    SoRInput::setConfig(config, false); // applied at once, saved on leaving
}

void select(Menu &m, SystemMemory &memory, const Item &item) {
    ControlsConfig config = SoRInput::config();
    auto          &player = config.players[static_cast<std::size_t>(m.player)];
    switch (item.id) {
        case ItemId::Player1:
        case ItemId::Player2:
            m.topCursor = m.cursor;
            m.player    = item.id == ItemId::Player1 ? 0 : 1;
            goTo(m, Screen::Player, 0);
            m.playerCursor = 0;
            break;
        case ItemId::Scheme:
            player.scheme = player.scheme == Scheme::Original3 ? Scheme::Enhanced6 : Scheme::Original3;
            SoRInput::setConfig(config);
            break;
        case ItemId::Device: {
            m.playerCursor = m.cursor;
            int cursor     = 0;
            if (player.gamepad == GamepadChoice::Device) {
                const std::vector<SoRInput::GamepadInfo> pads = SoRInput::gamepads();
                for (std::size_t i = 0; i < pads.size() && i < 7; ++i)
                    if (pads[i].guid == player.gamepadGuid && pads[i].index == player.gamepadIndex)
                        cursor = static_cast<int>(i) + 1;
            } else if (player.gamepad == GamepadChoice::None) {
                cursor = static_cast<int>(std::min<std::size_t>(SoRInput::gamepads().size(), 7)) + 1;
            }
            goTo(m, Screen::Device, cursor);
            break;
        }
        case ItemId::BindGamepad:
            m.playerCursor = m.cursor;
            startBind(m, SoRInput::CaptureDevice::Gamepad);
            break;
        case ItemId::BindKeyboard:
            m.playerCursor = m.cursor;
            startBind(m, SoRInput::CaptureDevice::Keyboard);
            break;
        case ItemId::Deadzone:
            m.playerCursor = m.cursor;
            goTo(m, Screen::Deadzone, 0);
            break;
        case ItemId::DeviceAuto:
        case ItemId::DeviceNone:
        case ItemId::DevicePad: {
            if (item.id == ItemId::DevicePad) {
                const std::vector<SoRInput::GamepadInfo> pads = SoRInput::gamepads();
                if (item.pad >= static_cast<int>(pads.size()))
                    break;
                const SoRInput::GamepadInfo &pad = pads[static_cast<std::size_t>(item.pad)];
                player.gamepad                   = GamepadChoice::Device;
                player.gamepadGuid               = pad.guid;
                player.gamepadName               = pad.name;
                player.gamepadIndex              = pad.index;
            } else {
                player.gamepad = item.id == ItemId::DeviceAuto ? GamepadChoice::Auto : GamepadChoice::None;
            }
            SoRInput::setConfig(config);
            goTo(m, Screen::Player, m.playerCursor);
            break;
        }
        case ItemId::Back:
            break; // handled by the caller
    }
    playSound(memory, kSoundPick);
}

// Returns true when the menu closes.
bool goBack(Menu &m) {
    switch (m.screen) {
        case Screen::Top:
            return true;
        case Screen::Player:
            goTo(m, Screen::Top, m.topCursor);
            break;
        case Screen::Device:
            goTo(m, Screen::Player, m.playerCursor);
            break;
        case Screen::Deadzone:
            SoRInput::setConfig(SoRInput::config()); // save
            goTo(m, Screen::Player, m.playerCursor);
            break;
        case Screen::Bind:
            finishBind(m, false);
            break;
    }
    return false;
}

// "LEFT 12" / "RIGHT 57": the font has no signs.
std::string axisText(int value, std::string_view negative, std::string_view positive) {
    const int percent = value * 100 / 32767;
    if (percent == 0)
        return "0";
    return std::string(percent < 0 ? negative : positive) + " " + std::to_string(std::abs(percent));
}

void drawScreen(const Menu &m, Canvas &canvas) {
    const std::string playerTitle = "PLAYER " + std::to_string(m.player + 1);
    switch (m.screen) {
        case Screen::Top:
            canvas.text(kLabelColumn, kTitleRow, "CONTROLS", true);
            break;
        case Screen::Player:
            canvas.text(kLabelColumn, kTitleRow, playerTitle, true);
            break;
        case Screen::Device:
            canvas.text(kLabelColumn, kTitleRow, playerTitle + " DEVICE", true);
            break;
        case Screen::Bind: {
            const bool keyboard = m.bindDevice == SoRInput::CaptureDevice::Keyboard;
            canvas.text(kLabelColumn, kTitleRow, playerTitle + (keyboard ? " KEYBOARD" : " GAMEPAD"), true);
            canvas.text(keyboard ? 26 : 25, kTitleRow, keyboard ? "ESC CANCELS" : "START CANCELS", false);
            for (std::size_t i = 0; i < m.bindActions.size(); ++i) {
                const int    row     = 5 + static_cast<int>(i) * 2;
                const Action action  = m.bindActions[i];
                const bool   current = i == m.bindStep;
                std::string  value;
                if (current)
                    value = m.refusedFrames > 0 ? "ALREADY USED" : keyboard ? "PRESS A KEY" : "PRESS A BUTTON";
                else if (keyboard)
                    value = keyName(m.pending.keys[static_cast<std::size_t>(action)]);
                else
                    value = padName(m.pending.pad[static_cast<std::size_t>(action)]);
                canvas.text(kLabelColumn, row, actionLabel(action), current);
                canvas.text(20, row, fit(value, 20), current);
            }
            return;
        }
        case Screen::Deadzone: {
            const ControlsConfig config  = SoRInput::config();
            const int            percent = config.players[static_cast<std::size_t>(m.player)].deadzonePercent;
            const auto           stick   = SoRInput::leftStick(m.player);
            canvas.text(kLabelColumn, kTitleRow, playerTitle + " JOYSTICK DEADZONE", true);
            canvas.text(kLabelColumn, 7, "DEADZONE", true);
            canvas.text(18, 7, std::to_string(percent), true);
            if (!stick) {
                canvas.text(kLabelColumn, 9, "NO GAMEPAD", false);
            } else {
                const int tilt = std::max(std::abs(stick->x), std::abs(stick->y)) * 100 / 32767;
                canvas.text(kLabelColumn, 9, "STICK X", false);
                canvas.text(18, 9, axisText(stick->x, "LEFT", "RIGHT"), false);
                canvas.text(kLabelColumn, 11, "STICK Y", false);
                canvas.text(18, 11, axisText(stick->y, "UP", "DOWN"), false);
                canvas.text(kLabelColumn, 13, "TILT", false);
                canvas.text(18, 13, std::to_string(tilt), false);

                // 25 cells for 0-100%: the tilt as a bar, the deadzone as ':'.
                std::string bar(25, '.');
                for (int i = 0; i < 25 && i * 4 < tilt; ++i)
                    bar[static_cast<std::size_t>(i)] = 'I';
                bar[static_cast<std::size_t>(std::min(24, percent / 4))] = ':';
                canvas.text(kLabelColumn, 15, bar, true);
                canvas.text(kLabelColumn, 17, tilt >= percent ? "MOVING" : "INSIDE THE DEADZONE", tilt >= percent);
            }
            canvas.text(kLabelColumn, 21, "LEFT RIGHT: CHANGE IT", false);
            canvas.text(kLabelColumn, 23, "START: RETURN", false);
            return;
        }
    }

    const std::vector<Item> list        = items(m);
    const int               valueColumn = m.screen == Screen::Player ? 23 : 18;
    const ControlsConfig    config      = SoRInput::config();
    const auto             &player      = config.players[static_cast<std::size_t>(m.player)];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Item &item     = list[i];
        const bool  selected = static_cast<int>(i) == m.cursor;
        if (m.screen == Screen::Device) {
            canvas.text(kLabelColumn, item.row, fit(item.label, 30), selected);
            // '>' marks the saved choice; P1/P2 who reads each pad now.
            bool chosen = false;
            if (item.id == ItemId::DeviceAuto)
                chosen = player.gamepad == GamepadChoice::Auto;
            else if (item.id == ItemId::DeviceNone)
                chosen = player.gamepad == GamepadChoice::None;
            if (item.id == ItemId::DevicePad) {
                const std::vector<SoRInput::GamepadInfo> pads = SoRInput::gamepads();
                if (item.pad < static_cast<int>(pads.size())) {
                    const SoRInput::GamepadInfo &pad = pads[static_cast<std::size_t>(item.pad)];
                    chosen = player.gamepad == GamepadChoice::Device && player.gamepadGuid == pad.guid &&
                             player.gamepadIndex == pad.index;
                    for (int p = 0; p < 2; ++p) {
                        const auto reader = SoRInput::assignedGamepad(p);
                        if (reader && reader->id == pad.id)
                            canvas.text(36, item.row, p == 0 ? "P1" : "P2", selected);
                    }
                }
            }
            if (chosen)
                canvas.text(2, item.row, ">", selected);
            continue;
        }
        canvas.text(kLabelColumn, item.row, item.label, selected);
        canvas.text(valueColumn, item.row, fit(item.value, static_cast<std::size_t>(40 - valueColumn)), selected);
    }
}

void flush(SystemMemory &memory, Menu &m, const Canvas &canvas) {
    for (int row = 0; row < kRows; ++row) {
        int column = 0;
        while (column < kColumns) {
            const auto index = static_cast<std::size_t>(row * kColumns + column);
            if (m.shownValid && canvas.cells[index] == m.shown.cells[index]) {
                ++column;
                continue;
            }
            memory.writeLong(kVdpControl, vramWriteCommand(cellAddress(column, row)));
            while (column < kColumns) {
                const auto i = static_cast<std::size_t>(row * kColumns + column);
                if (m.shownValid && canvas.cells[i] == m.shown.cells[i])
                    break;
                memory.writeWord(kVdpData, canvas.cells[i]);
                ++column;
            }
        }
    }
    m.shown      = canvas;
    m.shownValid = true;
}

} // namespace

void drawText(SystemMemory &memory, int column, int row, std::string_view text, bool highlighted) {
    memory.writeLong(kVdpControl, vramWriteCommand(cellAddress(column, row)));
    for (const char c : text)
        memory.writeWord(kVdpData, static_cast<m_word>(glyph(c).value_or(0x00) | (highlighted ? 0u : kGreyText)));
}

void drawOptionsRow(SystemMemory &memory, bool highlighted) {
    // Where the ROM's CONTROL label and its five-row layout block were.
    const ControlsConfig config = SoRInput::config();
    drawText(memory, kLabelColumn, 11, "CONTROLS", highlighted);
    drawText(memory, 18, 11, "P1 " + std::string(schemeName(config.players[0].scheme)), highlighted);
    drawText(memory, 18, 13, "P2 " + std::string(schemeName(config.players[1].scheme)), highlighted);
}

void open() {
    Menu &m       = menu();
    m             = Menu{};
    m.isOpen      = true;
    m.swallow     = true;
    m.shownValid  = false;
}

bool isOpen() {
    return menu().isOpen;
}

Result frame(SystemMemory &memory, const PlayersControlState &remote) {
    Menu &m = menu();
    if (!m.isOpen)
        return Result::Closed;

    const SoRInput::MenuButtons held = SoRInput::menuButtons(remote, m.screen != Screen::Deadzone);
    const bool up      = m.up.step(held.up, true);
    const bool down    = m.down.step(held.down, true);
    const bool left    = m.left.step(held.left, true);
    const bool right   = m.right.step(held.right, true);
    const bool confirm = m.confirm.step(held.confirm, false);
    const bool back    = m.back.step(held.back, false);

    bool close = false;
    if (m.swallow) {
        m.swallow = false; // a key still held from opening is not a press
    } else if (m.screen == Screen::Bind) {
        updateBind(m, memory);
    } else if (m.screen == Screen::Deadzone) {
        if (left || right) {
            changeDeadzone(m, right ? 1 : -1);
            playSound(memory, kSoundMove);
        }
        if (confirm || back) {
            playSound(memory, kSoundPick);
            goBack(m);
        }
    } else {
        const std::vector<Item> list = items(m);
        clampCursor(m, list);
        if (up || down) {
            moveCursor(m, list, up ? -1 : 1);
            playSound(memory, kSoundMove);
        } else if (back) {
            playSound(memory, kSoundPick);
            close = goBack(m);
        } else if ((left || right) && !list.empty() && list[static_cast<std::size_t>(m.cursor)].id == ItemId::Scheme) {
            select(m, memory, list[static_cast<std::size_t>(m.cursor)]);
        } else if (confirm && !list.empty()) {
            const Item &item = list[static_cast<std::size_t>(m.cursor)];
            if (item.id == ItemId::Back) {
                playSound(memory, kSoundPick);
                close = goBack(m);
            } else if (item.enabled) {
                select(m, memory, item);
            }
        }
    }

    Canvas canvas;
    if (close) {
        flush(memory, m, canvas); // clear plane A for the OPTIONS screen
        m.isOpen = false;
        return Result::Closed;
    }
    if (m.screen != Screen::Bind && m.screen != Screen::Deadzone)
        clampCursor(m, items(m));
    drawScreen(m, canvas);
    flush(memory, m, canvas);
    return Result::Open;
}

} // namespace SoRControlsMenu
