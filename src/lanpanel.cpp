// In-game LAN panel, drawn in the style of the game's main-menu board:
// gold frame, purple board, dark slots between gold rails with yellow italic
// caps (teal on hover), green LCD boxes for lists and the address field.

#include "lanpanel.h"

#include "config.h"
#include "game.h"
#include "log.h"
#include "net.h"
#include "picker.h"
#include "race.h"
#include "wnd.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace rclan::panel {

namespace {

// ---------------------------------------------------------------------------
// Geometry

constexpr int kW = 330, kH = 606;          // same height as the game screen
constexpr int kCollapsedH = 64;
constexpr int kGap = 40;                   // distance to the game screen
constexpr int kRefW = 806, kRefH = 606;    // game screen (with border)
constexpr int kPad = 22;                   // frame + inner padding
constexpr UINT kNetMsg = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kPickTimer = 2; // reads the game's selection screen while it is open

// ---------------------------------------------------------------------------
// Colors (sampled from the main-menu board)

constexpr COLORREF kGoldHi = RGB(250, 222, 130), kGold = RGB(208, 160, 80), kGoldDk = RGB(110, 76, 30);
constexpr COLORREF kPurpleTop = RGB(92, 68, 118), kPurpleBot = RGB(38, 28, 62);
constexpr COLORREF kSlotTop = RGB(64, 56, 52), kSlotBot = RGB(28, 24, 24);
constexpr COLORREF kHoverTop = RGB(52, 170, 156), kHoverBot = RGB(18, 92, 86);
constexpr COLORREF kYellow = RGB(250, 210, 48), kWhite = RGB(255, 246, 214);
constexpr COLORREF kLcdBg = RGB(6, 36, 14), kLcdEdge = RGB(70, 150, 70), kLcdText = RGB(90, 255, 100);
constexpr COLORREF kLcdDim = RGB(40, 130, 50), kLcdSel = RGB(24, 92, 38);
constexpr COLORREF kError = RGB(255, 130, 80);

// ---------------------------------------------------------------------------
// Text (German / English)

enum S {
    TITLE, PLAYER, HOST, FOUND, SEARCHING, ADDRESS, CONNECT, HOSTING, JOIN_VIA, PLAYERS, FREE,
    YOU, HOST_TAG, WAITING_PLAYERS, READY, CLOSE, CONNECTING, CANCEL, CONNECTED, WAIT_HOST,
    LEAVE, OTHER_VERSION, E_PORT, E_CONNECT, E_ADDRESS, E_FULL, E_VERSION, E_LOST, E_CLOSED,
    RACE, START_RACE, NEXT_RACE, COMPUTER, O_TYPE, O_TRACK, O_CIRCUIT, O_MODE, O_LENGTH, O_POWER,
    O_SHERIFF, O_DIFF, O_BOTS, V_SINGLE, V_DUEL, V_SERIES, V_LAPS, V_MINUTES, V_ON, V_OFF, V_EASY,
    V_HARD, V_FILL, V_NONE, V_AMATEUR, V_PRO, V_ALL, V_RANDOM, E_DUEL, E_NOCAR, E_BUSY, E_NOGAME,
    SERIES_RACE, RACING, CLICK_HINT, R_LEFT, R_WAIT, O_GAME, V_SOCCER, V_BATTLE, V_SUPER, V_CUSTOM,
    E_MENU, PICKING, E_PICKING,
    S_COUNT
};
const char* const kDe[S_COUNT] = {
    "NETZWERK", "Spieler: %s", "SPIEL ER\xD6" "FFNEN", "GEFUNDENE SPIELE", "Suche im Netzwerk",
    "IP-ADRESSE", "VERBINDEN", "SPIEL ER\xD6" "FFNET", "Mitspieler verbinden sich mit", "SPIELER",
    "- frei -", "(du)", "(Host)", "Warte auf Mitspieler ...",
    "Alle bereit. Der Rennstart \xFC" "ber das Netzwerk ist noch in Arbeit.", "SCHLIESSEN",
    "VERBINDE ...", "ABBRECHEN", "VERBUNDEN", "Warte auf den Host ...", "VERLASSEN",
    "andere Version",
    "Port %s ist belegt (l\xE4" "uft schon ein Spiel?)", "Keine Verbindung m\xF6" "glich.",
    "Adresse ung\xFC" "ltig: %s", "Das Spiel ist voll.",
    "Anderes Spiel/Mod-Version - beide brauchen dieselbe Version.",
    "Verbindung zum Host verloren.", "Der Host hat das Spiel geschlossen.",
    "RENNEN", "RENNEN STARTEN", "N\xC4" "CHSTES RENNEN", "Computer [CPU]", "Rennart", "Strecke",
    "Meisterschaft", "Rennmodus", "Rennl\xE4" "nge", "Power-Ups", "Streckensheriff", "Schwierigkeit",
    "Computergegner", "Einzelstrecke", "Funkduell", "Streckenrennen", "Runden", "Minuten", "Ein",
    "Aus", "Leicht", "Schwer", "Auff\xFC" "llen", "Keine", "Amateur", "Profi", "Alle Strecken",
    "Zuf\xE4" "llig", "Funkduell geht nur mit genau 2 Spielern.",
    "Warte auf die Fahrzeugdaten der Mitspieler ...", "Ein Mitspieler ist noch in einem Rennen.",
    "Rennen \xFC" "bers Netzwerk gehen mit dieser Spielversion nicht.", "Rennen %d von %d: %s",
    "Rennen l\xE4" "uft ...", "Klick: weiter, Rechtsklick: zur\xFC" "ck",
    "%s ist weg - der Computer f\xE4" "hrt weiter", "Warte auf %s ...", "Spiel", "Funk-Fu\xDF" "ball",
    "Kampfball", "Super", "Eigene (%d)", "Die Auswahl geht nur vom Hauptmen\xFC" " aus.",
    "W\xE4" "hle im Spielfenster. Hauptmen\xFC" " oder Weiter f\xFC" "hrt zur\xFC" "ck.",
    "Erst die Auswahl im Spielfenster beenden.",
};
const char* const kEn[S_COUNT] = {
    "NETWORK", "Player: %s", "HOST GAME", "GAMES FOUND", "Searching the network",
    "IP ADDRESS", "CONNECT", "GAME OPEN", "Players connect to", "PLAYERS",
    "- free -", "(you)", "(host)", "Waiting for players ...",
    "Everyone's ready. Starting the race over the network is still in progress.", "CLOSE",
    "CONNECTING ...", "CANCEL", "CONNECTED", "Waiting for the host ...", "LEAVE",
    "other version",
    "Port %s is in use (is a game already running?)", "Could not connect.",
    "Invalid address: %s", "The game is full.",
    "Different game/mod version - both need the same version.",
    "Lost connection to the host.", "The host closed the game.",
    "RACE", "START RACE", "NEXT RACE", "Computer [CPU]", "Race type", "Track", "Championship",
    "Race mode", "Race length", "Power-ups", "Track sheriff", "Difficulty", "Computer players",
    "Single track", "R/C Battle", "Track series", "Laps", "Minutes", "On", "Off", "Easy", "Hard",
    "Fill up", "None", "Amateur", "Pro", "All tracks", "Random",
    "R/C Battle needs exactly 2 players.", "Waiting for the other players' car data ...",
    "A player is still in a race.", "Network races don't work with this game version.",
    "Race %d of %d: %s", "Race running ...", "Click: next, right click: back",
    "%s left - the computer drives on", "Waiting for %s ...", "Game", "R/C Soccer",
    "Battle Ball", "Super", "Custom (%d)", "Choosing works from the main menu only.",
    "Choose in the game window. Main Menu or Continue leads back.",
    "Finish choosing in the game window first.",
};
const char* const* g_text = kDe;

const char* T(S s) { return g_text[s]; }

void pick_language()
{
    char lang[8];
    config::get_str("LAN", "Language", "", lang, sizeof(lang));
    if (lang[0]) {
        g_text = (lang[0] == 'e' || lang[0] == 'E') ? kEn : kDe;
        return;
    }
    // Follow the game: its menu starts with "&Datei" in the German version.
    char first[32] = "";
    if (HMENU m = wnd::game_menu()) GetMenuStringA(m, 0, first, sizeof(first), MF_BYPOSITION);
    if (first[0]) {
        g_text = std::strstr(first, "Datei") ? kDe : kEn;
        return;
    }
    g_text = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN ? kDe : kEn;
}

// ---------------------------------------------------------------------------
// State (GUI thread)

enum Hit { H_NONE, H_HEADER, H_HOST, H_FIELD, H_CONNECT, H_LEAVE, H_START, H_OPT0 = 50, H_GAME0 = 100 };

HWND g_wnd, g_main;
bool g_collapsed, g_overlay, g_visible;
int g_hover = H_NONE;
bool g_field_focus, g_caret_on;
char g_addr[64];
char g_name[net::kNameLen] = "Player";
int g_anim;
net::Snapshot g_snap;
net::RaceSettings g_rs;     // host: current race options
int g_series_next;          // host: index of the next race of a track series
char g_race_msg[160];       // host: why the race can't start
bool g_was_racing;

struct HitRect {
    RECT r;
    int id;
};
HitRect g_hits[40];
int g_nhits;

HFONT g_title_font, g_button_font, g_small_font, g_lcd_font, g_lcd_big_font, g_opt_font;

void add_hit(const RECT& r, int id)
{
    if (g_nhits < (int)(sizeof(g_hits) / sizeof(g_hits[0]))) g_hits[g_nhits++] = {r, id};
}

int hit_test(int x, int y)
{
    POINT p = {x, y};
    for (int i = g_nhits - 1; i >= 0; --i)
        if (PtInRect(&g_hits[i].r, p)) return g_hits[i].id;
    return H_NONE;
}

void read_name()
{
    char n[net::kNameLen] = "";
    config::get_str("LAN", "Name", "", n, sizeof(n));
    if (!n[0]) { // the game's current player 1 profile
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sConfig\\prefs.ini", config::game_dir());
        GetPrivateProfileStringA("Player", "Player1", "", n, sizeof(n), path);
    }
    if (!n[0]) snprintf(n, sizeof(n), "Player");
    if (std::strcmp(n, g_name) != 0) {
        snprintf(g_name, sizeof(g_name), "%s", n);
        net::set_name(g_name);
    }
}

// ---------------------------------------------------------------------------
// Fonts

int CALLBACK font_found(const LOGFONTA*, const TEXTMETRICA*, DWORD, LPARAM found)
{
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

bool font_exists(HDC dc, const char* face)
{
    LOGFONTA lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "%s", face);
    bool found = false;
    EnumFontFamiliesExA(dc, &lf, font_found, (LPARAM)&found, 0);
    return found;
}

const char* first_font(HDC dc, std::initializer_list<const char*> faces)
{
    for (const char* f : faces)
        if (font_exists(dc, f)) return f;
    return "Arial";
}

HFONT make_font(const char* face, int h, int weight, bool italic, int width = 0)
{
    return CreateFontA(-h, width, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, face);
}

void create_fonts()
{
    HDC dc = GetDC(nullptr);
    const char* display = first_font(dc, {"Arial Black", "Impact", "Arial"});
    const char* lcd = first_font(dc, {"Lucida Console", "Courier New", "Consolas"});
    ReleaseDC(nullptr, dc);
    bool black = std::strcmp(display, "Arial") != 0;
    int heavy = black ? FW_NORMAL : FW_HEAVY; // Arial Black/Impact are heavy already
    g_title_font = make_font(display, 34, black ? heavy : FW_BOLD, true);
    g_button_font = make_font(display, 19, black ? heavy : FW_BOLD, true);
    g_small_font = make_font("Arial", 14, FW_BOLD, false);
    g_lcd_font = make_font(lcd, 15, FW_BOLD, false);
    g_opt_font = make_font(lcd, 13, FW_BOLD, false);
    g_lcd_big_font = make_font(lcd, 20, FW_BOLD, false);
    LOG("panel: fonts '%s' / '%s'", display, lcd);
}

// ---------------------------------------------------------------------------
// Drawing helpers

COLORREF mix(COLORREF a, COLORREF b, int num, int den)
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * num / den,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * num / den,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * num / den);
}

