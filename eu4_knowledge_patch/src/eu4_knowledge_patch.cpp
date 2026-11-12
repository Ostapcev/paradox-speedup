// EU4 Knowledge Patch 0.4.0-scheduler, for EU4 1.37.5.0 Windows x64.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "call_patch.hpp"
#include "equivalence.hpp"
#include "optimized_loop_patch.hpp"
#include "parallel_patch.hpp"
#include "parallel_pool.hpp"
#include "profile.hpp"
#include "scheduler_wait.hpp"
#include "two_phase.hpp"
#include "eu4_build.hpp"

extern "C" {
void* knowledge_original_target = nullptr;
void knowledge_shadow_hook(void* argument);
void knowledge_parallel_verify(void* current, void* other, void* relation) noexcept;
extern const std::uint8_t knowledge_optimized_core_begin[];
extern const std::uint8_t knowledge_optimized_core_end[];
extern const std::uint8_t knowledge_optimized_core_tls[];
extern const std::uint8_t knowledge_optimized_core_found_displacement[];
extern const std::uint8_t knowledge_optimized_core_out_displacement[];
extern const std::uint8_t knowledge_optimized_core_continuation_displacement[];
extern const std::uint8_t knowledge_optimized_claim_begin[];
extern const std::uint8_t knowledge_optimized_claim_end[];
extern const std::uint8_t knowledge_optimized_claim_tls[];
extern const std::uint8_t knowledge_optimized_claim_found_displacement[];
extern const std::uint8_t knowledge_optimized_claim_out_displacement[];
extern const std::uint8_t knowledge_optimized_claim_continuation_displacement[];
extern const std::uint8_t knowledge_parallel_apply_begin[];
extern const std::uint8_t knowledge_parallel_apply_end[];
extern const std::uint8_t knowledge_parallel_apply_tls[];
extern const std::uint8_t knowledge_parallel_apply_commit_displacement[];
extern const std::uint8_t knowledge_parallel_apply_fallback_displacement[];
extern const std::uint8_t knowledge_parallel_verify_begin[];
extern const std::uint8_t knowledge_parallel_verify_end[];
extern const std::uint8_t knowledge_parallel_verify_continuation_displacement[];
extern const std::uint8_t knowledge_parallel_timing_begin[];
extern const std::uint8_t knowledge_parallel_timing_end[];
extern const std::uint8_t knowledge_parallel_timing_continuation_displacement[];
extern const std::uint8_t knowledge_scheduler_wait_begin[];
extern const std::uint8_t knowledge_scheduler_wait_end[];
extern const std::uint8_t knowledge_scheduler_wait_resume_displacement[];
extern const std::uint8_t knowledge_scheduler_wait_top_displacement[];
}

namespace {

using knowledgepatch::Tag;

enum class Mode { shadow, optimized, parallel_shadow, parallel, disabled };
enum class SchedulerMode { disabled, efficient };

constexpr wchar_t version[] = L"0.4.0-scheduler";
std::atomic<Mode> mode{Mode::disabled};
std::atomic<LONG> installation_status{0};
std::atomic<std::uint64_t> calls{0};
std::atomic<std::uint64_t> country_pairs{0};
std::atomic<std::uint64_t> mismatched_pairs{0};
std::atomic<std::uint64_t> invalid_province_ids{0};
std::atomic<std::uint64_t> inverse_core_entries{0};
std::atomic<std::uint64_t> inverse_claim_entries{0};
std::atomic<std::uint64_t> verification_province_visits{0};
std::atomic<std::uint64_t> core_tag_comparisons{0};
std::atomic<std::uint64_t> claim_tag_comparisons{0};
std::atomic<std::uint64_t> inverse_ticks{0};
std::atomic<std::uint64_t> verification_ticks{0};
std::atomic<std::uint64_t> original_ticks{0};
std::atomic<std::uint64_t> parallel_ticks{0};
std::atomic<std::uint64_t> parallel_pairs{0};
std::atomic<std::uint64_t> parallel_unknown{0};
std::atomic<std::uint64_t> parallel_mismatches{0};
std::uintptr_t image_base = 0;
std::int64_t qpc_frequency = 1;
DWORD tls_slot = TLS_OUT_OF_INDEXES;
std::wstring log_path;
std::mutex log_mutex;
knowledgepatch::ParallelPool parallel_pool;

struct OptimizedSnapshot {
    const void* country;
    knowledgepatch::TagBits cores;
    knowledgepatch::TagBits claims;
    std::array<std::uint8_t, knowledgepatch::tag_count> pair_flags;
};
thread_local OptimizedSnapshot optimized_snapshot{};
thread_local std::uint64_t original_pair_begin = 0;
static_assert(offsetof(OptimizedSnapshot, country) == 0, "stub country offset");
static_assert(offsetof(OptimizedSnapshot, cores) == 8, "stub core bitset offset");
static_assert(offsetof(OptimizedSnapshot, claims) == 0x2008, "stub claim bitset offset");
static_assert(offsetof(OptimizedSnapshot, pair_flags) == 0x4008, "parallel flags offset");

void log(const char* format, ...) noexcept {
    if (log_path.empty()) return;
    char message[1600];
    const int prefix = std::snprintf(message, sizeof(message), "[%lu:%llu] ",
        GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    if (prefix < 0) return;
    va_list arguments;
    va_start(arguments, format);
    const int body = std::vsnprintf(message + prefix, sizeof(message) - prefix, format, arguments);
    va_end(arguments);
    if (body < 0) return;
    const auto used = (std::min)(sizeof(message) - 2,
        static_cast<std::size_t>(prefix) + static_cast<std::size_t>(body));
    message[used] = '\n';
    message[used + 1] = '\0';
    std::lock_guard<std::mutex> lock(log_mutex);
    HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, message, static_cast<DWORD>(used + 1), &written, nullptr);
        CloseHandle(file);
    }
}

