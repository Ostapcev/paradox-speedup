#include "search.hpp"

#include <emmintrin.h>

namespace modifierpatch {
namespace {

struct Entry {
    std::int32_t key;
    std::int32_t value;
};
static_assert(sizeof(Entry) == 8, "modifier entry layout");

}  // namespace

extern "C" void* add_search_found_target = nullptr;
extern "C" void* add_search_not_found_target = nullptr;
extern "C" void* subtract_search_found_target = nullptr;
extern "C" void* subtract_search_not_found_target = nullptr;

extern "C" std::uint64_t find_modifier_key(const void* entries,
    std::uint64_t count, std::uint32_t key) noexcept {
    const auto* items = static_cast<const Entry*>(entries);
    const auto needle = _mm_set1_epi32(static_cast<int>(key));
    std::uint64_t index = 0;
    for (; index + 4 <= count; index += 4) {
        const auto first = _mm_loadu_si128(reinterpret_cast<const __m128i*>(items + index));
        const auto second = _mm_loadu_si128(reinterpret_cast<const __m128i*>(items + index + 2));
        const auto first_equal = _mm_cmpeq_epi32(first, needle);
        const auto second_equal = _mm_cmpeq_epi32(second, needle);
        const auto first_mask = static_cast<unsigned>(
            _mm_movemask_ps(_mm_castsi128_ps(first_equal)));
        const auto second_mask = static_cast<unsigned>(
            _mm_movemask_ps(_mm_castsi128_ps(second_equal)));
        const unsigned matches = (first_mask | (second_mask << 4U)) & 0x55U;
        if (matches) return index + static_cast<std::uint64_t>(__builtin_ctz(matches) >> 1U);
    }
    for (; index < count; ++index) {
        if (static_cast<std::uint32_t>(items[index].key) == key) return index;
    }
    return count;
}

}  // namespace modifierpatch
