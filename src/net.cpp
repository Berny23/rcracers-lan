// LAN lobby: UDP discovery + TCP session (host = star center).
//
// Discovery (UDP, port P-1):  client broadcasts "RCLQ", hosts answer "RCLA".
// Session   (TCP, port P):    framed messages  [u16 len][u8 type][payload]
//   HELLO   c->h  proto u8, build u32, name[32]
//   WELCOME h->c  slot u8
//   REJECT  h->c  reason u8
//   LOBBY   h->c  n u8, n x {slot u8, name[32]}
//   PING    both  (keepalive; any traffic resets the timeout)
//   CAR      c->h  the client's car: cls i32, skill i32, p6 i32, record[300]
//   STATUS   c->h  busy u8 (in a race / looking at results)
//   SETTINGS h->c  RaceSettings
//   START    h->c  race::Start (the complete race setup)
//   INPUT    both  car u8, tick u32, throttle f32, steer f32, flags u32
//                  (clients send their car's, the host relays to the others)
//   HASH     c->h  tick u32, hash u32 (desync check)
//   DROP     h->c  car u8, tick u32: from that tick the car drives itself

// winsock2 must precede windows.h (pulled in by net.h)
#include <winsock2.h>
#include <ws2tcpip.h>

#include "net.h"

#include "config.h"
#include "log.h"

#include <cstdio>
#include <cstring>

namespace rclan::net {

namespace {

constexpr unsigned char kProto = 3;
constexpr int kDefaultPort = 47625;
constexpr DWORD kQueryInterval = 1500, kGameExpiry = 5000;
constexpr DWORD kPingInterval = 2000, kPeerTimeout = 10000, kConnectTimeout = 5000;
// During a race inputs flow constantly; a silent peer is gone (or frozen) much sooner.
constexpr DWORD kRacePingInterval = 500, kRaceTimeout = 4000;

enum MsgType : unsigned char { HELLO = 1, WELCOME, REJECT, LOBBY, PING, CAR, STATUS, SETTINGS, START,
                               INPUT, HASH, DROP };
enum Reject : unsigned char { FULL = 1, VERSION = 2 };

struct Conn {
    SOCKET s = INVALID_SOCKET;
    int slot = -1;              // host side: assigned player slot
    DWORD last_rx = 0;
    char rx[8192];
    int rxlen = 0;
};

// -- shared state (guarded by g_cs) -------------------------------------------
CRITICAL_SECTION g_cs;
// Initialized at load time: set_name() may run before start().
const struct CsInit { CsInit() { InitializeCriticalSection(&g_cs); } } g_cs_init;
HWND g_notify;
UINT g_msg;
Snapshot g_snap;              // what the UI sees
char g_name[kNameLen] = "Player";
enum class Cmd { None, Host, Join, Leave } g_cmd = Cmd::None;
char g_cmd_addr[64];

// -- network-thread-only state ----------------------------------------------
int g_port = kDefaultPort;
unsigned g_build;             // exe timestamp ^ image size
unsigned g_instance;          // random id to dedupe discovery answers
SOCKET g_query = INVALID_SOCKET;   // client-side discovery
SOCKET g_beacon = INVALID_SOCKET;  // host-side discovery (bound to P-1)
SOCKET g_listen = INVALID_SOCKET;
Conn g_peers[kMaxPlayers];         // host: clients
Conn g_session;                    // client: connection to host
DWORD g_connect_start, g_last_query, g_last_ping;
bool g_hello_sent;                 // client: TCP connected, HELLO sent
unsigned g_game_ids[kMaxGames];
DWORD g_game_seen[kMaxGames];
bool g_dirty;

// -- races --------------------------------------------------------------------
CRITICAL_SECTION g_send_cs; // sends come from the network and the GUI thread
const struct SendCsInit { SendCsInit() { InitializeCriticalSection(&g_send_cs); } } g_send_cs_init;
game::Car g_cars[kMaxPlayers];   // host: everyone's car (guarded by g_cs)
game::Car g_my_car;              // guarded by g_cs
bool g_my_car_set, g_my_car_dirty;
bool g_my_busy, g_my_busy_dirty;
bool g_settings_dirty;           // host: broadcast settings
race::Start g_pending_start;     // client: START waiting for the GUI thread
bool g_have_start;
int g_car_of_slot[kMaxPlayers];  // during a race: car index of each lobby slot (-1)
unsigned g_last_tick[game::kMaxCars]; // newest input tick received per car
bool g_racing;                   // a race was started (inputs/drops are meaningful)
struct HashRec { unsigned tick, hash; };
HashRec g_hashes[64];            // host: own recent hashes
bool g_desync_logged[kMaxPlayers];

void notify_ui()
{
    if (g_notify) PostMessageA(g_notify, g_msg, 0, 0);
}

void set_status(Status s, const char* detail = "")
{
    EnterCriticalSection(&g_cs);
    g_snap.status = s;
    snprintf(g_snap.status_detail, sizeof(g_snap.status_detail), "%s", detail);
    LeaveCriticalSection(&g_cs);
    g_dirty = true;
}

void set_mode(Mode m)
{
    EnterCriticalSection(&g_cs);
    g_snap.mode = m;
    if (m == Mode::Idle) {
        std::memset(g_snap.players, 0, sizeof(g_snap.players));
        g_snap.my_slot = -1;
    }
    LeaveCriticalSection(&g_cs);
    g_dirty = true;
}

void close_sock(SOCKET& s)
{
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}

void close_conn(Conn& c)
{
    close_sock(c.s);
    c.slot = -1;
    c.rxlen = 0;
}

void nonblocking(SOCKET s)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}

unsigned compute_build()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return nt->FileHeader.TimeDateStamp ^ nt->OptionalHeader.SizeOfImage;
}

