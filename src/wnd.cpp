// Window management: layout fix (see wnd.h) + logging, DirectDraw recon.

#include "wnd.h"

#include "config.h"
#include "iat.h"
#include "log.h"
#include "menubar.h"
#include "pacer.h"
#include "vtable.h"

#include <cstdio>
#include <cstring>

namespace rclan::wnd {

#define CALLER() __builtin_return_address(0)

static void describe(HWND h, char* out, size_t n)
{
    char cls[64] = "?", title[64] = "";
    if (h) {
        GetClassNameA(h, cls, sizeof(cls));
        GetWindowTextA(h, title, sizeof(title));
    }
    snprintf(out, n, "%p[%s \"%s\"]", (void*)h, cls, title);
}

static const char* class_name(LPCSTR c, char* buf, size_t n)
{
    if (!c) return "(null)";
    if (!HIWORD((UINT_PTR)c)) { snprintf(buf, n, "#%u", LOWORD((UINT_PTR)c)); return buf; }
    return c;
}

// ---------------------------------------------------------------------------
// Originals

using RegisterClassA_t  = ATOM(WINAPI*)(const WNDCLASSA*);
using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using ShowWindow_t      = BOOL(WINAPI*)(HWND, int);
using SetWindowPos_t    = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using MoveWindow_t      = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
using SetWindowLongA_t  = LONG(WINAPI*)(HWND, int, LONG);
using GetWindowLongA_t  = LONG(WINAPI*)(HWND, int);
using DestroyWindow_t   = BOOL(WINAPI*)(HWND);
using GetMenu_t         = HMENU(WINAPI*)(HWND);
using DrawMenuBar_t     = BOOL(WINAPI*)(HWND);
using DirectDrawCreate_t = HRESULT(WINAPI*)(GUID*, void**, void*);
using SetCapture_t      = HWND(WINAPI*)(HWND);
using ReleaseCapture_t  = BOOL(WINAPI*)();
using EnableWindow_t    = BOOL(WINAPI*)(HWND, BOOL);
using SetFocus_t        = HWND(WINAPI*)(HWND);

static RegisterClassA_t  o_RegisterClassA;
static CreateWindowExA_t o_CreateWindowExA;
static ShowWindow_t      o_ShowWindow;
static SetWindowPos_t    o_SetWindowPos;
static MoveWindow_t      o_MoveWindow;
static SetWindowLongA_t  o_SetWindowLongA;
static GetWindowLongA_t  o_GetWindowLongA;
static DestroyWindow_t   o_DestroyWindow;
static GetMenu_t         o_GetMenu;
static DrawMenuBar_t     o_DrawMenuBar;
static SetCapture_t      o_SetCapture;
static ReleaseCapture_t  o_ReleaseCapture;
static EnableWindow_t    o_EnableWindow;
static SetFocus_t        o_SetFocus;
static DirectDrawCreate_t o_DirectDrawCreate;

// ---------------------------------------------------------------------------
// Layout state (GUI thread only)

static bool    g_fix = true;        // [Window] FixLayout
static bool    g_exclusive;         // DirectDraw exclusive mode (races): hands off
static HWND    g_main, g_content, g_menubar;
static HMENU   g_menu;              // menu taken from the menu-bar popup
static bool    g_menu_shown;        // game wants the menu bar visible
static bool    g_native_menu;       // [Window] NativeMenuBar: Wine's menu bar instead of ours
static WNDPROC g_inner_proc;        // game's current main-window proc
static LayoutCallback g_layout_cb;
static bool    g_scale_race = true; // [Window] ScaleRace: race view fills the screen
static bool    g_race_view;         // the game shows a race in the content window
static RECT    g_race_rect;         // where race frames are drawn (main client coordinates)
static WNDPROC g_content_proc;      // game's proc of the content window (we subclass it)

// The game's window procedure keeps the content window where it wants it
// (WM_WINDOWPOSCHANGING); in race view it stays parked.
static LRESULT CALLBACK content_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    LRESULT r = CallWindowProcA(g_content_proc, h, msg, w, l);
    if (msg == WM_WINDOWPOSCHANGING && g_race_view && !IsRectEmpty(&g_race_rect)) {
        auto* wp = reinterpret_cast<WINDOWPOS*>(l);
        if (!(wp->flags & SWP_NOMOVE)) { // parked outside the main window's client area
            RECT c;
            GetClientRect(g_main, &c);
            wp->x = c.right, wp->y = 0;
        }
    }
    return r;
}

