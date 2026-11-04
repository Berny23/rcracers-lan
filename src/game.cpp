#include "game.h"

#include "build.h"
#include "log.h"

#include <cstring>

namespace rclan::game {

using build::A;

namespace {

// -- German retail 2.0 (see README "Version compatibility") -------------------

template <typename T>
T& at(unsigned addr) { return *reinterpret_cast<T*>(addr); }

// Functions (thiscall ones are called as fastcall with an unused EDX).
const unsigned kFnTimer = A(0x440aa0);       // fastcall(Timer*) -> bool
const unsigned kFnRaceStart = A(0x417290);   // thiscall(race)
const unsigned kFnTableClear = A(0x41a750);  // thiscall(table)
const unsigned kFnTableAdd = A(0x41a7b0);    // thiscall(table, name, cls, record, skill, p6) -> entry
const unsigned kFnAddBots = A(0x439e70);     // fastcall(hard)
const unsigned kFnLoadProfile = A(0x4375e0); // thiscall(profile, index 1..)
const unsigned kFnSetTrack = A(0x41fd30);    // thiscall(track, descriptor)
const unsigned kFnVoiceIdle = A(0x40cbc0);   // () -> bool: announcer finished speaking

struct Sig {
    unsigned addr;
    unsigned char bytes[12];
    int n;
};
const Sig kSigs[] = {
    {0x440aa0, {0x56, 0x57, 0x8b, 0xf1, 0xff, 0x15, 0xdc, 0xd3, 0x46, 0x00}, 10},
    {0x417290, {0x56, 0x8b, 0xf1, 0x68, 0x24, 0x75, 0x47, 0x00, 0x8d, 0x4e}, 10},
    {0x41a750, {0x53, 0x57, 0x8b, 0xf9, 0x33, 0xdb, 0x8b, 0x87, 0x50, 0x05}, 10},
    {0x41a7b0, {0x8b, 0xd1, 0x8b, 0x8a, 0x50, 0x05, 0x00, 0x00, 0x83, 0xf9}, 10},
    {0x439e70, {0x81, 0xec, 0x5c, 0x02, 0x00, 0x00, 0xa1, 0x40, 0xb8, 0x4e, 0x01}, 11},
    {0x4375e0, {0x83, 0xec, 0x0c, 0x8d, 0x44, 0x24, 0x00, 0x56, 0x8b, 0xf1}, 10},
    {0x41fd30, {0x8b, 0x54, 0x24, 0x04, 0x56, 0x8b, 0xc1, 0x8b, 0xf2, 0x57}, 10},
    {0x40cbc0, {0x8b, 0x0d, 0x50, 0xf6, 0x47, 0x00, 0x85, 0xc9, 0x74, 0x0a}, 10},
};

// Data.
const unsigned kRace = A(0x10dee38);       // [0] mode, +0x18 pause (byte), +0x1c state
const unsigned kHumans = A(0x14eb840);     // number of human players
const unsigned kPrefs = A(0x10ea7d8);      // Prefs ini values, dword array
constexpr int kPrefParticles = 6, kPrefPowerUps = 0xc, kPrefTraps = 0xd, kPrefCorner = 0xe,
              kPrefRaceType = 0xf, kPrefRaceLength = 0x10, kPrefOpponents = 0x12,
              kPrefOpponentsFriend = 0x13;
const unsigned kTrackList = A(0x14ebab8);  // +0x14 descriptors (0xf8 each), +0x18 count
const unsigned kTrack = A(0x10e2738);      // Track object
const unsigned kAllTracks = A(0x14eba98);  // every track incl. the duel arenas, same layout
const unsigned kTable = A(0x10e0510);      // 4 entries of 0x154, count at +0x550
const unsigned kProfile = A(0x10e7e10);    // loaded player: name[12], cls, records...
const unsigned kProfileCls = A(0x10e7e1c), kProfileRecords = A(0x10e7e20);
const unsigned kProfileSkill = A(0x10e8304), kProfileP6 = A(0x10e830c);
const unsigned kCars = A(0x10e4aa0);       // Vehicles object + 8, records of 0xa0c
constexpr int kCarSize = 0xa0c;
constexpr int kCarController = 0x680, kCarSteer = 0x6c8, kCarThrottle = 0x6cc, kCarFlags = 0x6d0;
const unsigned kViewCount = A(0x8a1130), kViewOf = A(0x10ded80);
const unsigned kRng = A(0x1500618), kRng2 = A(0x150061c);
const unsigned kIatTimeGetTime = A(0x46d3dc); // the game's import slot (through our hooks)

using FnThis0 = void(__fastcall*)(unsigned self, int edx);
using FnTableAdd = unsigned char*(__fastcall*)(unsigned self, int edx, const char* name, int cls,
                                               const void* record, int skill, int p6);
using FnThis1 = void(__fastcall*)(unsigned self, int edx, unsigned arg);
using FnFast1 = void(__fastcall*)(int arg);

bool g_ok;

// Option values replaced during a network race.
int g_saved_prefs[0x20];
bool g_prefs_saved;

} // namespace

bool init()
{
    g_ok = true;
    for (const Sig& s : kSigs) {
        if (!build::matches(s.addr, s.bytes, s.n)) {
            LOG("game: function at %#x (German build address) doesn't match on this build (%s) - network races disabled",
                s.addr, build::name());
            g_ok = false;
            break;
        }
    }
    if (g_ok) LOG("game: internals verified, network races available");
    return g_ok;
}

bool available() { return g_ok; }

// -- tracks -----------------------------------------------------------------

static const char* track_desc(int i)
{
    if (!g_ok || i < 0 || i >= track_count()) return nullptr;
    return at<const char*>(kTrackList + 0x14) + i * 0xf8;
}

// The arena of a duel mode, as the game picks it ("battle" / "soccer").
static const char* arena_desc(int mode)
{
    const char* id = mode == ModeSoccer ? "soccer" : "battle";
    int n = at<int>(kAllTracks + 0x18);
    const char* d = at<const char*>(kAllTracks + 0x14);
    for (int i = 0; d && i < n; ++i)
        if (!lstrcmpiA(d + i * 0xf8, id)) return d + i * 0xf8;
    LOG("game: arena %s not found", id);
    return nullptr;
}

int track_count() { return g_ok ? at<int>(kTrackList + 0x18) : 0; }
const char* track_name(int i) { const char* d = track_desc(i); return d ? d + 0xc : ""; }
const char* track_id(int i) { const char* d = track_desc(i); return d ? d : ""; }

// -- cars -------------------------------------------------------------------

bool local_car(Car* out)
{
    if (!g_ok) return false;
    reinterpret_cast<FnThis1>(kFnLoadProfile)(kProfile, 0, 1);
    int cls = at<int>(kProfileCls);
    if (cls < 0 || cls > 3) cls = 0;
    out->cls = cls;
    out->skill = at<int>(kProfileSkill);
    out->p6 = at<int>(kProfileP6);
    std::memcpy(out->record, reinterpret_cast<void*>(kProfileRecords + cls * kRecordSize),
                kRecordSize);
    return true;
}

static void set_prefs(const Options& o)
{
    int* p = &at<int>(kPrefs);
    if (!g_prefs_saved) {
        std::memcpy(g_saved_prefs, p, sizeof(g_saved_prefs));
        g_prefs_saved = true;
    }
    p[kPrefParticles] = o.particles;
    p[kPrefPowerUps] = o.power_ups;
    p[kPrefTraps] = o.traps;
    p[kPrefCorner] = o.sheriff;
    p[kPrefRaceType] = o.race_type;
    p[kPrefRaceLength] = o.race_length;
}

void restore_prefs()
{
    if (!g_prefs_saved) return;
    std::memcpy(&at<int>(kPrefs), g_saved_prefs, sizeof(g_saved_prefs));
    g_prefs_saved = false;
}

int build_table(const Options& o, const Car* cars, const char* const* names, int bots, bool hard,
                Entry* out)
{
    if (!g_ok) return 0;
    set_prefs(o);
    at<int>(kRace) = o.mode;
    reinterpret_cast<FnThis0>(kFnTableClear)(kTable, 0);
    for (int i = 0; i < o.humans && i < kMaxCars; ++i)
        reinterpret_cast<FnTableAdd>(kFnTableAdd)(kTable, 0, names[i], cars[i].cls, cars[i].record,
                                                  hard ? 1 : 0, cars[i].p6);
    if (bots > 0) {
        at<unsigned>(kRng) = o.seed; // reproducible picks (the table is sent to the clients anyway)
        // The game's own opponent picker; it adds `computerOpponents` cars
        // when there is one human (or the "opponents vs. a friend" option is on).
        int* p = &at<int>(kPrefs);
        int saved_n = p[kPrefOpponents], saved_m = p[kPrefOpponentsFriend];
        int saved_h = at<int>(kHumans);
        p[kPrefOpponents] = bots;
        p[kPrefOpponentsFriend] = 1;
        at<int>(kHumans) = 1;
        reinterpret_cast<FnFast1>(kFnAddBots)(hard ? 1 : 0);
        p[kPrefOpponents] = saved_n;
        p[kPrefOpponentsFriend] = saved_m;
        at<int>(kHumans) = saved_h;
    }
    int n = at<int>(kTable + 0x550);
    // Starting grid: a rotation of the table order, like the game does
    // (duels: fixed, the table order picks the side/team).
    unsigned start = n && o.mode < ModeBattle ? (o.seed >> 8) % n : 0;
    for (int i = 0; i < n; ++i) {
        unsigned char* e = reinterpret_cast<unsigned char*>(kTable + i * kEntrySize);
        reinterpret_cast<int*>(e)[2] = (start + i) % n;
        std::memcpy(out[i].raw, e, kEntrySize);
        reinterpret_cast<int*>(out[i].raw)[3] = 0;
        const char* name = *reinterpret_cast<const char**>(e + 0xc);
        std::memset(out[i].name, 0, kNameLen);
        if (name) std::strncpy(out[i].name, name, kNameLen - 1);
        if (i >= o.humans) mark_cpu(out[i].name, kNameLen); // computer opponents
    }
    return n;
}

void mark_cpu(char* name, int size)
{
    if (std::strstr(name, kCpuMark)) return;
    int len = (int)std::strlen(name), add = (int)std::strlen(kCpuMark);
    if (len + add >= size) len = size - 1 - add;
    std::strcpy(name + len, kCpuMark);
}

void set_name(int car, const char* name)
{
    // The table owns its names (the game's strdup/free).
    if (!g_ok || car < 0 || car >= car_count()) return;
    using FnDup = char*(__cdecl*)(const char*);
    using FnFree = void(__cdecl*)(void*);
    char*& slot = at<char*>(kTable + car * kEntrySize + 0xc);
    char* copy = reinterpret_cast<FnDup>(A(0x46c6e6))(name);
    if (!copy) return;
    if (slot) reinterpret_cast<FnFree>(A(0x4618dc))(slot);
    slot = copy;
}

void start_race(const Options& o, const Entry* entries, int n)
{
    if (!g_ok) return;
    set_prefs(o);
    at<int>(kRace) = o.mode;
    at<int>(kHumans) = o.humans;
    if (const char* d = o.mode >= ModeBattle ? arena_desc(o.mode) : track_desc(o.track))
        reinterpret_cast<FnThis1>(kFnSetTrack)(kTrack, 0, (unsigned)(UINT_PTR)d);
    reinterpret_cast<FnThis0>(kFnTableClear)(kTable, 0);
    for (int i = 0; i < n && i < kMaxCars; ++i) {
        const int* raw = reinterpret_cast<const int*>(entries[i].raw);
        unsigned char* e = reinterpret_cast<FnTableAdd>(kFnTableAdd)(
            kTable, 0, entries[i].name, raw[4], raw + 5, raw[0x50], raw[0x51]);
        if (!e) break;
        // Everything else (flags, grid position) as on the host; keep our name copy.
        unsigned name_ptr = reinterpret_cast<unsigned*>(e)[3];
        std::memcpy(e, entries[i].raw, kEntrySize);
        reinterpret_cast<unsigned*>(e)[3] = name_ptr;
    }
    at<unsigned>(kRng) = o.seed;
    at<unsigned>(kRng2) = o.seed ^ 0x9e3779b9u;
    LOG("game: starting race: mode %d, track %d (%s), %d cars, %d humans, seed %#x", o.mode,
        o.track, o.mode >= ModeBattle ? (o.mode == ModeSoccer ? "soccer" : "battle") : track_name(o.track),
        n, o.humans, o.seed);
    reinterpret_cast<FnThis0>(kFnRaceStart)(kRace, 0);
}

// -- running race -----------------------------------------------------------

bool race_running() { return g_ok && at<int>(kRace + 0x1c) != 0; }
int car_count() { return g_ok ? at<int>(kTable + 0x550) : 0; }

static unsigned char* car(int i) { return reinterpret_cast<unsigned char*>(kCars + i * kCarSize); }

Controls get_controls(int i)
{
    Controls c;
    std::memcpy(&c.throttle, car(i) + kCarThrottle, 4);
    std::memcpy(&c.steer, car(i) + kCarSteer, 4);
    c.flags = *reinterpret_cast<unsigned*>(car(i) + kCarFlags) & (kFlagBoost | kFlagFire);
    return c;
}

void set_controls(int i, const Controls& c)
{
    std::memcpy(car(i) + kCarThrottle, &c.throttle, 4);
    std::memcpy(car(i) + kCarSteer, &c.steer, 4);
    unsigned& f = *reinterpret_cast<unsigned*>(car(i) + kCarFlags);
    f = (f & ~(kFlagBoost | kFlagFire)) | (c.flags & (kFlagBoost | kFlagFire));
}

int& controller(int i) { return *reinterpret_cast<int*>(car(i) + kCarController); }
const unsigned char* car_record(int i) { return car(i); }
int car_record_size() { return kCarSize; }

void single_view(int c)
{
    at<int>(kViewCount) = 1;
    for (int i = 0; i < kMaxCars; ++i) at<int>(kViewOf + i * 4) = i == c ? 0 : -1;
}

bool& paused() { return at<bool>(kRace + 0x18); }

// The game blinks its race message with the tick counter (0x418db3: je
// skip-draw when bit 4 of the count is clear). While a network race waits
// for a player, the counter stands still, so the message could stay hidden.
void steady_messages(bool on)
{
    static const unsigned char kBlink[2] = {0x74, 0x1a};
    static const unsigned char kSteady[2] = {0x90, 0x90};
    auto* p = reinterpret_cast<unsigned char*>(A(0x418db3));
    if (!g_ok || std::memcmp(p, on ? kBlink : kSteady, 2) != 0) return;
    DWORD old;
    if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(p, on ? kSteady : kBlink, 2);
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
}

void race_message(const char* text, int ticks)
{
    // What the game's own message function (0x418d30) does: expiry tick, flag, text.
    if (!g_ok) return;
    at<unsigned>(A(0x10dfc60)) = at<unsigned>(A(0x1500630)) + ticks;
    char* buf = &at<char>(A(0x10dfc64));
    std::strncpy(buf, text, 0x4f);
    buf[0x4f] = '\0';
    at<unsigned char>(A(0x10dfcb4)) = 1;
}
unsigned& rng() { return at<unsigned>(kRng); }
unsigned& rng2() { return at<unsigned>(kRng2); }

// -- timer --------------------------------------------------------------------

// Same logic as the game's: one tick per 16 ms of timeGetTime, at most 6 per
// frame (then the schedule is reset instead of catching up).
unsigned now()
{
    return (*reinterpret_cast<DWORD(WINAPI**)()>(kIatTimeGetTime))();
}

unsigned __fastcall timer_default(Timer* t)
{
    unsigned now = game::now();
    t->now = now;
    unsigned next = t->next;
    bool tick = false;
    if (!t->fast) {
        if (next <= now) {
            if (t->steps < 6) {
                t->next = next + 16;
                tick = true;
            } else if (next < now) {
                t->next = now;
            }
        } else if (next > now + 16) {
            t->next = now + 16;
        }
    } else if (t->steps < 0x3c) {
        t->next = now + 1;
        tick = true;
    } else {
        t->next = now;
    }
    if (!tick) return 0;
    t->mask = 3;
    unsigned c = ++t->count;
    ++t->steps;
    for (unsigned d = 2; d < 16; ++d)
        if (c % d == 0) t->mask |= 1u << d;
    return 1;
}

static bool write_jmp(unsigned addr, const void* fn)
{
    auto* p = reinterpret_cast<unsigned char*>(addr);
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE9; // jmp fn
    int rel = (int)((UINT_PTR)fn - (UINT_PTR)(p + 5));
    std::memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void hook_timer(TimerFn fn)
{
    if (g_ok && write_jmp(kFnTimer, reinterpret_cast<const void*>(fn)))
        LOG("game: simulation timer hooked");
}

int race_state() { return g_ok ? at<int>(kRace + 0x1c) : 0; }
int intro_state() { return g_ok ? at<int>(kTrack + 0x554) : 0; }
unsigned intro_since() { return g_ok ? at<unsigned>(A(0x10e2730)) : 0; }
bool in_intro_check(unsigned caller) { return caller >= A(0x420500) && caller < A(0x4208c0); }

// Original logic: the current sample (if any) has finished and nothing is queued.
static VoiceIdleFn g_voice_hook;
__attribute__((noinline)) static bool __cdecl voice_idle_hooked()
{
    // Reached by a jmp from the function's entry: our return address is the caller's.
    return g_voice_hook((unsigned)(UINT_PTR)__builtin_return_address(0));
}

bool voice_idle_default()
{
    if (at<unsigned>(A(0x47f650))) {
        using FnPlaying = bool(__fastcall*)(unsigned self);
        if (!reinterpret_cast<FnPlaying>(A(0x40c860))(at<unsigned>(A(0x47f650)))) return false;
    }
    return at<int>(A(0x47f658)) == 0;
}

void hook_voice_idle(VoiceIdleFn fn)
{
    g_voice_hook = fn;
    if (g_ok && write_jmp(kFnVoiceIdle, reinterpret_cast<const void*>(&voice_idle_hooked)))
        LOG("game: announcer check hooked");
}

} // namespace rclan::game
