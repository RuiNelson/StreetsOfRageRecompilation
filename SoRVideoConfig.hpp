#pragma once

#include "system/graphics/VDP.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

// The video settings of OPTIONS > VIDEO, in `video.yaml` next to controls.yaml.
// The command line's options win over the file for the run they are given on.
namespace SoRVideoConfig {

// Defaults (user, 2026-11-19): on macOS the picture fills the window with HYBRID; on Windows, Linux and
// the PS5, whole multiples with NEAREST.
#ifdef __APPLE__
inline constexpr bool               kDefaultIntegerScaling = false;
inline constexpr VDP::ScalingFilter kDefaultScalingFilter  = VDP::Hybrid;
#else
inline constexpr bool               kDefaultIntegerScaling = true;
inline constexpr VDP::ScalingFilter kDefaultScalingFilter  = VDP::Nearest;
#endif

struct VideoConfig {
    VDP::Synchronization frameSync      = VDP::InternalTimer;
    bool                 widescreen     = false;
    bool                 fullscreen     = false;
    bool                 integerScaling = kDefaultIntegerScaling;
    VDP::ScalingFilter   scalingFilter  = kDefaultScalingFilter;
    bool                 frameGeneration = false; // MetalFX frame interpolation, macOS 26

    bool operator==(const VideoConfig &) const = default;
};

/// `auto`, `vsync`, `vsync-half`, `vsync-third`: the --frame-sync names.
std::string_view                    frameSyncKey(VDP::Synchronization frameSync);
std::optional<VDP::Synchronization> frameSyncFromKey(std::string_view key);

/// `nearest`, `bilinear`, `hybrid`, `metalfx`: the --scaling-filter names.
std::string_view                  scalingFilterKey(VDP::ScalingFilter filter);
std::optional<VDP::ScalingFilter> scalingFilterFromKey(std::string_view key);

/// `video.yaml`, in controls.yaml's folder.
std::filesystem::path defaultPath();

/// Reads @p path. A missing file is created with the defaults; an entry that
/// is missing or unreadable keeps its default. A file that cannot be parsed is
/// left alone (and reported) and the defaults are used.
VideoConfig loadOrCreate(const std::filesystem::path &path);

/// Writes @p config to @p path, creating the directory. False on failure.
bool save(const VideoConfig &config, const std::filesystem::path &path);

} // namespace SoRVideoConfig
