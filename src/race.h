#pragma once

#include "game.h"

#include <windows.h>

// Network races: lockstep simulation.
//
// Every machine runs the whole race (all cars, bots included) from the same
// start state; only the players' control inputs travel over the network. The
// game simulates in fixed 16 ms ticks, so a tick may only run once the inputs
// of every human car for that tick are known. Local input is sampled once per
// frame (from car 0's control fields, which the game fills from this
// machine's control1.ini bindings) and scheduled kDelay ticks ahead.
namespace rclan::race {

constexpr int kDelay = 3; // ticks of input delay (48 ms)

struct Input {
    float throttle, steer;
    unsigned flags; // game::kFlagBoost | game::kFlagFire
};

// Everything a machine needs to start the race; identical on all machines.
struct Start {
    game::Options opt;
    int ncars;
    game::Entry entries[game::kMaxCars];
    signed char slot_of_car[game::kMaxCars]; // lobby slot of a human car, -1 = bot
    unsigned sim_seed;                        // RNG state for the simulation ticks
};

using SendInput = void (*)(int car, unsigned tick, const Input& in);
using SendHash = void (*)(unsigned tick, unsigned hash);

void install(); // hooks the simulation timer (if game internals are available)

// Starts the race on the GUI thread. `my_slot` picks the local car.
void begin(const Start& s, int my_slot, SendInput send, SendHash hash);

bool active();
int local_car();

// From the network (any thread).
void on_input(int car, unsigned tick, const Input& in);
// Lobby slot left: from tick `from` on its car drives itself (computer).
void on_drop(int car, unsigned from);

// Localized in-race messages: "%s left ..." and "waiting for %s ...".
void set_texts(const char* left_fmt, const char* wait_fmt);

// Keys the race must not see while networked (pause, Esc). True = swallow.
bool filter_message(const MSG* m);

} // namespace rclan::race
