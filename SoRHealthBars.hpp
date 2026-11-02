#pragma once

#include "system/graphics/Framebuffer.hpp"
#include "system/memory/SystemMemory.hpp"

// `--qol bars`: a health bar over each regular enemy, in the VDP's host layer over
// the finished frame (VDP::setFrameOverlay), so nothing the console can see changes.
namespace SoRHealthBars {

/// CPU thread, when the frame's logic is done and its SAT has been uploaded: the
/// picture shows these positions until the next upload, so the bars stay on the sprites.
void capture(SystemMemory &memory);

/// Render thread: draws what capture() last saw.
void draw(Framebuffer &framebuffer, int width, int height);

} // namespace SoRHealthBars
