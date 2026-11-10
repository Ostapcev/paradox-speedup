#include <cstdio>
#include <string>

#include "caller_stats.hpp"

int main() {
    modifierpatch::initialize_caller_stats(0x140000000, 0x25E0000, 10000000);
    modifierpatch::Measurement first;
    first.comparisons = 20;
    first.dynamic = true;
    modifierpatch::Measurement second;
    second.comparisons = 5;
    const auto* caller_a = reinterpret_cast<const void*>(0x140001235);
    const auto* caller_b = reinterpret_cast<const void*>(0x140002345);
    modifierpatch::record_caller(caller_a, first, 10000);
    modifierpatch::record_caller(caller_a, first, 10000);
    modifierpatch::record_caller(caller_b, second, 90000);
    const auto by_calls = modifierpatch::caller_report(false);
    const auto by_time = modifierpatch::caller_report(true);
    if (by_calls.size() != 2 || by_calls[0].find("rva=0x1235") == std::string::npos ||
        by_calls[0].find("calls=2") == std::string::npos ||
        by_calls[0].find("max_call_comparisons=20") == std::string::npos) {
        std::fputs("Caller count ranking or aggregation failed\n", stderr);
        return 1;
    }
    if (by_time.size() != 2 || by_time[0].find("rva=0x2345") == std::string::npos ||
        by_time[0].find("original_ms=9") == std::string::npos) {
        std::fputs("Caller time ranking failed\n", stderr);
        return 2;
    }
    std::puts("caller stats tests passed: RVA capture, aggregation, count/time rankings");
    return 0;
}
