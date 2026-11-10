#include "function_stats.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace modifierpatch {
namespace {

constexpr std::size_t function_count = static_cast<std::size_t>(ProfileFunction::count);

struct Record {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> ticks{0};
    std::atomic<std::uint64_t> maximum_ticks{0};
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> maximum_active{0};
    std::atomic<unsigned> maximum_depth{0};
};

std::array<Record, function_count> records{};
std::array<std::vector<DWORD>, function_count> threads{};
std::mutex thread_mutex;
std::int64_t frequency = 1;
thread_local std::array<unsigned, function_count> depths{};
thread_local std::uint32_t published_threads = 0;

std::size_t index_of(ProfileFunction function) noexcept {
    return static_cast<std::size_t>(function);
}

template <typename T>
void update_max(std::atomic<T>& maximum, T value) noexcept {
    auto current = maximum.load(std::memory_order_relaxed);
    while (current < value && !maximum.compare_exchange_weak(current, value,
        std::memory_order_relaxed)) {
    }
}

std::uint64_t milliseconds(std::uint64_t ticks) noexcept {
    const auto divisor = static_cast<std::uint64_t>(frequency);
    return ticks / divisor * 1000 + ticks % divisor * 1000 / divisor;
}

std::uint64_t nanoseconds(std::uint64_t ticks) noexcept {
    const auto divisor = static_cast<std::uint64_t>(frequency);
    return ticks / divisor * 1000000000 + ticks % divisor * 1000000000 / divisor;
}

std::uint64_t microseconds(std::uint64_t ticks) noexcept {
    const auto divisor = static_cast<std::uint64_t>(frequency);
    return ticks / divisor * 1000000 + ticks % divisor * 1000000 / divisor;
}

}  // namespace

const char* function_name(ProfileFunction function) noexcept {
    constexpr std::array<const char*, function_count> names{
        "unit_parameters", "country_recalculation", "unit_modifier_build",
        "unit_combat_value", "modifier_copy", "modifier_scale",
        "country_cache_reset", "country_cache_rebuild"};
    return names[index_of(function)];
}

std::uintptr_t function_rva(ProfileFunction function) noexcept {
    constexpr std::array<std::uintptr_t, function_count> addresses{
        0xD54600, 0x93D130, 0xAE7230, 0xAEBEB0, 0x8EA2D0, 0x8EA920,
        0x93AE10, 0x939EF0};
    return addresses[index_of(function)];
}

void initialize_function_stats(std::int64_t performance_frequency) noexcept {
    frequency = performance_frequency > 0 ? performance_frequency : 1;
}

FunctionObservation enter_function(ProfileFunction function) noexcept {
    const auto index = index_of(function);
    auto& record = records[index];
    const auto active = record.active.fetch_add(1, std::memory_order_relaxed) + 1;
    update_max(record.maximum_active, active);
    const auto depth = ++depths[index];
    update_max(record.maximum_depth, depth);

    const auto bit = static_cast<std::uint32_t>(1) << index;
    if (!(published_threads & bit)) {
        published_threads |= bit;
        try {
            std::lock_guard<std::mutex> lock(thread_mutex);
            threads[index].push_back(GetCurrentThreadId());
        } catch (...) {
            // Timing and concurrency counters remain valid if a report cannot name every thread.
        }
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return FunctionObservation{static_cast<std::uint64_t>(now.QuadPart)};
}

void leave_function(ProfileFunction function, FunctionObservation observation) noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto index = index_of(function);
    auto& record = records[index];
    const auto elapsed = static_cast<std::uint64_t>(now.QuadPart) - observation.started;
    record.ticks.fetch_add(elapsed, std::memory_order_relaxed);
    record.calls.fetch_add(1, std::memory_order_relaxed);
    update_max(record.maximum_ticks, elapsed);
    --depths[index];
    record.active.fetch_sub(1, std::memory_order_relaxed);
}

std::vector<std::string> function_report(bool sort_by_time) {
    struct Snapshot {
        ProfileFunction function;
        std::uint64_t calls;
        std::uint64_t ticks;
        std::uint64_t maximum;
        unsigned maximum_active;
        unsigned maximum_depth;
        std::size_t thread_count;
    };
    std::vector<Snapshot> snapshots;
    for (std::size_t index = 0; index < function_count; ++index) {
        const auto calls = records[index].calls.load(std::memory_order_relaxed);
        if (!calls) continue;
        std::size_t thread_count = 0;
        {
            std::lock_guard<std::mutex> lock(thread_mutex);
            thread_count = threads[index].size();
        }
        snapshots.push_back(Snapshot{static_cast<ProfileFunction>(index), calls,
            records[index].ticks.load(std::memory_order_relaxed),
            records[index].maximum_ticks.load(std::memory_order_relaxed),
            records[index].maximum_active.load(std::memory_order_relaxed),
            records[index].maximum_depth.load(std::memory_order_relaxed), thread_count});
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& left, const Snapshot& right) {
        return left.ticks > right.ticks;
    });
    if (!sort_by_time) {
        std::stable_sort(snapshots.begin(), snapshots.end(), [](const Snapshot& left, const Snapshot& right) {
            return left.calls > right.calls;
        });
    }

    std::vector<std::string> output;
    output.reserve(snapshots.size());
    unsigned rank = 0;
    for (const auto& snapshot : snapshots) {
        char line[640];
        std::snprintf(line, sizeof(line),
            "function rank=%u sort=%s name=%s rva=0x%zx calls=%llu inclusive_ms=%llu "
            "avg_ns=%llu max_us=%llu threads=%zu max_concurrent=%u max_depth=%u",
            ++rank, sort_by_time ? "time" : "calls", function_name(snapshot.function),
            function_rva(snapshot.function), static_cast<unsigned long long>(snapshot.calls),
            static_cast<unsigned long long>(milliseconds(snapshot.ticks)),
            static_cast<unsigned long long>(nanoseconds(snapshot.ticks) / snapshot.calls),
            static_cast<unsigned long long>(microseconds(snapshot.maximum)), snapshot.thread_count,
            snapshot.maximum_active, snapshot.maximum_depth);
        output.emplace_back(line);
    }
    return output;
}

}  // namespace modifierpatch
