// IAT hooks on RCRacers.exe.
//
// Everything here is an IAT hook on RCRacers.exe only, so calls made by our
// own module (and by system DLLs) still go straight to the real functions.
// The game's clock (timeGetTime / GetTickCount) goes through here, so network
// races can run the simulation on their own schedule; the input and message
// hooks feed the race, the menu bar and the debug log.

#include "hooks.h"

#include "automation.h"
#include "config.h"
#include "dinput.h"
#include "iat.h"
#include "log.h"
#include "menubar.h"
#include "race.h"
#include "scroll.h"
#include "wnd.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mmsystem.h>


namespace rclan::hooks {

// ---------------------------------------------------------------------------
// Original function pointers (taken from the game's IAT)

using timeGetTime_t      = DWORD(WINAPI*)();
using GetTickCount_t     = DWORD(WINAPI*)();
using timeSetEvent_t     = MMRESULT(WINAPI*)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
using SetTimer_t         = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC);
using joyGetPosEx_t      = MMRESULT(WINAPI*)(UINT, LPJOYINFOEX);
using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
using GetKeyState_t      = SHORT(WINAPI*)(int);
using LoadLibraryA_t     = HMODULE(WINAPI*)(LPCSTR);
using GetProcAddress_t   = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using GetMessageA_t      = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
using PeekMessageA_t     = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

static timeGetTime_t      o_timeGetTime;
static GetTickCount_t     o_GetTickCount;
static timeSetEvent_t     o_timeSetEvent;
static SetTimer_t         o_SetTimer;
static joyGetPosEx_t      o_joyGetPosEx;
static GetAsyncKeyState_t o_GetAsyncKeyState;
static GetKeyState_t      o_GetKeyState;
static LoadLibraryA_t     o_LoadLibraryA;
static GetProcAddress_t   o_GetProcAddress;
static GetMessageA_t      o_GetMessageA;
static PeekMessageA_t     o_PeekMessageA;

// ---------------------------------------------------------------------------
// Logging helpers

// Remembers distinct values (caller addresses, virtual keys, ...) so each one
// is only logged once.
struct SeenSet {
    static constexpr int kMax = 128;
    CRITICAL_SECTION cs;
    const void* items[kMax];
    int count = 0;

    SeenSet() { InitializeCriticalSection(&cs); }

    // True the first time `p` is seen (until the set is full).
    bool first(const void* p)
    {
        EnterCriticalSection(&cs);
        bool fresh = true;
        for (int i = 0; i < count; ++i)
            if (items[i] == p) { fresh = false; break; }
        if (fresh) {
            if (count < kMax) items[count++] = p;
            else fresh = false;
        }
        LeaveCriticalSection(&cs);
        return fresh;
    }
};

static SeenSet seen_tick_callers, seen_joy_callers;
static SeenSet seen_async_keys, seen_keystate_keys, seen_msg_callers;

#define CALLER() __builtin_return_address(0)

// ---------------------------------------------------------------------------
// Game clock: timeGetTime as the game sees it.

namespace vclock {
static DWORD tick_offset;

static void init()
{
    // GetTickCount has a different epoch than timeGetTime; keep it.
    tick_offset = GetTickCount() - o_timeGetTime();
}

static DWORD now() { return o_timeGetTime(); }
} // namespace vclock

// ---------------------------------------------------------------------------
// Hooks

static DWORD WINAPI h_timeGetTime()
{
    return vclock::now();
}

static DWORD WINAPI h_GetTickCount()
{
    if (seen_tick_callers.first(CALLER()))
        LOG("GetTickCount: new caller %p", CALLER());
    if (!o_timeGetTime) return o_GetTickCount();
    return vclock::now() + vclock::tick_offset;
}

static MMRESULT WINAPI h_timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK cb,
                                      DWORD_PTR user, UINT flags)
{
    MMRESULT id = o_timeSetEvent(delay, res, cb, user, flags);
    LOG("timeSetEvent(delay=%u ms, res=%u, cb=%p, user=%#lx, flags=%#x) -> id %u  [caller %p]",
        delay, res, (void*)cb, (unsigned long)user, flags, id, CALLER());
    return id;
}

static UINT_PTR WINAPI h_SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC proc)
{
    UINT_PTR r = o_SetTimer(hwnd, id, elapse, proc);
    LOG("SetTimer(hwnd=%p, id=%lu, elapse=%u ms, proc=%p) -> %lu  [caller %p]",
        (void*)hwnd, (unsigned long)id, elapse, (void*)proc, (unsigned long)r, CALLER());
    return r;
}

