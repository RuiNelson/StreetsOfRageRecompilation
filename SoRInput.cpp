#include "SoRInput.hpp"

#include "SoRCheats.hpp"

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

// SOR_INPUT_TRACE=1: log every gamepad event and each player's actions when
// they change (stderr), to see what a controller really sends.
bool gTrace = false;
constexpr int    kMenuStickThreshold = 16384;

// Tap latches (State::keyTapped): one per reader, P1's and P2's samples and
// the CONTROLS menu, each consumed by its own reader.
constexpr std::size_t kLatchCount = 3;
constexpr std::size_t kMenuLatch  = 2;

struct Pad {
    SDL_JoystickID                             id      = 0;
    SDL_Gamepad                               *gamepad = nullptr;
    std::string                                guid;
    std::string                                name;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axes{};
    /// Per reader: pressed since its last read (see State::keyTapped).
    std::array<std::array<bool, SDL_GAMEPAD_BUTTON_COUNT>, kLatchCount> buttonTapped{};
};

// Written by the event watch (main thread) and setConfig; read by the sampler
// (CPU thread). Everything is behind one mutex.
struct State {
    std::mutex                           mutex;
    bool                                 started = false;
    std::filesystem::path                path;
    ControlsConfig                       config = ControlsConfig::defaults();
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    // Per reader (kLatchCount): pressed since its last read. A press
    // released before the next frame is sampled still counts for that one
    // frame, so a quick tap is never lost (posted macOS key events even
    // arrive as down/up pairs while held).
    std::array<std::array<bool, SDL_SCANCODE_COUNT>, kLatchCount> keyTapped{};
    std::vector<Pad>                                              pads; // in connection order

    // The CONTROLS menu's binding capture: the next key or pad input.
    bool           captureActive = false;
    CaptureDevice  captureDevice = CaptureDevice::Keyboard;
    SDL_JoystickID capturePad    = 0;
    Captured       captured;
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
    return s.config.players[static_cast<std::size_t>(player)].scheme;
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

// Esc, or Start on any gamepad, cancels a capture. A keyboard capture takes
// any other key; a gamepad capture takes a bindable input of the chosen pad
// (the D-pad and Start are the directions and Start).
void captureKeyLocked(State &s, SDL_Scancode scancode) {
    if (!s.captureActive || s.captured.status != Captured::Status::Pending)
        return;
    if (scancode == SDL_SCANCODE_ESCAPE)
        s.captured.status = Captured::Status::Cancelled;
    else if (s.captureDevice == CaptureDevice::Keyboard)
        s.captured = {Captured::Status::Key, scancode, {}};
}

void capturePadLocked(State &s, SDL_JoystickID id, const PadInput &input) {
    if (!s.captureActive || s.captured.status != Captured::Status::Pending)
        return;
    if (input.kind == PadInput::Kind::Button && input.button == SDL_GAMEPAD_BUTTON_START) {
        s.captured.status = Captured::Status::Cancelled;
        return;
    }
    if (s.captureDevice != CaptureDevice::Gamepad || id != s.capturePad)
        return;
    if (SoRControlsConfig::padInputName(input) == "none")
        return; // not bindable (D-pad)
    s.captured = {Captured::Status::Pad, SDL_SCANCODE_UNKNOWN, input};
}

bool handleEvent(void * /*userdata*/, SDL_Event *event) {
    State           &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.started)
        return true;

