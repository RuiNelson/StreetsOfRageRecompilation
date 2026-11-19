#include "SoRVideoConfig.hpp"

#include "SoRControlsConfig.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

namespace SoRVideoConfig {
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

std::string_view frameSyncKey(VDP::Synchronization frameSync) {
    switch (frameSync) {
        case VDP::VSync:
            return "vsync";
        case VDP::VSync2:
            return "vsync-half";
        case VDP::VSync3:
            return "vsync-third";
        case VDP::InternalTimer:
            break;
    }
    return "auto";
}

std::optional<VDP::Synchronization> frameSyncFromKey(std::string_view key) {
    for (const VDP::Synchronization frameSync : {VDP::InternalTimer, VDP::VSync, VDP::VSync2, VDP::VSync3})
        if (frameSyncKey(frameSync) == key)
            return frameSync;
    return std::nullopt;
}

std::string_view scalingFilterKey(VDP::ScalingFilter filter) {
    switch (filter) {
        case VDP::Nearest:
            return "nearest";
        case VDP::Bilinear:
            return "bilinear";
        case VDP::MetalFX:
            return "metalfx";
        case VDP::Hybrid:
            break;
    }
    return "hybrid";
}

std::optional<VDP::ScalingFilter> scalingFilterFromKey(std::string_view key) {
    for (const VDP::ScalingFilter filter : {VDP::Nearest, VDP::Bilinear, VDP::Hybrid, VDP::MetalFX})
        if (scalingFilterKey(filter) == key)
            return filter;
    return std::nullopt;
}

std::string_view frameGenerationKey(VDP::FrameGeneration mode) {
    switch (mode) {
        case VDP::GenerateFrames:
            return "on";
        case VDP::GenerateHoldPoses:
            return "no-pose-blend";
        case VDP::NoFrameGeneration:
            break;
    }
    return "off";
}

std::optional<VDP::FrameGeneration> frameGenerationFromKey(std::string_view key) {
    for (const VDP::FrameGeneration mode : {VDP::NoFrameGeneration, VDP::GenerateFrames, VDP::GenerateHoldPoses})
        if (frameGenerationKey(mode) == key)
            return mode;
    return std::nullopt;
}

std::filesystem::path defaultPath() {
    return SoRControlsConfig::defaultPath().parent_path() / "video.yaml";
}

VideoConfig loadOrCreate(const std::filesystem::path &path) {
    VideoConfig config;

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
        std::fprintf(stderr, "Could not read %s (%s): using the default video settings\n", path.string().c_str(), exception.what());
        return config;
    }
    if (!root.IsMap())
        return config;

    if (const YAML::Node frameSync = root["frame_sync"]; frameSync && frameSync.IsScalar()) {
        if (const auto value = frameSyncFromKey(frameSync.Scalar()))
            config.frameSync = *value;
    }
    readBool(root["widescreen"], config.widescreen);
    readBool(root["fullscreen"], config.fullscreen);
    readBool(root["integer_scaling"], config.integerScaling);
    if (const YAML::Node mode = root["frame_generation"]; mode && mode.IsScalar()) {
        if (const auto value = frameGenerationFromKey(mode.Scalar()))
            config.frameGeneration = *value;
    }
    if (const YAML::Node filter = root["scaling_filter"]; filter && filter.IsScalar()) {
        if (const auto value = scalingFilterFromKey(filter.Scalar()))
            config.scalingFilter = *value;
    }
    return config;
}

bool save(const VideoConfig &config, const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    YAML::Node root;
    root["frame_sync"]      = std::string(frameSyncKey(config.frameSync));
    root["widescreen"]      = config.widescreen;
    root["fullscreen"]      = config.fullscreen;
    root["integer_scaling"] = config.integerScaling;
    root["scaling_filter"]  = std::string(scalingFilterKey(config.scalingFilter));
    root["frame_generation"] = std::string(frameGenerationKey(config.frameGeneration));

    YAML::Emitter emitter;
    emitter << root;

    // Write next to the file and rename, so a crash never leaves half a file.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Streets of Rage video settings (OPTIONS > VIDEO).\n" << emitter.c_str() << '\n';
        if (!out)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

} // namespace SoRVideoConfig
