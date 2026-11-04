#pragma once

#include "net.h"

// The game's own selection screens as pickers for the LAN host: the track
// selection of a single race, the championship selection (with the custom
// track list), and the Funkduell selection.
//
// While a picker is open, the screen works as usual, but "Weiter" and
// "Hauptmenü" both lead back to the main menu instead of into a race; the
// current choice is read from the screen (poll) and goes into the LAN
// settings. Without an open picker, the game's menus are untouched.
namespace rclan::picker {

enum Kind { Track, Circuit, Duel };

void install();      // hooks the game's screen switch (if the build matches)
bool available();
bool at_main_menu(); // only from there a picker opens

// Opens the screen for `kind`, preselecting what `rs` has. Main thread.
bool open(Kind kind, const net::RaceSettings& rs);
bool active();

// Copies the screen's current choice into `rs`; true if it changed.
bool poll(net::RaceSettings& rs);

// Tracks (indices into game::track_id) of championship `circuit`
// (0 amateur, 1 pro, 2 custom = rs.custom, 3 super).
int circuit_tracks(const net::RaceSettings& rs, int* out, int max);

} // namespace rclan::picker
