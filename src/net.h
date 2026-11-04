#pragma once

#include "race.h"

#include <windows.h>

namespace rclan::net {

constexpr int kMaxPlayers = 4; // host + 3 (the CPU slots of a single-player race)
constexpr int kMaxGames = 8;
constexpr int kNameLen = 32;

enum class Mode { Idle, Hosting, Connecting, Joined };

// Status/error codes; the UI turns them into localized text.
enum class Status {
    None,
    Hosting,          // lobby open
    PortInUse,        // could not bind
    Connecting,
    ConnectFailed,
    BadAddress,
    RejectedFull,
    RejectedVersion,  // different game build or mod protocol
    ConnectionLost,
    HostClosed,
};

struct Player {
    bool used;
    char name[kNameLen];
};

struct GameInfo {
    char host_name[kNameLen];
    char addr[64];       // "ip:port", usable with join()
    int players, max_players;
    bool compatible;     // same game build + protocol
};

// Race options chosen by the host (shown to everyone).
enum RaceType : unsigned char { RaceSingle, RaceDuel, RaceSeries };
struct RaceSettings {
    unsigned char type;        // RaceType
    unsigned char track;       // single race / duel: index into the game's track list
    unsigned char circuit;     // series: 0 amateur, 1 pro, 2 custom, 3 super (the game's order)
    unsigned char power_ups, sheriff, traps;
    unsigned char minutes;     // race length in minutes instead of laps
    unsigned char length;      // 1..10
    unsigned char hard;        // realistic steering + strong opponents
    unsigned char bots;        // fill empty places with computer opponents
    unsigned char game;        // duel: 0 radio soccer, 1 battle ball
    unsigned char ncustom;     // series: the custom track list
    unsigned char custom[32];  //   (indices into the game's track list)
};

struct Snapshot {
    Mode mode;
    Status status;
    char status_detail[64];
    char local_ip[64];   // for "others can join via ..."
    Player players[kMaxPlayers];
    int my_slot;         // index into players, -1 if none
    GameInfo games[kMaxGames];
    int ngames;
    RaceSettings settings;          // host's (clients: as last received)
    bool have_car[kMaxPlayers];     // host: car info received
    bool busy[kMaxPlayers];         // host: player is in a race / results
};

// Overrides the game build id (test tools that aren't RCRacers.exe). Call before start().
void set_build(unsigned build);

// Starts the network thread. `notify` receives `msg` whenever state changes.
void start(HWND notify, UINT msg);

void set_name(const char* name);
void host();
void join(const char* addr); // "host" or "host:port"
void leave();

void snapshot(Snapshot* out);

// -- races --------------------------------------------------------------------
void set_settings(const RaceSettings& s); // host
void set_car(const game::Car& car);       // the local player's car (sent to the host)
bool car_of(int slot, game::Car* out);    // host: a player's car
void set_busy(bool busy);                 // local player is in a race / results

// Host: sends the race to all clients and starts it locally.
void start_race(const race::Start& s);
// Client: the notify message arrives with wParam 1 when the host started a
// race; call this then (GUI thread).
void begin_pending();

} // namespace rclan::net