// Local LAN address: the source address the OS would use for the default
// route (connect() on UDP sends nothing). Falls back to the host name lookup.
bool route_ip(char* ip, int n)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    bool ok = false;
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        sockaddr_in local = {};
        int len = sizeof(local);
        if (getsockname(s, (sockaddr*)&local, &len) == 0 && local.sin_addr.s_addr != 0 &&
            (ntohl(local.sin_addr.s_addr) >> 24) != 127)
            ok = inet_ntop(AF_INET, &local.sin_addr, ip, n) != nullptr;
    }
    closesocket(s);
    return ok;
}

void find_local_ip()
{
    char host[256], ip[64] = "?";
    if (route_ip(ip, sizeof(ip))) {
        // done
    } else if (gethostname(host, sizeof(host)) == 0) {
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* a = res; a; a = a->ai_next) {
                auto* sin = reinterpret_cast<sockaddr_in*>(a->ai_addr);
                if ((ntohl(sin->sin_addr.s_addr) >> 24) == 127) continue;
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                break;
            }
            freeaddrinfo(res);
        }
    }
    EnterCriticalSection(&g_cs);
    snprintf(g_snap.local_ip, sizeof(g_snap.local_ip), "%s", ip);
    LeaveCriticalSection(&g_cs);
}

// -- framing ----------------------------------------------------------------

bool send_frame(SOCKET s, unsigned char type, const void* data = nullptr, int len = 0)
{
    char buf[4096];
    if (s == INVALID_SOCKET || len + 3 > (int)sizeof(buf)) return false;
    unsigned short n = (unsigned short)(len + 1);
    std::memcpy(buf, &n, 2);
    buf[2] = (char)type;
    if (len) std::memcpy(buf + 3, data, len);
    EnterCriticalSection(&g_send_cs);
    int sent = 0;
    while (sent < len + 3) { // non-blocking sockets: retry briefly if the buffer is full
        int r = send(s, buf + sent, len + 3 - sent, 0);
        if (r > 0) { sent += r; continue; }
        if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; }
        break;
    }
    LeaveCriticalSection(&g_send_cs);
    return sent == len + 3;
}

// Reads available data; calls on_frame(type, payload, len) per complete frame.
// Returns false when the connection closed/failed.
template <typename F>
bool pump(Conn& c, F on_frame)
{
    int r = recv(c.s, c.rx + c.rxlen, sizeof(c.rx) - c.rxlen, 0);
    if (r == 0) { LOG("net: connection closed by peer"); return false; }
    if (r < 0) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) LOG("net: recv error %d", e);
        return e == WSAEWOULDBLOCK;
    }
    c.rxlen += r;
    c.last_rx = GetTickCount();
    for (;;) {
        if (c.rxlen < 2) break;
        unsigned short n;
        std::memcpy(&n, c.rx, 2);
        if (n == 0 || n > sizeof(c.rx) - 2) { LOG("net: bad frame length %u", n); return false; }
        if (c.rxlen < 2 + n) break;
        on_frame((unsigned char)c.rx[2], c.rx + 3, n - 1);
        if (c.s == INVALID_SOCKET) return true; // handler closed the connection
        std::memmove(c.rx, c.rx + 2 + n, c.rxlen - 2 - n);
        c.rxlen -= 2 + n;
    }
    return true;
}

// -- races --------------------------------------------------------------------

void send_to_clients(unsigned char type, const void* d, int len, int except_slot = -1)
{
    for (auto& p : g_peers)
        if (p.s != INVALID_SOCKET && p.slot >= 0 && p.slot != except_slot) send_frame(p.s, type, d, len);
}

