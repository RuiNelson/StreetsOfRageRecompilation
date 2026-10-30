#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Player controls, bound to game actions rather than Mega Drive buttons.
//
// The game reads SDL itself (SoRInput) and the bindings are kept in
// `~/.config/StreetsOfRageProject/controls.yaml`. Each player has a scheme,
// ORIGINAL 3 (Special, Punch, Jump: the ROM's three buttons) or ENHANCED 6
// (also Back Attack, Pick Item and Throw Knife, the old --altControls layout),
// and separate keyboard and gamepad bindings per scheme. On a gamepad the
// directions are always the D-pad and the left stick, and SDL Start is always
// Start; the keyboard binds both.
namespace SoRControlsConfig {

enum class Action : std::uint8_t {
    Up,
    Down,
    Left,
    Right,
    Start,
    Special,
    Punch,
    Jump,
    BackAttack,
    Pickup,
    Throw,
};
constexpr std::size_t kActionCount = 11;

using ActionMask = std::uint16_t;

constexpr ActionMask actionBit(Action action) {
    return static_cast<ActionMask>(1u << static_cast<unsigned>(action));
}

constexpr ActionMask kDirectionMask =
    actionBit(Action::Up) | actionBit(Action::Down) | actionBit(Action::Left) | actionBit(Action::Right);

enum class Scheme : std::uint8_t { Original3, Enhanced6 };
constexpr std::size_t kSchemeCount = 2;

/// Every action a scheme reacts to (directions and Start included).
ActionMask schemeActions(Scheme scheme);
/// The keyboard binding order: directions, the scheme's actions, Start.
std::span<const Action> keyboardActions(Scheme scheme);
/// The gamepad binding order: the scheme's actions only.
std::span<const Action> gamepadActions(Scheme scheme);

/// YAML key, e.g. "back_attack".
std::string_view actionKey(Action action);
std::string_view schemeKey(Scheme scheme);

/// A gamepad input an action can be bound to: a button or a trigger, which
/// counts as pressed past half travel.
struct PadInput {
    enum class Kind : std::uint8_t { None, Button, LeftTrigger, RightTrigger };
    Kind              kind   = Kind::None;
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;

    static PadInput ofButton(SDL_GamepadButton b) {
        return {Kind::Button, b};
    }
    static PadInput leftTrigger() {
        return {Kind::LeftTrigger, SDL_GAMEPAD_BUTTON_INVALID};
    }
    static PadInput rightTrigger() {
        return {Kind::RightTrigger, SDL_GAMEPAD_BUTTON_INVALID};
    }
    bool operator==(const PadInput &) const = default;
};

/// Positional names ("South", "LeftTrigger", ...), independent of the pad's labels.
std::string             padInputName(const PadInput &input);
std::optional<PadInput> padInputFromName(std::string_view name);

struct SchemeBindings {
    /// SDL_SCANCODE_UNKNOWN = unbound.
    std::array<SDL_Scancode, kActionCount> keys{};
    std::array<PadInput, kActionCount>     pad{};
};

enum class GamepadChoice : std::uint8_t { Auto, None, Device };

struct PlayerConfig {
    Scheme        scheme  = Scheme::Original3;
    GamepadChoice gamepad = GamepadChoice::Auto;
    /// GamepadChoice::Device: the pad's GUID, its name (shown when absent) and
    /// which of the connected pads with that GUID it is.
    std::string                                gamepadGuid;
    std::string                                gamepadName;
    int                                        gamepadIndex    = 0;
    int                                        deadzonePercent = 24;
    std::array<SchemeBindings, kSchemeCount> bindings{};

    SchemeBindings &schemeBindings(Scheme s) {
        return bindings[static_cast<std::size_t>(s)];
    }
    const SchemeBindings &schemeBindings(Scheme s) const {
        return bindings[static_cast<std::size_t>(s)];
    }
};

constexpr int kMaximumDeadzonePercent = 90;

struct ControlsConfig {
    std::array<PlayerConfig, 2> players{};

    static ControlsConfig defaults();
};

/// `~/.config/StreetsOfRageProject/controls.yaml`.
std::filesystem::path defaultPath();

/// Reads @p path. A missing file is created with the defaults; an entry that
/// is missing or unreadable keeps its default. A file that cannot be parsed is
/// left alone (and reported) and the defaults are used.
ControlsConfig loadOrCreate(const std::filesystem::path &path);

/// Writes @p config to @p path, creating the directory. False on failure.
bool save(const ControlsConfig &config, const std::filesystem::path &path);

} // namespace SoRControlsConfig
