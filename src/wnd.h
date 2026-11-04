#pragma once

#include <windows.h>

namespace rclan::wnd {

// IAT hooks on window management + DirectDraw setup.
//
// Window layout fix: the game fakes a full-screen app with
//   - a maximized captioned main window (logo backdrop) pushed to -4,-30,
//   - a child window holding the 800x600 game screen, positioned for a
//     full-screen parent,
//   - a separate popup window that only exists to show the menu bar.
// Tiling window managers tile the main window and float the menu bar. We
// turn the main window into a borderless full-screen popup, move the menu bar
// into it, and keep the game screen centered in the real client area.
void install();

HWND main_window();
HWND content_window();

// The game's menu (moved from its menu-bar window), for language detection.
HMENU game_menu();

// Called (on the GUI thread) whenever the layout changes, so overlays can
// reposition themselves. `content` is in main-window client coordinates.
// `menus` is false while a race owns the screen (DirectDraw exclusive mode).
using LayoutCallback = void (*)(HWND main, const RECT& client, const RECT& content, bool menus);
void set_layout_callback(LayoutCallback cb);

// Diagnostics, called from the worker thread: logs changes of the GUI
// thread's input state (focus, capture, menu mode, window under the cursor).
void poll_state();

// Runs fn(arg) on the game's GUI thread, from its message loop (the same
// context as menu commands and button clicks). False if there's no window yet.
bool run_on_gui(void (*fn)(void*), void* arg);

} // namespace rclan::wnd
