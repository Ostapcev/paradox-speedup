#pragma once

#include <cstddef>
#include <cstdint>

namespace flagcache {

enum class Mode { off, shadow, on };

struct Stats {
    std::uint64_t hits, misses, mismatches, bypassed;
};

// Redirects the call in 140d45250; off = nothing changed.
// patch_site=false: the call site belongs to searchmemo, which forwards calls outside its
// scopes to call().
bool install(std::uintptr_t image, Mode mode, char* error, std::size_t error_size,
    bool patch_site = true);
// The cache (or the game's function when off) for a call that arrives from elsewhere.
std::uint8_t call(void* unit, std::uint32_t id, char flag, std::int64_t extra);
// The daily AI phase on the main thread: cache valid only between these.
void begin_phase();
void end_phase();
Stats stats();
// Shared by the other per-phase caches.
bool phase_active();
std::uint64_t phase_generation();
const char* mode_name();

}  // namespace flagcache
