#include "SoRControlsConfig.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace SoRControlsConfig {
namespace {

constexpr int kFormatVersion = 1;

constexpr std::array<Action, 8> kOriginal3Keyboard = {
    Action::Up, Action::Down, Action::Left, Action::Right, Action::Special, Action::Punch, Action::Jump, Action::Start,
};
constexpr std::array<Action, 11> kEnhanced6Keyboard = {
    Action::Up,      Action::Down,       Action::Left,   Action::Right, Action::Special, Action::Punch,
    Action::Jump,    Action::BackAttack, Action::Pickup, Action::Throw, Action::Start,
};
constexpr std::array<Action, 3> kOriginal3Pad = {Action::Special, Action::Punch, Action::Jump};
constexpr std::array<Action, 6> kEnhanced6Pad = {
    Action::Special, Action::Punch, Action::Jump, Action::BackAttack, Action::Pickup, Action::Throw,
};

constexpr std::array<std::string_view, kActionCount> kActionKeys = {
    "up", "down", "left", "right", "start", "special", "punch", "jump", "back_attack", "pickup", "throw",
};

struct PadButtonName {
    SDL_GamepadButton button;
    std::string_view  name;
};

// The D-pad and Start are not bindable: they are the directions and Start.
constexpr std::array<PadButtonName, 20> kPadButtonNames = {{
    {SDL_GAMEPAD_BUTTON_SOUTH, "South"},
    {SDL_GAMEPAD_BUTTON_EAST, "East"},
    {SDL_GAMEPAD_BUTTON_WEST, "West"},
    {SDL_GAMEPAD_BUTTON_NORTH, "North"},
    {SDL_GAMEPAD_BUTTON_BACK, "Back"},
    {SDL_GAMEPAD_BUTTON_GUIDE, "Guide"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "LeftStick"},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "RightStick"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LeftShoulder"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RightShoulder"},
    {SDL_GAMEPAD_BUTTON_MISC1, "Misc1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, "RightPaddle1"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, "LeftPaddle1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, "RightPaddle2"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, "LeftPaddle2"},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, "Touchpad"},
    {SDL_GAMEPAD_BUTTON_MISC2, "Misc2"},
    {SDL_GAMEPAD_BUTTON_MISC3, "Misc3"},
    {SDL_GAMEPAD_BUTTON_MISC4, "Misc4"},
    {SDL_GAMEPAD_BUTTON_MISC5, "Misc5"},
}};

std::size_t index(Action action) {
    return static_cast<std::size_t>(action);
}

void bindKeys(SchemeBindings &bindings, std::span<const Action> actions, std::span<const SDL_Scancode> keys) {
    for (std::size_t i = 0; i < actions.size(); ++i)
        bindings.keys[index(actions[i])] = keys[i];
}

void bindPad(SchemeBindings &bindings, std::span<const Action> actions, std::span<const PadInput> inputs) {
    for (std::size_t i = 0; i < actions.size(); ++i)
        bindings.pad[index(actions[i])] = inputs[i];
}

PlayerConfig defaultPlayer(int player) {
    PlayerConfig config;
    const std::array<PadInput, 6> pad = {
        PadInput::rightTrigger(),
        PadInput::ofButton(SDL_GAMEPAD_BUTTON_WEST),
        PadInput::ofButton(SDL_GAMEPAD_BUTTON_SOUTH),
        PadInput::leftTrigger(),
        PadInput::ofButton(SDL_GAMEPAD_BUTTON_NORTH),
        PadInput::ofButton(SDL_GAMEPAD_BUTTON_EAST),
    };
    SchemeBindings &original3 = config.schemeBindings(Scheme::Original3);
    SchemeBindings &enhanced6 = config.schemeBindings(Scheme::Enhanced6);
    bindPad(original3, kOriginal3Pad, std::span(pad).first(3));
    bindPad(enhanced6, kEnhanced6Pad, pad);

    if (player == 0) {
        bindKeys(original3,
                 kOriginal3Keyboard,
                 std::array{SDL_SCANCODE_UP,
                            SDL_SCANCODE_DOWN,
                            SDL_SCANCODE_LEFT,
                            SDL_SCANCODE_RIGHT,
                            SDL_SCANCODE_Z,
                            SDL_SCANCODE_X,
                            SDL_SCANCODE_C,
                            SDL_SCANCODE_V});
        bindKeys(enhanced6,
                 kEnhanced6Keyboard,
                 std::array{SDL_SCANCODE_UP,
                            SDL_SCANCODE_DOWN,
                            SDL_SCANCODE_LEFT,
                            SDL_SCANCODE_RIGHT,
                            SDL_SCANCODE_Z,
                            SDL_SCANCODE_X,
                            SDL_SCANCODE_C,
                            SDL_SCANCODE_V,
                            SDL_SCANCODE_B,
                            SDL_SCANCODE_N,
                            SDL_SCANCODE_M});
    } else {
        bindKeys(original3,
                 kOriginal3Keyboard,
                 std::array{SDL_SCANCODE_W,
                            SDL_SCANCODE_S,
                            SDL_SCANCODE_A,
                            SDL_SCANCODE_D,
                            SDL_SCANCODE_H,
                            SDL_SCANCODE_J,
                            SDL_SCANCODE_K,
                            SDL_SCANCODE_L});
        bindKeys(enhanced6,
                 kEnhanced6Keyboard,
                 std::array{SDL_SCANCODE_W,
                            SDL_SCANCODE_S,
                            SDL_SCANCODE_A,
                            SDL_SCANCODE_D,
                            SDL_SCANCODE_H,
                            SDL_SCANCODE_J,
                            SDL_SCANCODE_K,
                            SDL_SCANCODE_Y,
                            SDL_SCANCODE_U,
                            SDL_SCANCODE_I,
                            SDL_SCANCODE_L});
    }
    return config;
}