void vgrad(HDC dc, const RECT& r, COLORREF top, COLORREF bot)
{
    int h = r.bottom - r.top;
    if (h <= 0) return;
    for (int y = 0; y < h; ++y) {
        HBRUSH b = CreateSolidBrush(mix(top, bot, y, h > 1 ? h - 1 : 1));
        RECT row = {r.left, r.top + y, r.right, r.top + y + 1};
        FillRect(dc, &row, b);
        DeleteObject(b);
    }
}

void fill(HDC dc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void frame(HDC dc, const RECT& r, COLORREF c, int w = 1)
{
    for (int i = 0; i < w; ++i) {
        RECT f = {r.left + i, r.top + i, r.right - i, r.bottom - i};
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &f, b);
        DeleteObject(b);
    }
}

// Text with a dark outline + drop shadow, like the game's menu lettering.
void text(HDC dc, const char* s, RECT r, UINT fmt, COLORREF c, HFONT f, bool shadow = true)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    if (shadow) {
        SetTextColor(dc, RGB(0, 0, 0));
        const int off[][2] = {{2, 2}, {3, 3}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (auto& o : off) {
            RECT s2 = {r.left + o[0], r.top + o[1], r.right + o[0], r.bottom + o[1]};
            DrawTextA(dc, s, -1, &s2, fmt);
        }
    }
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
}

