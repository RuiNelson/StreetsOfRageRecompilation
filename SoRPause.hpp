#pragma once

#include "data_types.hpp"
#include "system/graphics/Framebuffer.hpp"

#include <span>

class SystemMemory;

// The pause menu (user request; the ROM's pause is only the word PAUSE, which stays).
//
// While `$FFFA46 (pause_text_flag)` is set, handle_pause_start_input runs the
// menu: RESUME and FORFEIT, which each player concerned confirms with NO and YES.
// Its state is host UI, in C++: no work-RAM field changes for it, and what it
// does to the game goes through the ROM's own fields and routines. It is drawn over
// the finished frame (VDP::setFrameOverlay), since gameplay leaves no VRAM to spare,
// with the mode menu's font, kept from when that screen loads it.
namespace SoRPause {

/// On the mode menu's init, with its art in VRAM: keeps the font, and starts a game with nothing pending.
void onModeMenu(std::span<const m_byte> vram);

/// The frame overlay: draws the menu while the game is paused.
void drawOverlay(SystemMemory &memory, Framebuffer &framebuffer, int width, int height);

} // namespace SoRPause
