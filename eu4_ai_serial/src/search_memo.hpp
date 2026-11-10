#pragma once
// Port of Bellows army_search_memo (MIT, see LICENSE.bellows.txt).
#include <cstddef>
#include <cstdint>

namespace searchmemo {

enum class Mode { off, shadow, on };

struct Stats {
    std::uint64_t scopes, flag_hits, flag_misses, flag_outside, speed_hits, speed_misses,
        verifications, mismatches;
    bool disabled;
};

// Takes the call at 0xd45291 (zone-of-control flag) itself; calls outside a scope go to
// flagcache::call, so flagcache must be installed with patch_site=false.
bool install(std::uintptr_t image, Mode mode, char* error, std::size_t error_size);
Stats stats();
const char* mode_name();
// the per-scope memo of 140a273a0 (strongest friendly fort next to a province)
bool fort_active();
std::uint64_t fort_hits();
std::uint64_t fort_misses();

}  // namespace searchmemo