// Rounded gold frame with the purple board inside.
void board(HDC dc, const RECT& r)
{
    HRGN outer = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, 26, 26);
    SelectClipRgn(dc, outer);
    vgrad(dc, r, kGoldHi, kGoldDk);
    RECT in = {r.left + 6, r.top + 6, r.right - 6, r.bottom - 6};
    HRGN inner = CreateRoundRectRgn(in.left, in.top, in.right + 1, in.bottom + 1, 18, 18);
    SelectClipRgn(dc, inner);
    vgrad(dc, in, kPurpleTop, kPurpleBot);
    // subtle dark inner edge
    HBRUSH edge = CreateSolidBrush(RGB(20, 14, 30));
    FrameRgn(dc, inner, edge, 2, 2);
    DeleteObject(edge);
    SelectClipRgn(dc, nullptr);
    DeleteObject(inner);
    DeleteObject(outer);
}

// Gold rail (the bars between the game's menu slots).
void rail(HDC dc, int x0, int x1, int y)
{
    RECT r = {x0, y, x1, y + 4};
    vgrad(dc, r, kGoldHi, kGoldDk);
}

// A clickable slot button.
void button(HDC dc, const RECT& r, const char* label, int id)
{
    bool hot = g_hover == id;
    rail(dc, r.left - 4, r.right + 4, r.top - 4);
    RECT body = r;
    vgrad(dc, body, hot ? kHoverTop : kSlotTop, hot ? kHoverBot : kSlotBot);
    rail(dc, r.left - 4, r.right + 4, r.bottom);
    text(dc, label, body, DT_CENTER | DT_VCENTER | DT_SINGLELINE, hot ? kWhite : kYellow, g_button_font);
    add_hit(r, id);
}

// Green LCD box (track-selection style).
void lcd(HDC dc, const RECT& r)
{
    fill(dc, r, kLcdBg);
    frame(dc, r, kLcdEdge, 2);
}

void label(HDC dc, int y, const char* s)
{
    RECT r = {kPad, y, kW - kPad, y + 18};
    text(dc, s, r, DT_LEFT | DT_SINGLELINE, kGoldHi, g_small_font);
}

// Wrapped info/status text.
int para(HDC dc, int y, const char* s, COLORREF c)
{
    RECT r = {kPad, y, kW - kPad, kH - 16};
    HGDIOBJ old = SelectObject(dc, g_small_font);
    RECT calc = r;
    DrawTextA(dc, s, -1, &calc, DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, old);
    text(dc, s, r, DT_LEFT | DT_WORDBREAK, c, g_small_font);
    return y + (calc.bottom - calc.top) + 6;
}

