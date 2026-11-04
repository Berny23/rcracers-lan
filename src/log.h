#pragma once

namespace rclan::log {

// Opens (truncates) the log file. Safe to call once from DllMain.
void init(const char* path);

// printf-style, thread-safe. Also mirrored to OutputDebugStringA
// (visible in a Proton log with WINEDEBUG=+debugstr).
void write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace rclan::log

#define LOG(...) ::rclan::log::write(__VA_ARGS__)
