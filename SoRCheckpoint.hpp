#pragma once

#include <filesystem>

class SystemMemory;

// Checkpoint saves (user request): SAVE AND QUIT in the pause menu writes `checkpoint.yaml`, next to
// controls.yaml, and CONTINUE on the mode menu loads it and deletes it (a save is used once).
//
// A checkpoint is not a copy of the game: it is the round, the wave gate last reached, the mode, the
// characters, and each player's score, lives, continues, specials and health when that gate was reached,
// plus where the camera and the enemy-load-cue stream were then. The ROM rebuilds the rest: the round
// loads as usual, and the level pipeline resumes at its own state $00 (init_level_state) for the next
// wave, as it does when a wave is cleared.
namespace SoRCheckpoint {

/// `checkpoint.yaml`, in controls.yaml's folder.
std::filesystem::path defaultPath();

/// A checkpoint file is there (read once, then kept up to date by this module).
bool available();

/// CPU thread, at each frame boundary (both vblank waits): keeps the last wave start of the game under
/// way, and finishes a restore in progress.
void onFrame(SystemMemory &memory);

/// The pause menu's SAVE AND QUIT: writes the last wave start of this game. False when there is none
/// or the file could not be written.
bool save();

/// The mode menu's CONTINUE: loads the file, deletes it, sets up the mode, characters and players, and
/// starts the round (game state $28). False (and nothing changed) when the file is unusable; it is
/// deleted then too.
bool beginContinue(SystemMemory &memory);

/// Inside `spawn_round_players`, after the round has loaded: true when a checkpoint inside the round is
/// being restored, and then it has put back the camera and the pipeline (the caller redraws the planes).
bool restoreInsideRound(SystemMemory &memory);

} // namespace SoRCheckpoint
