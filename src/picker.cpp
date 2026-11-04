#include "picker.h"

#include "config.h"
#include "game.h"
#include "build.h"
#include "log.h"

#include <cstdio>
#include <cstring>

namespace rclan::picker {

using build::A;

namespace {

template <typename T>
T& at(unsigned addr) { return *reinterpret_cast<T*>(addr); }

// Screen switch: fastcall(tbi file, command handler; init, context), ret 8.
// Every menu screen change goes through it.
const unsigned kFnScreen = A(0x402770);
const unsigned kCtx = A(0x47daa8); // the menu flow object passed to every screen
struct Screen {
    unsigned tbi, handler, init;
};
const Screen kMainMenu = {A(0x473538), A(0x405430), A(0x405260)};
const Screen kTrackSelect = {A(0x479180), A(0x43aed0), A(0x43a930)};
const Screen kCircuitSelect = {A(0x478508), A(0x42ec50), A(0x42e840)};
const Screen kCustomCircuit = {A(0x4784f4), A(0x42f4d0), A(0x42edb0)};
const Screen kBattleSelect = {A(0x479200), A(0x43b250), A(0x43af10)};

// Screen state.
const unsigned kTrackRow = A(0x14eb870);    // track screen: list row (preselect)
const unsigned kTrackChosen = A(0x4791ec);  // track screen: index into kSingleTracks, -1 none
const unsigned kRowToSlot = A(0x46ec20);    // row -> slot of the track name table
const unsigned kSlotNames = A(0x479214);    // slot (1..16) -> internal track name
const unsigned kSingleTracks = A(0x14eba98); // track list of the single race screen
const unsigned kCircuit = A(0x10e75a0);     // 0 amateur, 1 pro, 2 custom, 3 super
const unsigned kCircuitLists = A(0x10e7510); // 4 track lists of 0x1c bytes
const unsigned kDuelGame = A(0x14eb860);    // 0 soccer, 1 battle
// Player 1's championship progress, which the circuit screens reset and save.
const unsigned kProfile = A(0x10e7e10), kProgress = A(0x10e8328), kProgressSize = 0x896 * 4;
const unsigned kFnLoadProfile = A(0x4375e0), kFnSaveProfile = A(0x437630);

struct Sig {
    unsigned addr;
    unsigned char bytes[13];
    int n;
};
const Sig kSigs[] = {
    {0x402770, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x3b, 0xc8, 0x46, 0x00}, 13},
    {0x43a9aa, {0x8b, 0x0d, 0x70, 0xb8, 0x4e, 0x01}, 6},       // track init reads the row
    {0x43aa0b, {0xb9, 0x98, 0xba, 0x4e, 0x01}, 5},             // ... looks up kSingleTracks
    {0x42e8fb, {0x8b, 0x0d, 0xa0, 0x75, 0x0e, 0x01}, 6},       // circuit init reads kCircuit
    {0x42f630, {0xb9, 0x48, 0x75, 0x0e, 0x01}, 5},             // custom list object
    {0x43afc8, {0x8b, 0x0d, 0x60, 0xb8, 0x4e, 0x01}, 6},       // battle init reads the game
    {0x437630, {0x6a, 0xff, 0x68, 0x98, 0xce, 0x46, 0x00}, 7},
};

using FnScreen = void(__fastcall*)(unsigned tbi, unsigned handler, unsigned init, unsigned ctx);
FnScreen g_orig; // trampoline
bool g_ok;
unsigned g_current; // tbi of the current screen
bool g_active;
Kind g_kind;
unsigned char g_progress[kProgressSize];
bool g_progress_saved;

void switch_to(const Screen& s) { g_orig(s.tbi, s.handler, s.init, kCtx); }

bool picker_screen(unsigned tbi)
{
    switch (g_kind) {
    case Track: return tbi == kTrackSelect.tbi;
    case Circuit: return tbi == kCircuitSelect.tbi || tbi == kCustomCircuit.tbi;
    case Duel: return tbi == kBattleSelect.tbi;
    }
    return false;
}

void finish()
{
    g_active = false;
    if (g_progress_saved) {
        g_progress_saved = false;
        if (std::memcmp(&at<unsigned char>(kProgress), g_progress, kProgressSize) != 0) {
            std::memcpy(&at<unsigned char>(kProgress), g_progress, kProgressSize);
            reinterpret_cast<void(__fastcall*)(unsigned, int, int)>(kFnSaveProfile)(kProfile, 0, 1);
            LOG("picker: restored the championship progress of player 1");
        }
    }
    LOG("picker: closed");
}

void __fastcall screen_hook(unsigned tbi, unsigned handler, unsigned init, unsigned ctx)
{
    if (g_active && !picker_screen(tbi)) {
        // Hauptmenü, or Weiter on its way into a race: back to the main menu.
        if (tbi != kMainMenu.tbi) LOG("picker: going to the main menu instead of %s", (const char*)tbi);
        finish();
        tbi = kMainMenu.tbi, handler = kMainMenu.handler, init = kMainMenu.init, ctx = kCtx;
    }
    g_current = tbi;
    g_orig(tbi, handler, init, ctx);
}

// Track list object: +0x14 descriptors (0xf8 bytes, id first), +0x18 count.
int list_count(unsigned list) { return at<int>(list + 0x18); }
const char* list_id(unsigned list, int i) { return at<const char*>(list + 0x14) + i * 0xf8; }

int track_by_id(const char* id)
{
    for (int t = 0; t < game::track_count(); ++t)
        if (!lstrcmpiA(game::track_id(t), id)) return t;
    return -1;
}

} // namespace

