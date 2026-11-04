#pragma once

#include <windows.h>

namespace rclan {

// Redirects `module`'s import of `dll!func` to `hook`.
// On success returns the patched IAT slot and stores the previous target in
// *original. Returns nullptr if the import isn't found.
void** patch_iat(HMODULE module, const char* dll, const char* func,
                 void* hook, void** original);

} // namespace rclan
