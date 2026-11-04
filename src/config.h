#pragma once

namespace rclan::config {

// Reads rclan.ini next to rclan.asi (usually <game>/scripts/rclan.ini).
int get_int(const char* section, const char* key, int def);
void get_str(const char* section, const char* key, const char* def, char* out, int n);
void set_str(const char* section, const char* key, const char* value);

// Directory of the game executable, with trailing backslash.
const char* game_dir();

} // namespace rclan::config