static constexpr DWORD kFrameStyles =
    WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

// Manage the main window's geometry? Not while DirectDraw owns the screen.
static bool geom() { return g_fix && !g_exclusive; }

HWND main_window() { return g_main; }
HWND content_window() { return g_content; }
HMENU game_menu() { return g_menu; }
void set_layout_callback(LayoutCallback cb) { g_layout_cb = cb; }

static RECT monitor_rect(HWND h)
{
    MONITORINFO mi = {sizeof(mi), {}, {}, 0};
    GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

// Main window covers the whole monitor -> Wine flags it _NET_WM_STATE_FULLSCREEN
// and tiling WMs leave it alone.
static bool covers(HWND h, const RECT& m)
{
    RECT r;
    return GetWindowRect(h, &r) && EqualRect(&r, &m);
}

static void fit_main()
{
    RECT m = monitor_rect(g_main);
    LOG("wnd: fit main window to monitor %ld,%ld %ldx%ld", m.left, m.top, m.right - m.left, m.bottom - m.top);
    o_SetWindowPos(g_main, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// Center the game screen in the main window's real client area.
static void layout()
{
    if (!g_main) return;
    RECT client;
    GetClientRect(g_main, &client);
    bool menus = g_menu_shown && !g_exclusive;
    if (!g_native_menu && g_menu && g_menubar) {
        menubar::attach(g_main, g_menu, g_menubar);
        menubar::place(client.right, menus);
        if (menus) client.top = menubar::kHeight; // our bar sits in the client area
    }
    pacer::place(g_main, client, !g_exclusive && IsWindowVisible(g_main));
    RECT content = {0, 0, 0, 0};
    if (g_content && g_race_view) {
        // Race: the game keeps drawing into its own window at its own size,
        // parked just outside the visible area; every frame is stretched onto
        // the main window instead (h_StretchBlt), as large as fits at 4:3.
        int cw = client.right, ch = client.bottom - client.top;
        int w = cw, h = cw * 3 / 4;
        if (h > ch) h = ch, w = ch * 4 / 3;
        int x = (cw - w) / 2, y = client.top + (ch - h) / 2;
        g_race_rect = {x, y, x + w, y + h};
        RECT wr;
        GetWindowRect(g_content, &wr);
        POINT cur = {wr.left, wr.top};
        ScreenToClient(g_main, &cur);
        if (cur.x != client.right || cur.y != 0) {
            LOG("wnd: race view at %d,%d %dx%d", x, y, w, h);
            o_SetWindowPos(g_content, nullptr, client.right, 0, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            InvalidateRect(g_main, nullptr, FALSE);
        }
        content = g_race_rect;
    } else if (g_content) {
        RECT wr;
        GetWindowRect(g_content, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        int x = (client.right - w) / 2, y = client.top + (client.bottom - client.top - h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        POINT cur = {wr.left, wr.top};
        ScreenToClient(g_main, &cur);
        if (cur.x != x || cur.y != y)
            o_SetWindowPos(g_content, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        content = {x, y, x + w, y + h};
    }
    if (g_layout_cb) g_layout_cb(g_main, client, content, menus);
}

static void show_menu(bool on)
{
    g_menu_shown = on;
    if (!g_main || !g_menu) return;
    if (!g_native_menu) {
        LOG("wnd: menu bar %s", on ? "shown" : "hidden");
        layout();
        return;
    }
    if ((GetMenu(g_main) != nullptr) == on) return;
    SetMenu(g_main, on ? g_menu : nullptr);
    // Wine doesn't always recompute the non-client area after SetMenu (seen
    // under gamescope), leaving the bar undrawn and the client size unchanged.
    o_SetWindowPos(g_main, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    DrawMenuBar(g_main);
    RECT cr;
    GetClientRect(g_main, &cr);
    LOG("wnd: menu bar %s main window (client %ldx%ld)", on ? "attached to" : "detached from",
        cr.right, cr.bottom);
    layout();
}

// Is `id` a command of the moved menu (vs. some other WM_COMMAND source)?
static bool is_menu_command(WPARAM w, LPARAM l)
{
    return g_menu && l == 0 && HIWORD(w) == 0 &&
           GetMenuState(g_menu, LOWORD(w), MF_BYCOMMAND) != (UINT)-1;
}

// Our subclass of the main window. Stays installed even when the game swaps
// its own window proc (see h_SetWindowLongA).
static const UINT kRunMsg = WM_APP + 0x3c1; // run_on_gui()

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kRunMsg && w) {
        reinterpret_cast<void (*)(void*)>(w)(reinterpret_cast<void*>(l));
        return 0;
    }
    if (g_menubar) {
        switch (m) {
        case WM_COMMAND:
            if (is_menu_command(w, l)) return SendMessageA(g_menubar, m, w, l);
            break;
        case WM_INITMENU:
        case WM_INITMENUPOPUP:
        case WM_UNINITMENUPOPUP:
        case WM_MENUSELECT:
        case WM_ENTERMENULOOP:
        case WM_EXITMENULOOP:
            SendMessageA(g_menubar, m, w, l);
            break;
        }
    }
    if (m == WM_NCCALCSIZE && l && g_menu && GetMenu(h) == g_menu) {
        // Some Wine setups (seen under gamescope) reserve no space for the
        // menu bar of a window that covers the whole screen, so the bar is
        // never drawn. Reserve it ourselves if nothing did.
        RECT* rc = reinterpret_cast<RECT*>(l); // rgrc[0] for wParam TRUE too
        LONG top = rc->top;
        LRESULT r = CallWindowProcA(g_inner_proc, h, m, w, l);
        if (rc->top == top) {
            rc->top += GetSystemMetrics(SM_CYMENU) + 1;
            static bool logged;
            if (!logged) LOG("wnd: reserving menu bar space ourselves (%d px)", (int)(rc->top - top));
            logged = true;
        }
        return r;
    }
    switch (m) {
    case WM_ENTERMENULOOP: LOG("wnd: main WM_ENTERMENULOOP"); break;
    case WM_EXITMENULOOP:  LOG("wnd: main WM_EXITMENULOOP"); break;
    case WM_CAPTURECHANGED: LOG("wnd: main WM_CAPTURECHANGED -> %p", (void*)l); break;
    case WM_CANCELMODE:    LOG("wnd: main WM_CANCELMODE"); break;
    case WM_ACTIVATE:      LOG("wnd: main WM_ACTIVATE %u", LOWORD(w)); break;
    case WM_ACTIVATEAPP:   LOG("wnd: main WM_ACTIVATEAPP %u", (unsigned)w); break;
    case WM_NCLBUTTONDOWN: LOG("wnd: main WM_NCLBUTTONDOWN hit=%u", (unsigned)w); break;
    case WM_LBUTTONDOWN:   LOG("wnd: main WM_LBUTTONDOWN %d,%d", (short)LOWORD(l), (short)HIWORD(l)); break;
    case WM_LBUTTONUP:     LOG("wnd: main WM_LBUTTONUP %d,%d", (short)LOWORD(l), (short)HIWORD(l)); break;
    }
    LRESULT r = CallWindowProcA(g_inner_proc, h, m, w, l);
    if (m == WM_WINDOWPOSCHANGING && geom()) {
        // The game re-computes its own geometry here (client area below where
        // its separate menu-bar window used to be). Always cover the monitor.
        auto* wp = reinterpret_cast<WINDOWPOS*>(l);
        if ((wp->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
            RECT mr = monitor_rect(h);
            wp->x = mr.left;
            wp->y = mr.top;
            wp->cx = mr.right - mr.left;
            wp->cy = mr.bottom - mr.top;
            wp->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
    }
    if (m == WM_DISPLAYCHANGE) {
        LOG("wnd: WM_DISPLAYCHANGE %ux%u, %ubpp", LOWORD(l), HIWORD(l), (unsigned)w);
        if (geom()) fit_main();
    }
    if (m == WM_WINDOWPOSCHANGED) {
        RECT wr, cr;
        GetWindowRect(h, &wr);
        GetClientRect(h, &cr);
        LOG("wnd: main window now %ld,%ld %ldx%ld (client %ldx%ld)", wr.left, wr.top,
            wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom);
    }
    if ((m == WM_SIZE || m == WM_WINDOWPOSCHANGED) && geom()) layout();
    return r;
}

// ---------------------------------------------------------------------------
// USER32 hooks

static ATOM WINAPI h_RegisterClassA(const WNDCLASSA* wc)
{
    ATOM a = o_RegisterClassA(wc);
    char b[16];
    LOG("RegisterClassA(\"%s\", style=%#x, wndproc=%p, menu=%s) -> %#x  [caller %p]",
        class_name(wc->lpszClassName, b, sizeof(b)), wc->style, (void*)wc->lpfnWndProc,
        wc->lpszMenuName ? "yes" : "no", a, CALLER());
    return a;
}

// Hover descriptions (tooltips) of the menus are small popups owned by the main
// window. As separate top-level windows they get their own X11/Wayland
// surface: gamescope flashes them fullscreen and other compositors may hand
// them focus. Make them children of the main window instead.
static WNDPROC g_tip_proc; // the game's tooltip window proc (one class)

static LRESULT CALLBACK tip_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT; // clicks go to the window below
    return CallWindowProcA(g_tip_proc, h, m, w, l);
}

static bool is_tooltip(HWND parent, DWORD style, HMENU menu, int h)
{
    return geom() && g_main && parent == g_main && !(style & WS_CHILD) && (style & WS_POPUP) &&
           (style & WS_CAPTION) != WS_CAPTION && !menu && h > 0 && h <= 64;
}

static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y,
                                     int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    // The first top-level window is the main window/backdrop.
    bool is_main = g_fix && !g_main && !parent;
    if (is_main) style = (style & ~kFrameStyles) | WS_POPUP;

    bool tip = is_tooltip(parent, style, menu, h);
    if (tip) {
        POINT pt = {x, y}; // screen -> main client coordinates, kept on screen
        ScreenToClient(g_main, &pt);
        RECT cr;
        GetClientRect(g_main, &cr);
        x = pt.x + w > cr.right ? cr.right - w : pt.x;
        y = pt.y + h > cr.bottom ? cr.bottom - h : pt.y;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        style = (style & ~(WS_POPUP | WS_SYSMENU)) | WS_CHILD | WS_CLIPSIBLINGS;
        ex |= WS_EX_NOACTIVATE;
    }

    HWND r = o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (tip && r) {
        WNDPROC p = (WNDPROC)o_SetWindowLongA(r, GWL_WNDPROC, (LONG)&tip_proc);
        if (p != &tip_proc) g_tip_proc = p;
        o_SetWindowPos(r, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        LOG("wnd: tooltip %p created as child of the main window at %d,%d", (void*)r, x, y);
    }
    char b[16], p[128];
    describe(parent, p, sizeof(p));
    LOG("CreateWindowExA(ex=%#lx, \"%s\", \"%s\", style=%#lx, %d,%d %dx%d, parent=%s, menu=%p) -> %p  [caller %p]",
        (unsigned long)ex, class_name(cls, b, sizeof(b)), title ? title : "", (unsigned long)style,
        x, y, w, h, p, (void*)menu, (void*)r, CALLER());
    if (!r || !g_fix) return r;

    if (is_main) {
        g_main = r;
        g_inner_proc = (WNDPROC)o_SetWindowLongA(r, GWL_WNDPROC, (LONG)&main_proc);
        LOG("wnd: main window %p subclassed (game proc %p)", (void*)r, (void*)g_inner_proc);
    } else if (g_main && parent == g_main) {
        if ((style & WS_CHILD) && !g_content) {
            g_content = r;
            LOG("wnd: game screen window %p", (void*)r);
        } else if (!(style & WS_CHILD) && !g_menubar) {
            if (HMENU m = o_GetMenu(r)) {
                g_menubar = r;
                g_menu = m;
                SetMenu(r, nullptr); // the main window takes it over
                LOG("wnd: menu-bar window %p, menu %p moved to main window", (void*)r, (void*)m);
            }
        }
    }
    return r;
}

static BOOL WINAPI h_ShowWindow(HWND h, int cmd)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("ShowWindow(%s, %d)  [caller %p]", d, cmd, CALLER());
    if (g_fix && h && h == g_menubar) {
        bool was = g_menu_shown;
        show_menu(cmd != SW_HIDE);
        return was; // the popup itself is never shown
    }
    // No hover descriptions on top of an open menu of ours.
    if (h && cmd != SW_HIDE && menubar::is_open() && (WNDPROC)GetWindowLongA(h, GWL_WNDPROC) == &tip_proc)
        return FALSE;
    if (geom() && h && h == g_main && (cmd == SW_MAXIMIZE || cmd == SW_SHOWMAXIMIZED)) {
        BOOL r = o_ShowWindow(h, SW_SHOWNORMAL);
        fit_main();
        return r;
    }
    return o_ShowWindow(h, cmd);
}

static BOOL WINAPI h_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("SetWindowPos(%s, after=%p, %d,%d %dx%d, flags=%#x)  [caller %p]", d, (void*)after, x, y,
        cx, cy, flags, CALLER());
    if (geom() && h && h == g_main && (flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
        RECT m = monitor_rect(h);
        // Already in place: don't let the game's re-layout resize the window
        // again (every resize makes gamescope re-present it -> flicker).
        if (covers(h, m)) flags |= SWP_NOMOVE | SWP_NOSIZE;
        else flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        return o_SetWindowPos(h, after, m.left, m.top, m.right - m.left, m.bottom - m.top, flags);
    }
    BOOL r = o_SetWindowPos(h, after, x, y, cx, cy, flags);
    if (geom() && h && h == g_content) layout();
    return r;
}

static BOOL WINAPI h_MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("MoveWindow(%s, %d,%d %dx%d)  [caller %p]", d, x, y, w, hh, CALLER());
    if (geom() && h && h == g_main) {
        RECT m = monitor_rect(h);
        if (covers(h, m)) return TRUE; // nothing to do (see h_SetWindowPos)
        return o_MoveWindow(h, m.left, m.top, m.right - m.left, m.bottom - m.top, repaint);
    }
    BOOL r = o_MoveWindow(h, x, y, w, hh, repaint);
    if (geom() && h && h == g_content) layout(); // keep the size, fix the position
    return r;
}

static LONG WINAPI h_SetWindowLongA(HWND h, int idx, LONG v)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("SetWindowLongA(%s, %d, %#lx)  [caller %p]", d, idx, (unsigned long)v, CALLER());
    if (h && h == g_content && idx == GWL_STYLE) {
        // The game frames the content window (caption, sizing border) for races.
        bool race = g_fix && g_scale_race && (v & WS_CAPTION) == WS_CAPTION;
        if (race) v &= ~kFrameStyles;
        if (race && !g_content_proc) {
            g_content_proc = (WNDPROC)o_GetWindowLongA(h, GWL_WNDPROC);
            o_SetWindowLongA(h, GWL_WNDPROC, (LONG)&content_proc);
        }
        if (race != g_race_view) {
            g_race_view = race;
            SetRectEmpty(&g_race_rect);
            LOG("wnd: race view %s", race ? "on (scaled to the window)" : "off");
        }
    }
    if (g_fix && h && h == g_main) {
        if (idx == GWL_WNDPROC) { // keep our subclass on top
            LONG prev = (LONG)g_inner_proc;
            g_inner_proc = (WNDPROC)v;
            return prev;
        }
        if (idx == GWL_STYLE && !g_exclusive) v = (v & ~kFrameStyles) | WS_POPUP;
    }
    return o_SetWindowLongA(h, idx, v);
}

static LONG WINAPI h_GetWindowLongA(HWND h, int idx)
{
    if (g_fix && h && h == g_main && idx == GWL_WNDPROC) return (LONG)g_inner_proc;
    return o_GetWindowLongA(h, idx);
}

static HMENU WINAPI h_GetMenu(HWND h)
{
    if (g_fix && h && h == g_menubar) return g_menu;
    return o_GetMenu(h);
}

static BOOL WINAPI h_DrawMenuBar(HWND h)
{
    if (g_fix && h && h == g_menubar) {
        if (!g_native_menu) {
            menubar::refresh();
            return TRUE;
        }
        return g_menu_shown ? o_DrawMenuBar(g_main) : TRUE;
    }
    return o_DrawMenuBar(h);
}

static BOOL WINAPI h_DestroyWindow(HWND h)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("DestroyWindow(%s)  [caller %p]", d, CALLER());
    if (h && h == g_menubar) { // the menu would die with the main window otherwise twice
        if (g_main && GetMenu(g_main) == g_menu) SetMenu(g_main, nullptr);
        SetMenu(h, g_menu);    // hand it back so DestroyWindow frees it
        g_menubar = nullptr;
        g_menu = nullptr;
    } else if (h && h == g_content) {
        g_content = nullptr;
        g_content_proc = nullptr;
        g_race_view = false;
    } else if (h && h == g_main) {
        g_main = nullptr;
    }
    return o_DestroyWindow(h);
}

// ---------------------------------------------------------------------------
// DirectDraw (IDirectDraw v1): 6 CreateSurface, 19 RestoreDisplayMode,
// 20 SetCooperativeLevel, 21 SetDisplayMode

using DD_CreateSurface_t = HRESULT(WINAPI*)(void*, void*, void**, void*);
using DD_SetCoop_t       = HRESULT(WINAPI*)(void*, HWND, DWORD);
using DD_SetMode_t       = HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD);
using DD_Restore_t       = HRESULT(WINAPI*)(void*);

static HRESULT WINAPI h_dd_CreateSurface(void* self, void* desc, void** out, void* outer)
{
    HRESULT hr = vtable::orig_as<DD_CreateSurface_t>(self, 6)(self, desc, out, outer);
    // DDSURFACEDESC: dwSize, dwFlags, dwHeight, dwWidth, ... ddsCaps at offset 104
    auto* d = static_cast<DWORD*>(desc);
    LOG("IDirectDraw::CreateSurface(flags=%#lx, %lux%lu, caps=%#lx) -> %#lx  [caller %p]",
        (unsigned long)d[1], (unsigned long)d[3], (unsigned long)d[2], (unsigned long)d[26],
        (unsigned long)hr, CALLER());
    return hr;
}

static HRESULT WINAPI h_dd_SetCoop(void* self, HWND h, DWORD flags)
{
    char d[128];
    describe(h, d, sizeof(d));
    HRESULT hr = vtable::orig_as<DD_SetCoop_t>(self, 20)(self, h, flags);
    LOG("IDirectDraw::SetCooperativeLevel(%s, %#lx) -> %#lx  [caller %p]", d, (unsigned long)flags,
        (unsigned long)hr, CALLER());
    if (SUCCEEDED(hr) && g_fix && h == g_main) {
        bool excl = (flags & 0x10 /*DDSCL_EXCLUSIVE*/) != 0;
        if (excl != g_exclusive) {
            g_exclusive = excl;
            LOG("wnd: %s DirectDraw exclusive mode", excl ? "entering" : "leaving");
            if (excl && g_layout_cb) {
                RECT none = {0, 0, 0, 0};
                g_layout_cb(g_main, none, none, false);
            }
            if (!excl) { // back to the menus: restore our layout
                LONG st = o_GetWindowLongA(g_main, GWL_STYLE);
                o_SetWindowLongA(g_main, GWL_STYLE, (st & ~kFrameStyles) | WS_POPUP);
                fit_main();
                layout();
            }
        }
    }
    return hr;
}

static HRESULT WINAPI h_dd_SetMode(void* self, DWORD w, DWORD h, DWORD bpp)
{
    HRESULT hr = vtable::orig_as<DD_SetMode_t>(self, 21)(self, w, h, bpp);
    LOG("IDirectDraw::SetDisplayMode(%lux%lux%lu) -> %#lx  [caller %p]", (unsigned long)w,
        (unsigned long)h, (unsigned long)bpp, (unsigned long)hr, CALLER());
    return hr;
}

static HRESULT WINAPI h_dd_Restore(void* self)
{
    HRESULT hr = vtable::orig_as<DD_Restore_t>(self, 19)(self);
    LOG("IDirectDraw::RestoreDisplayMode() -> %#lx  [caller %p]", (unsigned long)hr, CALLER());
    return hr;
}

static HRESULT WINAPI h_DirectDrawCreate(GUID* guid, void** out, void* outer)
{
    HRESULT hr = o_DirectDrawCreate(guid, out, outer);
    LOG("DirectDrawCreate -> %#lx, %p  [caller %p]", (unsigned long)hr, out ? *out : nullptr, CALLER());
    if (SUCCEEDED(hr) && out && *out)
        vtable::hook(*out, "IDirectDraw", 23,
                     {{6, (void*)&h_dd_CreateSurface},
                      {19, (void*)&h_dd_Restore},
                      {20, (void*)&h_dd_SetCoop},
                      {21, (void*)&h_dd_SetMode}});
    return hr;
}

// Input-state diagnostics (clicks stop working after using the menu bar).

static HWND WINAPI h_SetCapture(HWND h)
{
    char d[128];
    describe(h, d, sizeof(d));
    HWND r = o_SetCapture(h);
    LOG("SetCapture(%s) prev=%p  [caller %p]", d, (void*)r, CALLER());
    return r;
}

static BOOL WINAPI h_ReleaseCapture()
{
    HWND c = GetCapture();
    BOOL r = o_ReleaseCapture();
    LOG("ReleaseCapture() had=%p  [caller %p]", (void*)c, CALLER());
    return r;
}

static BOOL WINAPI h_EnableWindow(HWND h, BOOL on)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("EnableWindow(%s, %d)  [caller %p]", d, on, CALLER());
    return o_EnableWindow(h, on);
}

static HWND WINAPI h_SetFocus(HWND h)
{
    char d[128];
    describe(h, d, sizeof(d));
    LOG("SetFocus(%s)  [caller %p]", d, CALLER());
    return o_SetFocus(h);
}

void poll_state()
{
    if (!g_main) return;
    struct State {
        DWORD flags;
        HWND active, focus, capture, menu_owner, fg, under;
        BOOL main_on, content_on;
    };
    static State last;
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(g_main, nullptr), &gti)) return;
    POINT pt;
    GetCursorPos(&pt);
    State s = {gti.flags, gti.hwndActive, gti.hwndFocus, gti.hwndCapture, gti.hwndMenuOwner,
               GetForegroundWindow(), WindowFromPoint(pt), IsWindowEnabled(g_main),
               g_content ? IsWindowEnabled(g_content) : TRUE};
    if (!std::memcmp(&s, &last, sizeof(s))) return;
    last = s;
    char under[128];
    describe(s.under, under, sizeof(under));
    LOG("state: flags=%#lx active=%p focus=%p capture=%p menuowner=%p fg=%p enabled=%d/%d under cursor %ld,%ld: %s",
        s.flags, (void*)s.active, (void*)s.focus, (void*)s.capture, (void*)s.menu_owner, (void*)s.fg,
        s.main_on, s.content_on, pt.x, pt.y, under);
}

