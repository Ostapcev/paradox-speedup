#pragma once

#include <cstddef>
#include <cstdint>

#include "unit_flag_cache.hpp"

namespace pathcache {

using flagcache::Mode;
using flagcache::Stats;

// Redirects 5 army AI calls of the pathfinder 14096c520; off = nothing changed.
bool install(std::uintptr_t image, Mode mode, char* error, std::size_t error_size);
Stats stats();
const char* mode_name();

}  // namespace pathcache
