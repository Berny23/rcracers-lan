#include "race.h"

#include "config.h"
#include "build.h"
#include "log.h"
#include "wnd.h"

#include <cstdio>

#include <cstring>

namespace rclan::race {

using build::A;

namespace {

// Is this game (or one of its windows, like the panel) in the foreground?
bool focused()
{
    HWND fg = GetForegroundWindow(), main = wnd::main_window();
    if (!fg || !main) return true; // unknown: don't drop input
    return GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(main, GA_ROOTOWNER);
}

constexpr int kRing = 512;    // ticks of input history kept (~8 s)
constexpr int kHashEvery = 64; // ticks between state hashes (~1 s)

struct Slot {
    unsigned tick;
    bool have;
    Input in;
};

CRITICAL_SECTION g_cs;
const struct CsInit { CsInit() { InitializeCriticalSection(&g_cs); } } g_cs_init;

// Shared with the network thread (g_cs).
Slot g_ring[kRing][game::kMaxCars];
unsigned g_drop_from[game::kMaxCars]; // 0 = still playing

// GUI thread only.
bool g_active;
int g_ncars, g_local;
bool g_human[game::kMaxCars];
int g_ctrl[game::kMaxCars]; // controller type of human cars at the start
SendInput g_send;
SendHash g_send_hash;
unsigned g_sent;            // highest tick whose local input is scheduled
unsigned g_sim_rng, g_sim_rng2, g_frame_rng, g_frame_rng2;
bool g_in_tick, g_frame_start, g_seeded;
game::Controls g_raw;       // car 0 as the frame's input handling left it
unsigned g_flags;           // boost/fire presses not yet sent
DWORD g_wait_since;
unsigned g_waits;
char g_names[game::kMaxCars][game::kNameLen];
const char* g_left_fmt = "%s left - the computer drives on";
const char* g_wait_fmt = "Waiting for %s ...";
volatile LONG g_left_pending; // bit per car: show "left" message
bool g_wait_shown;
bool g_debug;

void store(int car, unsigned tick, const Input& in)
{
    Slot& s = g_ring[tick % kRing][car];
    s.tick = tick;
    s.have = true;
    s.in = in;
}

bool have_all(unsigned tick)
{
    EnterCriticalSection(&g_cs);
    bool ok = true;
    for (int i = 0; i < g_ncars && ok; ++i) {
        if (!g_human[i] || (g_drop_from[i] && tick >= g_drop_from[i])) continue;
        const Slot& s = g_ring[tick % kRing][i];
        ok = s.have && s.tick == tick;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void apply(unsigned tick)
{
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_ncars; ++i) {
        if (!g_human[i]) continue;
        if (g_drop_from[i] && tick >= g_drop_from[i]) {
            if (game::controller(i) != 2) LOG("race: car %d is now driven by the computer", i);
            game::controller(i) = 2;
            continue;
        }
        const Input& in = g_ring[tick % kRing][i].in;
        game::set_controls(i, {in.throttle, in.steer, in.flags});
        game::controller(i) = g_ctrl[i]; // no "computer takes over" key in network races
    }
    LeaveCriticalSection(&g_cs);
}

// Local input for ticks up to `upto`.
void schedule_local(unsigned upto)
{
    while (g_sent < upto) {
        ++g_sent;
        Input in = {g_raw.throttle, g_raw.steer, g_flags};
        g_flags = 0;
        EnterCriticalSection(&g_cs);
        store(g_local, g_sent, in);
        LeaveCriticalSection(&g_cs);
        if (g_send) g_send(g_local, g_sent, in);
    }
}

// Car records also hold pointers (sounds, models) whose values differ between
// machines; dwords in the heap/module address range are left out. As floats
// these are ~1e-37, which no simulation value is. Same for a render-only flag.
unsigned hash_car(int i)
{
    unsigned h = 2166136261u;
    const unsigned* r = reinterpret_cast<const unsigned*>(game::car_record(i));
    for (int k = 0; k < game::car_record_size() / 4; ++k) {
        unsigned v = r[k];
        if (v >= 0x00400000u && v < 0x10000000u) continue;
        if (k == 0x6d0 / 4) v &= ~1u; // flags: bit 0 is set by rendering (car not drawn)
        h = (h ^ v) * 16777619u;
    }
    return h;
}

unsigned hash_state()
{
    unsigned h = 2166136261u;
    for (int i = 0; i < g_ncars; ++i) h = (h ^ hash_car(i)) * 16777619u;
    return h;
}

// The simulation uses the game's RNG inside ticks only; anything drawing
// random numbers between ticks (rendering, sound) gets a separate stream so
// differences in frame rate can't change the race.
void enter_tick()
{
    g_frame_rng = game::rng();
    g_frame_rng2 = game::rng2();
    game::rng() = g_sim_rng;
    game::rng2() = g_sim_rng2;
    g_in_tick = true;
}

void leave_tick()
{
    if (!g_in_tick) return;
    g_sim_rng = game::rng();
    g_sim_rng2 = game::rng2();
    game::rng() = g_frame_rng;
    game::rng2() = g_frame_rng2;
    g_in_tick = false;
}

void end_frame()
{
    // Car 0's fields belong to the frame's input handling between ticks.
    game::set_controls(0, g_raw);
    g_frame_start = true;
}

void finish()
{
    leave_tick();
    g_active = false;
    game::restore_prefs();
    game::steady_messages(false);
    LOG("race: network race ended");
}

bool hold_ticks(unsigned count);

unsigned gate(game::Timer* t)
{
    unsigned now = game::now();
    t->now = now;
    if (t->next > now) {
        if (t->next > now + 16) t->next = now + 16;
        return 0;
    }
    if (t->steps >= 6) {
        if (t->next < now) t->next = now;
        return 0;
    }
    if (hold_ticks(t->count)) {
        t->next = now; // no catching up afterwards
        return 0;
    }
    unsigned tick = t->count + 1;
    schedule_local(tick + kDelay);
    if (!have_all(tick)) {
        if (!g_wait_since) g_wait_since = GetTickCount();
        if (!g_wait_shown && GetTickCount() - g_wait_since > 1000) {
            g_wait_shown = true;
            char msg[96] = "";
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < g_ncars; ++i) {
                if (!g_human[i] || (g_drop_from[i] && tick >= g_drop_from[i])) continue;
                const Slot& sl = g_ring[tick % kRing][i];
                if (!(sl.have && sl.tick == tick)) { snprintf(msg, sizeof(msg), g_wait_fmt, g_names[i]); break; }
            }
            LeaveCriticalSection(&g_cs);
            if (msg[0]) game::race_message(msg, 100000);
        }
        return 0;
    }
    if (g_wait_shown) {
        g_wait_shown = false;
        game::race_message("", 1);
    }
    if (g_wait_since) {
        DWORD waited = GetTickCount() - g_wait_since;
        if (waited > 200) LOG("race: waited %lu ms for inputs of tick %u", waited, tick);
        g_wait_since = 0;
        ++g_waits;
    }
    if (!g_seeded) { // first tick: everything before was loading
        g_seeded = true;
        LOG("race: first tick");
    }
    apply(tick);
    if (g_debug && tick % 640 == 0 && tick <= 3200) { // dumps for comparing machines
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%srclan_dump_%u.bin", config::game_dir(), tick);
        if (FILE* f = fopen(path, "wb")) {
            for (int i = 0; i < g_ncars; ++i)
                fwrite(game::car_record(i), 1, game::car_record_size(), f);
            fwrite(reinterpret_cast<const void*>(A(0x10dee38)), 1, 0x100, f); // race object
            fwrite(reinterpret_cast<const void*>(A(0x1500600)), 1, 0x60, f);  // timer, rng
            fclose(f);
        }
    }
    if (tick % kHashEvery == 0) {
        unsigned h = hash_state();
        LOG("race: tick %u hash %08x", tick, h);
        if (g_send_hash) g_send_hash(tick, h);
    }
    t->next += 16;
    t->mask = 3;
    unsigned c = ++t->count;
    ++t->steps;
    for (unsigned d = 2; d < 16; ++d)
        if (c % d == 0) t->mask |= 1u << d;
    enter_tick();
    return 1;
}

unsigned __fastcall timer(game::Timer* t)
{
    if (!g_active) return game::timer_default(t);
    leave_tick();
    if (!game::race_running()) {
        finish();
        return game::timer_default(t);
    }
    if (LONG left = InterlockedExchange(&g_left_pending, 0)) {
        for (int i = 0; i < g_ncars; ++i)
            if (left & (1 << i)) {
                char msg[96];
                snprintf(msg, sizeof(msg), g_left_fmt, g_names[i]);
                game::race_message(msg, 300);
                game::mark_cpu(g_names[i], sizeof(g_names[i])); // "Name [CPU]" from now on
                game::set_name(i, g_names[i]);
                g_wait_shown = false;
            }
    }
    if (g_frame_start) {
        g_frame_start = false;
        if (g_debug) {
            static int last[3] = {-9, -9, -9};
            int cur[3] = {game::race_state(), game::intro_state(), (int)game::intro_since()};
            if (std::memcmp(cur, last, sizeof(cur))) {
                LOG("race: at tick %u: race state %d, intro state %d since %d", t->count, cur[0], cur[1],
                    cur[2]);
                std::memcpy(last, cur, sizeof(cur));
            }
        }
        g_raw = game::get_controls(0);
        if (!focused()) g_raw = {}; // keys meant for another window
        g_flags |= g_raw.flags;
        game::single_view(g_local);
        game::paused() = false;
    }
    unsigned r = gate(t);
    if (!r) end_frame();
    return r;
}

// The countdown advances only once the announcer has finished speaking
// ("3", "2", "1"), which depends on audio playback. Inside network ticks it
// counts as finished, so all machines release the cars at the same tick.
bool voice_idle(unsigned caller)
{
    if (g_active && g_in_tick) return true;
    if (g_active && game::in_intro_check(caller)) // flight ends after exactly 300 ticks, no key skip
        return *reinterpret_cast<unsigned*>(A(0x1500630)) - game::intro_since() >= game::kIntroFlight;
    return game::voice_idle_default();
}

// Before the race runs, the start sequence advances per frame (see game.h).
// Ticks wait while it loads and at each phase boundary until the frame code
// has moved on, so every machine switches phase at the same tick.
bool hold_ticks(unsigned count)
{
    if (game::race_state() != 5) return true;
    unsigned since = count - game::intro_since();
    switch (game::intro_state()) {
    case 0: return false;
    case 2: return since >= game::kIntroFlight;
    case 3: return since >= game::kIntroFade;
    default: return true; // loading
    }
}

} // namespace

void install()
{
    if (!game::available()) return;
    game::hook_timer(&timer);
    game::hook_voice_idle(&voice_idle);
}

void begin(const Start& s, int my_slot, SendInput send, SendHash hash)
{
    if (!game::available()) return;
    g_ncars = s.ncars;
    g_local = 0;
    for (int i = 0; i < s.ncars; ++i) {
        g_human[i] = s.slot_of_car[i] >= 0;
        if (s.slot_of_car[i] == my_slot) g_local = i;
    }
    EnterCriticalSection(&g_cs);
    std::memset(g_ring, 0, sizeof(g_ring));
    std::memset(g_drop_from, 0, sizeof(g_drop_from));
    for (unsigned t = 1; t <= (unsigned)kDelay; ++t)
        for (int i = 0; i < s.ncars; ++i)
            if (g_human[i]) store(i, t, Input{});
    LeaveCriticalSection(&g_cs);
    g_sent = kDelay;
    g_send = send;
    g_send_hash = hash;
    g_sim_rng = s.sim_seed;
    g_sim_rng2 = s.sim_seed ^ 0x5bd1e995u;
    g_in_tick = g_seeded = false;
    g_frame_start = true;
    g_flags = 0;
    g_wait_since = 0;
    g_waits = 0;
    g_wait_shown = false;
    g_left_pending = 0;
    for (int i = 0; i < game::kMaxCars; ++i)
        snprintf(g_names[i], sizeof(g_names[i]), "%s", i < s.ncars ? s.entries[i].name : "");
    LOG("race: starting network race, local car %d of %d", g_local, s.ncars);
    game::start_race(s.opt, s.entries, s.ncars);
    for (int i = 0; i < s.ncars; ++i) g_ctrl[i] = game::race_running() ? game::controller(i) : 0;
    g_debug = config::get_int("Debug", "RaceDump", 0) != 0;
    if (g_debug) {
        LOG("race: ctrl %d %d %d %d", g_ctrl[0], g_ctrl[1], g_ctrl[2], g_ctrl[3]);
        const unsigned* p = reinterpret_cast<const unsigned*>(A(0x10ea7d8));
        char line[512];
        int n = 0;
        for (int i = 0; i < 0x20; ++i) n += snprintf(line + n, sizeof(line) - n, " %x", p[i]);
        LOG("race: prefs%s", line);
    }
    g_active = game::race_running();
    if (g_active) game::steady_messages(true);
    if (!g_active) LOG("race: the game didn't start the race");
}

bool active() { return g_active; }
int local_car() { return g_local; }

void on_input(int car, unsigned tick, const Input& in)
{
    if (car < 0 || car >= game::kMaxCars) return;
    EnterCriticalSection(&g_cs);
    store(car, tick, in);
    LeaveCriticalSection(&g_cs);
}

void on_drop(int car, unsigned from)
{
    if (car < 0 || car >= game::kMaxCars) return;
    EnterCriticalSection(&g_cs);
    bool first = !g_drop_from[car];
    g_drop_from[car] = from ? from : 1;
    LeaveCriticalSection(&g_cs);
    if (first) InterlockedOr(&g_left_pending, 1 << car);
    LOG("race: car %d leaves at tick %u", car, from);
}

void set_texts(const char* left_fmt, const char* wait_fmt)
{
    g_left_fmt = left_fmt;
    g_wait_fmt = wait_fmt;
}

bool filter_message(const MSG* m)
{
    if (!g_active) return false;
    switch (m->message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
        return m->wParam == VK_ESCAPE || m->wParam == 'P' || m->wParam == VK_PAUSE;
    case WM_CHAR:
        return m->wParam == 27 || m->wParam == 'p' || m->wParam == 'P';
    }
    return false;
}

} // namespace rclan::race