// ---------------------------------------------------------------------------
// GDI blits (windowed races draw their frames with StretchBlt)

using StretchBlt_t = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
static StretchBlt_t o_StretchBlt;

static BOOL WINAPI h_StretchBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                                int sh, DWORD rop)
{
    if (g_race_view && g_content && src && !IsRectEmpty(&g_race_rect) && WindowFromDC(dc) == g_content) {
        // A frame (or part of one) into the parked race window: stretch it
        // onto the main window. The game draws 1:1 at its window size.
        RECT c;
        if (GetClientRect(g_content, &c) && c.right > 0 && c.bottom > 0) {
            int fw = c.right, fh = c.bottom;
            int x0 = sx < 0 ? 0 : sx, y0 = sy < 0 ? 0 : sy;
            int x1 = sx + sw > fw ? fw : sx + sw, y1 = sy + sh > fh ? fh : sy + sh;
            if (x1 <= x0 || y1 <= y0) return TRUE;
            int rw = g_race_rect.right - g_race_rect.left, rh = g_race_rect.bottom - g_race_rect.top;
            auto sc = [](int v, int to, int from) { return (int)((long long)v * to / from); };
            int dx0 = g_race_rect.left + sc(x0, rw, fw), dy0 = g_race_rect.top + sc(y0, rh, fh);
            int dx1 = g_race_rect.left + sc(x1, rw, fw), dy1 = g_race_rect.top + sc(y1, rh, fh);
            HDC mdc = GetDC(g_main);
            if (mdc) {
                SetStretchBltMode(mdc, COLORONCOLOR);
                BOOL r = o_StretchBlt(mdc, dx0, dy0, dx1 - dx0, dy1 - dy0, src, x0, y0, x1 - x0, y1 - y0, rop);
                ReleaseDC(g_main, mdc);
                return r;
            }
        }
    }
    return o_StretchBlt(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

void install()
{
    g_fix = config::get_int("Window", "FixLayout", 1) != 0;
    g_native_menu = config::get_int("Window", "NativeMenuBar", 0) != 0;
    g_scale_race = config::get_int("Window", "ScaleRace", 1) != 0;
    LOG("wnd: layout fix %s", g_fix ? "enabled" : "disabled ([Window] FixLayout=0)");

    struct { const char* dll; const char* fn; void* hook; void** orig; } defs[] = {
#define H(dll, fn) { dll, #fn, (void*)&h_##fn, (void**)&o_##fn }
        H("USER32.dll", RegisterClassA),
        H("USER32.dll", CreateWindowExA),
        H("USER32.dll", ShowWindow),
        H("USER32.dll", SetWindowPos),
        H("USER32.dll", MoveWindow),
        H("USER32.dll", SetWindowLongA),
        H("USER32.dll", GetWindowLongA),
        H("USER32.dll", DestroyWindow),
        H("USER32.dll", GetMenu),
        H("USER32.dll", DrawMenuBar),
        H("DDRAW.dll",  DirectDrawCreate),
        H("USER32.dll", SetCapture),
        H("USER32.dll", ReleaseCapture),
        H("USER32.dll", EnableWindow),
        H("USER32.dll", SetFocus),
        H("GDI32.dll",  StretchBlt),
#undef H
    };
    HMODULE exe = GetModuleHandleA(nullptr);
    for (auto& d : defs)
        if (!patch_iat(exe, d.dll, d.fn, d.hook, d.orig))
            LOG("FAILED to hook %s!%s", d.dll, d.fn);
}

bool run_on_gui(void (*fn)(void*), void* arg)
{
    return g_main && PostMessageA(g_main, kRunMsg, (WPARAM)fn, (LPARAM)arg);
}

} // namespace rclan::wnd
