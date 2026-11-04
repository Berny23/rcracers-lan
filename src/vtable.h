#pragma once

#include <initializer_list>
#include <utility>

namespace rclan::vtable {

// Patches `slots` (index -> hook) in obj's vtable, once per distinct vtable.
// `nslots` = how many entries the interface has (originals are saved).
void hook(void* obj, const char* what, int nslots,
          std::initializer_list<std::pair<int, void*>> slots);

// Original function for `slot` of self's vtable (nullptr if not hooked).
void* orig(void* self, int slot);

template <typename F>
F orig_as(void* self, int slot)
{
    return reinterpret_cast<F>(orig(self, slot));
}

} // namespace rclan::vtable