static MMRESULT WINAPI h_joyGetPosEx(UINT joy, LPJOYINFOEX info)
{
    if (seen_joy_callers.first(CALLER()))
        LOG("joyGetPosEx(joy=%u): new caller %p", joy, CALLER());
    return o_joyGetPosEx(joy, info);
}

static SHORT WINAPI h_GetAsyncKeyState(int vk)
{
    if (seen_async_keys.first((const void*)(UINT_PTR)vk))
        LOG("GetAsyncKeyState: first query of VK 0x%02X  [caller %p]", vk, CALLER());
    return o_GetAsyncKeyState(vk);
}

static SHORT WINAPI h_GetKeyState(int vk)
{
    if (seen_keystate_keys.first((const void*)(UINT_PTR)vk))
        LOG("GetKeyState: first query of VK 0x%02X  [caller %p]", vk, CALLER());
    return o_GetKeyState(vk);
}

// Keyboard input reaches the game as posted WM_KEYDOWN/WM_KEYUP messages
// (the game's event dispatcher switches on message IDs). Every posted message
// passes through GetMessageA/PeekMessageA(PM_REMOVE) - this is where remote
// input will be injected and local input filtered later.
static void on_message_retrieved(const MSG* m, const char* api, const void* caller)
{
    if (seen_msg_callers.first(caller))
        LOG("%s: new message-loop caller %p", api, caller);
    switch (m->message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        const char* kind = m->message == WM_KEYDOWN   ? "KEYDOWN"
                         : m->message == WM_KEYUP     ? "KEYUP"
                         : m->message == WM_SYSKEYDOWN ? "SYSKEYDOWN" : "SYSKEYUP";
        bool repeat = m->message == WM_KEYDOWN && (m->lParam & 0x40000000);
        if (!repeat) // auto-repeat floods the log while a key is held
            LOG("%s %-10s vk=0x%02X scan=0x%02X%s time=%lu hwnd=%p", api, kind,
                (unsigned)m->wParam, (unsigned)((m->lParam >> 16) & 0x1FF),
                (m->lParam & (1 << 24)) ? " (ext)" : "", (unsigned long)m->time, (void*)m->hwnd);
        break;
    }
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP:
        LOG("%s %s hit=%u at %d,%d hwnd=%p", api, m->message == WM_NCLBUTTONDOWN ? "NCLBUTTONDOWN" : "NCLBUTTONUP",
            (unsigned)m->wParam, (short)LOWORD(m->lParam), (short)HIWORD(m->lParam), (void*)m->hwnd);
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        LOG("%s %s at %d,%d hwnd=%p", api,
            m->message == WM_LBUTTONDOWN ? "LBUTTONDOWN" : m->message == WM_LBUTTONUP ? "LBUTTONUP"
            : m->message == WM_RBUTTONDOWN ? "RBUTTONDOWN" : "RBUTTONUP",
            (short)LOWORD(m->lParam), (short)HIWORD(m->lParam), (void*)m->hwnd);
        break;
    }
}

static BOOL WINAPI h_GetMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max)
{
    BOOL r = o_GetMessageA(msg, hwnd, min, max);
    if (r > 0) {
        on_message_retrieved(msg, "GetMessageA", CALLER());
        if (menubar::key_filter(msg, true) || scroll::on_message(msg) || race::filter_message(msg))
            msg->message = WM_NULL;
    }
    return r;
}

static BOOL WINAPI h_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
    BOOL r = o_PeekMessageA(msg, hwnd, min, max, flags);
    if (r && (flags & PM_REMOVE)) on_message_retrieved(msg, "PeekMessageA", CALLER());
    if (r && menubar::key_filter(msg, (flags & PM_REMOVE) != 0)) msg->message = WM_NULL; // keys for our menu
    else if (r && (flags & PM_REMOVE) && (scroll::on_message(msg) || race::filter_message(msg)))
        msg->message = WM_NULL;    // wheel on lists
    return r;
}

static HMODULE WINAPI h_LoadLibraryA(LPCSTR name)
{
    HMODULE m = o_LoadLibraryA(name);
    LOG("LoadLibraryA(\"%s\") -> %p  [caller %p]", name ? name : "(null)", (void*)m, CALLER());
    return m;
}

static FARPROC WINAPI h_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC real = o_GetProcAddress(mod, name);
    FARPROC p = dinput::maybe_wrap(name, real);
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(mod, path, sizeof(path));
    const char* file = path;
    for (const char* c = path; *c; ++c)
        if (*c == '\\' || *c == '/') file = c + 1;
    if (HIWORD((UINT_PTR)name))
        LOG("GetProcAddress(%s, \"%s\") -> %p%s  [caller %p]", file, name, (void*)real,
            p != real ? " (wrapped)" : "", CALLER());
    else
        LOG("GetProcAddress(%s, #%u) -> %p  [caller %p]", file, LOWORD((UINT_PTR)name),
            (void*)p, CALLER());
    return p;
}