std::uint64_t ticks_to_microseconds(std::uint64_t ticks) noexcept {
    const auto frequency = static_cast<std::uint64_t>(qpc_frequency);
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

template <typename T>
T read_memory(std::uintptr_t address) noexcept {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

Tag packed_tag(std::uintptr_t address) noexcept {
    return static_cast<Tag>(read_memory<std::uint64_t>(address) >> 32);
}

class RuntimeWorld {
public:
    RuntimeWorld(std::uintptr_t image, const void* current) noexcept
        : current_(reinterpret_cast<std::uintptr_t>(current)) {
        gamestate_ = read_memory<std::uintptr_t>(image + eu4build::rva(knowledgepatch::gamestate_pointer_rva));
        province_base_ = read_memory<std::uintptr_t>(gamestate_ + 0x1CA8);
        countries_ = read_memory<std::uintptr_t>(gamestate_ + 0x1D08);
        country_count_ = read_memory<std::int32_t>(gamestate_ + 0x1D14);
    }

    Tag current_tag() const noexcept { return packed_tag(current_ + 0x20); }
    std::size_t country_count() const noexcept {
        return country_count_ > 0 ? static_cast<std::size_t>(country_count_) : 0;
    }
    Tag country_tag(std::size_t index) const noexcept {
        return packed_tag(country(index) + 0x20);
    }
    bool country_is_valid_pair(std::size_t index) const noexcept {
        const auto packed = read_memory<std::uint64_t>(country(index) + 0x20);
        const auto tag = static_cast<Tag>(packed >> 32);
        const auto is_special = [&](std::uintptr_t rva) {
            return static_cast<std::int32_t>(tag) == read_memory<std::int32_t>(image_base + eu4build::rva(rva));
        };
        // Exact filters at 0x9414aa..0x9414ef. The original never touches
        // provinces for skipped entries, so workers must not inspect them.
        return tag != current_tag() && ((packed >> 56) & 0xFF) != 0 &&
            ((packed >> 48) & 0xFF) != 7 && !is_special(0x233D8A4) &&
            !is_special(0x233D8D8) && !is_special(0x233D8DC);
    }
    bool valid_province(int id) const noexcept {
        // EU4 province ids are far below this defensive corruption limit.
        return id >= 0 && id < 0x100000 && province_base_ != 0;
    }
    Tag province_owner(int id) const noexcept { return packed_tag(province(id) + 0x468); }

    template <typename F>
    void for_each_current_core(F&& fn) const noexcept {
        for_each_province_node(current_ + 0x1378, static_cast<F&&>(fn));
    }
    template <typename F>
    void for_each_current_claim(F&& fn) const noexcept {
        for_each_province_node(current_ + 0x1390, static_cast<F&&>(fn));
    }
    template <typename F>
    void for_each_owned(std::size_t index, F&& fn) const noexcept {
        for_each_owned_pointer(reinterpret_cast<const void*>(country(index)),
            static_cast<F&&>(fn));
    }
    template <typename F>
    void for_each_owned_pointer(const void* other, F&& fn) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(other);
        const auto begin = read_memory<std::uintptr_t>(address + 0x1338);
        const int count = read_memory<std::int32_t>(address + 0x1344);
        for (int position = 0; position < count; ++position) {
            const int id = read_memory<std::int32_t>(begin + static_cast<std::size_t>(position) * 4);
            if (!fn(id)) return;
        }
    }
    template <typename F>
    void for_each_core_tag(int id, F&& fn) const noexcept {
        const auto province_address = province(id);
        auto current = read_memory<std::uintptr_t>(province_address + 0x490);
        const auto end = read_memory<std::uintptr_t>(province_address + 0x498);
        while (current != end) {
            if (!fn(static_cast<Tag>(read_memory<std::uint64_t>(current) >> 32))) return;
            current += 8;
        }
    }
    template <typename F>
    void for_each_claim_tag(int id, F&& fn) const noexcept {
        auto node = read_memory<std::uintptr_t>(province(id) + 0x4C0);
        while (node) {
            if (!fn(static_cast<Tag>(read_memory<std::uint64_t>(node) >> 32))) return;
            node = read_memory<std::uintptr_t>(node + 0x10);
        }
    }

    bool same_religion(int id) const noexcept {
        const auto current_religion = read_memory<std::uintptr_t>(current_ + 0x1228);
        const auto province_religion = read_memory<std::uintptr_t>(province(id) + 0x440);
        return read_memory<std::uintptr_t>(current_religion + 0x88) ==
            read_memory<std::uintptr_t>(province_religion + 0x88);
    }
    Tag other_overlord_tag(std::size_t index) const noexcept {
        using Resolve = void* (*)(void* country, void* output, std::uint8_t recursive);
        const auto resolve = reinterpret_cast<Resolve>(image_base + eu4build::rva(0x3D7C00));
        std::uint64_t packed = 0;
        resolve(reinterpret_cast<void*>(country(index)), &packed, 1);
        return static_cast<Tag>(packed >> 32);
    }
    bool other_requires_first_distance(std::size_t index) const noexcept {
        // Byte 6 of the packed tag at country+0x20 (see 0x941f12).
        return read_memory<std::uint8_t>(country(index) + 0x26) != 1;
    }
    bool in_other_range(int id, std::size_t index) const noexcept {
        using Distance = bool (*)(void* province, void* capital, std::uint8_t recursive);
        const auto distance = reinterpret_cast<Distance>(image_base + eu4build::rva(0x9FABE0));
        // 0x94151e: other+0x14b0, cached at rbp+0xa0 for 0x941f1c.
        const auto capital = read_memory<std::uintptr_t>(country(index) + 0x14B0);
        return distance(reinterpret_cast<void*>(province(id)), reinterpret_cast<void*>(capital), 1);
    }
    bool in_current_range(int id) const noexcept {
        using Distance = bool (*)(void* province, void* capital, std::uint8_t recursive);
        const auto distance = reinterpret_cast<Distance>(image_base + eu4build::rva(0x9FABE0));
        const auto capital = read_memory<std::uintptr_t>(current_ + 0x14B0);
        return distance(reinterpret_cast<void*>(province(id)), reinterpret_cast<void*>(capital), 1);
    }
    bool special_visibility_is_virtual(int id) const noexcept {
        const auto group = read_memory<std::uintptr_t>(current_ + 0xF30);
        const auto special = read_memory<std::uintptr_t>(group + 0x578);
        return special != 0 && read_memory<std::uintptr_t>(province(id) + 0x8A0) != 0;
    }
    bool colonial_visibility(int id) const noexcept {
        using Colonial = bool (*)(void* province, std::uint64_t current_packed_tag);
        const auto colonial = reinterpret_cast<Colonial>(image_base + eu4build::rva(0xA1D330));
        return colonial(reinterpret_cast<void*>(province(id)),
            read_memory<std::uint64_t>(current_ + 0x20));
    }

private:
    std::uintptr_t country(std::size_t index) const noexcept {
        return read_memory<std::uintptr_t>(countries_ + index * 8);
    }
    std::uintptr_t province(int id) const noexcept {
        return province_base_ + static_cast<std::uintptr_t>(id) * 0x2E10;
    }
    template <typename F>
    void for_each_province_node(std::uintptr_t head_address, F&& fn) const noexcept {
        auto node = read_memory<std::uintptr_t>(head_address);
        while (node) {
            const int id = read_memory<std::int32_t>(node);
            if (!fn(id)) return;
            node = read_memory<std::uintptr_t>(node + 0x10);
        }
    }

    std::uintptr_t current_ = 0;
    std::uintptr_t gamestate_ = 0;
    std::uintptr_t province_base_ = 0;
    std::uintptr_t countries_ = 0;
    std::int32_t country_count_ = 0;
};

class PhaseClock {
public:
    PhaseClock() noexcept { QueryPerformanceCounter(&begin_); }
    void inverse_finished() noexcept { QueryPerformanceCounter(&inverse_); }
    void finish() noexcept {
        QueryPerformanceCounter(&end_);
        inverse_elapsed = static_cast<std::uint64_t>(inverse_.QuadPart - begin_.QuadPart);
        verification_elapsed = static_cast<std::uint64_t>(end_.QuadPart - inverse_.QuadPart);
    }

    std::uint64_t inverse_elapsed = 0;
    std::uint64_t verification_elapsed = 0;

private:
    LARGE_INTEGER begin_{};
    LARGE_INTEGER inverse_{};
    LARGE_INTEGER end_{};
};

void add_stats(const knowledgepatch::Comparison& comparison, const PhaseClock& clock) noexcept {
    const auto& stats = comparison.stats;
    country_pairs.fetch_add(stats.country_pairs, std::memory_order_relaxed);
    inverse_core_entries.fetch_add(stats.inverse_core_entries, std::memory_order_relaxed);
    inverse_claim_entries.fetch_add(stats.inverse_claim_entries, std::memory_order_relaxed);
    verification_province_visits.fetch_add(stats.verification_province_visits,
        std::memory_order_relaxed);
    core_tag_comparisons.fetch_add(stats.core_tag_comparisons, std::memory_order_relaxed);
    claim_tag_comparisons.fetch_add(stats.claim_tag_comparisons, std::memory_order_relaxed);
    invalid_province_ids.fetch_add(stats.invalid_province_ids, std::memory_order_relaxed);
    inverse_ticks.fetch_add(clock.inverse_elapsed, std::memory_order_relaxed);
    verification_ticks.fetch_add(clock.verification_elapsed, std::memory_order_relaxed);
}

void log_stats(std::uint64_t count) noexcept;

void run_optimized(void* current) noexcept {
    LARGE_INTEGER begin{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&begin);
    optimized_snapshot.country = nullptr;
    TlsSetValue(tls_slot, nullptr);
    optimized_snapshot.cores.clear();
    optimized_snapshot.claims.clear();
    RuntimeWorld world(image_base, current);
    std::uint64_t core_entries = 0;
    std::uint64_t claim_entries = 0;
    std::uint64_t invalid_ids = 0;
    world.for_each_current_core([&](int province_id) {
        ++core_entries;
        if (world.valid_province(province_id)) {
            optimized_snapshot.cores.set(world.province_owner(province_id));
        } else {
            ++invalid_ids;
        }
        return true;
    });
    world.for_each_current_claim([&](int province_id) {
        ++claim_entries;
        if (world.valid_province(province_id)) {
            optimized_snapshot.claims.set(world.province_owner(province_id));
        } else {
            ++invalid_ids;
        }
        return true;
    });
    optimized_snapshot.country = current;
    if (!TlsSetValue(tls_slot, &optimized_snapshot)) {
        optimized_snapshot.country = nullptr;
        static std::atomic<int> warned{0};
        if (warned.exchange(1) == 0) log("TlsSetValue failed: error=%lu; original loops will run",
            GetLastError());
    }
    QueryPerformanceCounter(&end);
    const auto elapsed = static_cast<std::uint64_t>(end.QuadPart - begin.QuadPart);
    inverse_ticks.fetch_add(elapsed, std::memory_order_relaxed);
    inverse_core_entries.fetch_add(core_entries, std::memory_order_relaxed);
    inverse_claim_entries.fetch_add(claim_entries, std::memory_order_relaxed);
    invalid_province_ids.fetch_add(invalid_ids, std::memory_order_relaxed);
    const auto count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 256 == 0) log_stats(count);
}

