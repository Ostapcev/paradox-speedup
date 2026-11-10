#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace modifierpatch {

inline constexpr int modifier_sentinel = 0x286;

struct Measurement {
    std::int32_t destination_count = 0;
    std::int32_t source_count = 0;
    std::uint64_t comparisons = 0;
    std::uint64_t direct_lookups = 0;
    std::uint64_t dynamic_inputs = 0;
    std::uint64_t appended = 0;
    bool dynamic = false;
    bool alias = false;
    bool valid = true;
};

struct Scratch {
    struct Slot {
        int key = 0;
        int index = 0;
        std::uint64_t generation = 0;
    };
    std::vector<int> keys;
    std::vector<Slot> slots;
    std::uint64_t generation = 0;
};

Measurement measure_merge(const void* destination, const void* source, Scratch& scratch);

}  // namespace modifierpatch
