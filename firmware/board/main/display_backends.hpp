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
}  // namespace halo::board