constexpr int kInputLen = 17;
void encode_input(char* b, int car, unsigned tick, const race::Input& in)
{
    b[0] = (char)car;
    std::memcpy(b + 1, &tick, 4);
    std::memcpy(b + 5, &in.throttle, 4);
    std::memcpy(b + 9, &in.steer, 4);
    std::memcpy(b + 13, &in.flags, 4);
}
void decode_input(const char* b, int* car, unsigned* tick, race::Input* in)
{
    *car = (unsigned char)b[0];
    std::memcpy(tick, b + 1, 4);
    std::memcpy(&in->throttle, b + 5, 4);
    std::memcpy(&in->steer, b + 9, 4);
    std::memcpy(&in->flags, b + 13, 4);
}

Mode current_mode()
{
    EnterCriticalSection(&g_cs);
    Mode m = g_snap.mode;
    LeaveCriticalSection(&g_cs);
    return m;
}

// Race callbacks (GUI thread).
void race_send_input(int car, unsigned tick, const race::Input& in)
{
    char b[kInputLen];
    encode_input(b, car, tick, in);
    if (current_mode() == Mode::Hosting) send_to_clients(INPUT, b, kInputLen);
    else send_frame(g_session.s, INPUT, b, kInputLen);
}

void race_send_hash(unsigned tick, unsigned hash)
{
    if (current_mode() == Mode::Hosting) {
        EnterCriticalSection(&g_cs);
        g_hashes[(tick / 64) % 64] = {tick, hash};
        LeaveCriticalSection(&g_cs);
    } else {
        char b[8];
        std::memcpy(b, &tick, 4);
        std::memcpy(b + 4, &hash, 4);
        send_frame(g_session.s, HASH, b, 8);
    }
}

void note_tick(int car, unsigned tick)
{
    if (car >= 0 && car < game::kMaxCars && tick > g_last_tick[car]) g_last_tick[car] = tick;
}

// Host: a player left during a race - their car drives itself from the first
// tick nobody has an input for (no machine can have simulated past it).
void drop_slot(int slot)
{
    if (!g_racing || !race::active() || slot < 0 || slot >= kMaxPlayers) return;
    int car = g_car_of_slot[slot];
    if (car < 0) return;
    g_car_of_slot[slot] = -1;
    unsigned from = g_last_tick[car] + 1;
    char b[5] = {(char)car};
    std::memcpy(b + 1, &from, 4);
    send_to_clients(DROP, b, 5);
    race::on_drop(car, from);
}

void begin_racing(const race::Start& st)
{
    for (int& c : g_car_of_slot) c = -1;
    for (int i = 0; i < st.ncars; ++i)
        if (st.slot_of_car[i] >= 0 && st.slot_of_car[i] < kMaxPlayers) g_car_of_slot[(int)st.slot_of_car[i]] = i;
    for (unsigned& t : g_last_tick) t = race::kDelay;
    std::memset(g_hashes, 0, sizeof(g_hashes));
    std::memset(g_desync_logged, 0, sizeof(g_desync_logged));
    g_racing = true;
}

void send_settings(SOCKET s = INVALID_SOCKET)
{
    RaceSettings rs;
    EnterCriticalSection(&g_cs);
    rs = g_snap.settings;
    LeaveCriticalSection(&g_cs);
    if (s != INVALID_SOCKET) send_frame(s, SETTINGS, &rs, sizeof(rs));
    else send_to_clients(SETTINGS, &rs, sizeof(rs));
}

void send_my_car()
{
    char b[12 + game::kRecordSize];
    EnterCriticalSection(&g_cs);
    bool ok = g_my_car_set;
    std::memcpy(b, &g_my_car.cls, 4);
    std::memcpy(b + 4, &g_my_car.skill, 4);
    std::memcpy(b + 8, &g_my_car.p6, 4);
    std::memcpy(b + 12, g_my_car.record, game::kRecordSize);
    g_my_car_dirty = false;
    LeaveCriticalSection(&g_cs);
    if (ok) send_frame(g_session.s, CAR, b, sizeof(b));
}

void send_my_status()
{
    EnterCriticalSection(&g_cs);
    char b = g_my_busy;
    g_my_busy_dirty = false;
    LeaveCriticalSection(&g_cs);
    send_frame(g_session.s, STATUS, &b, 1);
}