void install()
{
    if (!game::available()) return;
    for (const Sig& s : kSigs)
        if (!build::matches(s.addr, s.bytes, s.n)) {
            LOG("picker: code at %#x doesn't match - game screens not used as pickers", s.addr);
            return;
        }
    // Trampoline: the first 6 bytes (mov eax, fs:[0]), then back into the function.
    auto* tramp = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    auto* fn = reinterpret_cast<unsigned char*>(kFnScreen);
    std::memcpy(tramp, fn, 6);
    tramp[6] = 0xE9;
    int rel = (int)((UINT_PTR)(fn + 6) - (UINT_PTR)(tramp + 11));
    std::memcpy(tramp + 7, &rel, 4);
    g_orig = reinterpret_cast<FnScreen>(tramp);

    DWORD old;
    if (!VirtualProtect(fn, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    fn[0] = 0xE9;
    rel = (int)((UINT_PTR)&screen_hook - (UINT_PTR)(fn + 5));
    std::memcpy(fn + 1, &rel, 4);
    fn[5] = 0x90;
    VirtualProtect(fn, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 6);
    g_ok = true;
    LOG("picker: screen switch hooked");
}

bool available() { return g_ok; }
bool at_main_menu() { return g_ok && g_current == kMainMenu.tbi && !game::race_running(); }
bool active() { return g_active; }

bool open(Kind kind, const net::RaceSettings& rs)
{
    if (!at_main_menu() || g_active) return false;
    g_kind = kind;
    g_active = true;
    switch (kind) {
    case Track: {
        // Preselect the row showing our track.
        const char* id = game::track_count() ? game::track_id(rs.track % game::track_count()) : "";
        for (int row = 0; row < 14; ++row) {
            int slot = at<int>(kRowToSlot + row * 4);
            if (slot > 0 && slot <= 16 && !lstrcmpiA(at<const char*>(kSlotNames + slot * 4), id)) {
                at<int>(kTrackRow) = row;
                break;
            }
        }
        at<int>(A(0x10dee38)) = game::ModeSingle; // the screen's flow belongs to a single race
        switch_to(kTrackSelect);
        break;
    }
    case Circuit:
        reinterpret_cast<void(__fastcall*)(unsigned, int, int)>(kFnLoadProfile)(kProfile, 0, 1);
        std::memcpy(g_progress, &at<unsigned char>(kProgress), kProgressSize);
        g_progress_saved = true;
        at<int>(kCircuit) = rs.circuit & 3;
        at<int>(A(0x10dee38)) = game::ModeChampionship;
        switch_to(rs.circuit == 2 ? kCustomCircuit : kCircuitSelect);
        break;
    case Duel:
        at<int>(kDuelGame) = rs.game ? 1 : 0;
        switch_to(kBattleSelect);
        break;
    }
    LOG("picker: opened (%d)", (int)kind);
    return true;
}

bool poll(net::RaceSettings& rs)
{
    if (!g_active) return false;
    net::RaceSettings before = rs;
    switch (g_kind) {
    case Track: {
        int i = at<int>(kTrackChosen);
        if (i >= 0 && i < list_count(kSingleTracks)) {
            int t = track_by_id(list_id(kSingleTracks, i));
            if (t >= 0) rs.track = (unsigned char)t;
        }
        break;
    }
    case Circuit: {
        int c = at<int>(kCircuit);
        if (c >= 0 && c <= 3) rs.circuit = (unsigned char)c;
        if (c == 2) { // the custom list as the screen shows it (add/remove/clear/random)
            unsigned list = kCircuitLists + 2 * 0x1c;
            int n = 0;
            for (int i = 0; i < list_count(list) && n < (int)sizeof(rs.custom); ++i) {
                int t = track_by_id(list_id(list, i));
                if (t >= 0) rs.custom[n++] = (unsigned char)t;
            }
            rs.ncustom = (unsigned char)n;
        }
        break;
    }
    case Duel:
        rs.game = at<int>(kDuelGame) ? 1 : 0;
        break;
    }
    return std::memcmp(&before, &rs, sizeof(rs)) != 0;
}

int circuit_tracks(const net::RaceSettings& rs, int* out, int max)
{
    int n = 0;
    if (rs.circuit == 2) {
        for (int i = 0; i < rs.ncustom && n < max; ++i) out[n++] = rs.custom[i];
        return n;
    }
    if (g_ok) { // the game's lists (Config\circuit.ini, loaded at startup)
        unsigned list = kCircuitLists + (rs.circuit & 3) * 0x1c;
        for (int i = 0; i < list_count(list) && n < max; ++i) {
            int t = track_by_id(list_id(list, i));
            if (t >= 0) out[n++] = t;
        }
        if (n) return n;
    }
    static const char* const kSections[] = {"Amateur", "Pro", "Custom", "Super"};
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sConfig\\circuit.ini", config::game_dir());
    for (int k = 1; k <= 32 && n < max; ++k) {
        char key[16], id[64];
        snprintf(key, sizeof(key), "Track%d", k);
        GetPrivateProfileStringA(kSections[rs.circuit & 3], key, "", id, sizeof(id), path);
        if (!id[0]) break;
        int t = track_by_id(id);
        if (t >= 0) out[n++] = t;
    }
    return n;
}

} // namespace rclan::picker
