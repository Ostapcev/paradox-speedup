#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "function_stats.hpp"

int main() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    modifierpatch::initialize_function_stats(frequency.QuadPart);
    using modifierpatch::ProfileFunction;

    const auto outer = modifierpatch::enter_function(ProfileFunction::country_recalculation);
    const auto nested = modifierpatch::enter_function(ProfileFunction::country_recalculation);
    modifierpatch::leave_function(ProfileFunction::country_recalculation, nested);
    modifierpatch::leave_function(ProfileFunction::country_recalculation, outer);
    const auto unit = modifierpatch::enter_function(ProfileFunction::unit_parameters);
    modifierpatch::leave_function(ProfileFunction::unit_parameters, unit);
    const auto reset = modifierpatch::enter_function(ProfileFunction::country_cache_reset);
    modifierpatch::leave_function(ProfileFunction::country_cache_reset, reset);
    const auto rebuild = modifierpatch::enter_function(ProfileFunction::country_cache_rebuild);
    modifierpatch::leave_function(ProfileFunction::country_cache_rebuild, rebuild);

    const auto by_time = modifierpatch::function_report(true);
    const auto by_calls = modifierpatch::function_report(false);
    const auto country_time = std::find_if(by_time.begin(), by_time.end(), [](const auto& line) {
        return line.find("name=country_recalculation") != std::string::npos;
    });
    if (by_time.size() != 4 || by_calls.size() != 4 || country_time == by_time.end() ||
        country_time->find("calls=2") == std::string::npos ||
        country_time->find("max_concurrent=2") == std::string::npos ||
        country_time->find("max_depth=2") == std::string::npos ||
        country_time->find("threads=1") == std::string::npos ||
        by_calls.front().find("name=country_recalculation") == std::string::npos ||
        by_calls.back().find("calls=1") == std::string::npos ||
        std::string(modifierpatch::function_name(ProfileFunction::country_cache_reset)) !=
            "country_cache_reset" ||
        std::string(modifierpatch::function_name(ProfileFunction::country_cache_rebuild)) !=
            "country_cache_rebuild" ||
        modifierpatch::function_rva(ProfileFunction::country_cache_reset) != 0x93AE10 ||
        modifierpatch::function_rva(ProfileFunction::country_cache_rebuild) != 0x939EF0) {
        std::fputs("Function statistics or nesting report is incorrect\n", stderr);
        return 1;
    }
    std::puts("function stats tests passed: inclusive time, nesting, threads, rankings");
    return 0;
}