// Host: race messages from a client.
void host_race_frame(Conn& p, unsigned char type, const char* d, int len)
{
    switch (type) {
    case CAR:
        if (len < 12 + game::kRecordSize) return;
        EnterCriticalSection(&g_cs);
        std::memcpy(&g_cars[p.slot].cls, d, 4);
        std::memcpy(&g_cars[p.slot].skill, d + 4, 4);
        std::memcpy(&g_cars[p.slot].p6, d + 8, 4);
        std::memcpy(g_cars[p.slot].record, d + 12, game::kRecordSize);
        g_snap.have_car[p.slot] = true;
        LeaveCriticalSection(&g_cs);
        g_dirty = true;
        break;
    case STATUS:
        if (len < 1) return;
        EnterCriticalSection(&g_cs);
        g_snap.busy[p.slot] = d[0] != 0;
        LeaveCriticalSection(&g_cs);
        g_dirty = true;
        break;
    case INPUT: {
        if (len < kInputLen || !g_racing) return;
        int car;
        unsigned tick;
        race::Input in;
        decode_input(d, &car, &tick, &in);
        if (car != g_car_of_slot[p.slot]) return; // only its own car
        note_tick(car, tick);
        race::on_input(car, tick, in);
        send_to_clients(INPUT, d, kInputLen, p.slot);
        break;
    }
    case HASH: {
        if (len < 8) return;
        unsigned tick, hash;
        std::memcpy(&tick, d, 4);
        std::memcpy(&hash, d + 4, 4);
        EnterCriticalSection(&g_cs);
        HashRec h = g_hashes[(tick / 64) % 64];
        LeaveCriticalSection(&g_cs);
        if (h.tick == tick && h.hash != hash && !g_desync_logged[p.slot]) {
            g_desync_logged[p.slot] = true;
            LOG("net: DESYNC with player %d at tick %u (%08x vs %08x)", p.slot, tick, h.hash, hash);
        }
        break;
    }
    }
}

// Client: race messages from the host.
void client_race_frame(unsigned char type, const char* d, int len)
{
    switch (type) {
    case SETTINGS:
        if (len < (int)sizeof(RaceSettings)) return;
        EnterCriticalSection(&g_cs);
        std::memcpy(&g_snap.settings, d, sizeof(RaceSettings));
        LeaveCriticalSection(&g_cs);
        g_dirty = true;
        break;
    case START:
        if (len < (int)sizeof(race::Start)) return;
        EnterCriticalSection(&g_cs);
        std::memcpy(&g_pending_start, d, sizeof(race::Start));
        g_have_start = true;
        LeaveCriticalSection(&g_cs);
        begin_racing(g_pending_start);
        LOG("net: race start received");
        if (g_notify) PostMessageA(g_notify, g_msg, 1, 0);
        break;
    case INPUT: {
        if (len < kInputLen || !g_racing) return;
        int car;
        unsigned tick;
        race::Input in;
        decode_input(d, &car, &tick, &in);
        note_tick(car, tick);
        race::on_input(car, tick, in);
        break;
    }
    case DROP: {
        if (len < 5 || !g_racing) return;
        unsigned from;
        std::memcpy(&from, d + 1, 4);
        race::on_drop((unsigned char)d[0], from);
        break;
    }
    }
}

// -- lobby (host) -----------------------------------------------------------

void broadcast_lobby()
{
    char buf[1 + kMaxPlayers * (1 + kNameLen)];
    int n = 0, len = 1;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (!g_snap.players[i].used) continue;
        buf[len++] = (char)i;
        std::memcpy(buf + len, g_snap.players[i].name, kNameLen);
        len += kNameLen;
        ++n;
    }
    LeaveCriticalSection(&g_cs);
    buf[0] = (char)n;
    for (auto& p : g_peers)
        if (p.s != INVALID_SOCKET && p.slot >= 0) send_frame(p.s, LOBBY, buf, len);
    g_dirty = true;
}

void free_slot(int slot)
{
    if (slot < 0) return;
    EnterCriticalSection(&g_cs);
    LOG("net: player %d (%s) left", slot, g_snap.players[slot].name);
    g_snap.players[slot].used = false;
    g_snap.have_car[slot] = false;
    g_snap.busy[slot] = false;
    LeaveCriticalSection(&g_cs);
    drop_slot(slot);
    broadcast_lobby();
}

void stop_all()
{
    close_sock(g_beacon);
    close_sock(g_listen);
    for (auto& p : g_peers) close_conn(p);
    close_conn(g_session);
}

void start_host()
{
    stop_all();
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a.sin_port = htons((u_short)g_port);
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0 || listen(g_listen, 4) != 0) {
        LOG("net: cannot listen on TCP %d (error %d)", g_port, WSAGetLastError());
        close_sock(g_listen);
        char d[16];
        snprintf(d, sizeof(d), "%d", g_port);
        set_status(Status::PortInUse, d);
        return;
    }
    nonblocking(g_listen);

    g_beacon = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a.sin_port = htons((u_short)(g_port - 1));
    if (bind(g_beacon, (sockaddr*)&a, sizeof(a)) != 0) {
        // Still playable via manual IP; just not discoverable.
        LOG("net: cannot bind UDP %d for discovery (error %d)", g_port - 1, WSAGetLastError());
        close_sock(g_beacon);
    } else {
        nonblocking(g_beacon);
    }

    find_local_ip();
    EnterCriticalSection(&g_cs);
    std::memset(g_snap.players, 0, sizeof(g_snap.players));
    g_snap.players[0].used = true;
    std::memcpy(g_snap.players[0].name, g_name, kNameLen);
    g_snap.my_slot = 0;
    std::memset(g_snap.have_car, 0, sizeof(g_snap.have_car));
    std::memset(g_snap.busy, 0, sizeof(g_snap.busy));
    if (g_my_car_set) {
        g_cars[0] = g_my_car;
        g_snap.have_car[0] = true;
    }
    LeaveCriticalSection(&g_cs);
    g_racing = false;
    set_mode(Mode::Hosting);
    set_status(Status::Hosting);
    LOG("net: hosting on TCP %d / UDP %d", g_port, g_port - 1);
}

