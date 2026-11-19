#pragma once

#include "SoRMenuCanvas.hpp"

#include <string>

class SystemMemory;

// The on-screen keyboard that sets a player's initials (up to three letters or spaces), shared by
// OPTIONS > QUALITY-OF-LIFE and EXTRA > SURVIVAL MODE: three rows of letters, then SPACE, DEL, OK and CANCEL.
namespace SoRInitialsKeyboard {

struct Keyboard {
    std::string title; // "PLAYER 1 INITIALS"
    std::string text;
    int         keyRow = 0;
    int         keyCol = 0;
};

/// A keyboard for @p title, starting with @p text.
Keyboard open(std::string title, std::string text);

enum class Result { Open, Ok, Cancel };
/// One frame on the menu's presses. Ok: the initials are in `text` (not normalized). Back is Cancel.
Result step(Keyboard &keyboard, SystemMemory &memory, bool up, bool down, bool left, bool right, bool confirm,
            bool back);

/// The keyboard over the whole canvas, with its two help lines.
void draw(const Keyboard &keyboard, SoRMenuCanvas::Canvas &canvas);

} // namespace SoRInitialsKeyboard
