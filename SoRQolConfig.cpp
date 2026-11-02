#include "SoRQolConfig.hpp"

#include "SoRControlsConfig.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

namespace SoRQolConfig {
namespace {

void readBool(const YAML::Node &node, bool &value) {
    if (!node)
        return;
    try {
        value = node.as<bool>();
    } catch (const YAML::Exception &) {
    }
}

void readInitials(const YAML::Node &node, std::string &value) {
    if (!node)
        return;
    try {
        value = normalizeInitials(node.as<std::string>());
    } catch (const YAML::Exception &) {
    }
}

} // namespace

std::string normalizeInitials(std::string_view text) {
    std::string result;
    for (const char c : text) {
        if (result.size() == 3)
            break;
        if (c >= 'a' && c <= 'z')
            result += static_cast<char>(c - 'a' + 'A');
        else if ((c >= 'A' && c <= 'Z') || c == ' ')
            result += c;
    }
    if (result.find_first_not_of(' ') == std::string::npos)
        result.clear();
    return result;
}

bool enable(QolConfig &config, std::string_view name) {
    if (name == "cam") {
        config.rollCameraEarlier = true;
        return true;
    }
    if (name == "grab") {
        config.friendGrabKeepsWeapon = true;
        return true;
    }
    if (name == "damage") {
        config.enemyDamageCircles = true;
        return true;
    }
    if (name == "cheat") {
        config.cheatEnabled = true;
        return true;
    }
    if (name == "scores") {
        config.persistHighScores = true;
        return true;
    }
    return false;
}

QolConfig &current() {
    static QolConfig config;
    return config;
}

std::filesystem::path defaultPath() {
    return SoRControlsConfig::defaultPath().parent_path() / "qol.yaml";
}

QolConfig loadOrCreate(const std::filesystem::path &path) {
    QolConfig config;

    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (!save(config, path))
            std::fprintf(stderr, "Could not write %s\n", path.string().c_str());
        return config;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception &exception) {
        std::fprintf(stderr, "Could not read %s (%s): using the default quality-of-life settings\n", path.string().c_str(), exception.what());
        return config;
    }
    if (!root.IsMap())
        return config;

    readBool(root["cam"], config.rollCameraEarlier);
    readBool(root["grab"], config.friendGrabKeepsWeapon);
    readBool(root["damage"], config.enemyDamageCircles);
    readBool(root["scores"], config.persistHighScores);
    readBool(root["cheat"], config.cheatEnabled);
    readInitials(root["initials1"], config.initialsP1);
    readInitials(root["initials2"], config.initialsP2);
    return config;
}

bool save(const QolConfig &config, const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    YAML::Node root;
    root["cam"]    = config.rollCameraEarlier;
    root["grab"]   = config.friendGrabKeepsWeapon;
    root["damage"] = config.enemyDamageCircles;
    root["scores"] = config.persistHighScores;
    root["cheat"]  = config.cheatEnabled;
    root["initials1"] = config.initialsP1;
    root["initials2"] = config.initialsP2;

    YAML::Emitter emitter;
    emitter << root;

    // Write next to the file and rename, so a crash never leaves half a file.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Streets of Rage quality-of-life settings (OPTIONS > QUALITY-OF-LIFE).\n" << emitter.c_str() << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace SoRQolConfig
