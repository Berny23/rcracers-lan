// Custom-drawn replacement for the game's Win32 menu bar. See menubar.h.
//
// While a menu is open the bar holds the mouse capture (plain SetCapture, no
// Win32 menu mode, so Wine doesn't grab the X pointer) and does all hit
// testing itself; the dropdowns are child windows of the main window that
// only paint. Coordinates: the bar sits at 0,0 of the main window's client
// area, so bar client coordinates == main client coordinates.

#include "menubar.h"

#include "log.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace rclan::menubar {

namespace {

constexpr COLORREF kGoldHi = RGB(250, 222, 130), kGold = RGB(208, 160, 80), kGoldDk = RGB(110, 76, 30);
constexpr COLORREF kBarTop = RGB(74, 54, 98), kBarBot = RGB(30, 22, 48);
constexpr COLORREF kPurpleTop = RGB(92, 68, 118), kPurpleBot = RGB(38, 28, 62);
constexpr COLORREF kHoverTop = RGB(52, 170, 156), kHoverBot = RGB(18, 92, 86);
constexpr COLORREF kYellow = RGB(250, 210, 48), kWhite = RGB(255, 246, 214), kGray = RGB(150, 138, 160);

constexpr int kItemH = 24, kSepH = 9, kFrame = 3, kGutter = 26, kArrowW = 22, kBarPad = 11;
constexpr int kMaxTop = 16, kMaxLevels = 4, kMaxItems = 48;

struct Item {
    char text[96];  // label with & mnemonics, accelerator stripped
    char accel[32]; // text after '\t'
    UINT id, type, state;
    HMENU sub;
    RECT rc;        // bar: bar coords; dropdown: level window coords
    char mnemonic;  // lowercase, 0 if none
};

struct Level {
    HMENU menu;
    HWND wnd;
    RECT rc; // main client coords
    Item items[kMaxItems];
    int n;
    int hover; // -1: none
};

HWND g_main, g_bar, g_target;
HMENU g_menu;
HFONT g_font;
bool g_visible;
DWORD g_thread;

Item g_top[kMaxTop];
int g_ntop;
int g_hot = -1;  // bar item under the mouse while closed
int g_open = -1; // bar item whose menu is open; -1 = closed
Level g_lv[kMaxLevels];
HWND g_lv_wnd[kMaxLevels];
int g_depth; // open dropdown levels
bool g_tracking_leave, g_releasing;

// ---------------------------------------------------------------------------
// Menu data

bool is_sep(const Item& it) { return (it.type & MFT_SEPARATOR) != 0; }
bool is_disabled(const Item& it) { return (it.state & (MFS_DISABLED | MFS_GRAYED)) != 0; }
bool selectable(const Item& it) { return !is_sep(it) && !is_disabled(it); }

int read_items(HMENU m, Item* out, int max)
{
    int n = GetMenuItemCount(m);
    if (n < 0) n = 0;
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) {
        Item& it = out[i];
        it = Item{};
        char buf[160] = "";
        MENUITEMINFOA mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mii.dwTypeData = buf;
        mii.cch = sizeof(buf) - 1;
        if (!GetMenuItemInfoA(m, i, TRUE, &mii)) {
            it.type = MFT_SEPARATOR;
            continue;
        }
        it.id = mii.wID;
        it.type = mii.fType;
        it.state = mii.fState;
        it.sub = mii.hSubMenu;
        if (it.type & (MFT_BITMAP | MFT_OWNERDRAW)) buf[0] = '\0';
        if (char* tab = std::strchr(buf, '\t')) {
            *tab = '\0';
            std::snprintf(it.accel, sizeof(it.accel), "%s", tab + 1);
        }
        std::snprintf(it.text, sizeof(it.text), "%s", buf);
        for (const char* p = buf; *p; ++p) {
            if (*p != '&') continue;
            if (p[1] == '&') { ++p; continue; }
            if (p[1]) it.mnemonic = (char)std::tolower((unsigned char)p[1]);
            break;
        }
    }
    return n;
}

int text_width(HDC dc, const char* s)
{
    RECT r = {0, 0, 0, 0};
    DrawTextA(dc, s, -1, &r, DT_CALCRECT | DT_SINGLELINE);
    return r.right;
}

void layout_bar()
{
    g_ntop = g_menu ? read_items(g_menu, g_top, kMaxTop) : 0;
    HDC dc = GetDC(g_bar);
    HGDIOBJ old = SelectObject(dc, g_font);
    int x = 6;
    for (int i = 0; i < g_ntop; ++i) {
        int w = text_width(dc, g_top[i].text) + 2 * kBarPad;
        g_top[i].rc = {x, 0, x + w, kHeight};
        x += w;
    }
    SelectObject(dc, old);
    ReleaseDC(g_bar, dc);
}

