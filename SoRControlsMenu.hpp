#pragma once

#include "data_types.hpp"

#include <string_view>

class SystemMemory;
struct PlayersControlState;

// OPTIONS > CONTROLS: the in-game controls menu.
//
// It replaces the ROM's CONTROL row (the A/B/C layouts). The row itself
// stays OPTIONS sub-state $18/$1A; confirming it opens this menu, which runs
// one frame per call of `$1390 (options_input_controls)` while it is open.
// The menu is host UI: its state lives in C++, not in work RAM (no game
// field changes while it is open, and autoplay never reads it). It draws
// with the OPTIONS screen's own font into plane A, white (palette line 0)
// for the selected entry and grey (line 1) for the others, as the ROM does,
// and changes the controls through SoRInput (saved to controls.yaml).
namespace SoRControlsMenu {

/// Draws @p text at plane A cell (@p column, @p row) with the menu font.
/// Characters the font lacks are drawn as spaces.
void drawText(SystemMemory &memory, int column, int row, std::string_view text, bool highlighted);

/// The OPTIONS screen's CONTROLS row: its label and a summary of each
/// player's scheme.
void drawOptionsRow(SystemMemory &memory, bool highlighted);

void open();
bool isOpen();

enum class Result { Open, Closed };
/// One menu frame. On Closed the menu has cleared plane A and the caller
/// redraws the OPTIONS screen.
Result frame(SystemMemory &memory, const PlayersControlState &remote);

} // namespace SoRControlsMenu