struct ParallelContext {
    const RuntimeWorld* world = nullptr;
    knowledgepatch::OwnerBits inverse;
    std::array<std::uint8_t, knowledgepatch::tag_count>* flags = nullptr;
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> invalid_ids{0};
    std::atomic<std::uint64_t> unknown{0};
    std::atomic<std::uint64_t> processed{0};
};

void parallel_country_job(void* opaque, std::size_t worker_index) noexcept {
    (void)worker_index;
    auto& context = *static_cast<ParallelContext*>(opaque);
    std::uint64_t local_invalid = 0;
    std::uint64_t local_unknown = 0;
    std::uint64_t local_processed = 0;
    for (;;) {
        const auto index = context.next.fetch_add(1, std::memory_order_relaxed);
        if (index >= context.world->country_count()) break;
        const auto tag = context.world->country_tag(index);
        const auto result = knowledgepatch::compute_pair_flags(
            *context.world, index, context.inverse, local_invalid);
        if (!result) continue;
        (*context.flags)[tag] = result;
        ++local_processed;
        if (result & knowledgepatch::pair_visibility_unknown) ++local_unknown;
    }
    context.invalid_ids.fetch_add(local_invalid, std::memory_order_relaxed);
    context.unknown.fetch_add(local_unknown, std::memory_order_relaxed);
    context.processed.fetch_add(local_processed, std::memory_order_relaxed);
}

