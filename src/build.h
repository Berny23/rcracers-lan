#pragma once

// Which RCRacers.exe is running, and where the game internals the mod uses
// are in it.
//
// The code names addresses as they are in the German retail 2.0 build;
// A() translates them for the running build. Supported: German retail 2.0
// ("Graupner Edition", Jul 28 2000) and the English "Traxxas Edition"
// (Jun 6 2000). Function signatures are still verified on every build.
namespace rclan::build {

enum Id { Unknown, German, English };

Id id();
const char* name();

// Address in the running build of German-build address `de` (0 if the
// build has no known counterpart).
unsigned A(unsigned de);

// Do the bytes at German-build address `de` match `bytes` (as in the German
// build)? Dwords that are known German addresses are compared translated.
bool matches(unsigned de, const unsigned char* bytes, int n);

} // namespace rclan::build
