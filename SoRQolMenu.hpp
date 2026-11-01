#pragma once

#include "SoRControlsConfig.hpp"
#include "SoRQolConfig.hpp"

class SystemMemory;

// OPTIONS > QUALITY-OF-LIFE: the changes to the original game, kept in qol.yaml.
// Nothing changes until SAVE AND APPLY.
//
// A row the ROM does not have, after VIDEO: OPTIONS sub-states $32/$34, past the
// end of the ROM's table, handled by the CONTROLS row's own routines
// (options_highlight_controls, options_input_controls). Like the VIDEO menu it
// is host UI, drawn with the same font and with its state in C++.
namespace SoRQolMenu {

/// The OPTIONS screen's QUALITY-OF-LIFE row.
void drawOptionsRow(SystemMemory &memory, bool highlighted);

/// The OPTIONS screen's EXIT row, one row below the ROM's to make room for this one.
void drawExitRow(SystemMemory &memory, bool highlighted);

void open();
bool isOpen();

enum class Result { Open, Closed };
/// One menu frame; @p remote is the remote client's actions (both players).
/// On Closed the menu has cleared plane A and the caller redraws the OPTIONS
/// screen. SAVE AND APPLY has by then saved the settings and made them current.
Result frame(SystemMemory &memory, SoRControlsConfig::ActionMask remote);

} // namespace SoRQolMenu