void run_parallel(void* current) noexcept {
    LARGE_INTEGER begin{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&begin);
    optimized_snapshot.country = nullptr;
    TlsSetValue(tls_slot, nullptr);
    optimized_snapshot.cores.clear();
    optimized_snapshot.claims.clear();
    optimized_snapshot.pair_flags.fill(0);
    if (parallel_pool.size() == 0) {
        static std::atomic<int> warned{0};
        if (warned.exchange(1) == 0) log("parallel pool is unavailable; original loops will run");
        return;
    }

    RuntimeWorld world(image_base, current);
    ParallelContext context;
    context.world = &world;
    context.flags = &optimized_snapshot.pair_flags;
    context.inverse.cores.clear();
    context.inverse.claims.clear();
    std::uint64_t core_entries = 0;
    std::uint64_t claim_entries = 0;
    world.for_each_current_core([&](int province_id) {
        ++core_entries;
        if (world.valid_province(province_id)) {
            context.inverse.cores.set(world.province_owner(province_id));
        } else {
            context.invalid_ids.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    });
    world.for_each_current_claim([&](int province_id) {
        ++claim_entries;
        if (world.valid_province(province_id)) {
            context.inverse.claims.set(world.province_owner(province_id));
        } else {
            context.invalid_ids.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    });

    static std::atomic<int> phase_logged{0};
    const bool log_phase = phase_logged.exchange(1) == 0;
    if (log_phase) log("parallel phase begin: workers=%zu", parallel_pool.size());
    parallel_pool.run(&parallel_country_job, &context);
    if (log_phase) log("parallel phase end: processed=%llu unknown=%llu",
        static_cast<unsigned long long>(context.processed.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(context.unknown.load(std::memory_order_relaxed)));
    optimized_snapshot.cores = context.inverse.cores;
    optimized_snapshot.claims = context.inverse.claims;
    optimized_snapshot.country = current;
    if (!TlsSetValue(tls_slot, &optimized_snapshot)) {
        optimized_snapshot.country = nullptr;
        static std::atomic<int> warned{0};
        if (warned.exchange(1) == 0) log("TlsSetValue failed: error=%lu; original loops will run",
            GetLastError());
    }
    QueryPerformanceCounter(&end);
    const auto elapsed = static_cast<std::uint64_t>(end.QuadPart - begin.QuadPart);
    const auto pairs = context.processed.load(std::memory_order_relaxed);
    const auto invalid_ids = context.invalid_ids.load(std::memory_order_relaxed);
    parallel_ticks.fetch_add(elapsed, std::memory_order_relaxed);
    parallel_pairs.fetch_add(pairs, std::memory_order_relaxed);
    parallel_unknown.fetch_add(context.unknown.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    country_pairs.fetch_add(pairs, std::memory_order_relaxed);
    inverse_core_entries.fetch_add(core_entries, std::memory_order_relaxed);
    inverse_claim_entries.fetch_add(claim_entries, std::memory_order_relaxed);
    invalid_province_ids.fetch_add(invalid_ids, std::memory_order_relaxed);
    const auto count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 256 == 0) log_stats(count);
}

const char* mode_name(Mode selected) noexcept {
    switch (selected) {
        case Mode::shadow: return "shadow";
        case Mode::optimized: return "optimized";
        case Mode::parallel_shadow: return "parallel_shadow";
        case Mode::parallel: return "parallel";
        case Mode::disabled: return "disabled";
    }
    return "unknown";
}

void log_stats(std::uint64_t count) noexcept {
    const auto inverse_us = ticks_to_microseconds(inverse_ticks.load(std::memory_order_relaxed));
    const auto verification_us = ticks_to_microseconds(
        verification_ticks.load(std::memory_order_relaxed));
    const auto original_tick_count = original_ticks.load(std::memory_order_relaxed);
    const auto parallel_tick_count = parallel_ticks.load(std::memory_order_relaxed);
    const auto original_us = ticks_to_microseconds(original_tick_count);
    const auto parallel_us = ticks_to_microseconds(parallel_tick_count);
    const auto speedup_x100 = parallel_tick_count
        ? original_tick_count * 100 / parallel_tick_count : 0;
    const auto selected = mode.load(std::memory_order_relaxed);
    const auto scheduler = knowledgepatch::scheduler_wait_stats();
    log("stats mode=%s calls=%llu pairs=%llu mismatched_pairs=%llu invalid_ids=%llu "
        "inverse_total_ms=%llu verification_total_ms=%llu inverse_avg_us=%llu "
        "verification_avg_us=%llu inverse_cores=%llu inverse_claims=%llu "
        "verification_province_visits=%llu core_tag_comparisons=%llu claim_tag_comparisons=%llu "
        "original_total_ms=%llu original_avg_us=%llu parallel_total_ms=%llu "
        "parallel_avg_us=%llu speedup_x100=%llu parallel_pairs=%llu "
        "parallel_unknown=%llu parallel_mismatches=%llu workers=%zu "
        "scheduler_wait_calls=%llu scheduler_wait_iterations=%llu "
        "scheduler_yields=%llu scheduler_max_wait=%llu",
        mode_name(selected),
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(country_pairs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(mismatched_pairs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(invalid_province_ids.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(inverse_us / 1000),
        static_cast<unsigned long long>(verification_us / 1000),
        static_cast<unsigned long long>(count ? inverse_us / count : 0),
        static_cast<unsigned long long>(count ? verification_us / count : 0),
        static_cast<unsigned long long>(inverse_core_entries.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(inverse_claim_entries.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(verification_province_visits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(core_tag_comparisons.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(claim_tag_comparisons.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(original_us / 1000),
        static_cast<unsigned long long>(count ? original_us / count : 0),
        static_cast<unsigned long long>(parallel_us / 1000),
        static_cast<unsigned long long>(count ? parallel_us / count : 0),
        static_cast<unsigned long long>(speedup_x100),
        static_cast<unsigned long long>(parallel_pairs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(parallel_unknown.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(parallel_mismatches.load(std::memory_order_relaxed)),
        parallel_pool.size(),
        static_cast<unsigned long long>(scheduler.calls),
        static_cast<unsigned long long>(scheduler.iterations),
        static_cast<unsigned long long>(scheduler.yields),
        static_cast<unsigned long long>(scheduler.max_iterations));
}

void log_mismatch(const knowledgepatch::Mismatch& mismatch) noexcept {
    log("MISMATCH current_tag=%u other_tag=%u country_index=%zu "
        "core_expected=%u core_inverse=%u core_expected_province=%d core_inverse_province=%d "
        "claim_expected=%u claim_inverse=%u claim_expected_province=%d claim_inverse_province=%d",
        static_cast<unsigned>(mismatch.current_tag), static_cast<unsigned>(mismatch.other_tag),
        mismatch.country_index, mismatch.core_expected ? 1U : 0U, mismatch.core_inverse ? 1U : 0U,
        mismatch.core_expected_province, mismatch.core_inverse_province,
        mismatch.claim_expected ? 1U : 0U, mismatch.claim_inverse ? 1U : 0U,
        mismatch.claim_expected_province, mismatch.claim_inverse_province);
}

bool paths_for_module(HMODULE module, std::wstring& directory) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    directory = path;
    log_path = directory + L"\\eu4_knowledge_patch.log";
    return true;
}

Mode read_mode(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_knowledge_patch.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"knowledge", L"mode", L"shadow", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    if (_wcsicmp(value, L"shadow") == 0) return Mode::shadow;
    if (_wcsicmp(value, L"optimized") == 0) return Mode::optimized;
    if (_wcsicmp(value, L"parallel_shadow") == 0) return Mode::parallel_shadow;
    if (_wcsicmp(value, L"parallel") == 0) return Mode::parallel;
    return Mode::disabled;
}

SchedulerMode read_scheduler_mode(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_knowledge_patch.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"scheduler", L"wait", L"disabled", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    if (_wcsicmp(value, L"efficient") == 0) return SchedulerMode::efficient;
    return SchedulerMode::disabled;
}

DWORD install_checked(HMODULE module) {
    std::wstring directory;
    if (!paths_for_module(module, directory)) {
        installation_status.store(-1);
        return 1;
    }
    wchar_t exe_path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
        installation_status.store(-2);
        return 2;
    }
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    exe_name = exe_name ? exe_name + 1 : exe_path;
    if (_wcsicmp(exe_name, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    try {
        const auto hash = knowledgepatch::executable_sha256(exe_path);
        if (hash.empty()) {
            log("executable SHA-256 unavailable");
        } else {
            log("executable SHA-256=%ls expected=%s", hash.c_str(),
                hash == knowledgepatch::expected_exe_sha256 ? "yes" : "no (signature checks decide)");
        }
    } catch (...) {
        log("executable SHA-256 failed; signature checks will still run");
    }

    const auto selected_mode = read_mode(directory);
    const auto selected_scheduler = read_scheduler_mode(directory);
    if (selected_mode == Mode::disabled && selected_scheduler == SchedulerMode::disabled) {
        log("disabled by configuration; no bytes changed; version=%ls", version);
        installation_status.store(2);
        return 0;
    }
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto compatibility = knowledgepatch::check_eu4_image(image);
    if (!compatibility.compatible) {
        log("REFUSED: %s; no bytes changed", compatibility.error);
        installation_status.store(-4);
        return 4;
    }
    log("compatibility passed: EU4 1.37.5 x64; checked_sites=%zu",
        compatibility.checked_sites);

    image_base = image;
    knowledge_original_target = reinterpret_cast<void*>(image + eu4build::rva(knowledgepatch::shadow_target_rva));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    speedpatch::CallPatch shadow;
    knowledgepatch::LoopPatch core;
    knowledgepatch::LoopPatch claim;
    knowledgepatch::ParallelPatch parallel_apply;
    knowledgepatch::ParallelPatch parallel_verify;
    knowledgepatch::ParallelPatch scheduler_wait;
    char error[160]{};
    const bool parallel_selected = selected_mode == Mode::parallel_shadow ||
        selected_mode == Mode::parallel;
    const auto cleanup = [&]() noexcept {
        mode.store(Mode::disabled);
        knowledgepatch::discard_parallel_patch(scheduler_wait);
        knowledgepatch::discard_parallel_patch(parallel_verify);
        knowledgepatch::discard_parallel_patch(parallel_apply);
        knowledgepatch::discard_loop(claim);
        knowledgepatch::discard_loop(core);
        speedpatch::discard_call(shadow);
        parallel_pool.stop();
        if (tls_slot != TLS_OUT_OF_INDEXES) TlsFree(tls_slot);
        tls_slot = TLS_OUT_OF_INDEXES;
    };

    if (selected_mode == Mode::optimized || parallel_selected) {
        tls_slot = TlsAlloc();
        if (tls_slot == TLS_OUT_OF_INDEXES || tls_slot >= 64) {
            log("REFUSED: no direct-access TLS slot is available; no bytes changed");
            cleanup();
            installation_status.store(-5);
            return 5;
        }
    }
    if (parallel_selected) {
        const auto workers = (std::max)(1U, (std::min)(4U, std::thread::hardware_concurrency()));
        if (!parallel_pool.start(workers)) {
            log("REFUSED: cannot start the parallel worker pool; no game bytes changed");
            cleanup();
            installation_status.store(-5);
            return 5;
        }
    }
    bool prepared = true;
    if (selected_mode != Mode::disabled) {
        prepared = speedpatch::prepare_call(
            reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::shadow_call_rva)),
            knowledge_original_target, reinterpret_cast<const void*>(&knowledge_shadow_hook),
            shadow, error, sizeof(error));
    }
    constexpr std::uint8_t core_expected[knowledgepatch::core_loop_site_size] = {
        0x48, 0x8B, 0x0A, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x41, 0x3B, 0xC8};
    constexpr std::uint8_t claim_expected[knowledgepatch::claim_loop_site_size] = {
        0x48, 0x8B, 0x08, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x3B, 0xCA};
    const knowledgepatch::LoopHookImage core_image{
        knowledge_optimized_core_begin, knowledge_optimized_core_end,
        knowledge_optimized_core_tls, knowledge_optimized_core_found_displacement,
        knowledge_optimized_core_out_displacement,
        knowledge_optimized_core_continuation_displacement};
    const knowledgepatch::LoopHookImage claim_image{
        knowledge_optimized_claim_begin, knowledge_optimized_claim_end,
        knowledge_optimized_claim_tls, knowledge_optimized_claim_found_displacement,
        knowledge_optimized_claim_out_displacement,
        knowledge_optimized_claim_continuation_displacement};
    if (prepared && selected_mode == Mode::optimized) {
        const auto tls_offset = static_cast<std::uint32_t>(0x1480 + tls_slot * 8);
        prepared = knowledgepatch::prepare_loop(
            reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::core_loop_rva)),
            reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::core_continuation_rva)),
            reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::core_found_rva)),
            reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::core_out_rva)),
            core_expected, sizeof(core_expected), core_image, tls_offset, core, error, sizeof(error));
        if (prepared) {
            prepared = knowledgepatch::prepare_loop(
                reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::claim_loop_rva)),
                reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::claim_continuation_rva)),
                reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::claim_found_rva)),
                reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::claim_out_rva)),
                claim_expected, sizeof(claim_expected), claim_image, tls_offset, claim,
                error, sizeof(error));
        }
    }
    constexpr std::uint8_t apply_expected[23] = {
        0x45, 0x32, 0xFF, 0x32, 0xC9, 0x45, 0x32, 0xC0,
        0x44, 0x88, 0x85, 0x40, 0x02, 0x00, 0x00, 0x45,
        0x32, 0xC9, 0x44, 0x88, 0x4C, 0x24, 0x38};
    constexpr std::uint8_t verify_expected[9] = {
        0x4C, 0x8B, 0x75, 0x80, 0x48, 0x8B, 0x74, 0x24, 0x60};
    const knowledgepatch::ParallelHookImage apply_image{
        knowledge_parallel_apply_begin, knowledge_parallel_apply_end,
        knowledge_parallel_apply_tls, knowledge_parallel_apply_commit_displacement,
        knowledge_parallel_apply_fallback_displacement};
    const knowledgepatch::ParallelHookImage verify_image{
        knowledge_parallel_verify_begin, knowledge_parallel_verify_end, nullptr,
        knowledge_parallel_verify_continuation_displacement, nullptr};
    const knowledgepatch::ParallelHookImage timing_image{
        knowledge_parallel_timing_begin, knowledge_parallel_timing_end, nullptr,
        knowledge_parallel_timing_continuation_displacement, nullptr};
    if (prepared && parallel_selected) {
        const auto tls_offset = static_cast<std::uint32_t>(0x1480 + tls_slot * 8);
        if (selected_mode == Mode::parallel) {
            prepared = knowledgepatch::prepare_parallel_patch(
                reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::parallel_apply_rva)),
                reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::parallel_apply_commit_rva)),
                reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::parallel_apply_fallback_rva)),
                apply_expected, sizeof(apply_expected), apply_image, tls_offset,
                parallel_apply, error, sizeof(error));
        } else {
            prepared = knowledgepatch::prepare_parallel_patch(
                reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::parallel_apply_rva)),
                reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::parallel_apply_fallback_rva)),
                nullptr, apply_expected, sizeof(apply_expected), timing_image, tls_offset,
                parallel_apply, error, sizeof(error));
            if (prepared) {
                prepared = knowledgepatch::prepare_parallel_patch(
                    reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::parallel_verify_rva)),
                    reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::parallel_verify_continuation_rva)),
                    nullptr, verify_expected, sizeof(verify_expected), verify_image, tls_offset,
                    parallel_verify, error, sizeof(error));
            }
        }
    }
    constexpr std::uint8_t scheduler_expected[13] = {
        0x0F, 0x57, 0xC0, 0xF3, 0x0F, 0x7F, 0x44,
        0x24, 0x20, 0x48, 0x8B, 0x5F, 0x18};
    const knowledgepatch::ParallelHookImage scheduler_image{
        knowledge_scheduler_wait_begin, knowledge_scheduler_wait_end, nullptr,
        knowledge_scheduler_wait_resume_displacement,
        knowledge_scheduler_wait_top_displacement};
    if (prepared && selected_scheduler == SchedulerMode::efficient) {
        prepared = knowledgepatch::prepare_parallel_patch(
            reinterpret_cast<std::uint8_t*>(image + eu4build::rva(knowledgepatch::scheduler_wait_rva)),
            reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::scheduler_wait_resume_rva)),
            reinterpret_cast<const void*>(image + eu4build::rva(knowledgepatch::scheduler_wait_top_rva)),
            scheduler_expected, sizeof(scheduler_expected), scheduler_image, 0,
            scheduler_wait, error, sizeof(error));
    }
    if (!prepared) {
        log("REFUSED: %s; no game bytes changed", error);
        cleanup();
        installation_status.store(-6);
        return 6;
    }

    mode.store(selected_mode);
    bool committed = true;
    if (selected_mode != Mode::disabled) {
        committed = speedpatch::commit_call(shadow, error, sizeof(error));
    }
    if (committed && selected_mode == Mode::optimized) {
        committed = knowledgepatch::commit_loop(core, error, sizeof(error));
        if (committed) committed = knowledgepatch::commit_loop(claim, error, sizeof(error));
    }
    if (committed && selected_mode == Mode::parallel) {
        committed = knowledgepatch::commit_parallel_patch(parallel_apply, error, sizeof(error));
    }
    if (committed && selected_mode == Mode::parallel_shadow) {
        committed = knowledgepatch::commit_parallel_patch(parallel_apply, error, sizeof(error));
        if (committed) {
            committed = knowledgepatch::commit_parallel_patch(parallel_verify, error, sizeof(error));
        }
    }
    if (committed && selected_scheduler == SchedulerMode::efficient) {
        committed = knowledgepatch::commit_parallel_patch(scheduler_wait, error, sizeof(error));
    }
    if (!committed) {
        log("REFUSED: %s; rolling back every installed hook", error);
        cleanup();
        installation_status.store(-7);
        return 7;
    }
    installation_status.store(selected_mode == Mode::disabled ? 6 :
        selected_mode == Mode::optimized ? 3 :
        selected_mode == Mode::parallel ? 4 : selected_mode == Mode::parallel_shadow ? 5 : 1);
    if (selected_mode == Mode::disabled) {
        log("initialized version=%ls knowledge_mode=disabled scheduler_wait=efficient; "
            "only the scheduler wait loop changed", version);
    } else if (selected_mode == Mode::optimized) {
        log("initialized version=%ls mode=optimized call_rva=0x%zx core_rva=0x%zx "
            "claim_rva=0x%zx tls_slot=%lu; one CALL and two loops changed",
            version, knowledgepatch::shadow_call_rva, knowledgepatch::core_loop_rva,
            knowledgepatch::claim_loop_rva, tls_slot);
    } else if (parallel_selected) {
        log("initialized version=%ls mode=%s call_rva=0x%zx hook_rva=0x%zx "
            "tls_slot=%lu workers=%zu; parallel results are %s",
            version, mode_name(selected_mode), knowledgepatch::shadow_call_rva,
            selected_mode == Mode::parallel ? knowledgepatch::parallel_apply_rva :
                knowledgepatch::parallel_verify_rva,
            tls_slot, parallel_pool.size(),
            selected_mode == Mode::parallel ? "applied sequentially with fallback" :
                "compared against original results");
    } else {
        log("initialized version=%ls mode=shadow call_rva=0x%zx target_rva=0x%zx; "
            "original behavior retained; only one CALL changed",
            version, knowledgepatch::shadow_call_rva, knowledgepatch::shadow_target_rva);
    }
    if (selected_scheduler == SchedulerMode::efficient && selected_mode != Mode::disabled) {
        log("scheduler_wait=efficient wait_rva=0x%zx resume_rva=0x%zx top_rva=0x%zx; "
            "13 bytes changed", knowledgepatch::scheduler_wait_rva,
            knowledgepatch::scheduler_wait_resume_rva, knowledgepatch::scheduler_wait_top_rva);
    }
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
        log("REFUSED: unhandled C++ exception during installation");
        installation_status.store(-7);
        return 7;
    }
}

}  // namespace

