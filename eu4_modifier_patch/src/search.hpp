#pragma once

#include <cstdint>

namespace modifierpatch {

extern "C" std::uint64_t find_modifier_key(const void* entries,
    std::uint64_t count, std::uint32_t key) noexcept;

extern "C" void* add_search_found_target;
extern "C" void* add_search_not_found_target;
extern "C" void* subtract_search_found_target;
extern "C" void* subtract_search_not_found_target;

extern "C" void add_search_relay();
extern "C" void subtract_search_relay();

}  // namespace modifierpatch
