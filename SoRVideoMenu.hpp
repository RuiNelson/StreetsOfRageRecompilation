#pragma once

#include "SoRControlsConfig.hpp"
#include "SoRVideoConfig.hpp"

#include <optional>

class SystemMemory;

// OPTIONS > VIDEO: the frame sync, the picture, window or fullscreen and integer
// scaling, kept in video.yaml. Nothing changes until SAVE AND APPLY.
//
// A row the ROM does not have, after CONTROLS: OPTIONS sub-states $2E/$30, past
// the end of the ROM's table, handled by the CONTROLS row's own routines
// (options_highlight_controls, options_input_controls). Like the CONTROLS menu
// it is host UI, drawn with the same font and with its state in C++.
namespace SoRVideoMenu {

/// The OPTIONS screen's VIDEO row: its label and a summary of @p current.
void drawOptionsRow(SystemMemory &memory, const SoRVideoConfig::VideoConfig &current, bool highlighted);

/// @p refreshHz is the display's, 0 when unknown; @p normalHz the game's normal speed.
void open(const SoRVideoConfig::VideoConfig &current, int refreshHz, int normalHz);
bool isOpen();

enum class Result { Open, Closed };
/// One menu frame; @p remote is the remote client's actions (both players).
/// On Closed the menu has cleared plane A and the caller redraws the OPTIONS
/// screen.
Result frame(SystemMemory &memory, SoRControlsConfig::ActionMask remote);

/// After Closed: what SAVE AND APPLY chose (already saved), once. Empty after CANCEL.
std::optional<SoRVideoConfig::VideoConfig> takeChosen();

} // namespace SoRVideoMenu
