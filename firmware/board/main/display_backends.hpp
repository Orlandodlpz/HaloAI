// Backend selection is one build flag, never ifdefs scattered through UI code.
// Set with: idf.py -DHALO_PANEL=crowpanel build   (or waveshare)
#pragma once
#include "halo/display.hpp"

namespace halo::board {
// Returns the single display backend this firmware was built with.
Display& display();
const char* panel_name();
}  // namespace halo::board
