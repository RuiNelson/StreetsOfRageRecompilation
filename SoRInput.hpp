#pragma once

#include "SoRControlsConfig.hpp"

#include <filesystem>
#include <optional>

struct PlayerControlsState;

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

/// The scheme @p player uses now (the session override, else controls.yaml).
Scheme scheme(int player);

/// Puts both players on @p scheme for this session without saving it
/// (--altControls); std::nullopt returns to controls.yaml.
void setSchemeOverride(std::optional<Scheme> scheme);

/// Remote (autoplay) Mega Drive buttons as actions, with the mappings the
/// game used before it bound actions: ORIGINAL 3 A=Special B=Punch C=Jump;
/// ENHANCED 6 B=Punch C=Jump A=Back Attack X=Special Y=Pick Item Z=Throw.
ActionMask fromMegaDriveButtons(const PlayerControlsState &buttons, Scheme scheme);

ControlsConfig config();
/// Applies @p config at once and, when @p persist, writes controls.yaml.
void setConfig(const ControlsConfig &config, bool persist = true);

} // namespace SoRInput