std::string_view gamepadChoiceKey(GamepadChoice choice) {
    switch (choice) {
        case GamepadChoice::None:
            return "none";
        case GamepadChoice::Device:
            return "device";
        case GamepadChoice::Auto:
            break;
    }
    return "auto";
}

template <typename T> void readScalar(const YAML::Node &node, T &value) {
    if (!node || !node.IsScalar())
        return;
    try {
        value = node.as<T>();
    } catch (const YAML::Exception &) {
    }
}

void readSchemeBindings(const YAML::Node &node, Scheme scheme, SchemeBindings &bindings) {
    if (!node || !node.IsMap())
        return;
    if (const YAML::Node keyboard = node["keyboard"]; keyboard && keyboard.IsMap()) {
        for (const Action action : keyboardActions(scheme)) {
            std::string name;
            readScalar(keyboard[std::string(actionKey(action))], name);
            if (name.empty())
                continue;
            if (name == "none") {
                bindings.keys[index(action)] = SDL_SCANCODE_UNKNOWN;
                continue;
            }
            const SDL_Scancode scancode = SDL_GetScancodeFromName(name.c_str());
            if (scancode != SDL_SCANCODE_UNKNOWN)
                bindings.keys[index(action)] = scancode;
        }
    }
    if (const YAML::Node gamepad = node["gamepad"]; gamepad && gamepad.IsMap()) {
        for (const Action action : gamepadActions(scheme)) {
            std::string name;
            readScalar(gamepad[std::string(actionKey(action))], name);
            if (name.empty())
                continue;
            if (const std::optional<PadInput> input = padInputFromName(name))
                bindings.pad[index(action)] = *input;
        }
    }
}

void readPlayer(const YAML::Node &node, PlayerConfig &player) {
    if (!node || !node.IsMap())
        return;

    std::string text;
    readScalar(node["scheme"], text);
    for (std::size_t s = 0; s < kSchemeCount; ++s)
        if (text == schemeKey(static_cast<Scheme>(s)))
            player.scheme = static_cast<Scheme>(s);

    text.clear();
    readScalar(node["gamepad"], text);
    for (const GamepadChoice choice : {GamepadChoice::Auto, GamepadChoice::None, GamepadChoice::Device})
        if (text == gamepadChoiceKey(choice))
            player.gamepad = choice;
    readScalar(node["gamepad_guid"], player.gamepadGuid);
    readScalar(node["gamepad_name"], player.gamepadName);
    readScalar(node["gamepad_index"], player.gamepadIndex);
    if (player.gamepad == GamepadChoice::Device && player.gamepadGuid.empty())
        player.gamepad = GamepadChoice::Auto;
    if (player.gamepadIndex < 0)
        player.gamepadIndex = 0;

    readScalar(node["deadzone"], player.deadzonePercent);
    if (player.deadzonePercent < 0)
        player.deadzonePercent = 0;
    if (player.deadzonePercent > kMaximumDeadzonePercent)
        player.deadzonePercent = kMaximumDeadzonePercent;

    for (std::size_t s = 0; s < kSchemeCount; ++s) {
        const Scheme scheme = static_cast<Scheme>(s);
        readSchemeBindings(node[std::string(schemeKey(scheme))], scheme, player.schemeBindings(scheme));
    }
}

