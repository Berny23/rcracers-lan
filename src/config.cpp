#include "config.h"

#include <windows.h>

#include <cstring>

namespace rclan::config {

static char g_ini[MAX_PATH];
static char g_game_dir[MAX_PATH];

static void dir_of(HMODULE mod, char* out)
{
    DWORD n = GetModuleFileNameA(mod, out, MAX_PATH);
    char* slash = std::strrchr(out, '\\');
    if (n && slash) slash[1] = '\0';
    else out[0] = '\0';
}

static const char* ini()
{
    if (!g_ini[0]) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ini), &self);
        dir_of(self, g_ini);
        std::strncat(g_ini, "rclan.ini", MAX_PATH - std::strlen(g_ini) - 1);
    }
    return g_ini;
}

int get_int(const char* section, const char* key, int def)
{
    return (int)GetPrivateProfileIntA(section, key, def, ini());
}

void get_str(const char* section, const char* key, const char* def, char* out, int n)
{
    GetPrivateProfileStringA(section, key, def, out, n, ini());
}

void set_str(const char* section, const char* key, const char* value)
{
    WritePrivateProfileStringA(section, key, value, ini());
}

const char* game_dir()
{
    if (!g_game_dir[0]) dir_of(nullptr, g_game_dir);
    return g_game_dir;
}

} // namespace rclan::config