    if (gTrace) {
        switch (event->type) {
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                std::fprintf(stderr,
                             "[input] pad %u button %s %s\n",
                             (unsigned)event->gbutton.which,
                             SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(event->gbutton.button)),
                             event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "down" : "up");
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                std::fprintf(stderr,
                             "[input] pad %u axis %s = %d\n",
                             (unsigned)event->gaxis.which,
                             SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(event->gaxis.axis)),
                             (int)event->gaxis.value);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                std::fprintf(stderr,
                             "[input] pad %u %s\n",
                             (unsigned)event->gdevice.which,
                             event->type == SDL_EVENT_GAMEPAD_ADDED ? "added" : "removed");
                break;
            default:
                break;
        }
    }

    switch (event->type) {
        case SDL_EVENT_KEY_DOWN:
            // Ctrl/Alt/Cmd chords are the host's hotkeys, not game input.
            if (!event->key.repeat && (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
                event->key.scancode < SDL_SCANCODE_COUNT) {
                s.keys[event->key.scancode] = true;
                for (auto &tapped : s.keyTapped)
                    tapped[event->key.scancode] = true;
                captureKeyLocked(s, event->key.scancode);
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
                const auto button   = static_cast<SDL_GamepadButton>(event->gbutton.button);
                const bool down     = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                pad.buttons[button] = down;
                if (down) {
                    for (auto &tapped : pad.buttonTapped)
                        tapped[button] = true;
                    capturePadLocked(s, pad.id, PadInput::ofButton(button));
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            for (Pad &pad : s.pads) {
                if (pad.id != event->gaxis.which || event->gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT)
                    continue;
                const Sint16 before          = pad.axes[event->gaxis.axis];
                pad.axes[event->gaxis.axis] = event->gaxis.value;
                if (before < kTriggerThreshold && event->gaxis.value >= kTriggerThreshold) {
                    if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                        capturePadLocked(s, pad.id, PadInput::leftTrigger());
                    else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                        capturePadLocked(s, pad.id, PadInput::rightTrigger());
                }
            }
            break;
        default:
            break;
    }
    return true; // ignored for event watches
}

} // namespace

void start(const std::filesystem::path &configPath) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    const char *traceValue = SDL_getenv("SOR_INPUT_TRACE");
    gTrace                 = traceValue != nullptr && *traceValue != '\0' && *traceValue != '0';
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

    mask = static_cast<ActionMask>(mask & SoRControlsConfig::schemeActions(scheme));
    if (gTrace) {
        static std::array<ActionMask, 2> traced{};
        if (traced[static_cast<std::size_t>(player)] != mask) {
            traced[static_cast<std::size_t>(player)] = mask;
            const Pad *pad = assignedPadsLocked(s)[static_cast<std::size_t>(player)];
            std::fprintf(stderr,
                         "[input] P%d actions %03x (scheme %s, pad %s, LT %d, RT %d)\n",
                         player + 1,
                         (unsigned)mask,
                         scheme == Scheme::Enhanced6 ? "enhanced6" : "original3",
                         pad ? pad->name.c_str() : "none",
                         pad ? (int)pad->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] : 0,
                         pad ? (int)pad->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] : 0);
        }
    }
    return mask;
}

