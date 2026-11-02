#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// The options of OPTIONS > QUALITY-OF-LIFE, in `qol.yaml` next to controls.yaml:
// changes to the original game, all off by default. `--qol` turns them on for
// the run it is given on.
namespace SoRQolConfig {

struct QolConfig {
    bool rollCameraEarlier     = false; // --qol cam
    bool friendGrabKeepsWeapon = false; // --qol grab
    bool enemyDamageCircles    = false; // --qol damage
    bool persistHighScores     = false; // --qol scores
    bool cheatEnabled          = false; // --qol cheat

    // Initials for the high-score table, up to three letters or spaces; empty: the player types them.
    // Menu and file only, no command-line option. Player 2 under AI Assist is always "AI ".
    std::string initialsP1;
    std::string initialsP2;

    bool operator==(const QolConfig &) const = default;
};

/// @p text as initials: upper case, letters and spaces only, at most three; empty when there is no letter.
std::string normalizeInitials(std::string_view text);

/// Turns on the option `--qol` calls @p name. False if there is none.
bool enable(QolConfig &config, std::string_view name);

/// What the game runs with now: host state, outside the 68000's memory.
QolConfig &current();

/// `qol.yaml`, in controls.yaml's folder.
std::filesystem::path defaultPath();

/// Reads @p path. A missing file is created with the defaults; an entry that
/// is missing or unreadable keeps its default. A file that cannot be parsed is
/// left alone (and reported) and the defaults are used.
QolConfig loadOrCreate(const std::filesystem::path &path);

/// Writes @p config to @p path, creating the directory. False on failure.
bool save(const QolConfig &config, const std::filesystem::path &path);

} // namespace SoRQolConfig