void status_line(HDC dc, int y)
{
    using net::Status;
    char buf[160];
    const char* d = g_snap.status_detail;
    switch (g_snap.status) {
    case Status::PortInUse: snprintf(buf, sizeof(buf), T(E_PORT), d); break;
    case Status::ConnectFailed: snprintf(buf, sizeof(buf), "%s", T(E_CONNECT)); break;
    case Status::BadAddress: snprintf(buf, sizeof(buf), T(E_ADDRESS), d); break;
    case Status::RejectedFull: snprintf(buf, sizeof(buf), "%s", T(E_FULL)); break;
    case Status::RejectedVersion: snprintf(buf, sizeof(buf), "%s", T(E_VERSION)); break;
    case Status::ConnectionLost: snprintf(buf, sizeof(buf), "%s", T(E_LOST)); break;
    case Status::HostClosed: snprintf(buf, sizeof(buf), "%s", T(E_CLOSED)); break;
    default: return;
    }
    para(dc, y, buf, kError);
}

// Player list (4 slots) in an LCD box.
int player_list(HDC dc, int y)
{
    label(dc, y, T(PLAYERS));
    y += 20;
    RECT box = {kPad, y, kW - kPad, y + 4 * 22 + 8};
    lcd(dc, box);
    // The host's own settings are current; the snapshot follows them later.
    const net::RaceSettings& rs = g_snap.mode == net::Mode::Hosting ? g_rs : g_snap.settings;
    for (int i = 0; i < net::kMaxPlayers; ++i) {
        const net::Player& p = g_snap.players[i];
        char line[96];
        if (p.used)
            snprintf(line, sizeof(line), "%d %s %s", i + 1, p.name,
                     i == g_snap.my_slot ? T(YOU) : i == 0 ? T(HOST_TAG) : "");
        else
            snprintf(line, sizeof(line), "%d %s", i + 1,
                     rs.bots && rs.type != net::RaceDuel ? T(COMPUTER) : T(FREE));
        RECT r = {box.left + 10, box.top + 4 + i * 22, box.right - 8, box.top + 4 + (i + 1) * 22};
        text(dc, line, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
             p.used ? kLcdText : kLcdDim, g_lcd_font, false);
    }
    return box.bottom + 12;
}

void paint_idle(HDC dc, int y)
{
    char buf[96];
    snprintf(buf, sizeof(buf), T(PLAYER), g_name);
    para(dc, y, buf, kWhite);
    y += 30;

    button(dc, {kPad + 4, y, kW - kPad - 4, y + 40}, T(HOST), H_HOST);
    y += 62;

    label(dc, y, T(FOUND));
    y += 20;
    RECT box = {kPad, y, kW - kPad, y + 4 * 28 + 10};
    lcd(dc, box);
    if (g_snap.ngames == 0) {
        char s[64];
        snprintf(s, sizeof(s), "%s%.*s", T(SEARCHING), g_anim % 4, "...");
        RECT r = {box.left + 10, box.top + 6, box.right - 8, box.top + 32};
        text(dc, s, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE, kLcdDim, g_lcd_font, false);
    }
    for (int i = 0; i < g_snap.ngames && i < 4; ++i) {
        const net::GameInfo& g = g_snap.games[i];
        RECT r = {box.left + 4, box.top + 5 + i * 28, box.right - 4, box.top + 5 + (i + 1) * 28};
        int id = H_GAME0 + i;
        if (g_hover == id && g.compatible) fill(dc, r, kLcdSel);
        char line[96];
        if (g.compatible)
            snprintf(line, sizeof(line), "%s  %d/%d", g.host_name, g.players, g.max_players);
        else
            snprintf(line, sizeof(line), "%s  (%s)", g.host_name, T(OTHER_VERSION));
        RECT tr = {r.left + 6, r.top, r.right - 4, r.bottom};
        text(dc, line, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
             g.compatible ? kLcdText : kLcdDim, g_lcd_font, false);
        if (g.compatible) add_hit(r, id);
    }
    y = box.bottom + 18;

    label(dc, y, T(ADDRESS));
    y += 20;
    RECT field = {kPad, y, kW - kPad, y + 34};
    lcd(dc, field);
    if (g_field_focus) frame(dc, field, kLcdText, 2);
    char shown[80];
    snprintf(shown, sizeof(shown), "%s%s", g_addr, g_field_focus && g_caret_on ? "_" : "");
    RECT tr = {field.left + 10, field.top, field.right - 8, field.bottom};
    text(dc, shown, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE, kLcdText, g_lcd_big_font, false);
    add_hit(field, H_FIELD);
    y = field.bottom + 18;

    button(dc, {kPad + 4, y, kW - kPad - 4, y + 40}, T(CONNECT), H_CONNECT);
    y += 58;
    status_line(dc, y);
}

// -- race options -----------------------------------------------------------

enum OptRow { R_TYPE, R_TRACK, R_MODE, R_LENGTH, R_DIFF, R_POWER, R_SHERIFF, R_BOTS, R_COUNT };
bool is_checkbox(int row) { return row == R_POWER || row == R_SHERIFF || row == R_BOTS; }
// Funkduell: always timed (length = minutes), just the two players.
bool row_shown(int row, const net::RaceSettings& rs)
{
    return rs.type != net::RaceDuel || (row != R_MODE && row != R_BOTS);
}

