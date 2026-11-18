#pragma once

#include "SoRControlsConfig.hpp"
#include "SoRSurvival.hpp"

class SystemMemory;

// EXTRA (user): the mode menu's row for special modes, before QUIT. It is the OPTIONS screen with none of
// OPTIONS' rows: the mode menu goes there as it goes to OPTIONS (select_menu_resolve_choice), and while the
// menu is open game_start_screen_update runs it instead of the OPTIONS rows. Host UI like the CONTROLS menu,
// drawn with the same font, with its state in C++.
//
// Screens: EXTRA (SURVIVAL MODE, BACK); SURVIVAL MODE (PLAYERS, ROUND, START, BACK, and the records); the
// result of a survival game (OK), where the game comes back to after GAME OVER.
namespace SoRExtraMenu {

/// The mode menu chose EXTRA: the next OPTIONS screen build opens this menu instead.
void request();

/// A survival game is over: the mode menu's next init goes straight to EXTRA, on the result.
void showResult(const SoRSurvival::Result &result);

/// The mode menu's init: true when it is to go straight to EXTRA (showResult), as if EXTRA had been chosen.
bool resultPending();

/// Inside options_menu_build: true when EXTRA was chosen, and then the menu is open and drawn.
bool openIfRequested(SystemMemory &memory);

bool isOpen();

enum class Result { Open, Back, Start };
/// One menu frame; @p remote is the remote client's actions (both players). On Back or Start the menu has
/// cleared plane A and closed; the caller fades the screen out.
Result frame(SystemMemory &memory, SoRControlsConfig::ActionMask remote);

/// After Start, at the end of the fade (options_menu_return): starts the survival game chosen, once. False
/// when the menu was left with BACK, and the caller goes back to the mode menu.
bool takeStart(SystemMemory &memory);

} // namespace SoRExtraMenu
