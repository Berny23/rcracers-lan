#pragma once

#include <windows.h>

namespace rclan::menubar {

// The game's menu (Datei, Rennoptionen, ...) drawn by us as a child window of
// the main window, in the style of the LAN panel, instead of Wine's native
// menu bar. Native menus open separate X11 popup windows and grab the
// pointer; under KWin/XWayland that left the game without mouse clicks after
// using a menu, and gamescope flashed the popups.
//
// The game's HMENU stays the source of truth: it is re-read every time a menu
// opens (after WM_INITMENU / WM_INITMENUPOPUP to the game, which updates the
// check marks), and selections are posted as WM_COMMAND to `target`, the
// game's original menu-bar window.

constexpr int kHeight = 26;

// Creates the bar on first call (GUI thread).
void attach(HWND main, HMENU menu, HWND target);

// Called from the layout code: bar width and whether menus are available.
void place(int width, bool visible);

bool is_open();
void close();
void refresh(); // the game changed its menu (DrawMenuBar)

// Keyboard handling, called for every message the game's loop retrieves.
// Returns true if the message belongs to the menu and must be hidden from the
// game. `removed` is false for PM_NOREMOVE peeks (then nothing is acted on).
bool key_filter(const MSG* m, bool removed);

} // namespace rclan::menubar
