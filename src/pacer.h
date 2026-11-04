#pragma once

#include <windows.h>

namespace rclan::pacer {

// Steady frame pacing for the menus. The game redraws its menu screens
// irregularly at ~20 fps. As a fullscreen window that makes compositors with
// adaptive sync (KWin "Automatic") swing the display's refresh rate, which
// flickers, and the cursor only moves at the game's frame rate. While active,
// a 1x1 child window in a corner of the main window is repainted (alternating
// between two almost identical dark colors) faster than the display refreshes,
// so a new frame is ready for every refresh. XWayland sends at most one frame
// per refresh, so the extra repaints cost next to nothing.

// Creates the pixel window and the pacing thread on first call (GUI thread),
// then positions it in the bottom-left corner of `client` (main client coords).
void place(HWND main, const RECT& client, bool active);

} // namespace rclan::pacer
