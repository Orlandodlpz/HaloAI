// Backend selection is one build flag, never ifdefs scattered through UI code.
// Set with: idf.py -DHALO_PANEL=crowpanel build   (or waveshare)
#pragma once
#include "halo/display.hpp"

namespace halo::board {
// Returns the single display backend this firmware was built with.
Display& display();
const char* panel_name();
// Drive every pixel between the extremes a few times to clear ghosting. Costs a
// full refresh per cycle, so it is for boot and for the WALLPAPER moment, not
// for ordinary screens.
void deep_clean(int cycles);

// Bisection aid: writes a pattern straight into the framebuffer, bypassing the
// rasteriser, then pushes and refreshes. If these show but drawn screens do not,
// the fault is above the framebuffer; if these do not show either, it is below.
//   0 all black   1 all white   2 left half   3 right half   4 top half
//   5 a black square at the origin
void debug_pattern(int which);
}  // namespace halo::board