extern "C" std::uint64_t knowledge_qpc_now() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart);
}

extern "C" void knowledge_parallel_timing_start() noexcept {
    original_pair_begin = knowledge_qpc_now();
}

extern "C" void knowledge_record_original_pair() noexcept {
    const auto begin = original_pair_begin;
    original_pair_begin = 0;
    if (!begin) return;
    const auto end = knowledge_qpc_now();
    if (end > begin) original_ticks.fetch_add(end - begin, std::memory_order_relaxed);
}

extern "C" void knowledge_shadow_verify(void* current) noexcept {
    if (!current) return;
    const auto selected = mode.load(std::memory_order_relaxed);
    if (selected == Mode::optimized) {
        run_optimized(current);
        return;
    }
    if (selected == Mode::parallel_shadow || selected == Mode::parallel) {
        run_parallel(current);
        return;
    }
    if (selected != Mode::shadow) return;
    RuntimeWorld world(image_base, current);
    PhaseClock clock;
    auto comparison = knowledgepatch::compare_inverse(world, clock);
    clock.finish();
    add_stats(comparison, clock);
    if (comparison.mismatched_pairs) {
        const auto previous = mismatched_pairs.fetch_add(
            comparison.mismatched_pairs, std::memory_order_relaxed);
        if (comparison.has_first_mismatch && previous < 32) log_mismatch(comparison.first);
    }
    const auto count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 256 == 0) log_stats(count);
}

