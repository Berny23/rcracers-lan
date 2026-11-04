#include "iat.h"

#include <cstring>

namespace rclan {

void** patch_iat(HMODULE module, const char* dll, const char* func,
                 void* hook, void** original)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        if (lstrcmpiA(reinterpret_cast<const char*>(base + imp->Name), dll) != 0)
            continue;
        if (!imp->OriginalFirstThunk) // names unavailable (bound/stripped import)
            continue;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), func) != 0)
                continue;

            auto** slot = reinterpret_cast<void**>(&slots->u1.Function);
            DWORD old;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return nullptr;
            if (original)
                *original = *slot;
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return slot;
        }
    }
    return nullptr;
}

} // namespace rclan
