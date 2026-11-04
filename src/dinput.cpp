// DirectInput (ANSI, DX3-7 era) hooks via COM vtable patching.
//
// The game loads DINPUT.DLL at runtime and fetches DirectInputCreateA via
// GetProcAddress. We hand it a wrapper, then patch the vtables of the objects
// it receives. No game addresses are involved, so this works on any build.
//
// Vtable layouts (identical for IDirectInputDeviceA/2A/7A, prefix-compatible):
//   IDirectInputA:       0 QI, 1 AddRef, 2 Release, 3 CreateDevice, ...
//   IDirectInputDeviceA: 0 QI, ... 9 GetDeviceState, 10 GetDeviceData,
//                        11 SetDataFormat, ...

#include "dinput.h"

#include "log.h"
#include "vtable.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace rclan::dinput {

#define CALLER() __builtin_return_address(0)

// ---------------------------------------------------------------------------
// Minimal DirectInput definitions (avoids dinput.h / dxguid)

struct DIDATAFORMAT_ {
    DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs;
    void* rgodf;
};

static const GUID kSysMouse    = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

enum Slot { QI = 0, CreateDevice = 3, GetDeviceState = 9, GetDeviceData = 10, SetDataFormat = 11, kSlots = 18 };

using DirectInputCreateA_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, void*);
using QI_t             = HRESULT(WINAPI*)(void*, REFIID, void**);
using CreateDevice_t   = HRESULT(WINAPI*)(void*, REFGUID, void**, void*);
using GetDeviceState_t = HRESULT(WINAPI*)(void*, DWORD, void*);
using GetDeviceData_t  = HRESULT(WINAPI*)(void*, DWORD, void*, DWORD*, DWORD);
using SetDataFormat_t  = HRESULT(WINAPI*)(void*, const DIDATAFORMAT_*);

static DirectInputCreateA_t o_DirectInputCreateA;
static CRITICAL_SECTION g_cs;
static const struct CsInit { CsInit() { InitializeCriticalSection(&g_cs); } } g_cs_init;
static std::atomic<bool> g_active{false}; // DirectInputCreateA was wrapped

static void lock()   { EnterCriticalSection(&g_cs); }
static void unlock() { LeaveCriticalSection(&g_cs); }