void log_parallel_details(void* current, void* other, Tag other_tag,
    std::uint8_t flags, std::uint8_t actual) noexcept {
    RuntimeWorld world(image_base, current);
    const Tag current_tag = world.current_tag();
    bool direct_core = false;
    bool direct_claim = false;
    int direct_core_province = -1;
    int direct_claim_province = -1;
    world.for_each_owned_pointer(other, [&](int province_id) {
        if (!world.valid_province(province_id)) return true;
        if (!direct_core) {
            world.for_each_core_tag(province_id, [&](Tag tag) {
                if (tag != current_tag) return true;
                direct_core = true;
                direct_core_province = province_id;
                return false;
            });
        }
        if (!direct_claim) {
            world.for_each_claim_tag(province_id, [&](Tag tag) {
                if (tag != current_tag) return true;
                direct_claim = true;
                direct_claim_province = province_id;
                return false;
            });
        }
        return !(direct_core && direct_claim);
    });

    bool inverse_core = false;
    bool inverse_claim = false;
    int inverse_core_province = -1;
    int inverse_claim_province = -1;
    world.for_each_current_core([&](int province_id) {
        if (!world.valid_province(province_id) || world.province_owner(province_id) != other_tag) {
            return true;
        }
        inverse_core = true;
        inverse_core_province = province_id;
        return false;
    });
    world.for_each_current_claim([&](int province_id) {
        if (!world.valid_province(province_id) || world.province_owner(province_id) != other_tag) {
            return true;
        }
        inverse_claim = true;
        inverse_claim_province = province_id;
        return false;
    });
    log("PARALLEL_DETAIL current_tag=%u other_tag=%u flags=0x%02x relation=0x%02x "
        "direct=[core=%u claim=%u] inverse=[core=%u claim=%u] "
        "direct_provinces=[core=%d claim=%d] inverse_provinces=[core=%d claim=%d]",
        static_cast<unsigned>(current_tag), static_cast<unsigned>(other_tag),
        static_cast<unsigned>(flags), static_cast<unsigned>(actual),
        direct_core ? 1U : 0U, direct_claim ? 1U : 0U,
        inverse_core ? 1U : 0U, inverse_claim ? 1U : 0U,
        direct_core_province, direct_claim_province,
        inverse_core_province, inverse_claim_province);
}

