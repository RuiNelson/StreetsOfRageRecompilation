#pragma once

#include "system/graphics/Framebuffer.hpp"
#include "system/memory/SystemMemory.hpp"

// `--qol damage`: a circle over each regular enemy that has taken damage, in the VDP's host layer over the
// finished frame (VDP::setFrameOverlay), so nothing the console can see changes. A disc grows inside a ring and
// fills it when the enemy is dead.
namespace SoRDamageCircles {

/// CPU thread, when the frame's logic is done and its SAT has been uploaded: the
/// picture shows these positions until the next upload, so the circles stay on the sprites.
void capture(SystemMemory &memory);

/// Render thread: draws what capture() last saw.
void draw(Framebuffer &framebuffer, int width, int height);

} // namespace SoRDamageCircles
