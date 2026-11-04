#pragma once

#include <windows.h>

namespace rclan::dinput {

// Called from the GetProcAddress hook. If `name` is DirectInputCreateA,
// returns our wrapper (remembering `real`); otherwise returns `real`.
FARPROC maybe_wrap(LPCSTR name, FARPROC real);

// Logs and resets call counters.

} // namespace rclan::dinput