void load_settings()
{
    g_rs.type = (unsigned char)config::get_int("Race", "Type", net::RaceSingle);
    g_rs.track = (unsigned char)config::get_int("Race", "Track", 0);
    g_rs.circuit = (unsigned char)config::get_int("Race", "Circuit", 0);
    g_rs.power_ups = (unsigned char)config::get_int("Race", "PowerUps", 1);
    g_rs.sheriff = (unsigned char)config::get_int("Race", "Sheriff", 1);
    g_rs.traps = 1;
    g_rs.minutes = (unsigned char)config::get_int("Race", "Minutes", 0);
    g_rs.length = (unsigned char)config::get_int("Race", "Length", 3);
    g_rs.hard = (unsigned char)config::get_int("Race", "Hard", 1);
    g_rs.bots = (unsigned char)config::get_int("Race", "Bots", 1);
    g_rs.game = (unsigned char)(config::get_int("Race", "Game", 0) != 0);
    char ids[512];
    config::get_str("Race", "Custom", "", ids, sizeof(ids));
    g_rs.ncustom = 0;
    for (char* t = std::strtok(ids, ","); t && g_rs.ncustom < sizeof(g_rs.custom); t = std::strtok(nullptr, ","))
        for (int i = 0; i < game::track_count(); ++i)
            if (!lstrcmpiA(game::track_id(i), t)) { g_rs.custom[g_rs.ncustom++] = (unsigned char)i; break; }
    if (g_rs.type > net::RaceSeries) g_rs.type = net::RaceSingle;
    if (g_rs.circuit > 3) g_rs.circuit = 0;
    if (g_rs.length < 1 || g_rs.length > 10) g_rs.length = 3;
}

void save_settings()
{
    char v[16];
    auto put = [&](const char* k, int x) { snprintf(v, sizeof(v), "%d", x); config::set_str("Race", k, v); };
    put("Type", g_rs.type);
    put("Track", g_rs.track);
    put("Circuit", g_rs.circuit);
    put("PowerUps", g_rs.power_ups);
    put("Sheriff", g_rs.sheriff);
    put("Minutes", g_rs.minutes);
    put("Length", g_rs.length);
    put("Hard", g_rs.hard);
    put("Bots", g_rs.bots);
    put("Game", g_rs.game);
    std::string ids;
    for (int i = 0; i < g_rs.ncustom; ++i) {
        if (i) ids += ',';
        ids += game::track_id(g_rs.custom[i]);
    }
    config::set_str("Race", "Custom", ids.c_str());
}

int circuit_tracks(int* out, int max) { return picker::circuit_tracks(g_rs, out, max); }

void option_text(int row, const net::RaceSettings& rs, char* name, char* value, int n)
{
    const char* v = "";
    char buf[64];
    switch (row) {
    case R_TYPE:
        snprintf(name, n, "%s", T(O_TYPE));
        v = rs.type == net::RaceDuel ? T(V_DUEL) : rs.type == net::RaceSeries ? T(V_SERIES) : T(V_SINGLE);
        break;
    case R_TRACK:
        if (rs.type == net::RaceSeries) {
            snprintf(name, n, "%s", T(O_CIRCUIT));
            const S c[] = {V_AMATEUR, V_PRO, V_CUSTOM, V_SUPER};
            snprintf(buf, sizeof(buf), T(c[rs.circuit & 3]), rs.ncustom);
            v = buf;
        } else if (rs.type == net::RaceDuel) {
            snprintf(name, n, "%s", T(O_GAME));
            v = rs.game ? T(V_BATTLE) : T(V_SOCCER);
        } else {
            snprintf(name, n, "%s", T(O_TRACK));
            v = game::track_count() ? game::track_name(rs.track % game::track_count()) : "-";
        }
        break;
    case R_MODE: snprintf(name, n, "%s", T(O_MODE)); v = rs.minutes ? T(V_MINUTES) : T(V_LAPS); break;
    case R_LENGTH:
        snprintf(name, n, "%s", T(O_LENGTH));
        snprintf(buf, sizeof(buf), "%d %s", rs.length,
                 rs.minutes || rs.type == net::RaceDuel ? T(V_MINUTES) : T(V_LAPS));
        v = buf;
        break;
    case R_POWER: snprintf(name, n, "%s", T(O_POWER)); v = rs.power_ups ? "1" : ""; break;
    case R_SHERIFF: snprintf(name, n, "%s", T(O_SHERIFF)); v = rs.sheriff ? "1" : ""; break;
    case R_DIFF: snprintf(name, n, "%s", T(O_DIFF)); v = rs.hard ? T(V_HARD) : T(V_EASY); break;
    case R_BOTS: snprintf(name, n, "%s", T(O_BOTS)); v = rs.bots ? "1" : ""; break;
    }
    snprintf(value, n, "%s", v);
}

void step_option(int row, int dir)
{
    auto cyc = [&](unsigned char& x, int count) { x = (unsigned char)((x + dir + count) % count); };
    switch (row) {
    case R_TYPE: cyc(g_rs.type, 3); g_series_next = 0; break;
    case R_TRACK: {
        if (dir < 0) return;
        picker::Kind k = g_rs.type == net::RaceSeries ? picker::Circuit
                         : g_rs.type == net::RaceDuel  ? picker::Duel
                                                       : picker::Track;
        if (picker::active()) snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_PICKING));
        else if (!picker::open(k, g_rs)) snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_MENU));
        else { g_series_next = 0; g_race_msg[0] = '\0'; SetTimer(g_wnd, kPickTimer, 100, nullptr); }
        return;
    }
    case R_MODE: g_rs.minutes ^= 1; break;
    case R_LENGTH: g_rs.length = (unsigned char)((g_rs.length - 1 + dir + 10) % 10 + 1); break;
    case R_POWER: g_rs.power_ups ^= 1; break; // checkboxes: either click toggles
    case R_SHERIFF: g_rs.sheriff ^= 1; break;
    case R_DIFF: g_rs.hard ^= 1; break;
    case R_BOTS: g_rs.bots ^= 1; break;
    }
    save_settings();
    net::set_settings(g_rs);
    g_race_msg[0] = '\0';
}

