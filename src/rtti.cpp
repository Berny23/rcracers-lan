#include "rtti.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace rclan::rtti {

namespace {

struct Image {
    const unsigned char* base = nullptr;
    size_t size = 0;
    const unsigned char* code = nullptr;
    size_t code_size = 0;
};

const Image& image()
{
    static Image img;
    if (img.base) return img;
    auto* b = reinterpret_cast<const unsigned char*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
    img.base = b;
    img.size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            img.code = b + s->VirtualAddress;
            img.code_size = s->Misc.VirtualSize;
            break;
        }
    return img;
}

// Every aligned dword in the image equal to `value`, starting after `from`.
const unsigned* find_dword(unsigned value, const unsigned* from)
{
    const Image& img = image();
    auto* p = from ? from + 1 : reinterpret_cast<const unsigned*>(img.base);
    auto* end = reinterpret_cast<const unsigned*>(img.base + img.size - 4);
    for (; p < end; ++p)
        if (*p == value) return p;
    return nullptr;
}

struct Cached {
    char name[64];
    void* const* vt;
};
Cached g_cache[32];
int g_ncache;

} // namespace

bool in_code(const void* p)
{
    const Image& img = image();
    auto* c = static_cast<const unsigned char*>(p);
    return c >= img.code && c < img.code + img.code_size;
}

void* const* vtable(const char* name)
{
    for (int i = 0; i < g_ncache; ++i)
        if (!std::strcmp(g_cache[i].name, name)) return g_cache[i].vt;

    const Image& img = image();
    char mangled[80];
    int n = std::snprintf(mangled, sizeof(mangled), ".?AV%s@@", name) + 1; // incl. NUL
    void* const* result = nullptr;
    for (size_t off = 0; off + n < img.size && !result; ++off) {
        if (img.base[off] != '.' || std::memcmp(img.base + off, mangled, n) != 0) continue;
        unsigned td = (unsigned)(uintptr_t)(img.base + off - 8); // TypeDescriptor
        // CompleteObjectLocator: signature 0, offset 0, cdOffset, pTypeDescriptor, ...
        for (const unsigned* p = find_dword(td, nullptr); p && !result; p = find_dword(td, p)) {
            const unsigned* col = p - 3;
            if (col[0] != 0 || col[1] != 0) continue; // the primary vtable only
            // vtable[-1] == &COL
            for (const unsigned* q = find_dword((unsigned)(uintptr_t)col, nullptr); q;
                 q = find_dword((unsigned)(uintptr_t)col, q)) {
                auto* vt = reinterpret_cast<void* const*>(q + 1);
                if (in_code(vt[0])) {
                    result = vt;
                    break;
                }
            }
        }
    }
    if (g_ncache < (int)(sizeof(g_cache) / sizeof(g_cache[0]))) {
        std::snprintf(g_cache[g_ncache].name, sizeof(g_cache[0].name), "%s", name);
        g_cache[g_ncache++].vt = result;
    }
    LOG("rtti: %s vtable %p", name, (void*)result);
    return result;
}

int find_objects(void* const* vt, void** out, int max)
{
    if (!vt) return 0;
    int found = 0;
    const unsigned v = (unsigned)(uintptr_t)vt;
    MEMORY_BASIC_INFORMATION mbi;
    for (auto* a = reinterpret_cast<unsigned char*>(0x10000);
         a < reinterpret_cast<unsigned char*>(0x7fff0000) &&
         VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi);
         a = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & PAGE_GUARD) ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
            continue;
        auto* p = static_cast<const unsigned*>(mbi.BaseAddress);
        auto* end = p + mbi.RegionSize / 4;
        for (; p < end; ++p)
            if (*p == v) {
                if (found < max) out[found] = const_cast<unsigned*>(p);
                ++found;
            }
    }
    return found < max ? found : max;
}

} // namespace rclan::rtti
