#include "vtable.h"

#include "log.h"

#include <windows.h>

#include <cstring>

namespace rclan::vtable {

namespace {
constexpr int kMaxSlots = 48;
constexpr int kMaxVtbls = 32;

struct Entry {
    void** vtbl;
    void* orig[kMaxSlots];
};

Entry g_entries[kMaxVtbls];
int g_n = 0;
CRITICAL_SECTION g_cs;
const struct CsInit { CsInit() { InitializeCriticalSection(&g_cs); } } g_cs_init;
} // namespace

void* orig(void* self, int slot)
{
    void** vt = *static_cast<void***>(self);
    void* r = nullptr;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_n; ++i)
        if (g_entries[i].vtbl == vt) { r = g_entries[i].orig[slot]; break; }
    LeaveCriticalSection(&g_cs);
    return r;
}

void hook(void* obj, const char* what, int nslots,
          std::initializer_list<std::pair<int, void*>> slots)
{
    void** vt = *static_cast<void***>(obj);
    if (nslots > kMaxSlots) nslots = kMaxSlots;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_n; ++i)
        if (g_entries[i].vtbl == vt) { LeaveCriticalSection(&g_cs); return; }
    if (g_n == kMaxVtbls) {
        LeaveCriticalSection(&g_cs);
        LOG("vtable: registry full, not hooking %s vtbl %p", what, (void*)vt);
        return;
    }
    Entry& e = g_entries[g_n++];
    e.vtbl = vt;
    std::memset(e.orig, 0, sizeof(e.orig));
    std::memcpy(e.orig, vt, sizeof(void*) * nslots);
    DWORD old;
    VirtualProtect(vt, sizeof(void*) * nslots, PAGE_READWRITE, &old);
    for (auto& s : slots)
        vt[s.first] = s.second;
    VirtualProtect(vt, sizeof(void*) * nslots, old, &old);
    LeaveCriticalSection(&g_cs);
    LOG("vtable: hooked %s vtable %p", what, (void*)vt);
}

} // namespace rclan::vtable