// The options in an LCD box; clickable for the host.
int options_box(HDC dc, int y, const net::RaceSettings& rs, bool editable)
{
    label(dc, y, T(RACE));
    y += 18;
    constexpr int kRow = 21;
    int shown = 0;
    for (int row = 0; row < R_COUNT; ++row) shown += row_shown(row, rs);
    RECT box = {kPad, y, kW - kPad, y + shown * kRow + 8};
    lcd(dc, box);
    int name_w = 0; // name column: as wide as the longest name
    HGDIOBJ old_font = SelectObject(dc, g_opt_font);
    for (int row = 0; row < R_COUNT; ++row) {
        if (!row_shown(row, rs)) continue;
        char name[48], value[64];
        option_text(row, rs, name, value, sizeof(name));
        SIZE sz;
        if (GetTextExtentPoint32A(dc, name, (int)std::strlen(name), &sz) && sz.cx > name_w) name_w = sz.cx;
    }
    SelectObject(dc, old_font);
    name_w += 10;
    for (int row = 0, line = 0; row < R_COUNT; ++row) {
        if (!row_shown(row, rs)) continue;
        RECT r = {box.left + 4, box.top + 4 + line * kRow, box.right - 4, box.top + 4 + (line + 1) * kRow};
        ++line;
        int id = H_OPT0 + row;
        if (editable && g_hover == id) fill(dc, r, kLcdSel);
        char name[48], value[64];
        option_text(row, rs, name, value, sizeof(name));
        RECT nr = {r.left + 6, r.top, r.left + 6 + name_w, r.bottom};
        text(dc, name, nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, kLcdDim, g_opt_font, false);
        if (is_checkbox(row)) { // box in the value column
            int cy = (r.top + r.bottom) / 2, cx = r.left + 6 + name_w;
            RECT cb = {cx, cy - 6, cx + 12, cy + 6};
            frame(dc, cb, kLcdText);
            if (value[0]) {
                RECT in = {cb.left + 3, cb.top + 3, cb.right - 3, cb.bottom - 3};
                fill(dc, in, kLcdText);
            }
        } else {
            RECT vr = {r.left + 6 + name_w, r.top, r.right - 4, r.bottom};
            text(dc, value, vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, kLcdText, g_opt_font, false);
        }
        if (editable) add_hit(r, id);
    }
    return box.bottom + 10;
}

// Host: everything needed to start, or why not (into g_race_msg).
bool can_start()
{
    g_race_msg[0] = '\0';
    if (!game::available()) { snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_NOGAME)); return false; }
    if (picker::active()) { snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_PICKING)); return false; }
    int n = 0;
    for (int i = 0; i < net::kMaxPlayers; ++i) {
        if (!g_snap.players[i].used) continue;
        ++n;
        if (!g_snap.have_car[i]) { snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_NOCAR)); return false; }
        if (i != 0 && g_snap.busy[i]) { snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_BUSY)); return false; }
    }
    if (g_rs.type == net::RaceDuel && n != 2) {
        snprintf(g_race_msg, sizeof(g_race_msg), "%s", T(E_DUEL));
        return false;
    }
    return true;
}

void start_race()
{
    if (game::race_running() || race::active()) return;
    game::Car mine;
    if (game::local_car(&mine)) net::set_car(mine);
    net::snapshot(&g_snap);
    if (!can_start()) return;

    static race::Start st;
    std::memset(&st, 0, sizeof(st));
    game::Car cars[game::kMaxCars];
    const char* names[game::kMaxCars];
    int slots[game::kMaxCars], humans = 0;
    for (int i = 0; i < net::kMaxPlayers && humans < game::kMaxCars; ++i) {
        if (!g_snap.players[i].used || !net::car_of(i, &cars[humans])) continue;
        names[humans] = g_snap.players[i].name;
        slots[humans++] = i;
    }
    int track = g_rs.track;
    if (g_rs.type == net::RaceSeries) {
        int list[32];
        int count = circuit_tracks(list, 32);
        if (count == 0) return;
        track = list[g_series_next % count];
        g_series_next = (g_series_next + 1) % count;
    }
    unsigned seed = GetTickCount() * 2654435761u ^ (unsigned)GetCurrentProcessId();
    bool duel = g_rs.type == net::RaceDuel;
    st.opt = {duel ? (g_rs.game ? game::ModeBattle : game::ModeSoccer) : game::ModeSingle,
              humans, track, g_rs.power_ups, g_rs.traps, g_rs.sheriff, g_rs.minutes, g_rs.length,
              0, seed};
    int bots = duel || !g_rs.bots ? 0 : game::kMaxCars - humans; // duels: the two players only
    st.ncars = game::build_table(st.opt, cars, names, bots, g_rs.hard != 0, st.entries);
    for (int i = 0; i < game::kMaxCars; ++i) st.slot_of_car[i] = i < humans ? (signed char)slots[i] : -1;
    st.sim_seed = seed * 747796405u + 2891336453u;
    LOG("panel: starting race (mode %d, %d humans, %d cars, track %s)", st.opt.mode, humans, st.ncars,
        duel ? "-" : game::track_name(track));
    net::start_race(st);
}