// ---------------------------------------------------------------------------
// Drawing

COLORREF mix(COLORREF a, COLORREF b, int num, int den)
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * num / den,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * num / den,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * num / den);
}

void fill(HDC dc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void vgrad(HDC dc, const RECT& r, COLORREF top, COLORREF bot)
{
    int h = r.bottom - r.top;
    for (int y = 0; y < h; ++y)
        fill(dc, {r.left, r.top + y, r.right, r.top + y + 1}, mix(top, bot, y, h > 1 ? h - 1 : 1));
}

void frame(HDC dc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

void text(HDC dc, const char* s, RECT r, UINT fmt, COLORREF c)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    const int off[][2] = {{1, 1}, {2, 2}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (auto& o : off) {
        RECT s2 = {r.left + o[0], r.top + o[1], r.right + o[0], r.bottom + o[1]};
        DrawTextA(dc, s, -1, &s2, fmt);
    }
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, fmt);
}

void hover_box(HDC dc, const RECT& r)
{
    vgrad(dc, r, kHoverTop, kHoverBot);
    frame(dc, r, kGold);
}

void paint_bar(HDC dc, int w)
{
    vgrad(dc, {0, 0, w, kHeight - 2}, kBarTop, kBarBot);
    fill(dc, {0, kHeight - 2, w, kHeight - 1}, kGold);
    fill(dc, {0, kHeight - 1, w, kHeight}, kGoldDk);
    HGDIOBJ old = SelectObject(dc, g_font);
    for (int i = 0; i < g_ntop; ++i) {
        const Item& it = g_top[i];
        bool hot = i == g_open || (g_open < 0 && i == g_hot);
        if (hot) hover_box(dc, {it.rc.left + 1, 2, it.rc.right - 1, kHeight - 3});
        text(dc, it.text, it.rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
             is_disabled(it) ? kGray : hot ? kWhite : kYellow);
    }
    SelectObject(dc, old);
}

void draw_check(HDC dc, const RECT& r, bool radio, COLORREF c)
{
    int cx = r.left + kGutter / 2 + 1, cy = (r.top + r.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, c);
    HGDIOBJ op = SelectObject(dc, pen);
    if (radio) {
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, b);
        Ellipse(dc, cx - 3, cy - 3, cx + 4, cy + 4);
        SelectObject(dc, ob);
        DeleteObject(b);
    } else {
        POINT pts[] = {{cx - 5, cy}, {cx - 2, cy + 3}, {cx + 5, cy - 4}};
        Polyline(dc, pts, 3);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

void draw_arrow(HDC dc, const RECT& r, COLORREF c)
{
    int x = r.right - 12, cy = (r.top + r.bottom) / 2;
    POINT pts[] = {{x - 3, cy - 5}, {x + 3, cy}, {x - 3, cy + 5}};
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Polygon(dc, pts, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}

void paint_level(HDC dc, const Level& L)
{
    int w = L.rc.right - L.rc.left, h = L.rc.bottom - L.rc.top;
    vgrad(dc, {0, 0, w, h}, kPurpleTop, kPurpleBot);
    frame(dc, {0, 0, w, h}, kGoldDk);
    frame(dc, {1, 1, w - 1, h - 1}, kGoldHi);
    frame(dc, {2, 2, w - 2, h - 2}, kGold);
    HGDIOBJ old = SelectObject(dc, g_font);
    for (int i = 0; i < L.n; ++i) {
        const Item& it = L.items[i];
        if (is_sep(it)) {
            int y = (it.rc.top + it.rc.bottom) / 2;
            fill(dc, {it.rc.left + 6, y - 1, it.rc.right - 6, y}, kGoldDk);
            fill(dc, {it.rc.left + 6, y, it.rc.right - 6, y + 1}, kGold);
            continue;
        }
        bool hot = i == L.hover && !is_disabled(it);
        if (hot) hover_box(dc, it.rc);
        COLORREF c = is_disabled(it) ? kGray : hot ? kWhite : kYellow;
        RECT tr = {it.rc.left + kGutter, it.rc.top, it.rc.right - kArrowW, it.rc.bottom};
        text(dc, it.text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE, c);
        if (it.accel[0]) text(dc, it.accel, tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, c);
        if (it.state & MFS_CHECKED) draw_check(dc, it.rc, (it.type & MFT_RADIOCHECK) != 0, c);
        if (it.sub) draw_arrow(dc, it.rc, c);
    }
    SelectObject(dc, old);
}

// Double-buffered WM_PAINT.
template <typename F>
void buffered_paint(HWND h, F draw)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);
    draw(mem, rc.right);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// ---------------------------------------------------------------------------
// Open/close

void invalidate_bar() { InvalidateRect(g_bar, nullptr, FALSE); }

int first_selectable(const Level& L)
{
    for (int i = 0; i < L.n; ++i)
        if (selectable(L.items[i])) return i;
    return -1;
}

void close_levels(int from)
{
    for (int k = from; k < g_depth; ++k) ShowWindow(g_lv[k].wnd, SW_HIDE);
    if (from < g_depth) g_depth = from;
}

void close_all()
{
    close_levels(0);
    bool was_open = g_open >= 0;
    g_open = -1;
    g_hot = -1;
    if (was_open) {
        g_releasing = true;
        if (GetCapture() == g_bar) ReleaseCapture();
        g_releasing = false;
    }
    invalidate_bar();
}

// Opens `menu` as dropdown level g_depth at x,y (main client coords);
// `alt_x` is where to put it instead if it doesn't fit on the right.
void push_level(HMENU menu, int x, int y, int alt_x, bool keyboard)
{
    if (g_depth >= kMaxLevels) return;
    Level& L = g_lv[g_depth];
    L.menu = menu;
    L.n = read_items(menu, L.items, kMaxItems);
    L.hover = -1;

    HDC dc = GetDC(g_main);
    HGDIOBJ old = SelectObject(dc, g_font);
    int tw = 0, aw = 0, yy = kFrame;
    for (int i = 0; i < L.n; ++i) {
        Item& it = L.items[i];
        int ih = is_sep(it) ? kSepH : kItemH;
        it.rc = {kFrame, yy, 0, yy + ih};
        yy += ih;
        if (is_sep(it)) continue;
        int t = text_width(dc, it.text), a = it.accel[0] ? text_width(dc, it.accel) : 0;
        if (t > tw) tw = t;
        if (a > aw) aw = a;
    }
    SelectObject(dc, old);
    ReleaseDC(g_main, dc);
    int w = 2 * kFrame + kGutter + tw + (aw ? aw + 24 : 0) + kArrowW;
    if (w < 140) w = 140;
    int h = yy + kFrame;
    for (int i = 0; i < L.n; ++i) L.items[i].rc.right = w - kFrame;

    RECT cr;
    GetClientRect(g_main, &cr);
    if (x + w > cr.right) x = alt_x >= 0 ? alt_x - w : cr.right - w;
    if (y + h > cr.bottom) y = cr.bottom - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    L.rc = {x, y, x + w, y + h};
    if (keyboard) L.hover = first_selectable(L);

    L.wnd = g_lv_wnd[g_depth];
    SetWindowLongPtrA(L.wnd, GWLP_USERDATA, g_depth);
    ++g_depth;
    SetWindowPos(L.wnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(L.wnd, nullptr, FALSE);
}

void open_sub(int k, int i, bool keyboard)
{
    Level& P = g_lv[k];
    const Item& it = P.items[i];
    if (!it.sub || is_disabled(it)) return;
    close_levels(k + 1);
    SendMessageA(g_target, WM_INITMENUPOPUP, (WPARAM)it.sub, MAKELPARAM(i, FALSE));
    push_level(it.sub, P.rc.right - kFrame, P.rc.top + it.rc.top - kFrame, P.rc.left + kFrame, keyboard);
}

void open_top(int t, bool keyboard)
{
    if (t < 0 || t >= g_ntop) return;
    close_levels(0);
    if (g_open < 0) {
        SetCapture(g_bar);
        SendMessageA(g_target, WM_INITMENU, (WPARAM)g_menu, 0); // game updates check marks
        layout_bar();
    }
    g_open = t;
    const Item& it = g_top[t];
    if (it.sub && !is_disabled(it)) {
        SendMessageA(g_target, WM_INITMENUPOPUP, (WPARAM)it.sub, MAKELPARAM(t, FALSE));
        push_level(it.sub, it.rc.left, kHeight, -1, keyboard);
    }
    invalidate_bar();
}

void select(const Item& it)
{
    if (!selectable(it) || it.sub) return;
    UINT id = it.id;
    close_all();
    LOG("menubar: command %u", id);
    PostMessageA(g_target, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

// ---------------------------------------------------------------------------
// Hit testing (main client coords)

int top_at(POINT p)
{
    if (p.y < 0 || p.y >= kHeight) return -1;
    for (int i = 0; i < g_ntop; ++i)
        if (p.x >= g_top[i].rc.left && p.x < g_top[i].rc.right) return i;
    return -1;
}

int level_at(POINT p)
{
    for (int k = g_depth - 1; k >= 0; --k)
        if (PtInRect(&g_lv[k].rc, p)) return k;
    return -1;
}

int item_at(const Level& L, POINT p)
{
    POINT q = {p.x - L.rc.left, p.y - L.rc.top};
    for (int i = 0; i < L.n; ++i)
        if (!is_sep(L.items[i]) && PtInRect(&L.items[i].rc, q)) return i;
    return -1;
}

void set_hover(int k, int i)
{
    if (g_lv[k].hover == i) return;
    g_lv[k].hover = i;
    InvalidateRect(g_lv[k].wnd, nullptr, FALSE);
}

void on_mouse_move(POINT p)
{
    if (g_open < 0) {
        int t = top_at(p);
        if (t != g_hot) {
            g_hot = t;
            invalidate_bar();
        }
        if (!g_tracking_leave) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g_bar, 0};
            g_tracking_leave = TrackMouseEvent(&tme) != FALSE;
        }
        return;
    }
    int k = level_at(p);
    if (k >= 0) {
        int i = item_at(g_lv[k], p);
        set_hover(k, i);
        if (i >= 0 && g_lv[k].items[i].sub) {
            if (!(g_depth > k + 1 && g_lv[k + 1].menu == g_lv[k].items[i].sub)) open_sub(k, i, false);
        } else if (i >= 0) {
            close_levels(k + 1);
        }
        return;
    }
    int t = top_at(p);
    if (t >= 0 && t != g_open) {
        open_top(t, false);
        return;
    }
    // Outside: drop the hover of the innermost menu unless it leads to an open submenu.
    if (g_depth > 0) set_hover(g_depth - 1, -1);
}

void on_button_down(POINT p)
{
    int t = top_at(p);
    if (g_open < 0) {
        if (t >= 0) open_top(t, false);
        return;
    }
    if (t >= 0) {
        if (t == g_open) close_all();
        else open_top(t, false);
        return;
    }
    if (level_at(p) < 0) close_all(); // click outside closes (and is swallowed)
}

void on_button_up(POINT p)
{
    if (g_open < 0) return;
    int k = level_at(p);
    if (k >= 0) {
        int i = item_at(g_lv[k], p);
        if (i >= 0) select(g_lv[k].items[i]);
        return;
    }
    int t = top_at(p);
    if (t >= 0 && t == g_open && !g_top[t].sub) select(g_top[t]);
}

// ---------------------------------------------------------------------------
// Keyboard

void move_hover(int dir)
{
    if (g_depth == 0) return;
    Level& L = g_lv[g_depth - 1];
    if (L.n == 0) return;
    int i = L.hover;
    for (int step = 0; step < L.n; ++step) {
        i = i < 0 ? (dir > 0 ? 0 : L.n - 1) : (i + dir + L.n) % L.n;
        if (selectable(L.items[i])) break;
    }
    set_hover(g_depth - 1, i);
}

void activate_hover()
{
    if (g_depth == 0) {
        if (g_open >= 0) select(g_top[g_open]);
        return;
    }
    int k = g_depth - 1;
    int i = g_lv[k].hover;
    if (i < 0) return;
    if (g_lv[k].items[i].sub) open_sub(k, i, true);
    else select(g_lv[k].items[i]);
}

void on_keydown(WPARAM vk)
{
    switch (vk) {
    case VK_ESCAPE:
        if (g_depth > 1) close_levels(g_depth - 1);
        else close_all();
        break;
    case VK_UP: move_hover(-1); break;
    case VK_DOWN: move_hover(+1); break;
    case VK_RIGHT: {
        int k = g_depth - 1;
        if (k >= 0 && g_lv[k].hover >= 0 && g_lv[k].items[g_lv[k].hover].sub) open_sub(k, g_lv[k].hover, true);
        else if (g_ntop) open_top((g_open + 1) % g_ntop, true);
        break;
    }
    case VK_LEFT:
        if (g_depth > 1) close_levels(g_depth - 1);
        else if (g_ntop) open_top((g_open - 1 + g_ntop) % g_ntop, true);
        break;
    case VK_RETURN:
    case VK_SPACE: activate_hover(); break;
    case VK_MENU:
    case VK_F10: close_all(); break;
    }
}

void on_char(char c)
{
    c = (char)std::tolower((unsigned char)c);
    if (g_depth == 0) return;
    int k = g_depth - 1;
    for (int i = 0; i < g_lv[k].n; ++i) {
        const Item& it = g_lv[k].items[i];
        if (it.mnemonic != c || !selectable(it)) continue;
        set_hover(k, i);
        activate_hover();
        return;
    }
}

int top_by_mnemonic(char c)
{
    c = (char)std::tolower((unsigned char)c);
    for (int i = 0; i < g_ntop; ++i)
        if (g_top[i].mnemonic == c && !is_disabled(g_top[i])) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Window procs

POINT lparam_pt(LPARAM l) { return {(short)LOWORD(l), (short)HIWORD(l)}; }

LRESULT CALLBACK bar_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT:
        if (g_open < 0) layout_bar(); // the game adds menus later (e.g. "Modes")
        buffered_paint(h, [](HDC dc, int width) { paint_bar(dc, width); });
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        on_mouse_move(lparam_pt(l));
        return 0;
    case WM_MOUSELEAVE:
        g_tracking_leave = false;
        if (g_open < 0 && g_hot >= 0) {
            g_hot = -1;
            invalidate_bar();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        on_button_down(lparam_pt(l));
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        on_button_up(lparam_pt(l));
        return 0;
    case WM_CAPTURECHANGED:
        if (!g_releasing && g_open >= 0 && (HWND)l != h) close_all();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcA(h, m, w, l);
}

LRESULT CALLBACK drop_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        int k = (int)GetWindowLongPtrA(h, GWLP_USERDATA);
        buffered_paint(h, [k](HDC dc, int) {
            if (k >= 0 && k < g_depth) paint_level(dc, g_lv[k]);
        });
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcA(h, m, w, l);
}

} // namespace

// ---------------------------------------------------------------------------

void attach(HWND main, HMENU menu, HWND target)
{
    if (g_bar) {
        g_menu = menu;
        g_target = target;
        return;
    }
    g_main = main;
    g_menu = menu;
    g_target = target;
    g_thread = GetCurrentThreadId();

    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = bar_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "RCLanMenuBar";
    RegisterClassA(&wc);
    wc.lpfnWndProc = drop_proc;
    wc.lpszClassName = "RCLanMenuDrop";
    RegisterClassA(&wc);

    g_font = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    g_bar = CreateWindowExA(WS_EX_NOACTIVATE, "RCLanMenuBar", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 100,
                            kHeight, main, nullptr, inst, nullptr);
    for (int k = 0; k < kMaxLevels; ++k)
        g_lv_wnd[k] = CreateWindowExA(WS_EX_NOACTIVATE, "RCLanMenuDrop", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                                      10, 10, main, nullptr, inst, nullptr);
    layout_bar();
    LOG("menubar: created %p (%d top-level menus)", (void*)g_bar, g_ntop);
}

void place(int width, bool visible)
{
    if (!g_bar) return;
    if (!visible) {
        if (g_open >= 0) close_all();
        if (g_visible) ShowWindow(g_bar, SW_HIDE);
        g_visible = false;
        return;
    }
    RECT r;
    GetWindowRect(g_bar, &r);
    if (!g_visible) layout_bar();
    if (!g_visible || r.right - r.left != width) {
        SetWindowPos(g_bar, HWND_TOP, 0, 0, width, kHeight, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        invalidate_bar();
    }
    g_visible = true;
}

bool is_open() { return g_open >= 0; }

void close()
{
    if (g_open >= 0) close_all();
}

void refresh()
{
    if (!g_bar) return;
    layout_bar();
    invalidate_bar();
}

bool key_filter(const MSG* m, bool removed)
{
    if (!g_bar || !g_visible || GetCurrentThreadId() != g_thread) return false;
    UINT msg = m->message;
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_CHAR && msg != WM_SYSKEYDOWN &&
        msg != WM_SYSKEYUP && msg != WM_SYSCHAR)
        return false;
    if (g_open < 0) {
        // Alt+letter opens the matching menu, like the native menu bar.
        if (msg == WM_SYSKEYDOWN && (m->lParam & (1 << 29))) {
            int t = top_by_mnemonic((char)m->wParam);
            if (t < 0) return false;
            if (removed) open_top(t, true);
            return true;
        }
        if (msg == WM_SYSCHAR) return top_by_mnemonic((char)m->wParam) >= 0;
        return false;
    }
    if (!removed) return true;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) on_keydown(m->wParam);
    else if (msg == WM_CHAR) on_char((char)m->wParam);
    return true;
}

} // namespace rclan::menubar