extern "C" void knowledge_parallel_verify(
    void* current, void* other, void* relation) noexcept {
    knowledge_record_original_pair();
    if (!current || !other || !relation ||
        mode.load(std::memory_order_relaxed) != Mode::parallel_shadow) {
        return;
    }
    const auto* snapshot = static_cast<const OptimizedSnapshot*>(TlsGetValue(tls_slot));
    if (!snapshot || snapshot->country != current) return;
    const auto other_tag = read_memory<Tag>(reinterpret_cast<std::uintptr_t>(other) + 0x24);
    const auto flags = snapshot->pair_flags[other_tag];
    if (!(flags & knowledgepatch::pair_valid)) return;
    const auto actual = read_memory<std::uint8_t>(
        reinterpret_cast<std::uintptr_t>(relation) + 0x45);
    const auto different = [&](std::uint8_t computed, std::uint8_t original) {
        return static_cast<bool>(flags & computed) != static_cast<bool>(actual & original);
    };
    bool mismatch = different(knowledgepatch::pair_religion, 0x10) ||
        different(knowledgepatch::pair_core, 0x08) ||
        different(knowledgepatch::pair_claim, 0x04);
    if (!(flags & knowledgepatch::pair_visibility_unknown)) {
        mismatch = mismatch || different(knowledgepatch::pair_visible, 0x40);
    }
    if (!mismatch) return;
    const auto previous = parallel_mismatches.fetch_add(1, std::memory_order_relaxed);
    if (previous < 32) {
        log("PARALLEL_MISMATCH current=%p other=%p other_tag=%u flags=0x%02x relation=0x%02x "
            "computed=[religion=%u core=%u claim=%u visible=%u unknown=%u]",
            current, other, static_cast<unsigned>(other_tag), static_cast<unsigned>(flags),
            static_cast<unsigned>(actual),
            (flags & knowledgepatch::pair_religion) ? 1U : 0U,
            (flags & knowledgepatch::pair_core) ? 1U : 0U,
            (flags & knowledgepatch::pair_claim) ? 1U : 0U,
            (flags & knowledgepatch::pair_visible) ? 1U : 0U,
            (flags & knowledgepatch::pair_visibility_unknown) ? 1U : 0U);
        log_parallel_details(current, other, other_tag, flags, actual);
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4KnowledgePatchStatus() {
    return installation_status.load();
}

extern "C" __declspec(dllexport) std::uint64_t EU4KnowledgePatchCallCount() {
    return calls.load();
}

extern "C" __declspec(dllexport) std::uint64_t EU4KnowledgePatchMismatchCount() {
    return mismatched_pairs.load();
}
