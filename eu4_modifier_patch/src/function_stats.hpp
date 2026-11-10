#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace modifierpatch {

enum class ProfileFunction : std::size_t {
    unit_parameters,
    country_recalculation,
    unit_modifier_build,
    unit_combat_value,
    modifier_copy,
    modifier_scale,
    country_cache_reset,
    country_cache_rebuild,
    count
};

struct FunctionObservation {
    std::uint64_t started = 0;
};

void initialize_function_stats(std::int64_t performance_frequency) noexcept;
FunctionObservation enter_function(ProfileFunction function) noexcept;
void leave_function(ProfileFunction function, FunctionObservation observation) noexcept;
std::vector<std::string> function_report(bool sort_by_time);
const char* function_name(ProfileFunction function) noexcept;
std::uintptr_t function_rva(ProfileFunction function) noexcept;

}  // namespace modifierpatch
