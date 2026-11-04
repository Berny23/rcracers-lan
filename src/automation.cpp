#include "automation.h"

#include "config.h"
#include "game.h"
#include "lanpanel.h"
#include "build.h"
#include "log.h"
#include "race.h"
#include "rtti.h"
#include "wnd.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rclan::automation {

using build::A;

static int g_enabled = -1;

bool enabled()
{
    if (g_enabled < 0) g_enabled = config::get_int("Debug", "Automation", 0) != 0;
    return g_enabled;
}

static void post_mouse(HWND target, int x, int y, bool click)
{
    POINT pt = {x, y};
    ClientToScreen(target, &pt);
    SetCursorPos(pt.x, pt.y);
    LPARAM lp = MAKELPARAM(x, y);
    PostMessageA(target, WM_MOUSEMOVE, 0, lp);
    if (click) {
        PostMessageA(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageA(target, WM_LBUTTONUP, 0, lp);
    }
}

// netrace TRACK SEED BOTS: a lockstep race with only the local player (tests
// determinism: same seed + no input must give the same hashes every run).
static int g_nr_track, g_nr_bots;
static unsigned g_nr_seed;
static void netrace_gui(void*)
{
    game::Car car;
    if (!game::local_car(&car)) return;
    static race::Start s;
    std::memset(&s, 0, sizeof(s));
    s.opt = {game::ModeSingle, 1, g_nr_track, 1, 1, 0, 0, 2, 0, g_nr_seed};
    const char* names[] = {"Test"};
    s.ncars = game::build_table(s.opt, &car, names, g_nr_bots, true, s.entries);
    for (int i = 0; i < game::kMaxCars; ++i) s.slot_of_car[i] = i == 0 ? 0 : -1;
    s.sim_seed = g_nr_seed * 2654435761u;
    race::begin(s, 0, nullptr, nullptr);
}

static void run(char* line)
{
    char cmd[32] = "";
    int a = 0, b = 0;
    if (sscanf(line, "%31s", cmd) != 1) return;
    char* args = line + std::strlen(cmd);

    if (!std::strcmp(cmd, "click") || !std::strcmp(cmd, "move") || !std::strcmp(cmd, "clickmain")) {
        if (sscanf(args, "%d %d", &a, &b) != 2) return;
        HWND target = !std::strcmp(cmd, "clickmain") ? wnd::main_window() : wnd::content_window();
        if (!target) return;
        if (target == wnd::main_window()) { // deliver to the child window under the point
            POINT p = {a, b};
            HWND child = ChildWindowFromPointEx(target, p, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
            if (child && child != target) {
                MapWindowPoints(target, child, &p, 1);
                target = child;
                a = p.x;
                b = p.y;
            }
        }
        LOG("automation: %s %d %d", cmd, a, b);
        post_mouse(target, a, b, std::strcmp(cmd, "move") != 0);
    } else if (!std::strcmp(cmd, "key")) {
        unsigned vk = std::strtoul(args, nullptr, 16);
        HWND target = wnd::main_window();
        if (!target || !vk) return;
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti); // the window with keyboard focus
        if (GetGUIThreadInfo(GetWindowThreadProcessId(target, nullptr), &gti) && gti.hwndFocus)
            target = gti.hwndFocus;
        LOG("automation: key %02X", vk);
        UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        PostMessageA(target, WM_KEYDOWN, vk, 1 | (scan << 16));
        PostMessageA(target, WM_KEYUP, vk, 1 | (scan << 16) | 0xC0000000u);
    } else if (!std::strcmp(cmd, "menudump")) {
        HMENU bar = wnd::game_menu();
        LOG("automation: menu bar %p", (void*)bar);
        for (int i = 0; bar && i < GetMenuItemCount(bar); ++i) {
            char text[64] = "";
            GetMenuStringA(bar, i, text, sizeof(text), MF_BYPOSITION);
            LOG("  [%d] %s", i, text);
            HMENU sub = GetSubMenu(bar, i);
            for (int j = 0; sub && j < GetMenuItemCount(sub); ++j) {
                GetMenuStringA(sub, j, text, sizeof(text), MF_BYPOSITION);
                LOG("      id=%u %s", GetMenuItemID(sub, j), text);
            }
        }
    } else if (!std::strcmp(cmd, "command")) { // WM_COMMAND to the main window (menu path)
        unsigned id = std::strtoul(args, nullptr, 10);
        if (!wnd::main_window() || !id) return;
        LOG("automation: WM_COMMAND %u", id);
        PostMessageA(wnd::main_window(), WM_COMMAND, MAKEWPARAM(id, 0), 0);
    } else if (!std::strcmp(cmd, "uidump")) {
        // RE aid: the game's UI object ring (0x1505df0 in the German build).
        int** head = reinterpret_cast<int**>(A(0x1505df0));
        int* p = *head;
        for (int n = 0; p && n < 64; ++n) {
            char line[512];
            int len = snprintf(line, sizeof(line), "  obj %p vt=%08x:", (void*)p, (unsigned)p[0]);
            for (int i = 1; i < 0x20 && len < (int)sizeof(line) - 12; ++i)
                len += snprintf(line + len, sizeof(line) - len, " %x", (unsigned)p[i]);
            LOG("%s", line);
            p = reinterpret_cast<int*>(p[2]);
            if (p == *head) break;
        }
    } else if (!std::strcmp(cmd, "peek")) { // peek ADDR COUNT: dump dwords
        unsigned addr = 0, count = 16;
        sscanf(args, "%x %u", &addr, &count);
        if (!addr || IsBadReadPtr((void*)addr, count * 4)) return;
        char line[1024];
        int len = snprintf(line, sizeof(line), "  %08x:", addr);
        for (unsigned i = 0; i < count && len < (int)sizeof(line) - 12; ++i)
            len += snprintf(line + len, sizeof(line) - len, " %x", ((unsigned*)addr)[i]);
        LOG("%s", line);
    } else if (!std::strcmp(cmd, "objs")) { // objs CLASS [DWORDS]: live objects of an RTTI class
        char name[64] = "";
        unsigned count = 0x30;
        if (sscanf(args, "%63s %x", name, &count) < 1) return;
        void* objs[32];
        int n = rtti::find_objects(rtti::vtable(name), objs, 32);
        LOG("automation: %d %s objects", n, name);
        for (int k = 0; k < n; ++k) {
            const unsigned* o = static_cast<const unsigned*>(objs[k]);
            char line[2048];
            int len = snprintf(line, sizeof(line), "  %p:", objs[k]);
            for (unsigned i = 1; i < count && len < (int)sizeof(line) - 12; ++i)
                len += snprintf(line + len, sizeof(line) - len, " %x", o[i]);
            LOG("%s", line);
        }
    } else if (!std::strcmp(cmd, "netrace")) {
        g_nr_track = 0; g_nr_seed = 1; g_nr_bots = 3;
        sscanf(args, "%d %u %d", &g_nr_track, &g_nr_seed, &g_nr_bots);
        LOG("automation: netrace track %d seed %u bots %d", g_nr_track, g_nr_seed, g_nr_bots);
        wnd::run_on_gui(netrace_gui, nullptr);
    } else if (!std::strcmp(cmd, "lan")) { // lan host | join ADDR | start | opt ROW DIR | leave
        static char lan_cmd[128];
        while (*args == ' ') ++args;
        snprintf(lan_cmd, sizeof(lan_cmd), "%s", args);
        LOG("automation: lan %s", lan_cmd);
        wnd::run_on_gui([](void* a) { panel::command(static_cast<const char*>(a)); }, lan_cmd);
    } else if (!std::strcmp(cmd, "log")) {
        while (*args == ' ') ++args;
        LOG("automation: --- %s ---", args);
    }
}

void poll()
{
    if (!enabled()) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%srclan_cmd.txt", config::game_dir());
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char line[256];
    char lines[32][256];
    int n = 0;
    while (n < 32 && fgets(line, sizeof(line), f)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        std::strcpy(lines[n++], line);
    }
    fclose(f);
    DeleteFileA(path);
    for (int i = 0; i < n; ++i) {
        run(lines[i]);
        Sleep(150); // let the game process each step
    }
}

} // namespace rclan::automation
