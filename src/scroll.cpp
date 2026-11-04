#include "scroll.h"

#include "log.h"
#include "rtti.h"
#include "wnd.h"

#include <cstring>

namespace rclan::scroll {

namespace {

// MyScrollBar layout (from its update method, vtable slot 1):
//   +0x04..+0x10  x, y, w, h in game-screen coordinates
//   +0x34         dirty flag
//   +0x4c         owner; owner+0x10 != 0 while the control is active
//   +0x88         arrow step     +0xb0 / +0xb4  range     +0xbc  position
constexpr int kX = 0x04, kY = 0x08, kW = 0x0c, kH = 0x10, kDirty = 0x34, kOwner = 0x4c;
constexpr int kStep = 0x88, kMin = 0xb0, kMax = 0xb4, kPos = 0xbc;

using Notify = void(__fastcall*)(void* self);

void* const* g_vt;
Notify g_notify;
bool g_init, g_ok;

int& field(void* o, int off) { return *reinterpret_cast<int*>(static_cast<char*>(o) + off); }

// The method that applies a new position is called right after the clamp in
// the update method:
//   8B CE                 mov ecx, esi
//   C7 46 34 01 00 00 00  mov dword [esi+34h], 1
//   81 CF 00 02 00 00     or edi, 200h
//   E8 rel32              call notify
Notify find_notify(const void* update)
{
    static const unsigned char pat[] = {0x8B, 0xCE, 0xC7, 0x46, 0x34, 0x01, 0x00, 0x00, 0x00,
                                        0x81, 0xCF, 0x00, 0x02, 0x00, 0x00, 0xE8};
    auto* p = static_cast<const unsigned char*>(update);
    for (int i = 0; i < 0x800 && rtti::in_code(p + i + sizeof(pat) + 4); ++i) {
        if (std::memcmp(p + i, pat, sizeof(pat)) != 0) continue;
        const unsigned char* next = p + i + sizeof(pat) + 4;
        auto* target = next + *reinterpret_cast<const int*>(p + i + sizeof(pat));
        if (rtti::in_code(target)) return reinterpret_cast<Notify>(const_cast<unsigned char*>(target));
    }
    return nullptr;
}

bool init()
{
    if (g_init) return g_ok;
    g_init = true;
    g_vt = rtti::vtable("MyScrollBar");
    if (g_vt) g_notify = find_notify(g_vt[1]);
    g_ok = g_vt && g_notify;
    LOG("scroll: mouse wheel %s (MyScrollBar vtable %p, notify %p)", g_ok ? "enabled" : "UNAVAILABLE",
        (void*)g_vt, (void*)g_notify);
    return g_ok;
}

bool plausible(void* o)
{
    if (IsBadReadPtr(o, kPos + 4) || *static_cast<void* const* const*>(o) != g_vt) return false;
    int w = field(o, kW), h = field(o, kH);
    if (w <= 0 || h <= 0 || w > 4000 || h > 4000) return false;
    if (field(o, kMax) < field(o, kMin)) return false;
    void* owner = reinterpret_cast<void*>(field(o, kOwner));
    return owner && !IsBadReadPtr(owner, 0x14) && field(owner, 0x10) != 0;
}

// Cached scroll bars of the current screen; rescanned when stale.
void* g_bars[16];
int g_nbars;
DWORD g_scanned;

void rescan()
{
    void* found[64];
    int n = rtti::find_objects(g_vt, found, 64);
    g_nbars = 0;
    for (int i = 0; i < n && g_nbars < 16; ++i)
        if (plausible(found[i])) g_bars[g_nbars++] = found[i];
    g_scanned = GetTickCount();
}

// The scroll bar belonging to the list under `pt` (game-screen coordinates):
// its vertical extent contains the point and it is the nearest one to the
// right of it (lists have their bar on the right).
void* pick(POINT pt)
{
    void* best = nullptr;
    int best_d = 1 << 30;
    for (int i = 0; i < g_nbars; ++i) {
        void* o = g_bars[i];
        if (!plausible(o)) continue;
        int x = field(o, kX), y = field(o, kY), w = field(o, kW), h = field(o, kH);
        if (pt.y < y || pt.y >= y + h || pt.x >= x + w) continue;
        int d = x - pt.x;
        if (d < best_d) {
            best_d = d;
            best = o;
        }
    }
    return best;
}

} // namespace

bool on_message(const MSG* m)
{
    if (m->message != WM_MOUSEWHEEL || !init()) return false;
    HWND content = wnd::content_window();
    if (!content || !IsWindowVisible(content)) return false;
    POINT pt = {(short)LOWORD(m->lParam), (short)HIWORD(m->lParam)}; // screen coordinates
    ScreenToClient(content, &pt);

    if (GetTickCount() - g_scanned > 1000 || !pick(pt)) rescan();
    void* bar = pick(pt);
    if (!bar) return false;

    UINT lines = 3;
    SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == 0 || lines > 10) lines = 3;
    int notches = -(short)HIWORD(m->wParam) / WHEEL_DELTA;
    if (!notches) notches = (short)HIWORD(m->wParam) > 0 ? -1 : 1;
    int step = field(bar, kStep) > 0 ? field(bar, kStep) : 1;
    int pos = field(bar, kPos) + notches * (int)lines * step;
    if (pos < field(bar, kMin)) pos = field(bar, kMin);
    if (pos > field(bar, kMax)) pos = field(bar, kMax);
    if (pos != field(bar, kPos)) {
        field(bar, kPos) = pos;
        field(bar, kDirty) = 1;
        g_notify(bar);
    }
    return true;
}

} // namespace rclan::scroll
