#pragma once

#include "data_types.hpp"

#include <filesystem>

class SystemMemory;

// Pre-rendered soundtrack: plays the FLAC renders of tools/soundtrack instead
// of emulating the YM2612, PSG and Z80 DAC in real time.
//
// The game's 68000 sound engine keeps running unchanged, so every work-RAM
// field ($FFF000-$FFF36F) evolves exactly as on hardware and game logic that
// reads it (the pause gate on stop_music) is unaffected; only the chip output
// is replaced. Three engine entries drive the host mixer:
//
// - $72D08 sound_process_queue: replicate the queue's priority pick, which is
//   what the engine is about to start;
// - $72F8A sound_reset_all: stop the music and the sound effects (not a PCM
//   voice, which the Z80 keeps streaming);
// - $73066 sound_fade_music, called right after the queue every engine tick:
//   start what was picked, follow the $E0 fade (the music's DAC+PSG stem stops
//   at once, its FM stem steps down) and the engine gate (paused, the music
//   plays on with its FM stem 6 dB down).
namespace SoRSoundtrack {

/// Opens the audio device and loads `directory/manifest.tsv`. Returns false
/// (and plays nothing) when the manifest or the mixer is unavailable.
bool open(const std::filesystem::path &directory);
void close();
bool isOpen();

/// CPU thread: called at each traced 68000 subroutine entry.
void onEntry(m_long entry, SystemMemory &memory);

} // namespace SoRSoundtrack
