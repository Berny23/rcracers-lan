#pragma once

namespace rclan::hooks {

// Patches the game's IAT. Must run before the game's own code starts.
void install();

// Background thread body: hotkeys + periodic stats.
void worker_loop();

} // namespace rclan::hooks
