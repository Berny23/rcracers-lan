#pragma once

namespace rclan::panel {

// In-game LAN panel: a custom-drawn child window of the game's main window,
// placed next to the 800x600 game screen (collapsible tab if there's no room).
void install();

// Debug automation (GUI thread): "host", "join ADDR", "start", "opt ROW DIR", "leave".
void command(const char* cmd);

} // namespace rclan::panel
