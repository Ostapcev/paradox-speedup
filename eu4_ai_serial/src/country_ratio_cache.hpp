#pragma once

#include <cstddef>
#include <cstdint>

#include "unit_flag_cache.hpp"

namespace ratiocache {

using flagcache::Mode;
using flagcache::Stats;

// Redirects the call of 1403747b0 in 1403b4a60; off = nothing changed.
bool install(std::uintptr_t image, Mode mode, char* error, std::size_t error_size);
Stats stats();
const char* mode_name();

}  // namespace ratiocache