void host_accept()
{
    SOCKET s = accept(g_listen, nullptr, nullptr);
    if (s == INVALID_SOCKET) return;
    nonblocking(s);
    for (auto& p : g_peers) {
        if (p.s != INVALID_SOCKET) continue;
        p.s = s;
        p.slot = -1;
        p.rxlen = 0;
        p.last_rx = GetTickCount();
        return;
    }
    unsigned char r = FULL; // no connection slot left
    send_frame(s, REJECT, &r, 1);
    closesocket(s);
}

void host_on_frame(Conn& p, unsigned char type, const char* d, int len)
{
    if (p.slot >= 0 && type != HELLO) {
        host_race_frame(p, type, d, len);
        return;
    }
    if (type != HELLO || p.slot >= 0) return; // PINGs just refresh last_rx
    if (len < 1 + 4 + kNameLen) return;
    unsigned build;
    std::memcpy(&build, d + 1, 4);
    if ((unsigned char)d[0] != kProto || build != g_build) {
        unsigned char r = VERSION;
        send_frame(p.s, REJECT, &r, 1);
        LOG("net: rejected client (proto %u, build %#x)", (unsigned char)d[0], build);
        close_conn(p);
        return;
    }
    EnterCriticalSection(&g_cs);
    int slot = -1;
    for (int i = 1; i < kMaxPlayers; ++i)
        if (!g_snap.players[i].used) { slot = i; break; }
    if (slot >= 0) {
        g_snap.players[slot].used = true;
        char want[kNameLen];
        std::memcpy(want, d + 5, kNameLen);
        want[kNameLen - 1] = '\0';
        if (!want[0]) snprintf(want, sizeof(want), "Player");
        // Names must be unique in the lobby: "Name", "Name (2)", ...
        char name[kNameLen];
        snprintf(name, sizeof(name), "%s", want);
        for (int k = 2; k < 10; ++k) {
            bool taken = false;
            for (int i = 0; i < kMaxPlayers; ++i)
                if (i != slot && g_snap.players[i].used && !lstrcmpiA(g_snap.players[i].name, name)) taken = true;
            if (!taken) break;
            snprintf(name, sizeof(name), "%.*s (%d)", kNameLen - 6, want, k);
        }
        std::memcpy(g_snap.players[slot].name, name, kNameLen);
        LOG("net: player %d (%s) joined", slot, g_snap.players[slot].name);
    }
    LeaveCriticalSection(&g_cs);
    if (slot < 0) {
        unsigned char r = FULL;
        send_frame(p.s, REJECT, &r, 1);
        close_conn(p);
        return;
    }
    p.slot = slot;
    unsigned char sl = (unsigned char)slot;
    send_frame(p.s, WELCOME, &sl, 1);
    send_settings(p.s);
    broadcast_lobby();
}

