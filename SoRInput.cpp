#include "SoRInput.hpp"

#include "SoRCheats.hpp"
#include "system/controllers/Controllers.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace SoRInput {
namespace {

using SoRControlsConfig::Action;
using SoRControlsConfig::actionBit;
using SoRControlsConfig::GamepadChoice;
using SoRControlsConfig::PadInput;
using SoRControlsConfig::PlayerConfig;
using SoRControlsConfig::SchemeBindings;

constexpr Sint16 kTriggerThreshold = 16384;

struct Pad {
    SDL_JoystickID                             id      = 0;
    SDL_Gamepad                               *gamepad = nullptr;
    std::string                                guid;
    std::string                                name;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes{};
    /// Per player: pressed since that player's last sample (see State::keyTapped).
    std::array<std::array<bool, SDL_GAMEPAD_BUTTON_COUNT>, 2> buttonTapped{};
};

// Written by the event watch (main thread) and setConfig; read by the sampler
// (CPU thread). Everything is behind one mutex.
struct State {
    std::mutex                           mutex;
    bool                                 started = false;
    std::filesystem::path                path;
    ControlsConfig                       config = ControlsConfig::defaults();
    std::optional<Scheme>                schemeOverride;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    // Per player: pressed since that player's last sample. A press released
    // before the next frame is sampled still counts for that one frame, so a
    // quick tap is never lost (posted macOS key events even arrive as
    // down/up pairs while held).
    std::array<std::array<bool, SDL_SCANCODE_COUNT>, 2> keyTapped{};
    std::vector<Pad>                                    pads; // in connection order
};

State &state() {
    static State instance;
    return instance;
}

std::string guidString(SDL_JoystickID id) {
    char text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), text, sizeof text);
    return text;
}

void openPadLocked(State &s, SDL_JoystickID id) {
    if (std::ranges::any_of(s.pads, [id](const Pad &pad) { return pad.id == id; }))
        return;
    SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
    if (gamepad == nullptr)
        return;
    const char *name = SDL_GetGamepadName(gamepad);
    s.pads.push_back(Pad{.id = id, .gamepad = gamepad, .guid = guidString(id), .name = name ? name : ""});
}

void closePadLocked(State &s, SDL_JoystickID id) {
    const auto pad = std::ranges::find_if(s.pads, [id](const Pad &p) { return p.id == id; });
    if (pad == s.pads.end())
        return;
    SDL_CloseGamepad(pad->gamepad);
    s.pads.erase(pad);
}

Scheme effectiveSchemeLocked(const State &s, int player) {
    return s.schemeOverride.value_or(s.config.players[static_cast<std::size_t>(player)].scheme);
}

void publishSchemesLocked(const State &s) {
    for (int player = 0; player < 2; ++player)
        SoRCheats::setEnhancedControls(player, effectiveSchemeLocked(s, player) == Scheme::Enhanced6);
}

// The pad each player reads: explicit choices first (the n-th connected pad
// with the saved GUID), then AUTO hands out the remaining pads in player
// order. A chosen pad that is not connected counts as AUTO.
std::array<const Pad *, 2> assignedPadsLocked(const State &s) {
    std::array<const Pad *, 2> assigned{nullptr, nullptr};
    std::vector<bool>          taken(s.pads.size(), false);
    std::array<bool, 2>        automatic{false, false};

    for (std::size_t player = 0; player < 2; ++player) {
        const PlayerConfig &config = s.config.players[player];
        if (config.gamepad == GamepadChoice::None)
            continue;
        if (config.gamepad == GamepadChoice::Device) {
            int ordinal = 0;
            for (std::size_t i = 0; i < s.pads.size(); ++i) {
                if (s.pads[i].guid != config.gamepadGuid)
                    continue;
                if (ordinal++ == config.gamepadIndex && !taken[i]) {
                    assigned[player] = &s.pads[i];
                    taken[i]         = true;
                    break;
                }
            }
        }
        automatic[player] = assigned[player] == nullptr;
    }

    for (std::size_t player = 0; player < 2; ++player) {
        if (!automatic[player])
            continue;
        for (std::size_t i = 0; i < s.pads.size(); ++i) {
            if (!taken[i]) {
                assigned[player] = &s.pads[i];
                taken[i]         = true;
                break;
            }
        }
    }
    return assigned;
}

