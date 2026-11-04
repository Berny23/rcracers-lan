#pragma once

#include <windows.h>

// Access to game internals needed for network races: race setup, the car
// table, car control fields, the fixed-step simulation timer, the RNG.
//
// These are fixed addresses of the German retail 2.0 build. Each function's
// first bytes are verified at startup; on any other build available() is
// false and network races are disabled (the lobby still works).
namespace rclan::game {

bool init();      // verifies the build; call once at startup
bool available();

// -- tracks -------------------------------------------------------------------
int track_count();
const char* track_name(int i); // display name ("Azteken")
const char* track_id(int i);   // internal name ("aztec")

// -- the local player's car (Player1 profile, selected vehicle) ----------------
constexpr int kRecordSize = 300;
struct Car {
    int cls;       // vehicle slot of the profile (0..3)
    int skill;     // 1 = realistic steering / strong opponents, 0 = easy
    int p6;
    unsigned char record[kRecordSize];
};
bool local_car(Car* out); // main thread

// -- race setup ---------------------------------------------------------------
constexpr int kMaxCars = 4;
constexpr int kEntrySize = 0x154;
constexpr int kNameLen = 32;

struct Entry {
    unsigned char raw[kEntrySize]; // as in the game's table (name pointer zeroed)
    char name[kNameLen];
};

enum Mode : int { ModeSingle = 0, ModeChampionship = 1, ModeFriend = 2, ModeBattle = 4, ModeSoccer = 5 };

struct Options {
    int mode;        // Mode
    int humans;      // number of human cars (first in the table)
    int track;       // index into the track list (duel modes: their own arena)
    int power_ups, traps, sheriff, race_type, race_length, particles;
    unsigned seed;   // RNG state for setup/loading
};

// Host: builds the car table with the game's own functions (humans from
// `cars`, then `bots` computer opponents), returns the number of entries.
int build_table(const Options& o, const Car* cars, const char* const* names, int bots, bool hard,
                Entry* out);

// Every machine: installs options + table and starts the race (main thread,
// from the main menu).
void start_race(const Options& o, const Entry* entries, int n);
void restore_prefs(); // after the race: the player's own option values

// Computer-driven cars carry this marker after their name.
constexpr const char* kCpuMark = " [CPU]";
void mark_cpu(char* name, int size);
void set_name(int car, const char* name); // name in the running race's car table

// -- running race ---------------------------------------------------------------
bool race_running(); // the race object is active
int car_count();
// Control fields of car i: throttle (-1..1), steering (-1..1), flags.
constexpr unsigned kFlagBoost = 0x800, kFlagFire = 0x10;
struct Controls {
    float throttle, steer;
    unsigned flags; // kFlagBoost | kFlagFire bits
};
Controls get_controls(int car);
void set_controls(int car, const Controls& c);
int& controller(int car);              // 0/1 human, 2 computer
const unsigned char* car_record(int car); // whole simulation record
int car_record_size();
void single_view(int car);             // one viewport, following `car`
bool& paused();                        // race pause flag
void race_message(const char* text, int ticks); // the game's centered race message
void steady_messages(bool on);                  // no blinking (network races)

// RNG states (simulation uses the first).
unsigned& rng();
unsigned& rng2();

// The fixed-step timer the race loop asks "run another 16 ms tick?".
// `fn` replaces it (the original logic is available as timer_default).
struct Timer {
    unsigned count, now, steps, next, mask;
    unsigned char fast;
};
using TimerFn = unsigned(__fastcall*)(Timer*);
void hook_timer(TimerFn fn);
unsigned __fastcall timer_default(Timer* t);
unsigned now(); // the game's timeGetTime (through our hook)

// "Has the announcer finished speaking?" - the race countdown waits for it,
// which ties the start of the race to audio playback timing.
// The hook gets the caller's address.
using VoiceIdleFn = bool (*)(unsigned caller);
void hook_voice_idle(VoiceIdleFn fn);
bool voice_idle_default();

// Race start sequence. The race object goes 2 (loading) -> 4 -> 5 (running);
// the track intro (camera flight over the track) goes 4,5,7,8,1 (loading) ->
// 2 (flight: until the announcer is done and 300 ticks passed, or a key)
// -> 3 (60 ticks fade) -> 0 (countdown/race). These advance per frame and
// read the tick counter, so network races hold the ticks at each boundary.
int race_state();
int intro_state();
unsigned intro_since();              // tick count when the current intro phase began
bool in_intro_check(unsigned caller); // the announcer check of the intro flight
constexpr unsigned kIntroFlight = 300, kIntroFade = 60;

} // namespace rclan::game
