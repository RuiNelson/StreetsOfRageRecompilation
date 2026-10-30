#pragma once

#include "SoRControlsConfig.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>


// Keyboard and gamepad input, read from SDL by the game itself.
//
// MegaDriveEnvironment's Controllers is told not to read SDL
// (setPhysicalInputEnabled(false)); this module follows the same pattern
// instead: an SDL event watch on the main thread keeps the key, button and
// axis state of every device under a mutex, and the joypad sampler on the CPU
// thread turns it into each player's actions through controls.yaml.
namespace SoRInput {

using SoRControlsConfig::ActionMask;
using SoRControlsConfig::ControlsConfig;
using SoRControlsConfig::Scheme;

/// Main thread, after SDL is initialised: loads (or creates) @p configPath,
/// opens the connected gamepads and starts watching SDL events.
void start(const std::filesystem::path &configPath);
/// Main thread: stops watching and closes the gamepads.
void stop();

/// CPU thread, once per sampled frame: the actions @p player (0 or 1) holds
/// on the keyboard and on their gamepad, limited to their scheme's actions.
/// A key or button pressed since the player's previous sample counts even if
/// it was already released, and that latch is consumed here.
ActionMask physicalActions(int player);

/// The scheme @p player uses (controls.yaml).
Scheme scheme(int player);

ControlsConfig config();
/// Applies @p config at once and, when @p persist, writes controls.yaml.
void setConfig(const ControlsConfig &config, bool persist = true);

// ── For the OPTIONS > CONTROLS menu ───────────────────────────────────────

struct GamepadInfo {
    SDL_JoystickID id = 0;
    std::string    guid;
    std::string    name;
    /// Which of the connected pads with this GUID it is (0 for the first).
    int index = 0;
};

/// The connected gamepads, in connection order.
std::vector<GamepadInfo> gamepads();
/// The gamepad @p player reads now, if any.
std::optional<GamepadInfo> assignedGamepad(int player);
bool                       keyboardAvailable();

struct StickReading {
    int x = 0; ///< -32768..32767, left negative
    int y = 0; ///< -32768..32767, up negative
};
/// The raw left stick of @p player's gamepad.
std::optional<StickReading> leftStick(int player);

struct MenuButtons {
    bool up = false, down = false, left = false, right = false, confirm = false, back = false;
};
/// Once per menu frame: what any device holds (or tapped since the last
/// call). Keyboard: arrows, Enter, Esc and both players' bindings (their
/// directions; Punch, Jump or Special confirm; Start goes back). Gamepads:
/// the D-pad, the left stick when @p sticks, South or a bound action to
/// confirm, Start to go back. @p remote, the remote client's actions (both
/// players): directions, Punch/Jump/Special to confirm, Start to go back.
MenuButtons menuButtons(ActionMask remote, bool sticks = true);

enum class CaptureDevice { Keyboard, Gamepad };
struct Captured {
    enum class Status { Pending, Cancelled, Key, Pad };
    Status                       status = Status::Pending;
    SDL_Scancode                 key    = SDL_SCANCODE_UNKNOWN;
    SoRControlsConfig::PadInput pad;
};
/// Starts waiting for the next key (Keyboard) or bindable input of
/// @p player's gamepad (Gamepad); false when that player has no gamepad.
/// Esc, or Start on any gamepad, cancels.
bool beginCapture(CaptureDevice device, int player);
/// Pending, Cancelled, or the next captured input (which is then consumed).
Captured takeCapture();
void     endCapture();

} // namespace SoRInput
