// rclan-peer.exe - headless second player for testing the LAN lobby.
//
//   rclan-peer.exe host NAME [BUILD]        open a lobby
//   rclan-peer.exe join ADDR NAME [BUILD]   join a lobby ("ip" or "ip:port")
//   rclan-peer.exe list [BUILD]             print discovered games
//
// BUILD (hex) must match the game's build id ("net: started ... build id" in
// rclan.log), otherwise the game reports a version mismatch.
// Runs until killed; prints the lobby state whenever it changes.

#include "../src/log.h"
#include "../src/net.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The race code asks for the game window (focus check); the peer has none.
namespace rclan::wnd {
HWND main_window() { return nullptr; }
} // namespace rclan::wnd

static const char* mode_name(rclan::net::Mode m)
{
    using rclan::net::Mode;
    return m == Mode::Idle ? "idle" : m == Mode::Hosting ? "hosting" : m == Mode::Connecting ? "connecting" : "joined";
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: rclan-peer host NAME [BUILD] | join ADDR NAME [BUILD] | list [BUILD]\n");
        return 1;
    }
    rclan::log::init("rclan-peer.log");
    const char* cmd = argv[1];
    const char* build = nullptr;
    if (!std::strcmp(cmd, "host") && argc > 3) build = argv[3];
    if (!std::strcmp(cmd, "join") && argc > 4) build = argv[4];
    if (!std::strcmp(cmd, "list") && argc > 2) build = argv[2];
    if (build) rclan::net::set_build((unsigned)std::strtoul(build, nullptr, 16));

    if (!std::strcmp(cmd, "host") && argc > 2) rclan::net::set_name(argv[2]);
    if (!std::strcmp(cmd, "join") && argc > 3) rclan::net::set_name(argv[3]);
    rclan::net::start(nullptr, 0);
    if (!std::strcmp(cmd, "host")) rclan::net::host();
    if (!std::strcmp(cmd, "join") && argc > 2) rclan::net::join(argv[2]);

    char last[2048] = "";
    for (;;) {
        Sleep(200);
        rclan::net::Snapshot s;
        rclan::net::snapshot(&s);
        char out[2048];
        int n = std::snprintf(out, sizeof(out), "mode=%s status=%d %s | players:", mode_name(s.mode),
                              (int)s.status, s.status_detail);
        for (int i = 0; i < rclan::net::kMaxPlayers; ++i)
            if (s.players[i].used)
                n += std::snprintf(out + n, sizeof(out) - n, " [%d]%s%s", i, s.players[i].name,
                                   i == s.my_slot ? "*" : "");
        n += std::snprintf(out + n, sizeof(out) - n, " | games:");
        for (int i = 0; i < s.ngames; ++i)
            n += std::snprintf(out + n, sizeof(out) - n, " %s@%s(%d/%d%s)", s.games[i].host_name,
                               s.games[i].addr, s.games[i].players, s.games[i].max_players,
                               s.games[i].compatible ? "" : ",incompatible");
        if (std::strcmp(out, last) != 0) {
            std::printf("%s\n", out);
            std::fflush(stdout);
            std::strcpy(last, out);
        }
    }
}