bool padButtonHeld(const Pad &pad, SDL_GamepadButton button, int player) {
    return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT &&
           (pad.buttons[button] || pad.buttonTapped[static_cast<std::size_t>(player)][button]);
}

bool padInputHeld(const Pad &pad, const PadInput &input, int player) {
    switch (input.kind) {
        case PadInput::Kind::Button:
            return padButtonHeld(pad, input.button, player);
        case PadInput::Kind::LeftTrigger:
            return pad.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] >= kTriggerThreshold;
        case PadInput::Kind::RightTrigger:
            return pad.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] >= kTriggerThreshold;
        case PadInput::Kind::None:
            break;
    }
    return false;
}

void setIf(ActionMask &mask, bool held, Action action) {
    if (held)
        mask = static_cast<ActionMask>(mask | actionBit(action));
}

bool handleEvent(void * /*userdata*/, SDL_Event *event) {
    State           &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.started)
        return true;

    switch (event->type) {
        case SDL_EVENT_KEY_DOWN:
            // Ctrl/Alt/Cmd chords are the host's hotkeys, not game input.
            if (!event->key.repeat && (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
                event->key.scancode < SDL_SCANCODE_COUNT) {
                s.keys[event->key.scancode]         = true;
                s.keyTapped[0][event->key.scancode] = true;
                s.keyTapped[1][event->key.scancode] = true;
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (event->key.scancode < SDL_SCANCODE_COUNT)
                s.keys[event->key.scancode] = false;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Key-ups are not delivered to an unfocused window.
            s.keys.fill(false);
            for (auto &tapped : s.keyTapped)
                tapped.fill(false);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            openPadLocked(s, event->gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            closePadLocked(s, event->gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            for (Pad &pad : s.pads) {
                if (pad.id != event->gbutton.which || event->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT)
                    continue;
                const bool down                    = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                pad.buttons[event->gbutton.button] = down;
                if (down) {
                    pad.buttonTapped[0][event->gbutton.button] = true;
                    pad.buttonTapped[1][event->gbutton.button] = true;
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            for (Pad &pad : s.pads)
                if (pad.id == event->gaxis.which && event->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT)
                    pad.axes[event->gaxis.axis] = event->gaxis.value;
            break;
        default:
            break;
    }
    return true; // ignored for event watches
}

} // namespace

void start(const std::filesystem::path &configPath) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    State &s = state();
    {
        std::lock_guard lock(s.mutex);
        s.path   = configPath;
        s.config = SoRControlsConfig::loadOrCreate(configPath);
        s.keys.fill(false);

        // Pads already connected sent their ADDED events before the watch
        // existed.
        int             count = 0;
        SDL_JoystickID *ids   = SDL_GetGamepads(&count);
        for (int i = 0; ids != nullptr && i < count; ++i)
            openPadLocked(s, ids[i]);
        SDL_free(ids);

        s.started = true;
        publishSchemesLocked(s);
    }
    SDL_AddEventWatch(handleEvent, nullptr);
}

void stop() {
    SDL_RemoveEventWatch(handleEvent, nullptr);
    State          &s = state();
    std::lock_guard lock(s.mutex);
    s.started = false;
    for (Pad &pad : s.pads)
        SDL_CloseGamepad(pad.gamepad);
    s.pads.clear();
}

ActionMask physicalActions(int player) {
    if (player < 0 || player > 1)
        return 0;

    State          &s = state();
    std::lock_guard lock(s.mutex);
    const Scheme          scheme   = effectiveSchemeLocked(s, player);
    const PlayerConfig   &config   = s.config.players[static_cast<std::size_t>(player)];
    const SchemeBindings &bindings = config.schemeBindings(scheme);

    auto      &keyTapped = s.keyTapped[static_cast<std::size_t>(player)];
    ActionMask mask      = 0;
    for (const Action action : SoRControlsConfig::keyboardActions(scheme)) {
        const SDL_Scancode scancode = bindings.keys[static_cast<std::size_t>(action)];
        setIf(mask, scancode != SDL_SCANCODE_UNKNOWN && (s.keys[scancode] || keyTapped[scancode]), action);
    }
    keyTapped.fill(false);

    if (const Pad *pad = assignedPadsLocked(s)[static_cast<std::size_t>(player)]) {
        // The D-pad and the left stick are always the directions, with the
        // deadzone applied to each axis as MegaDriveEnvironment did.
        const int threshold = std::max(1, config.deadzonePercent * 32767 / 100);
        const int x         = pad->axes[SDL_GAMEPAD_AXIS_LEFTX];
        const int y         = pad->axes[SDL_GAMEPAD_AXIS_LEFTY];
        setIf(mask, padButtonHeld(*pad, SDL_GAMEPAD_BUTTON_DPAD_UP, player) || y <= -threshold, Action::Up);
        setIf(mask, padButtonHeld(*pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, player) || y >= threshold, Action::Down);
        setIf(mask, padButtonHeld(*pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, player) || x <= -threshold, Action::Left);
        setIf(mask, padButtonHeld(*pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, player) || x >= threshold, Action::Right);
        setIf(mask, padButtonHeld(*pad, SDL_GAMEPAD_BUTTON_START, player), Action::Start);
        for (const Action action : SoRControlsConfig::gamepadActions(scheme))
            setIf(mask, padInputHeld(*pad, bindings.pad[static_cast<std::size_t>(action)], player), action);
    }
    for (Pad &pad : s.pads)
        pad.buttonTapped[static_cast<std::size_t>(player)].fill(false);

    return static_cast<ActionMask>(mask & SoRControlsConfig::schemeActions(scheme));
}

Scheme scheme(int player) {
    State          &s = state();
    std::lock_guard lock(s.mutex);
    return effectiveSchemeLocked(s, std::clamp(player, 0, 1));
}

void setSchemeOverride(std::optional<Scheme> scheme) {
    State          &s = state();
    std::lock_guard lock(s.mutex);
    s.schemeOverride = scheme;
    publishSchemesLocked(s);
}

ActionMask fromMegaDriveButtons(const PlayerControlsState &buttons, Scheme scheme) {
    ActionMask mask = 0;
    setIf(mask, buttons.up, Action::Up);
    setIf(mask, buttons.down, Action::Down);
    setIf(mask, buttons.left, Action::Left);
    setIf(mask, buttons.right, Action::Right);
    setIf(mask, buttons.start, Action::Start);
    if (scheme == Scheme::Enhanced6) {
        setIf(mask, buttons.b, Action::Punch);
        setIf(mask, buttons.c, Action::Jump);
        setIf(mask, buttons.a, Action::BackAttack);
        setIf(mask, buttons.x, Action::Special);
        setIf(mask, buttons.y, Action::Pickup);
        setIf(mask, buttons.z, Action::Throw);
    } else {
        setIf(mask, buttons.a, Action::Special);
        setIf(mask, buttons.b, Action::Punch);
        setIf(mask, buttons.c, Action::Jump);
    }
    return mask;
}

ControlsConfig config() {
    State          &s = state();
    std::lock_guard lock(s.mutex);
    return s.config;
}

void setConfig(const ControlsConfig &config, bool persist) {
    State                &s = state();
    std::filesystem::path path;
    {
        std::lock_guard lock(s.mutex);
        s.config = config;
        path     = s.path;
        publishSchemesLocked(s);
    }
    if (persist && !path.empty() && !SoRControlsConfig::save(config, path))
        std::fprintf(stderr, "Could not save the controls to %s\n", path.string().c_str());
}

} // namespace SoRInput