static void fmt_guid(const GUID& g, char* out, size_t n)
{
    snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
             g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// ---------------------------------------------------------------------------
// Device registry

enum class Kind { Keyboard, Mouse, Other };
static const char* kind_name(Kind k)
{
    return k == Kind::Keyboard ? "keyboard" : k == Kind::Mouse ? "mouse" : "joystick/other";
}

struct Device {
    int id;
    Kind kind;
    void* ifaces[4]; // the object plus QueryInterface'd aliases
    int n_ifaces;
    DWORD data_size;
    bool logged_state, logged_data;
    BYTE last_kbd[256];
};
static Device g_devs[16];
static int g_ndevs = 0;

static Device* find_dev(void* iface)
{
    for (int i = 0; i < g_ndevs; ++i)
        for (int j = 0; j < g_devs[i].n_ifaces; ++j)
            if (g_devs[i].ifaces[j] == iface) return &g_devs[i];
    return nullptr;
}


// ---------------------------------------------------------------------------
// Device hooks

static HRESULT WINAPI h_dev_GetDeviceState(void* self, DWORD size, void* data)
{
    auto o = vtable::orig_as<GetDeviceState_t>(self, GetDeviceState);
    HRESULT hr = o(self, size, data);

    lock();
    Device* d = find_dev(self);
    if (d && !d->logged_state) {
        d->logged_state = true;
        LOG("dinput: dev#%d (%s) first GetDeviceState(size=%lu) -> %#lx  [caller %p]", d->id,
            kind_name(d->kind), (unsigned long)size, (unsigned long)hr, CALLER());
    }
    // Log keyboard state changes as the list of held DIK scan codes.
    if (d && d->kind == Kind::Keyboard && size == 256 && SUCCEEDED(hr)) {
        auto* keys = static_cast<const BYTE*>(data);
        bool changed = false;
        for (int i = 0; i < 256; ++i)
            if ((keys[i] ^ d->last_kbd[i]) & 0x80) { changed = true; break; }
        if (changed) {
            char buf[512];
            int n = 0;
            for (int i = 0; i < 256 && n < (int)sizeof(buf) - 6; ++i)
                if (keys[i] & 0x80) n += snprintf(buf + n, sizeof(buf) - n, " %02X", i);
            buf[n] = '\0';
            LOG("dinput: dev#%d keys held:%s", d->id, n ? buf : " (none)");
            std::memcpy(d->last_kbd, keys, 256);
        }
    }
    unlock();
    return hr;
}

static HRESULT WINAPI h_dev_GetDeviceData(void* self, DWORD objsize, void* rgdod, DWORD* inout, DWORD flags)
{
    auto o = vtable::orig_as<GetDeviceData_t>(self, GetDeviceData);
    HRESULT hr = o(self, objsize, rgdod, inout, flags);

    lock();
    Device* d = find_dev(self);
    if (d && !d->logged_data) {
        d->logged_data = true;
        LOG("dinput: dev#%d (%s) first GetDeviceData(objsize=%lu, flags=%#lx) -> %#lx  [caller %p]",
            d->id, kind_name(d->kind), (unsigned long)objsize, (unsigned long)flags,
            (unsigned long)hr, CALLER());
    }
    unlock();
    return hr;
}

static HRESULT WINAPI h_dev_SetDataFormat(void* self, const DIDATAFORMAT_* fmt)
{
    auto o = vtable::orig_as<SetDataFormat_t>(self, SetDataFormat);
    HRESULT hr = o(self, fmt);
    lock();
    Device* d = find_dev(self);
    if (d && fmt) d->data_size = fmt->dwDataSize;
    unlock();
    LOG("dinput: dev#%d SetDataFormat(dataSize=%lu, objs=%lu) -> %#lx", d ? d->id : -1,
        fmt ? (unsigned long)fmt->dwDataSize : 0, fmt ? (unsigned long)fmt->dwNumObjs : 0,
        (unsigned long)hr);
    return hr;
}

static void hook_device_iface(void* dev);

// Device2A etc. are obtained via QueryInterface; make sure their vtable is
// hooked too and that they map to the same Device record.
static HRESULT WINAPI h_dev_QI(void* self, REFIID riid, void** out)
{
    auto o = vtable::orig_as<QI_t>(self, QI);
    HRESULT hr = o(self, riid, out);
    if (SUCCEEDED(hr) && out && *out && *out != self) {
        lock();
        Device* d = find_dev(self);
        if (d && !find_dev(*out) && d->n_ifaces < 4) d->ifaces[d->n_ifaces++] = *out;
        unlock();
        char g[40];
        fmt_guid(riid, g, sizeof(g));
        LOG("dinput: dev#%d QueryInterface(%s) -> %p", d ? d->id : -1, g, *out);
        hook_device_iface(*out);
    }
    return hr;
}

static void hook_device_iface(void* dev)
{
    vtable::hook(dev, "IDirectInputDeviceA", kSlots,
              {{QI, (void*)&h_dev_QI},
               {GetDeviceState, (void*)&h_dev_GetDeviceState},
               {GetDeviceData, (void*)&h_dev_GetDeviceData},
               {SetDataFormat, (void*)&h_dev_SetDataFormat}});
}

// ---------------------------------------------------------------------------
// IDirectInputA hooks

static HRESULT WINAPI h_di_CreateDevice(void* self, REFGUID guid, void** out, void* outer)
{
    auto o = vtable::orig_as<CreateDevice_t>(self, CreateDevice);
    HRESULT hr = o(self, guid, out, outer);

    char g[40];
    fmt_guid(guid, g, sizeof(g));
    Kind kind = std::memcmp(&guid, &kSysKeyboard, sizeof(GUID)) == 0 ? Kind::Keyboard
              : std::memcmp(&guid, &kSysMouse, sizeof(GUID)) == 0    ? Kind::Mouse
                                                                     : Kind::Other;
    int id = -1;
    if (SUCCEEDED(hr) && out && *out) {
        lock();
        if (g_ndevs < (int)(sizeof(g_devs) / sizeof(g_devs[0]))) {
            Device& d = g_devs[g_ndevs];
            std::memset(&d, 0, sizeof(d));
            d.id = id = g_ndevs++;
            d.kind = kind;
            d.ifaces[0] = *out;
            d.n_ifaces = 1;
        }
        unlock();
        hook_device_iface(*out);
    }
    LOG("dinput: CreateDevice(%s = %s) -> %#lx, dev#%d %p  [caller %p]", g, kind_name(kind),
        (unsigned long)hr, id, out ? *out : nullptr, CALLER());
    return hr;
}

static HRESULT WINAPI h_DirectInputCreateA(HINSTANCE inst, DWORD version, void** out, void* outer)
{
    HRESULT hr = o_DirectInputCreateA(inst, version, out, outer);
    LOG("dinput: DirectInputCreateA(version=%#lx) -> %#lx, %p  [caller %p]",
        (unsigned long)version, (unsigned long)hr, out ? *out : nullptr, CALLER());
    if (SUCCEEDED(hr) && out && *out)
        vtable::hook(*out, "IDirectInputA", 8, {{CreateDevice, (void*)&h_di_CreateDevice}});
    return hr;
}

// ---------------------------------------------------------------------------

FARPROC maybe_wrap(LPCSTR name, FARPROC real)
{
    if (!real || !HIWORD((UINT_PTR)name) || std::strcmp(name, "DirectInputCreateA") != 0)
        return real;
    g_active = true;
    o_DirectInputCreateA = reinterpret_cast<DirectInputCreateA_t>(reinterpret_cast<void*>(real));
    return reinterpret_cast<FARPROC>(reinterpret_cast<void*>(&h_DirectInputCreateA));
}

} // namespace rclan::dinput