void beacon_answer()
{
    char buf[64];
    sockaddr_in from;
    int flen = sizeof(from);
    int r = recvfrom(g_beacon, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
    if (r < 5 || std::memcmp(buf, "RCLQ", 4) != 0) return;
    char ans[4 + 1 + 4 + 4 + 2 + 1 + 1 + kNameLen];
    int n = 0, players = 0;
    EnterCriticalSection(&g_cs);
    for (auto& p : g_snap.players) players += p.used;
    LeaveCriticalSection(&g_cs);
    unsigned short port = (unsigned short)g_port;
    std::memcpy(ans + n, "RCLA", 4); n += 4;
    ans[n++] = (char)kProto;
    std::memcpy(ans + n, &g_build, 4); n += 4;
    std::memcpy(ans + n, &g_instance, 4); n += 4;
    std::memcpy(ans + n, &port, 2); n += 2;
    ans[n++] = (char)players;
    ans[n++] = (char)kMaxPlayers;
    std::memcpy(ans + n, g_name, kNameLen); n += kNameLen;
    sendto(g_beacon, ans, n, 0, (sockaddr*)&from, flen);
}

// -- client -----------------------------------------------------------------

void start_join(const char* target)
{
    stop_all();
    char host[64];
    snprintf(host, sizeof(host), "%s", target);
    int port = g_port;
    if (char* colon = std::strchr(host, ':')) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (!host[0] || getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        set_mode(Mode::Idle);
        set_status(Status::BadAddress, target);
        return;
    }
    sockaddr_in a = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    a.sin_port = htons((u_short)port);

    g_session.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    nonblocking(g_session.s);
    g_session.rxlen = 0;
    connect(g_session.s, (sockaddr*)&a, sizeof(a)); // completes asynchronously
    g_connect_start = g_session.last_rx = GetTickCount();
    g_hello_sent = false;
    char d[64];
    snprintf(d, sizeof(d), "%s:%d", host, port);
    LOG("net: connecting to %s", d);
    set_mode(Mode::Connecting);
    set_status(Status::Connecting, d);
}

void client_fail(Status s)
{
    LOG("net: session ended (status %d)", (int)s);
    if (g_racing && race::active()) {
        // Alone now: everyone else's car drives itself from where we lost them.
        EnterCriticalSection(&g_cs);
        int me = g_snap.my_slot >= 0 ? g_car_of_slot[g_snap.my_slot] : -1;
        LeaveCriticalSection(&g_cs);
        for (int slot = 0; slot < kMaxPlayers; ++slot) {
            int car = g_car_of_slot[slot];
            if (car >= 0 && car != me) race::on_drop(car, g_last_tick[car] + 1);
        }
    }
    g_racing = false;
    close_conn(g_session);
    set_mode(Mode::Idle);
    set_status(s);
}

void client_on_frame(unsigned char type, const char* d, int len)
{
    switch (type) {
    case WELCOME:
        if (len < 1) return;
        EnterCriticalSection(&g_cs);
        g_snap.my_slot = (unsigned char)d[0];
        LeaveCriticalSection(&g_cs);
        set_mode(Mode::Joined);
        set_status(Status::None);
        LOG("net: joined as player %d", (unsigned char)d[0]);
        g_racing = false;
        send_my_car();
        send_my_status();
        break;
    case REJECT:
        client_fail(len > 0 && d[0] == VERSION ? Status::RejectedVersion : Status::RejectedFull);
        break;
    case LOBBY: {
        if (len < 1) return;
        int n = (unsigned char)d[0];
        if (len < 1 + n * (1 + kNameLen)) return;
        EnterCriticalSection(&g_cs);
        std::memset(g_snap.players, 0, sizeof(g_snap.players));
        for (int i = 0; i < n; ++i) {
            const char* e = d + 1 + i * (1 + kNameLen);
            int slot = (unsigned char)e[0];
            if (slot >= kMaxPlayers) continue;
            g_snap.players[slot].used = true;
            std::memcpy(g_snap.players[slot].name, e + 1, kNameLen);
            g_snap.players[slot].name[kNameLen - 1] = '\0';
        }
        LeaveCriticalSection(&g_cs);
        g_dirty = true;
        break;
    }
    default:
        client_race_frame(type, d, len);
        break;
    }
}

void send_query()
{
    char q[5] = {'R', 'C', 'L', 'Q', (char)kProto};
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)(g_port - 1));
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(g_query, q, sizeof(q), 0, (sockaddr*)&a, sizeof(a));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // same-PC testing
    sendto(g_query, q, sizeof(q), 0, (sockaddr*)&a, sizeof(a));
}

void query_answer()
{
    char buf[128];
    sockaddr_in from;
    int flen = sizeof(from);
    int r = recvfrom(g_query, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
    if (r < 4 + 1 + 4 + 4 + 2 + 2 + kNameLen || std::memcmp(buf, "RCLA", 4) != 0) return;
    unsigned build, id;
    unsigned short port;
    std::memcpy(&build, buf + 5, 4);
    std::memcpy(&id, buf + 9, 4);
    std::memcpy(&port, buf + 13, 2);
    if (id == g_instance) return; // ourselves
    bool loopback = (ntohl(from.sin_addr.s_addr) >> 24) == 127;

    EnterCriticalSection(&g_cs);
    int idx = -1;
    for (int i = 0; i < g_snap.ngames; ++i)
        if (g_game_ids[i] == id) { idx = i; break; }
    if (idx < 0 && g_snap.ngames < kMaxGames) idx = g_snap.ngames++;
    if (idx >= 0) {
        GameInfo& g = g_snap.games[idx];
        bool had_lan_addr = g_game_ids[idx] == id && std::strncmp(g.addr, "127.", 4) != 0;
        if (!(loopback && had_lan_addr)) { // prefer the LAN address for display
            char ip[32];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            snprintf(g.addr, sizeof(g.addr), "%s:%u", ip, port);
        }
        std::memcpy(g.host_name, buf + 17, kNameLen);
        g.host_name[kNameLen - 1] = '\0';
        g.players = (unsigned char)buf[15];
        g.max_players = (unsigned char)buf[16];
        g.compatible = (unsigned char)buf[4] == kProto && build == g_build;
        g_game_ids[idx] = id;
        g_game_seen[idx] = GetTickCount();
    }
    LeaveCriticalSection(&g_cs);
    g_dirty = true;
}

void expire_games()
{
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_snap.ngames;) {
        if ((int)(now - g_game_seen[i]) > (int)kGameExpiry) {
            --g_snap.ngames;
            g_snap.games[i] = g_snap.games[g_snap.ngames];
            g_game_ids[i] = g_game_ids[g_snap.ngames];
            g_game_seen[i] = g_game_seen[g_snap.ngames];
            g_dirty = true;
        } else {
            ++i;
        }
    }
    LeaveCriticalSection(&g_cs);
}

