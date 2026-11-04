#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace rclan::log {

static CRITICAL_SECTION g_cs;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static DWORD g_start = 0;

void init(const char* path)
{
    InitializeCriticalSection(&g_cs);
    g_start = GetTickCount();
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void write(const char* fmt, ...)
{
    char buf[1024];
    DWORD ms = GetTickCount() - g_start;
    int n = std::snprintf(buf, sizeof(buf), "[%5lu.%03lu] ",
                          (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));

    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += (m < (int)sizeof(buf) - n - 2) ? m : (int)sizeof(buf) - n - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = '\0';

    EnterCriticalSection(&g_cs);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_file, buf, (DWORD)n, &written, nullptr);
    }
    LeaveCriticalSection(&g_cs);
    OutputDebugStringA(buf);
}

} // namespace rclan::log
