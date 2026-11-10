#include "caller_stats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace modifierpatch {
namespace {

constexpr unsigned caller_capacity = 512;
constexpr unsigned overflow_index = caller_capacity;

struct CallerRecord {
    std::uintptr_t address = 0;
    std::uintptr_t rva = 0;
    bool external = false;
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> dynamic_calls{0};
    std::atomic<std::uint64_t> comparisons{0};
    std::atomic<std::uint64_t> original_ticks{0};
    std::atomic<std::uint64_t> max_comparisons{0};
};

std::array<CallerRecord, caller_capacity + 1> records{};
std::atomic<unsigned> published{0};
std::mutex publication_mutex;
std::uintptr_t module_base = 0;
std::uintptr_t module_size = 0;
std::int64_t frequency = 1;

unsigned caller_index(const void* caller) {
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    thread_local std::unordered_map<std::uintptr_t, unsigned> cache;
    const auto cached = cache.find(address);
    if (cached != cache.end()) return cached->second;

    unsigned index = overflow_index;
    std::lock_guard<std::mutex> lock(publication_mutex);
    const auto count = published.load(std::memory_order_relaxed);
    for (unsigned current = 0; current < count; ++current) {
        if (!records[current].external && records[current].address == address) {
            index = current;
            break;
        }
    }
    if (index == overflow_index && count < caller_capacity) {
        index = count;
        auto& record = records[index];
        record.address = address;
        record.external = address < module_base || address >= module_base + module_size;
        if (!record.external) record.rva = address - module_base;
        published.store(count + 1, std::memory_order_release);
    }
    try {
        cache.emplace(address, index);
    } catch (...) {
        // An uncached lookup still produces correct aggregate statistics.
    }
    return index;
}

void update_max(std::atomic<std::uint64_t>& maximum, std::uint64_t value) noexcept {
    auto current = maximum.load(std::memory_order_relaxed);
    while (current < value && !maximum.compare_exchange_weak(current, value,
        std::memory_order_relaxed)) {
    }
}

std::uint64_t ticks_to_milliseconds(std::uint64_t ticks) noexcept {
    const auto divisor = static_cast<std::uint64_t>(frequency);
    return ticks / divisor * 1000 + ticks % divisor * 1000 / divisor;
}

std::uint64_t ticks_to_nanoseconds(std::uint64_t ticks) noexcept {
    const auto divisor = static_cast<std::uint64_t>(frequency);
    return ticks / divisor * 1000000000 + ticks % divisor * 1000000000 / divisor;
}

}  // namespace

void initialize_caller_stats(std::uintptr_t image_base, std::uintptr_t image_size,
    std::int64_t performance_frequency) noexcept {
    module_base = image_base;
    module_size = image_size;
    frequency = performance_frequency > 0 ? performance_frequency : 1;
}

void record_caller(const void* caller, const Measurement& measurement,
    std::uint64_t original_ticks) noexcept {
    auto& record = records[caller_index(caller)];
    record.calls.fetch_add(1, std::memory_order_relaxed);
    record.original_ticks.fetch_add(original_ticks, std::memory_order_relaxed);
    record.comparisons.fetch_add(measurement.comparisons, std::memory_order_relaxed);
    if (measurement.dynamic) record.dynamic_calls.fetch_add(1, std::memory_order_relaxed);
    update_max(record.max_comparisons, measurement.comparisons);
}

std::vector<std::string> caller_report(bool sort_by_time) {
    struct Snapshot {
        std::uintptr_t address;
        std::uintptr_t rva;
        bool external;
        std::uint64_t calls;
        std::uint64_t dynamic;
        std::uint64_t comparisons;
        std::uint64_t ticks;
        std::uint64_t maximum;
    };
    std::vector<Snapshot> snapshots;
    const auto count = published.load(std::memory_order_acquire);
    snapshots.reserve(count);
    std::uint64_t total_calls = 0;
    for (unsigned index = 0; index < count; ++index) {
        const auto& record = records[index];
        const auto calls = record.calls.load(std::memory_order_relaxed);
        total_calls += calls;
        snapshots.push_back(Snapshot{record.address, record.rva, record.external, calls,
            record.dynamic_calls.load(std::memory_order_relaxed),
            record.comparisons.load(std::memory_order_relaxed),
            record.original_ticks.load(std::memory_order_relaxed),
            record.max_comparisons.load(std::memory_order_relaxed)});
    }
    std::sort(snapshots.begin(), snapshots.end(), [&](const Snapshot& left, const Snapshot& right) {
        const auto left_key = sort_by_time ? left.ticks : left.calls;
        const auto right_key = sort_by_time ? right.ticks : right.calls;
        if (left_key != right_key) return left_key > right_key;
        return left.calls > right.calls;
    });
    if (snapshots.size() > 8) snapshots.resize(8);

    std::vector<std::string> output;
    output.reserve(snapshots.size());
    unsigned rank = 0;
    for (const auto& snapshot : snapshots) {
        char line[640];
        char location[64];
        if (snapshot.external) {
            std::snprintf(location, sizeof(location), "address=0x%zx", snapshot.address);
        } else {
            std::snprintf(location, sizeof(location), "rva=0x%zx", snapshot.rva);
        }
        const auto average_ns = snapshot.calls ? ticks_to_nanoseconds(snapshot.ticks) / snapshot.calls : 0;
        std::snprintf(line, sizeof(line),
            "caller rank=%u sort=%s %s calls=%llu call_share_ppm=%llu dynamic=%llu "
            "comparisons=%llu original_ms=%llu original_avg_ns=%llu max_call_comparisons=%llu",
            ++rank, sort_by_time ? "time" : "calls", location,
            static_cast<unsigned long long>(snapshot.calls),
            static_cast<unsigned long long>(total_calls ? snapshot.calls * 1000000 / total_calls : 0),
            static_cast<unsigned long long>(snapshot.dynamic),
            static_cast<unsigned long long>(snapshot.comparisons),
            static_cast<unsigned long long>(ticks_to_milliseconds(snapshot.ticks)),
            static_cast<unsigned long long>(average_ns),
            static_cast<unsigned long long>(snapshot.maximum));
        output.emplace_back(line);
    }
    return output;
}

}  // namespace modifierpatch