// -- thread -----------------------------------------------------------------

void tick()
{
    // Commands from the UI.
    EnterCriticalSection(&g_cs);
    Cmd cmd = g_cmd;
    char addr[64];
    std::memcpy(addr, g_cmd_addr, sizeof(addr));
    g_cmd = Cmd::None;
    LeaveCriticalSection(&g_cs);
    switch (cmd) {
    case Cmd::Host: start_host(); break;
    case Cmd::Join: start_join(addr); break;
    case Cmd::Leave:
        LOG("net: leaving");
        stop_all();
        set_mode(Mode::Idle);
        set_status(Status::None);
        break;
    case Cmd::None: break;
    }

    Mode mode;
    EnterCriticalSection(&g_cs);
    mode = g_snap.mode;
    LeaveCriticalSection(&g_cs);

    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    if (g_query != INVALID_SOCKET) FD_SET(g_query, &rd);
    if (g_beacon != INVALID_SOCKET) FD_SET(g_beacon, &rd);
    if (g_listen != INVALID_SOCKET) FD_SET(g_listen, &rd);
    for (auto& p : g_peers)
        if (p.s != INVALID_SOCKET) FD_SET(p.s, &rd);
    if (g_session.s != INVALID_SOCKET) {
        FD_SET(g_session.s, &rd);
        if (mode == Mode::Connecting) FD_SET(g_session.s, &wr);
    }
    timeval tv = {0, 50 * 1000};
    if (select(0, &rd, &wr, nullptr, &tv) == SOCKET_ERROR) Sleep(50);

    DWORD now = GetTickCount();
    if (g_query != INVALID_SOCKET && FD_ISSET(g_query, &rd)) query_answer();
    if (g_beacon != INVALID_SOCKET && FD_ISSET(g_beacon, &rd)) beacon_answer();
    if (g_listen != INVALID_SOCKET && FD_ISSET(g_listen, &rd)) host_accept();
    for (auto& p : g_peers) {
        if (p.s == INVALID_SOCKET || !FD_ISSET(p.s, &rd)) continue;
        if (!pump(p, [&](unsigned char t, const char* d, int l) { host_on_frame(p, t, d, l); })) {
            int slot = p.slot;
            close_conn(p);
            free_slot(slot);
        }
    }
    if (g_session.s != INVALID_SOCKET) {
        if (mode == Mode::Connecting && FD_ISSET(g_session.s, &wr) && !g_hello_sent) {
            int err = 0, len = sizeof(err);
            getsockopt(g_session.s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            if (err) {
                client_fail(Status::ConnectFailed);
            } else {
                char hello[1 + 4 + kNameLen] = {(char)kProto};
                std::memcpy(hello + 1, &g_build, 4);
                EnterCriticalSection(&g_cs);
                std::memcpy(hello + 5, g_name, kNameLen);
                LeaveCriticalSection(&g_cs);
                send_frame(g_session.s, HELLO, hello, sizeof(hello));
                g_hello_sent = true;
            }
        }
        if (g_session.s != INVALID_SOCKET && FD_ISSET(g_session.s, &rd) &&
            !pump(g_session, client_on_frame)) {
            EnterCriticalSection(&g_cs);
            Mode m = g_snap.mode;
            LeaveCriticalSection(&g_cs);
            client_fail(m == Mode::Joined ? Status::HostClosed : Status::ConnectFailed);
        }
    }

    // Race state changes.
    if (mode == Mode::Joined && g_session.s != INVALID_SOCKET) {
        if (g_my_car_dirty) send_my_car();
        if (g_my_busy_dirty) send_my_status();
    }
    if (mode == Mode::Hosting && g_settings_dirty) {
        g_settings_dirty = false;
        send_settings();
    }

    // Timers.
    if (mode == Mode::Idle && now - g_last_query >= kQueryInterval) {
        g_last_query = now;
        send_query();
    }
    if (mode == Mode::Idle || mode == Mode::Connecting) expire_games();
    if (mode == Mode::Connecting && g_session.s != INVALID_SOCKET &&
        now - g_connect_start > kConnectTimeout)
        client_fail(Status::ConnectFailed);
    bool racing = g_racing && race::active();
    DWORD timeout = racing ? kRaceTimeout : kPeerTimeout;
    // last_rx may be newer than `now` (set while pumping above): compare signed.
    auto silent = [&](DWORD last) { return (int)(now - last) > (int)timeout; };
    if (now - g_last_ping >= (racing ? kRacePingInterval : kPingInterval)) {
        g_last_ping = now;
        for (auto& p : g_peers)
            if (p.s != INVALID_SOCKET) send_frame(p.s, PING);
        if (mode == Mode::Joined) send_frame(g_session.s, PING);
    }
    for (auto& p : g_peers) {
        if (p.s != INVALID_SOCKET && silent(p.last_rx)) {
            LOG("net: player %d timed out", p.slot);
            int slot = p.slot;
            close_conn(p);
            free_slot(slot);
        }
    }
    if (mode == Mode::Joined && g_session.s != INVALID_SOCKET && silent(g_session.last_rx))
    {
        LOG("net: no data from the host for %lu ms", (unsigned long)(now - g_session.last_rx));
        client_fail(Status::ConnectionLost);
    }

    if (g_dirty) {
        g_dirty = false;
        notify_ui();
    }
}

DWORD WINAPI thread_main(LPVOID)
{
    for (;;) tick();
}

} // namespace

