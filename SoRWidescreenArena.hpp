#pragma once

#include "system/memory/SystemMemory.hpp"

// Widescreen additions to the picture that are not ports of a ROM routine (see SoRWidescreenArena.cpp).
namespace SoRWidescreenArena {

/// Right after build_sprite_attribute_table ($AE96), widescreen only: round 3's boss arena shows a third palm
/// trunk on Plane A in the right margin, whose top the ROM never draws; this appends the right palm's top
/// records (X $EB8) again, $60 to the right. Touches only the SAT buffer and its build counters.
void appendRound3PalmTop(SystemMemory &memory);

} // namespace SoRWidescreenArena