void paint_race_area(HDC dc, int y, bool host)
{
    y = options_box(dc, y, host ? g_rs : g_snap.settings, host);
    if (host) {
        const char* label_text = T(START_RACE);
        char info[96] = "";
        if (g_rs.type == net::RaceSeries) {
            int list[32];
            int count = circuit_tracks(list, 32);
            if (count) {
                snprintf(info, sizeof(info), T(SERIES_RACE), g_series_next + 1, count,
                         game::track_name(list[g_series_next % count]));
                if (g_series_next) label_text = T(NEXT_RACE);
            }
        }
        button(dc, {kPad + 4, y + 4, kW - kPad - 4, y + 40}, label_text, H_START);
        y += 46;
        if (picker::active() && !g_race_msg[0]) para(dc, y, T(PICKING), kYellow);
        else para(dc, y, g_race_msg[0] ? g_race_msg : info, g_race_msg[0] ? kError : kWhite);
    } else {
        para(dc, y, race::active() ? T(RACING) : T(WAIT_HOST), kWhite);
    }
}

void paint_hosting(HDC dc, int y)
{
    y -= 4;
    label(dc, y, T(JOIN_VIA));
    y += 18;
    RECT ipbox = {kPad, y, kW - kPad, y + 28};
    lcd(dc, ipbox);
    text(dc, g_snap.local_ip, ipbox, DT_CENTER | DT_VCENTER | DT_SINGLELINE, kLcdText, g_lcd_big_font, false);
    y = ipbox.bottom + 8;
    y = player_list(dc, y);
    paint_race_area(dc, y, true);
    y = kH - kPad - 40 - 4;
    button(dc, {kPad + 4, y, kW - kPad - 4, y + 36}, T(CLOSE), H_LEAVE);
}

void paint_connecting(HDC dc, int y)
{
    text(dc, T(CONNECTING), {kPad, y, kW - kPad, y + 24}, DT_CENTER | DT_SINGLELINE, kYellow, g_button_font);
    y += 40;
    RECT box = {kPad, y, kW - kPad, y + 34};
    lcd(dc, box);
    text(dc, g_snap.status_detail, box, DT_CENTER | DT_VCENTER | DT_SINGLELINE, kLcdText, g_lcd_font, false);
    y = kH - kPad - 40 - 8;
    button(dc, {kPad + 4, y, kW - kPad - 4, y + 40}, T(CANCEL), H_LEAVE);
}

void paint_joined(HDC dc, int y)
{
    text(dc, T(CONNECTED), {kPad, y - 4, kW - kPad, y + 20}, DT_CENTER | DT_SINGLELINE, kYellow, g_button_font);
    y += 26;
    y = player_list(dc, y);
    paint_race_area(dc, y, false);
    y = kH - kPad - 40 - 4;
    button(dc, {kPad + 4, y, kW - kPad - 4, y + 36}, T(LEAVE), H_LEAVE);
}

void paint(HDC dc, const RECT& rc)
{
    g_nhits = 0;
    // Backdrop behind the rounded corners: plain black like the game's tiles.
    fill(dc, rc, RGB(8, 8, 8));
    board(dc, rc);

    RECT title = {kPad, 14, kW - kPad, 58};
    char t[48];
    snprintf(t, sizeof(t), "%s%s", T(TITLE), g_collapsed ? "  +" : "");
    text(dc, t, title, DT_CENTER | DT_VCENTER | DT_SINGLELINE, g_hover == H_HEADER ? kWhite : kYellow,
         g_title_font);
    add_hit({0, 0, kW, 60}, H_HEADER);
    if (g_collapsed) return;
    rail(dc, kPad - 6, kW - kPad + 6, 62);

    int y = 76;
    switch (g_snap.mode) {
    case net::Mode::Idle: paint_idle(dc, y); break;
    case net::Mode::Hosting: paint_hosting(dc, y); break;
    case net::Mode::Connecting: paint_connecting(dc, y); break;
    case net::Mode::Joined: paint_joined(dc, y); break;
    }
}

// ---------------------------------------------------------------------------
// Input

void unfocus_field()
{
    if (!g_field_focus) return;
    g_field_focus = false;
    if (g_main) SetFocus(g_main); // keyboard back to the game
}

void do_connect()
{
    if (!g_addr[0]) return;
    config::set_str("LAN", "LastAddress", g_addr);
    unfocus_field();
    read_name();
    net::join(g_addr);
}

void refresh_car()
{
    game::Car car;
    if (!game::race_running() && game::local_car(&car)) net::set_car(car);
}

void click(int id)
{
    if (id != H_FIELD) unfocus_field();
    switch (id) {
    case H_HEADER: g_collapsed = !g_collapsed; break;
    case H_HOST: read_name(); refresh_car(); net::host(); net::set_settings(g_rs); break;
    case H_FIELD:
        g_field_focus = true;
        g_caret_on = true;
        SetFocus(g_wnd);
        break;
    case H_CONNECT: refresh_car(); do_connect(); break;
    case H_LEAVE: net::leave(); break;
    case H_START: start_race(); break;
    default:
        if (id >= H_OPT0 && id < H_OPT0 + R_COUNT && g_snap.mode == net::Mode::Hosting) {
            step_option(id - H_OPT0, 1);
            break;
        }
        if (id >= H_GAME0 && id - H_GAME0 < g_snap.ngames) {
            read_name();
            refresh_car();
            net::join(g_snap.games[id - H_GAME0].addr);
        }
        break;
    }
}