void start(HWND notify, UINT msg)
{
    static bool started = false;
    g_notify = notify;
    g_msg = msg;
    if (started) return;
    started = true;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_port = config::get_int("LAN", "Port", kDefaultPort);
    if (!g_build) g_build = compute_build();
    g_instance = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (unsigned)(UINT_PTR)&g_cs;
    g_snap.my_slot = -1;

    g_query = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE;
    setsockopt(g_query, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
    nonblocking(g_query);
    find_local_ip();

    LOG("net: started (port %d, build id %#x)", g_port, g_build);
    if (HANDLE t = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr)) CloseHandle(t);
}

void set_build(unsigned build) { g_build = build; }

void set_name(const char* name)
{
    EnterCriticalSection(&g_cs);
    snprintf(g_name, sizeof(g_name), "%s", name && *name ? name : "Player");
    LeaveCriticalSection(&g_cs);
}

void host()
{
    EnterCriticalSection(&g_cs);
    g_cmd = Cmd::Host;
    LeaveCriticalSection(&g_cs);
}

void join(const char* addr)
{
    EnterCriticalSection(&g_cs);
    g_cmd = Cmd::Join;
    snprintf(g_cmd_addr, sizeof(g_cmd_addr), "%s", addr);
    LeaveCriticalSection(&g_cs);
}

void leave()
{
    EnterCriticalSection(&g_cs);
    g_cmd = Cmd::Leave;
    LeaveCriticalSection(&g_cs);
}

void snapshot(Snapshot* out)
{
    EnterCriticalSection(&g_cs);
    *out = g_snap;
    LeaveCriticalSection(&g_cs);
}

// -- races --------------------------------------------------------------------

void set_settings(const RaceSettings& rs)
{
    EnterCriticalSection(&g_cs);
    g_snap.settings = rs;
    LeaveCriticalSection(&g_cs);
    g_settings_dirty = true;
}

void set_car(const game::Car& car)
{
    EnterCriticalSection(&g_cs);
    if (!g_my_car_set || std::memcmp(&car, &g_my_car, sizeof(car)) != 0) {
        g_my_car = car;
        g_my_car_set = true;
        g_my_car_dirty = true;
        if (g_snap.mode == Mode::Hosting) {
            g_cars[0] = car;
            g_snap.have_car[0] = true;
        }
    }
    LeaveCriticalSection(&g_cs);
}

bool car_of(int slot, game::Car* out)
{
    if (slot < 0 || slot >= kMaxPlayers) return false;
    EnterCriticalSection(&g_cs);
    bool ok = g_snap.have_car[slot];
    if (ok) *out = g_cars[slot];
    LeaveCriticalSection(&g_cs);
    return ok;
}

void set_busy(bool busy)
{
    EnterCriticalSection(&g_cs);
    if (busy != g_my_busy) {
        g_my_busy = busy;
        g_my_busy_dirty = true;
    }
    LeaveCriticalSection(&g_cs);
}

void start_race(const race::Start& st)
{
    begin_racing(st);
    send_to_clients(START, &st, sizeof(st));
    LOG("net: race started for %d cars", st.ncars);
    race::begin(st, 0, race_send_input, race_send_hash);
}

bool take_start(race::Start* out, int* my_slot)
{
    EnterCriticalSection(&g_cs);
    bool ok = g_have_start;
    if (ok) {
        *out = g_pending_start;
        *my_slot = g_snap.my_slot;
        g_have_start = false;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void begin_pending()
{
    static race::Start st;
    int slot;
    if (take_start(&st, &slot)) race::begin(st, slot, race_send_input, race_send_hash);
}

} // namespace rclan::net
