#pragma once

namespace rclan::automation {

// Development aid ([Debug] Automation=1 in rclan.ini): polls
// <game>\rclan_cmd.txt, executes its lines, then deletes it.
//   click X Y       left click at game-screen coordinates (800x600 space)
//   clickmain X Y   left click at main-window client coordinates
//   move X Y        mouse move (game-screen coordinates)
//   key VK          press+release virtual key (hex, e.g. 0D = Enter)
//   log TEXT        write TEXT to rclan.log (sync marker)
void poll();
bool enabled();

} // namespace rclan::automation
