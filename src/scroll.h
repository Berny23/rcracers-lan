#pragma once

#include <windows.h>

namespace rclan::scroll {

// Mouse-wheel support for the game's scroll lists (track selection, records,
// player list, ...). The game ignores WM_MOUSEWHEEL; we move the matching
// MyScrollBar (found via RTTI) the way its arrow buttons do.
// Called for every message the game's loop removes; returns true if handled.
bool on_message(const MSG* m);

} // namespace rclan::scroll
