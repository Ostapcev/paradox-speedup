#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "measurement.hpp"

namespace modifierpatch {

void initialize_caller_stats(std::uintptr_t image_base, std::uintptr_t image_size,
    std::int64_t performance_frequency) noexcept;
void record_caller(const void* caller, const Measurement& measurement,
    std::uint64_t original_ticks) noexcept;
std::vector<std::string> caller_report(bool sort_by_time);

}  // namespace modifierpatch
