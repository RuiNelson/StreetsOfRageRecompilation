#pragma once

#include "data_types.hpp"
#include "system/graphics/Framebuffer.hpp"
#include "system/memory/SystemMemory.hpp"

// EXTRA > GTA 6 (user, 2026-11-19): a meme mode. The campaign for one player, rounds, bosses and Mr. X as in the
// ROM, with P1 replaced by the police car of the special: an enemy or a boss it touches dies at once. It drives
// at three times Blaze's walking speed, cannot pick up food or weapons, and takes damage and loses lives as a
// player does. Its score keeps no high score.
//
// P1 stays the ROM's player object, so its hits, falls, deaths and continues are the ROM's. The car is drawn in
// the VDP's host layer (VDP::setFrameOverlay), from the special's own art, mapping and palette decoded from the
// ROM, and P1's sprite and shadow are left out of the sprite list. A touch is a strike through the ROM's contact
// test: while the car drives, P1's attack box (+$64) is the car and its damage (+$34) more than any health.
namespace SoRGta6 {

/// The EXTRA menu's GTA 6: one player, through character select, from the round OPTIONS has (as 1 PLAYER).
void begin(SystemMemory &memory);

/// A GTA 6 game is being played (from begin until the game leaves gameplay).
bool active();

/// CPU thread, at each frame boundary: the mode ends when the game is back at the title or the high scores.
void onFrame(SystemMemory &memory);

/// @p object is P1 as the car (a type-1 player in this mode).
bool isCar(SystemMemory &memory, m_long object);

/// @p object is the car on the road (standing or driving, alive): a touch kills.
bool carStrikes(SystemMemory &memory, m_long object);

/// What the car's touch deals (+$34), more than any enemy's or boss's health, and its reaction (+$42).
inline constexpr m_byte kStrikeDamage   = 0x7Fu;
inline constexpr m_byte kStrikeReaction = 0x01u; // knocked down

/// P1's attack box (+$64-$6F: X, lane and height pairs) as the car's body.
void writeStrikeBox(SystemMemory &memory, m_long object);

/// P1's sprite and P1's ground shadow are not shown: the car is.
bool hidesObject(SystemMemory &memory, m_long object);

/// The buttons the car has not got (punch and jump, so no attack, jump or pickup), from P1's input word.
inline constexpr m_word kMaskedButtons = 0x3030u;

/// CPU thread, when the frame's SAT has just gone up (as SoRDamageCircles): where the car is.
void capture(SystemMemory &memory);

/// Render thread: draws the car capture() last saw.
void draw(Framebuffer &framebuffer, int width, int height);

} // namespace SoRGta6