YAML::Node writePlayer(const PlayerConfig &player) {
    YAML::Node node;
    node["scheme"]        = std::string(schemeKey(player.scheme));
    node["gamepad"]       = std::string(gamepadChoiceKey(player.gamepad));
    node["gamepad_guid"]  = player.gamepadGuid;
    node["gamepad_name"]  = player.gamepadName;
    node["gamepad_index"] = player.gamepadIndex;
    node["deadzone"]      = player.deadzonePercent;
    for (std::size_t s = 0; s < kSchemeCount; ++s) {
        const Scheme          scheme   = static_cast<Scheme>(s);
        const SchemeBindings &bindings = player.schemeBindings(scheme);
        YAML::Node            schemeNode;
        for (const Action action : keyboardActions(scheme)) {
            const SDL_Scancode scancode = bindings.keys[index(action)];
            const char        *name     = scancode == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(scancode);
            schemeNode["keyboard"][std::string(actionKey(action))] = (name && *name) ? name : "none";
        }
        for (const Action action : gamepadActions(scheme))
            schemeNode["gamepad"][std::string(actionKey(action))] = padInputName(bindings.pad[index(action)]);
        node[std::string(schemeKey(scheme))] = schemeNode;
    }
    return node;
}

} // namespace

ActionMask schemeActions(Scheme scheme) {
    ActionMask mask = 0;
    for (const Action action : keyboardActions(scheme))
        mask = static_cast<ActionMask>(mask | actionBit(action));
    return mask;
}

std::span<const Action> keyboardActions(Scheme scheme) {
    if (scheme == Scheme::Enhanced6)
        return kEnhanced6Keyboard;
    return kOriginal3Keyboard;
}

std::span<const Action> gamepadActions(Scheme scheme) {
    if (scheme == Scheme::Enhanced6)
        return kEnhanced6Pad;
    return kOriginal3Pad;
}

std::string_view actionKey(Action action) {
    return kActionKeys[index(action)];
}

std::string_view schemeKey(Scheme scheme) {
    return scheme == Scheme::Enhanced6 ? "enhanced6" : "original3";
}

std::string padInputName(const PadInput &input) {
    switch (input.kind) {
        case PadInput::Kind::LeftTrigger:
            return "LeftTrigger";
        case PadInput::Kind::RightTrigger:
            return "RightTrigger";
        case PadInput::Kind::Button:
            for (const PadButtonName &entry : kPadButtonNames)
                if (entry.button == input.button)
                    return std::string(entry.name);
            break;
        case PadInput::Kind::None:
            break;
    }
    return "none";
}

std::optional<PadInput> padInputFromName(std::string_view name) {
    if (name == "none")
        return PadInput{};
    if (name == "LeftTrigger")
        return PadInput::leftTrigger();
    if (name == "RightTrigger")
        return PadInput::rightTrigger();
    for (const PadButtonName &entry : kPadButtonNames)
        if (entry.name == name)
            return PadInput::ofButton(entry.button);
    return std::nullopt;
}

ControlsConfig ControlsConfig::defaults() {
    return ControlsConfig{.players = {defaultPlayer(0), defaultPlayer(1)}};
}

std::filesystem::path defaultPath() {
    std::filesystem::path home;
    if (const char *folder = SDL_GetUserFolder(SDL_FOLDER_HOME))
        home = folder;
    else if (const char *env = std::getenv("HOME"))
        home = env;
    return home / ".config" / "StreetsOfRageProject" / "controls.yaml";
}

ControlsConfig loadOrCreate(const std::filesystem::path &path) {
    ControlsConfig config = ControlsConfig::defaults();

    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (!save(config, path))
            std::fprintf(stderr, "Could not create %s; using the default controls\n", path.string().c_str());
        return config;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception &exception) {
        std::fprintf(stderr,
                     "Could not read %s (%s); using the default controls and leaving the file alone\n",
                     path.string().c_str(),
                     exception.what());
        return config;
    }

    readPlayer(root["player1"], config.players[0]);
    readPlayer(root["player2"], config.players[1]);
    return config;
}

bool save(const ControlsConfig &config, const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    YAML::Node root;
    root["version"] = kFormatVersion;
    root["player1"] = writePlayer(config.players[0]);
    root["player2"] = writePlayer(config.players[1]);

    YAML::Emitter emitter;
    emitter << root;

    // Write next to the file and rename, so a crash never leaves half a file.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Streets of Rage controls.\n" << emitter.c_str() << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace SoRControlsConfig
