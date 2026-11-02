#pragma once

#include <cstdint>
#include <filesystem>

class SystemMemory;

// `--qol scores`: the ROM's ten-entry high-score table (`$FFFF4A (top10_score_table)`, twelve bytes each) is
// kept in `highscores.yaml`, next to controls.yaml. The table stays in 68000 work RAM as the ROM has it: this
// only copies it in at power-on and out when it changes.
namespace SoRHighScores {

/// `highscores.yaml`, in controls.yaml's folder.
std::filesystem::path defaultPath();

/// Copies the file over the table. A missing or unreadable file leaves the ROM's defaults. Either way the
/// table as it is now counts as saved.
void load(SystemMemory &memory);

/// Writes the file if the table differs from what was last loaded or saved. The two "new entry" flag bits are
/// not saved. Unless @p force, it waits while a player is typing a name, so the file never holds a half entry.
void saveIfChanged(SystemMemory &memory, bool force = false);

/// The type-$0F object of a dead player (@p object: `$FFB800` or `$FFB880`) is about to start the ROM's
/// name entry. If that player has initials (QUALITY-OF-LIFE, or "AI " for player 2 under AI Assist), enters
/// them as the ROM does when the name is done and returns true: the entry is in the table, sorted, and the
/// screen never appears. False when the ROM's name entry should run.
bool enterInitials(SystemMemory &memory, std::uint32_t object);

/// Removes the file (OPTIONS > QUALITY-OF-LIFE saved with the option off).
void erase();

} // namespace SoRHighScores
