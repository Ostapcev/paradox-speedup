#pragma once

#include <cstddef>
#include <cstdint>

#include "unit_flag_cache.hpp"

namespace scorecache {

using flagcache::Mode;
using flagcache::Stats;

// Redirects the call of 1401998f0 in 140198c70; off = nothing changed.
bool install(std::uintptr_t image, Mode mode, char* error, std::size_t error_size);
Stats stats();
const char* mode_name();

}  // namespace scorecache
