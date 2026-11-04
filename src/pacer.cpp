#include "pacer.h"

#include "config.h"
#include "log.h"

#include <atomic>

namespace rclan::pacer {

namespace {

HWND g_pixel;
std::atomic<bool> g_active{false};
double g_period_ms = 1000.0 / 120.0;
bool g_phase;

LRESULT CALLBACK pixel_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        g_phase = !g_phase;
        HBRUSH b = CreateSolidBrush(g_phase ? RGB(0, 0, 0) : RGB(2, 2, 2));
        RECT r = {0, 0, 1, 1};
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcA(h, m, w, l);
}

DWORD WINAPI pacer_thread(void*)
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    const double ticks_per_ms = freq.QuadPart / 1000.0;
    double next = 0;
    for (;;) {
        if (!g_active.load(std::memory_order_relaxed)) {
            Sleep(50);
            next = 0;
            continue;
        }
        QueryPerformanceCounter(&now);
        double t = now.QuadPart / ticks_per_ms;
        if (next == 0 || t - next > 4 * g_period_ms) next = t; // start / resync after a stall
        next += g_period_ms;
        double wait = next - t;
        if (wait > 0) Sleep((DWORD)wait);
        InvalidateRect(g_pixel, nullptr, FALSE);
    }
}

void create(HWND main)
{
    // Repaint at least twice per display refresh (and >= 240/s in case the
    // reported rate is wrong): a frame is always ready even if Sleep() is a
    // millisecond late.
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    int hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
    if (hz <= 1) hz = 60;
    if (hz > 360) hz = 360;
    g_period_ms = 1000.0 / (2 * hz > 240 ? 2.0 * hz : 240.0);

    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = pixel_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "RCLanPacer";
    RegisterClassA(&wc);
    g_pixel = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, "RCLanPacer", "", WS_CHILD, 0, 0, 1, 1, main,
                              nullptr, inst, nullptr);
    CreateThread(nullptr, 0, pacer_thread, nullptr, 0, nullptr);
    LOG("pacer: display %d Hz, repainting every %.2f ms while menus are shown", hz, g_period_ms);
}

} // namespace

void place(HWND main, const RECT& client, bool active)
{
    static int enabled = -1;
    if (enabled < 0) enabled = config::get_int("Window", "SteadyRefresh", 1) != 0;
    if (!enabled || !main) return;
    if (!g_pixel) create(main);
    if (!g_pixel) return;
    if (active)
        SetWindowPos(g_pixel, HWND_TOP, client.left, client.bottom - 1, 1, 1,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
    else
        ShowWindow(g_pixel, SW_HIDE);
    g_active = active;
}

} // namespace rclan::pacer
