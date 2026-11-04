// rclan.asi - LAN multiplayer mod for 3-D Ultra Radio Control Racers Deluxe.
// Loaded by Ultimate ASI Loader (installed as winmm.dll next to RCRacers.exe).

#include "build.h"
#include "game.h"
#include "hooks.h"
#include "lanpanel.h"
#include "log.h"
#include "race.h"
#include "picker.h"
#include "wnd.h"

#include <windows.h>

#include <cstring>

namespace {

void check_exe()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    LOG("exe base %p, timestamp %#lx, image size %#lx -> %s build", (void*)base,
        (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage,
        rclan::build::name());
}

DWORD WINAPI worker(LPVOID)
{
    rclan::hooks::worker_loop();
    return 0;
}

void startup()
{
    // Log next to the game executable.
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (n && slash && (slash - path) + sizeof("\\rclan.log") < MAX_PATH)
        std::strcpy(slash, "\\rclan.log");
    else
        std::strcpy(path, "rclan.log");
    rclan::log::init(path);

    LOG("rclan.asi loaded (built " __DATE__ " " __TIME__ ")");
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    LOG("process: %s", path);
    if (GetModuleFileNameA(GetModuleHandleA("winmm.dll"), path, MAX_PATH))
        LOG("winmm.dll in use: %s", path);
    check_exe();

    rclan::hooks::install();
    rclan::game::init();
    rclan::race::install();
    rclan::picker::install();
    rclan::wnd::install();
    rclan::panel::install();

    // Creating a thread under the loader lock is fine; it just starts running
    // once DllMain returns.
    if (HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr))
        CloseHandle(t);
    LOG("init done");
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        startup();
    }
    return TRUE;
}