Scheme scheme(int player) {
    State          &s = state();
    std::lock_guard lock(s.mutex);
    return effectiveSchemeLocked(s, std::clamp(player, 0, 1));
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

std::vector<GamepadInfo> gamepads() {
    State                   &s = state();
    std::lock_guard          lock(s.mutex);
    std::vector<GamepadInfo> result;
    for (const Pad &pad : s.pads) {
        int index = 0;
        for (const GamepadInfo &earlier : result)
            if (earlier.guid == pad.guid)
                ++index;
        result.push_back({pad.id, pad.guid, pad.name, index});
    }
    return result;
}

std::optional<GamepadInfo> assignedGamepad(int player) {
    const std::vector<GamepadInfo> all = gamepads();
    State                         &s   = state();
    std::lock_guard                lock(s.mutex);
    const Pad                     *pad = assignedPadsLocked(s)[static_cast<std::size_t>(std::clamp(player, 0, 1))];
    if (pad == nullptr)
        return std::nullopt;
    for (const GamepadInfo &info : all)
        if (info.id == pad->id)
            return info;
    return std::nullopt;
}

bool keyboardAvailable() {
    return SDL_HasKeyboard();
}

std::optional<StickReading> leftStick(int player) {
    State           &s = state();
    std::lock_guard lock(s.mutex);
    const Pad       *pad = assignedPadsLocked(s)[static_cast<std::size_t>(std::clamp(player, 0, 1))];
    if (pad == nullptr)
        return std::nullopt;
    return StickReading{pad->axes[SDL_GAMEPAD_AXIS_LEFTX], pad->axes[SDL_GAMEPAD_AXIS_LEFTY]};
}

MenuButtons menuButtons(ActionMask remote, bool sticks) {
    State           &s = state();
    std::lock_guard lock(s.mutex);
    MenuButtons     buttons;

    auto &keyTapped = s.keyTapped[kMenuLatch];
    auto  key       = [&](SDL_Scancode scancode) {
        return scancode != SDL_SCANCODE_UNKNOWN && (s.keys[scancode] || keyTapped[scancode]);
    };
    buttons.up      = key(SDL_SCANCODE_UP);
    buttons.down    = key(SDL_SCANCODE_DOWN);
    buttons.left    = key(SDL_SCANCODE_LEFT);
    buttons.right   = key(SDL_SCANCODE_RIGHT);
    buttons.confirm = key(SDL_SCANCODE_RETURN) || key(SDL_SCANCODE_KP_ENTER);
    buttons.back    = key(SDL_SCANCODE_ESCAPE);

    // Either player's own bindings work too: their directions, their Punch,
    // Jump or Special to confirm, their Start to go back.
    const std::array<const Pad *, 2> assigned = assignedPadsLocked(s);
    for (int player = 0; player < 2; ++player) {
        const Scheme          scheme   = effectiveSchemeLocked(s, player);
        const SchemeBindings &bindings = s.config.players[static_cast<std::size_t>(player)].schemeBindings(scheme);
        auto bound = [&](Action action) { return bindings.keys[static_cast<std::size_t>(action)]; };
        buttons.up      = buttons.up || key(bound(Action::Up));
        buttons.down    = buttons.down || key(bound(Action::Down));
        buttons.left    = buttons.left || key(bound(Action::Left));
        buttons.right   = buttons.right || key(bound(Action::Right));
        buttons.confirm = buttons.confirm || key(bound(Action::Punch)) || key(bound(Action::Jump)) ||
                          key(bound(Action::Special));
        buttons.back = buttons.back || key(bound(Action::Start));

        if (const Pad *pad = assigned[static_cast<std::size_t>(player)]) {
            for (const Action action : {Action::Punch, Action::Jump, Action::Special})
                buttons.confirm = buttons.confirm ||
                                  padInputHeld(*pad, bindings.pad[static_cast<std::size_t>(action)], kMenuLatch);
        }
    }
    keyTapped.fill(false);

    for (Pad &pad : s.pads) {
        auto held = [&](SDL_GamepadButton button) { return padButtonHeld(pad, button, kMenuLatch); };
        const int x = sticks ? pad.axes[SDL_GAMEPAD_AXIS_LEFTX] : 0;
        const int y = sticks ? pad.axes[SDL_GAMEPAD_AXIS_LEFTY] : 0;
        buttons.up      = buttons.up || held(SDL_GAMEPAD_BUTTON_DPAD_UP) || y <= -kMenuStickThreshold;
        buttons.down    = buttons.down || held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y >= kMenuStickThreshold;
        buttons.left    = buttons.left || held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x <= -kMenuStickThreshold;
        buttons.right   = buttons.right || held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x >= kMenuStickThreshold;
        buttons.confirm = buttons.confirm || held(SDL_GAMEPAD_BUTTON_SOUTH);
        buttons.back    = buttons.back || held(SDL_GAMEPAD_BUTTON_START);
        pad.buttonTapped[kMenuLatch].fill(false);
    }

    auto has        = [remote](Action action) { return (remote & actionBit(action)) != 0; };
    buttons.up      = buttons.up || has(Action::Up);
    buttons.down    = buttons.down || has(Action::Down);
    buttons.left    = buttons.left || has(Action::Left);
    buttons.right   = buttons.right || has(Action::Right);
    buttons.confirm = buttons.confirm || has(Action::Punch) || has(Action::Jump) || has(Action::Special);
    buttons.back    = buttons.back || has(Action::Start);
    return buttons;
}

bool beginCapture(CaptureDevice device, int player) {
    std::optional<SDL_JoystickID> pad;
    if (device == CaptureDevice::Gamepad) {
        const std::optional<GamepadInfo> info = assignedGamepad(player);
        if (!info)
            return false;
        pad = info->id;
    }
    State           &s = state();
    std::lock_guard lock(s.mutex);
    s.captureActive = true;
    s.captureDevice = device;
    s.capturePad    = pad.value_or(0);
    s.captured      = {};
    return true;
}

Captured takeCapture() {
    State           &s = state();
    std::lock_guard lock(s.mutex);
    const Captured   result = s.captured;
    if (result.status == Captured::Status::Key || result.status == Captured::Status::Pad)
        s.captured = {}; // wait for the next one
    return result;
}

void endCapture() {
    State           &s = state();
    std::lock_guard lock(s.mutex);
    s.captureActive = false;
    s.captured      = {};
}

} // namespace SoRInput