void on_key(WPARAM vk, LPARAM lp)
{
    if (!g_field_focus) return;
    size_t n = std::strlen(g_addr);
    if (vk == VK_BACK) {
        if (n) g_addr[n - 1] = '\0';
    } else if (vk == VK_RETURN) {
        do_connect();
    } else if (vk == VK_ESCAPE) {
        unfocus_field();
    } else {
        BYTE ks[256];
        GetKeyboardState(ks);
        WORD ch = 0;
        if (ToAscii((UINT)vk, (lp >> 16) & 0xFF, ks, &ch, 0) == 1) {
            char c = (char)ch;
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      c == '.' || c == ':' || c == '-';
            if (ok && n + 1 < sizeof(g_addr)) {
                g_addr[n] = c;
                g_addr[n + 1] = '\0';
            }
        }
    }
    g_caret_on = true;
}

void layout_panel(const RECT& client)
{
    // client.top is below our menu bar (if shown); center like the game screen.
    int cw = client.right, ch = client.bottom;
    int ref_x = (cw - kRefW) / 2, ref_y = client.top + (ch - client.top - kRefH) / 2;
    if (ref_y < client.top) ref_y = client.top;
    int x, y = ref_y;
    bool overlay = false;
    if (ref_x + kRefW + kGap + kW <= cw - 8) {
        x = ref_x + kRefW + kGap;            // right of the game screen
    } else if (ref_x - kGap - kW >= 8) {
        x = ref_x - kGap - kW;               // left of it
    } else {
        x = cw - kW - 8;                      // no room: collapsible tab
        y = client.top + 8;
        overlay = true;
    }
    if (overlay != g_overlay) {
        g_overlay = overlay;
        g_collapsed = overlay; // start collapsed when covering the game
    }
    int h = g_collapsed ? kCollapsedH : kH;
    if (y + h > ch) h = ch - y;
    SetWindowPos(g_wnd, HWND_TOP, x, y, kW, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int id = hit_test((short)LOWORD(l), (short)HIWORD(l));
        if (id != g_hover) {
            g_hover = id;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = H_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int id = hit_test((short)LOWORD(l), (short)HIWORD(l));
        click(id);
        if (id == H_HEADER) {
            RECT c;
            GetClientRect(g_main, &c);
            layout_panel(c);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int id = hit_test((short)LOWORD(l), (short)HIWORD(l));
        if (id >= H_OPT0 && id < H_OPT0 + R_COUNT && g_snap.mode == net::Mode::Hosting) {
            step_option(id - H_OPT0, -1);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        int id = hit_test(p.x, p.y);
        SetCursor(LoadCursor(nullptr, id == H_FIELD ? IDC_IBEAM : id != H_NONE ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_KEYDOWN:
        on_key(w, l);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CHAR:
        return 0; // handled in WM_KEYDOWN (works without TranslateMessage too)
    case WM_KILLFOCUS:
        g_field_focus = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (w == kPickTimer) {
            if (picker::poll(g_rs)) {
                save_settings();
                net::set_settings(g_rs);
                g_series_next = 0;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (!picker::active()) {
                KillTimer(h, kPickTimer);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        ++g_anim;
        g_caret_on = !g_caret_on;
        if (g_anim % 5 == 0) read_name();
        {
            bool racing = race::active() || game::race_running();
            net::set_busy(racing);
            if (g_was_racing && !racing) refresh_car(); // the profile may have changed
            g_was_racing = racing;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case kNetMsg:
        if (w == 1) net::begin_pending(); // the host started a race
        net::snapshot(&g_snap);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

void create(HWND main)
{
    g_main = main;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RCLanPanel";
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(0, wc.lpszClassName, "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, kW, kH, main,
                            nullptr, wc.hInstance, nullptr);
    pick_language();
    create_fonts();
    config::get_str("LAN", "LastAddress", "", g_addr, sizeof(g_addr));
    load_settings();
    race::set_texts(T(R_LEFT), T(R_WAIT));
    read_name();
    net::start(g_wnd, kNetMsg);
    net::snapshot(&g_snap);
    SetTimer(g_wnd, kTimer, 450, nullptr);
    LOG("panel: created %p", (void*)g_wnd);
}

void on_layout(HWND main, const RECT& client, const RECT&, bool menus)
{
    if (!menus) {
        if (g_wnd && g_visible) {
            unfocus_field();
            ShowWindow(g_wnd, SW_HIDE);
            g_visible = false;
        }
        return;
    }
    if (!g_wnd) create(main);
    if (!g_wnd) return;
    layout_panel(client);
    g_visible = true;
}

} // namespace

void install()
{
    if (!config::get_int("LAN", "Enabled", 1)) {
        LOG("panel: disabled ([LAN] Enabled=0)");
        return;
    }
    wnd::set_layout_callback(on_layout);
}

void command(const char* cmd)
{
    if (!g_wnd) return;
    char verb[16] = "", arg[64] = "";
    int dir = 1;
    sscanf(cmd, "%15s %63s %d", verb, arg, &dir);
    if (!std::strcmp(verb, "host")) click(H_HOST);
    else if (!std::strcmp(verb, "join")) { snprintf(g_addr, sizeof(g_addr), "%s", arg); click(H_CONNECT); }
    else if (!std::strcmp(verb, "start")) click(H_START);
    else if (!std::strcmp(verb, "leave")) click(H_LEAVE);
    else if (!std::strcmp(verb, "opt")) { if (g_snap.mode == net::Mode::Hosting) step_option(atoi(arg), dir); }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

} // namespace rclan::panel
