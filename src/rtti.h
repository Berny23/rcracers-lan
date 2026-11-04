#pragma once

#include <cstddef>

namespace rclan::rtti {

// The game is built with MSVC RTTI. Finding a class's vtable through its
// RTTI name (".?AVName@@") works on every build that has the class, unlike
// fixed addresses.

// vtable of class `name` in RCRacers.exe, nullptr if not found. Cached.
void* const* vtable(const char* name);

// Live heap objects whose vtable pointer is `vt` (scans committed private
// read/write memory). Returns the number found (up to `max`).
int find_objects(void* const* vt, void** out, int max);

// The game's code section (for sanity checks of function pointers).
bool in_code(const void* p);

} // namespace rclan::rtti