// ---------------------------------------------------------------------------
// Installation

struct HookDef {
    const char* dll;
    const char* func;
    void* hook;
    void** original;
    void** slot; // filled in by install()
};

#define HOOK(dll, fn) { dll, #fn, (void*)&h_##fn, (void**)&o_##fn, nullptr }
// [Game] SkipIntro=1: the game plays Intro.avi only if the file exists, so
// report it as missing.
using GetFileAttributesA_t = DWORD(WINAPI*)(LPCSTR);
static GetFileAttributesA_t o_GetFileAttributesA;

static DWORD WINAPI h_GetFileAttributesA(LPCSTR path)
{
    static int skip = -1;
    if (skip < 0) skip = config::get_int("Game", "SkipIntro", 0) != 0;
    if (skip && path) {
        const char* base = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
        if (!lstrcmpiA(base, "intro.avi")) {
            LOG("skipping intro video (%s)", path);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
    }
    return o_GetFileAttributesA(path);
}

// [Window] WindowedRace=1: the game's "-window" switch renders races in the
// normal game window (like the attract demo) instead of switching the display
// mode, which Wine shows as a small picture in the top-left corner.
using GetCommandLineA_t = LPSTR(WINAPI*)();
static GetCommandLineA_t o_GetCommandLineA;
static LPSTR WINAPI h_GetCommandLineA()
{
    static char line[1024];
    LPSTR orig = o_GetCommandLineA();
    if (!config::get_int("Window", "WindowedRace", 1) || !orig || strstr(orig, "-window")) return orig;
    if (!line[0]) {
        snprintf(line, sizeof(line), "%s -window", orig);
        LOG("command line: %s", line);
    }
    return line;
}

// The game's message pump sleeps in GetMessage while it has no active window
// (another program focused), which would freeze a network race for everyone:
// during one, the game always sees its window as active.
using GetActiveWindow_t = HWND(WINAPI*)();
static GetActiveWindow_t o_GetActiveWindow;
static HWND WINAPI h_GetActiveWindow()
{
    HWND h = o_GetActiveWindow();
    if (!h && rclan::race::active()) return rclan::wnd::main_window();
    return h;
}

static HookDef g_hooks[] = {
    HOOK("WINMM.dll",    timeGetTime),
    HOOK("KERNEL32.dll", GetTickCount),
    HOOK("WINMM.dll",    timeSetEvent),
    HOOK("USER32.dll",   SetTimer),
    HOOK("WINMM.dll",    joyGetPosEx),
    HOOK("USER32.dll",   GetAsyncKeyState),
    HOOK("USER32.dll",   GetKeyState),
    HOOK("KERNEL32.dll", LoadLibraryA),
    HOOK("KERNEL32.dll", GetProcAddress),
    HOOK("USER32.dll",   GetMessageA),
    HOOK("USER32.dll",   PeekMessageA),
    HOOK("KERNEL32.dll", GetFileAttributesA),
    HOOK("KERNEL32.dll", GetCommandLineA),
    HOOK("USER32.dll",   GetActiveWindow),
};
#undef HOOK

void install()
{
    HMODULE exe = GetModuleHandleA(nullptr);

    // timeGetTime first: the virtual clock needs the original pointer, and
    // GetTickCount's hook depends on the clock.
    for (auto& h : g_hooks) {
        h.slot = patch_iat(exe, h.dll, h.func, h.hook, h.original);
        if (h.slot)
            LOG("hooked %s!%s  (IAT slot %p, original %p)", h.dll, h.func, (void*)h.slot, *h.original);
        else
            LOG("FAILED to hook %s!%s", h.dll, h.func);
        if (h.slot && h.hook == (void*)&h_timeGetTime)
            vclock::init();
    }
}

// Detects if something (e.g. the ASI loader restoring its own IAT patches)
// overwrote one of our hooks after install().
static void verify_hooks()
{
    for (auto& h : g_hooks)
        if (h.slot && *h.slot != h.hook)
            LOG("WARNING: hook %s!%s was overwritten (slot now %p)", h.dll, h.func, *h.slot);
}

// ---------------------------------------------------------------------------
// Worker thread: automation commands and GUI state diagnostics; checks once
// after a few seconds that no other code replaced our hooks.

void worker_loop()
{
    unsigned tick = 0;
    for (;;) {
        Sleep(100);
        ++tick;
        automation::poll();
        wnd::poll_state();
        if (tick == 50) verify_hooks();
    }
}

} // namespace rclan::hooks
