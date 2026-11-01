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

} // namespace

bool enable(QolConfig &config, std::string_view name) {
    if (name == "cam") {
        config.rollCameraEarlier = true;
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
    return config;
}

bool save(const QolConfig &config, const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    YAML::Node root;
    root["cam"] = config.rollCameraEarlier;

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
