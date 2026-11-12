// EU4 Country Patch 0.4.0 experimental, for EU4 1.37.5.0 Windows x64.
//
// The daily country loop (14075b0d0) updates every country one after another
// on the main thread. Part of that work only reads game state: estate
// potentials 140723120(estate, tag) evaluated for every estate type of every
// country (~3.9% of the main thread). This plugin evaluates them for all
// countries in parallel right before the loop and, in the loop, answers from
// that table.
//
// Because earlier countries in the loop (and the country's own earlier steps)
// may change what a later evaluation reads, the precomputed answer is only an
// assumption. Use mode=shadow first: the game evaluates as usual and the
// plugin counts mismatches. mode=on is meant only after long runs with zero
// mismatches.
//
// Also caches military strength (14093c790 / 14093ca70 at 1402ee4e6..51e).
// Also precomputes the colonist range check 1403520d0(country, province, &1000)
// called daily for every colony in progress (colony=, ~3.8% of main thread):
// pairs asked during yesterday's serial province loop are evaluated in parallel
// right before today's loop (after the engine's own parallel province jobs).
// Also precomputes government reform checks done daily in 1407b2940: potential
// 1407a5880(reform, tag) and valid 1407a5ae0(reform, tag) for every reform of
// every country (reforms=, ~1.3% of main thread).
// Modes (eu4_country_patch.ini, [country] estates= / strength= / colony= / reforms=):
// off (default) | shadow | on
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace {

enum class Mode { off, shadow, on };

// mov rcx,r13; call 14075b0d0 (daily country loop)
constexpr std::uintptr_t phase_site_rva = 0x75c0c2;
constexpr std::uint8_t phase_bytes[] = {0x49, 0x8b, 0xcd, 0xe8, 0x06, 0xf0, 0xff, 0xff};
constexpr std::uintptr_t phase_call_rva = 0x75c0c5;
constexpr std::uintptr_t phase_function_rva = 0x75b0d0;
// mov rdx,[rdi]; mov rcx,rsi; call 140723120 (estate potential)
constexpr std::uintptr_t estate_site_rva = 0x37c931;
constexpr std::uint8_t estate_bytes[] = {0x48, 0x8b, 0x17, 0x48, 0x8b, 0xce, 0xe8, 0xe4, 0x67,
    0x3a, 0x00};
constexpr std::uintptr_t estate_call_rva = 0x37c937;
constexpr std::uintptr_t estate_function_rva = 0x723120;
// lea rdx,[rbp+0x660]; mov rcx,rdi; call 14093c790 / 14093ca70 (military strength cache)
// 14093c790 ignores r8: its home slot is overwritten as a dword before any read.
constexpr std::uintptr_t strength_call_rva[3] = {0x2ee4e6, 0x2ee502, 0x2ee51e};
constexpr std::uint8_t strength_site_bytes[3][5] = {{0xe8, 0xa5, 0xe2, 0x64, 0x00},
    {0xe8, 0x69, 0xe5, 0x64, 0x00}, {0xe8, 0x4d, 0xe5, 0x64, 0x00}};
constexpr std::uintptr_t strength_function_rva[3] = {0x93c790, 0x93ca70, 0x93ca70};
// The third call (14093ca70 with r8b=1) depends on countries updated earlier in
// the loop (shadow: ~0.04% mismatches), so only the first two are cached.
constexpr int strength_kinds = 2;
constexpr std::uintptr_t game_flags_rva = 0x233fe78;  // byte [ptr+0x7484] gates the cache
constexpr std::uintptr_t estate_list_rva = 0x233fe58;     // struct {T** begin, end, cap; int count}
constexpr std::uintptr_t estate_default_rva = 0x233fe50;  // lazily created default estate

// call 1403520d0 (colonist range check) inside 1403532c0
constexpr std::uintptr_t colony_call_rva = 0x353f76;
constexpr std::uint8_t colony_bytes[] = {0xe8, 0x55, 0xe1, 0xff, 0xff};
constexpr std::uintptr_t colony_function_rva = 0x3520d0;
// lea rcx,[rsp+0x30]; call 140b7bdf0 (timer) in 14075b2d0 right before the serial province loop
constexpr std::uintptr_t colony_begin_rva = 0x75b38c;
constexpr std::uint8_t colony_begin_bytes[] = {0xe8, 0x5f, 0x0a, 0x42, 0x00};
constexpr std::uintptr_t colony_begin_function_rva = 0xb7bdf0;
// mov rcx,r13; call 14075b2d0 (province daily phase)
constexpr std::uintptr_t colony_phase_rva = 0x75c137;
constexpr std::uint8_t colony_phase_bytes[] = {0xe8, 0x94, 0xf1, 0xff, 0xff};
constexpr std::uintptr_t colony_phase_function_rva = 0x75b2d0;

// mov rdx,rbx; mov rcx,r12; call 1407a5880 / 1407a5ae0 inside 1407b2940
constexpr std::uintptr_t reform_call_rva[2] = {0x7b29eb, 0x7b29fa};
constexpr std::uint8_t reform_bytes[2][5] = {{0xe8, 0x90, 0x2e, 0xff, 0xff},
    {0xe8, 0xe1, 0x30, 0xff, 0xff}};
constexpr std::uintptr_t reform_function_rva[2] = {0x7a5880, 0x7a5ae0};

using PhaseFn = void (*)(void*);
using EstateFn = std::uint64_t (*)(void*, std::uint64_t);
using StrengthFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint64_t);
StrengthFn original_strength[3] = {nullptr, nullptr, nullptr};
Mode strength_mode = Mode::off;
struct StrengthEntry {
    std::uint64_t value[strength_kinds];
    std::atomic<std::uint32_t> generation;
};
using ColonyFn = std::uint64_t (*)(void*, void*, int*);
using TimerFn = std::uint64_t (*)(void*);
ColonyFn original_colony = nullptr;
TimerFn original_timer = nullptr;
PhaseFn original_province_phase = nullptr;
Mode colony_mode = Mode::off;
constexpr int colony_slots = 8192;  // power of two
struct ColonyEntry {
    void* country;
    void* province;
    std::uint32_t seen_phase;  // main thread only
    std::atomic<std::uint32_t> answer;  // generation << 1 | result
};
ColonyEntry colony_table[colony_slots];
int colony_used[colony_slots];
int colony_used_count = 0;
int colony_jobs[colony_slots];
int colony_job_count = 0;
struct ColonyPair { void* country; void* province; };
ColonyPair colony_keep[colony_slots];
std::uint32_t colony_generation = 0;
bool colony_active = false;
DWORD main_thread = 0;
void* colony_state = nullptr;
const void* colony_key_state[3] = {nullptr, nullptr, nullptr};
std::atomic<int> job_kind{0};
std::atomic<std::uint64_t> c_phases{0}, c_jobs{0}, c_compared{0}, c_mismatches{0}, c_missing{0},
    c_answered{0}, c_other{0}, c_precompute_qpc{0}, c_original_qpc{0};

using ReformFn = std::uint64_t (*)(void*, std::uint64_t);
ReformFn original_reform[2] = {nullptr, nullptr};
Mode reform_mode = Mode::off;
constexpr int max_reforms = 32;
// code: bit0 potential, bit1 valid computed, bit2 valid
struct ReformEntry {
    const void* reform[max_reforms];
    std::uint8_t code[max_reforms];
    int count;
    std::atomic<std::uint32_t> generation;
};
ReformEntry reform_table[4096];  // = max_countries
bool reform_gate = false;
std::atomic<std::uint64_t> r_compared{0}, r_mismatches{0}, r_missing{0}, r_answered{0},
    r_computed{0};

PhaseFn original_phase = nullptr;
EstateFn original_estate = nullptr;
std::uintptr_t image = 0;
Mode mode = Mode::off;

constexpr int max_countries = 4096;
constexpr int max_estates = 64;
// entry = generation << 2 | (answer + 1); 0 = never computed
std::atomic<std::uint32_t> table[max_countries * max_estates];
std::uint32_t generation = 0;  // main thread writes before workers start
void* estates[max_estates];
int estate_count = 0;
bool phase_active = false;  // main thread only

StrengthEntry strength_table[max_countries];
bool strength_gate = false;  // game flag +0x7484 at precompute time
void** country_array = nullptr;
int country_total = 0;
std::atomic<int> next_country{0};

std::atomic<std::uint64_t> s_compared{0}, s_mismatches{0}, s_missing{0}, s_answered{0};
std::atomic<std::uint64_t> phases{0}, computed{0}, compared{0}, mismatches{0}, missing{0},
    answered{0}, precompute_qpc{0}, phase_qpc{0};
std::int64_t qpc_frequency = 1;
std::atomic<LONG> installation_status{0};
std::wstring log_path;
std::mutex log_mutex;

void log(const char* format, ...) noexcept {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> lock(log_mutex);
    FILE* file = _wfopen(log_path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "[%lu:%lu] %s\n", GetCurrentProcessId(), GetTickCount(), text);
    std::fclose(file);
}

inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline int country_id(std::uint64_t tag) noexcept {
    return static_cast<short>(tag >> 32);
}

// Same filter the loop applies before reaching the estate potentials.
inline bool country_eligible(const std::uint8_t* country) noexcept {
    return country[0x26] != 7;
}
inline bool estates_eligible(const std::uint8_t* country) noexcept {
    return *reinterpret_cast<const int*>(country + 0x1344) != 0;
}

void compute_country(int index) noexcept {
    const auto* country = static_cast<const std::uint8_t*>(country_array[index]);
    if (!country || !country_eligible(country)) return;
    const std::uint64_t tag = *reinterpret_cast<const std::uint64_t*>(country + 0x20);
    const int id = country_id(tag);
    if (id < 0 || id >= max_countries) return;
    if (strength_gate) {
        void* cached = const_cast<std::uint8_t*>(country) + 0x28c0;
        StrengthEntry& s = strength_table[id];
        std::uint64_t out = 0;
        s.value[0] = *original_strength[0](cached, &out, 0);
        s.value[1] = *original_strength[1](cached, &out, 0);
        s.generation.store(generation, std::memory_order_release);
    }
    if (reform_gate) {
        ReformEntry& r = reform_table[id];
        r.count = 0;
        const auto* government = *reinterpret_cast<const std::uint8_t* const*>(country + 0x1420);
        if (government) {
            const auto* stack = government + 0x38;  // deque<reform*>
            auto* const* map = *reinterpret_cast<void** const* const*>(stack + 0x20);
            const auto map_size = *reinterpret_cast<const std::uint64_t*>(stack + 0x28);
            const auto offset = *reinterpret_cast<const std::uint64_t*>(stack + 0x30);
            const auto size = *reinterpret_cast<const std::uint64_t*>(stack + 0x38);
            if (map && map_size && size <= static_cast<std::uint64_t>(max_reforms)) {
                for (std::uint64_t i = offset; i != offset + size; ++i) {
                    auto* const* block = static_cast<void* const*>(map[(i >> 1) & (map_size - 1)]);
                    void* reform = block[i & 1];
                    if (!reform) continue;
                    std::uint8_t code = 0;
                    if (static_cast<std::uint8_t>(original_reform[0](reform, tag)) != 0) {
                        code = 1 | 2;
                        if (static_cast<std::uint8_t>(original_reform[1](reform, tag)) != 0) code |= 4;
                    }
                    r.reform[r.count] = reform;
                    r.code[r.count] = code;
                    ++r.count;
                    r_computed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        r.generation.store(generation, std::memory_order_release);
    }
    if (!estates_eligible(country)) return;
    for (int e = 1; e < estate_count; ++e) {
        if (!estates[e]) continue;
        const bool answer = static_cast<std::uint8_t>(original_estate(estates[e], tag)) != 0;
        table[id * max_estates + e].store((generation << 2) | (answer ? 2u : 1u),
            std::memory_order_relaxed);
        computed.fetch_add(1, std::memory_order_relaxed);
    }
}

void compute_colony(int index) noexcept {
    ColonyEntry& e = colony_table[colony_jobs[index]];
    int percent = 1000;
    const bool answer =
        static_cast<std::uint8_t>(original_colony(e.country, e.province, &percent)) != 0;
    e.answer.store((colony_generation << 1) | (answer ? 1u : 0u), std::memory_order_release);
}

void run_cb_work() noexcept;
void run_pm_work() noexcept;
void run_merc_work() noexcept;
void run_hist_work() noexcept;
void run_maint_work() noexcept;

void run_work() noexcept {
    if (job_kind.load(std::memory_order_acquire) == 6) {
        run_maint_work();
        return;
    }
    if (job_kind.load(std::memory_order_acquire) == 5) {
        run_hist_work();
        return;
    }
    if (job_kind.load(std::memory_order_acquire) == 4) {
        run_merc_work();
        return;
    }
    if (job_kind.load(std::memory_order_acquire) == 3) {
        run_pm_work();
        return;
    }
    if (job_kind.load(std::memory_order_acquire) == 2) {
        run_cb_work();
        return;
    }
    if (job_kind.load(std::memory_order_acquire) == 1) {
        for (;;) {
            const int index = next_country.fetch_add(1, std::memory_order_relaxed);
            if (index >= colony_job_count) return;
            compute_colony(index);
        }
    }
    for (;;) {
        const int index = next_country.fetch_add(1, std::memory_order_relaxed);
        if (index >= country_total) return;
        compute_country(index);
    }
}

// ---- tiny worker pool
// Workers take one semaphore token per job. `active` is raised before a worker
// takes an index, so once the main thread sees the queue drained and
// active == 0, no worker can start evaluating anything until the next job.
constexpr int max_workers = 32;
int worker_count = 0;
std::atomic<int> active{0};
HANDLE job_semaphore = nullptr;
// Hot jobs (cb): after any job a worker spins for hot_spin_qpc watching job_gen, so a
// burst of short jobs does not pay a semaphore wake-up each time. Dispatchers of hot
// jobs bump job_gen and post semaphore tokens only for workers that are asleep.
std::atomic<unsigned> job_gen{0};
std::atomic<int> sleeping{0};
std::int64_t hot_spin_qpc = 0;

DWORD WINAPI worker(void*) {
    unsigned seen = job_gen.load();
    for (;;) {
        bool hot = false;
        if (hot_spin_qpc > 0) {
            const std::int64_t until = qpc_now() + hot_spin_qpc;
            for (int i = 0;; ++i) {
                if (job_gen.load(std::memory_order_acquire) != seen) { hot = true; break; }
                YieldProcessor();
                if ((i & 63) == 0 && qpc_now() > until) break;
            }
        }
        if (!hot) {
            sleeping.fetch_add(1);
            WaitForSingleObject(job_semaphore, INFINITE);
            sleeping.fetch_sub(1);
        }
        seen = job_gen.load(std::memory_order_acquire);
        active.fetch_add(1);
        run_work();
        active.fetch_sub(1);
    }
}

void precompute(void* state) noexcept {
    const auto* s = static_cast<const std::uint8_t*>(state);
    country_array = *reinterpret_cast<void** const*>(s + 0x1d08);
    country_total = *reinterpret_cast<const int*>(s + 0x1d14);
    const auto* list = *reinterpret_cast<std::uint8_t* const*>(image + eu4build::rva(estate_list_rva));
    void* fallback = *reinterpret_cast<void* const*>(image + eu4build::rva(estate_default_rva));
    estate_count = 0;
    strength_gate = false;
    if (!country_array || country_total <= 0) return;
    if (mode != Mode::off && list) {
        const int count = *reinterpret_cast<const int*>(list + 0x18);
        if (count > 1 && count <= max_estates) {
            auto* const* begin = *reinterpret_cast<void* const* const*>(list);
            auto* const* end = *reinterpret_cast<void* const* const*>(list + 8);
            const int stored = static_cast<int>(end - begin);
            for (int e = 1; e < count; ++e) estates[e] = e < stored ? begin[e] : fallback;
            estate_count = count;
        }
    }
    const auto* flags = *reinterpret_cast<const std::uint8_t* const*>(image + eu4build::rva(game_flags_rva));
    strength_gate = strength_mode != Mode::off && flags && flags[0x7484] != 0;
    reform_gate = reform_mode != Mode::off && flags && flags[0x7484] != 0 && flags[0x7485] == 0;
    if (estate_count == 0 && !strength_gate && !reform_gate) return;
    ++generation;
    job_kind.store(0, std::memory_order_release);
    next_country.store(0);
    ReleaseSemaphore(job_semaphore, worker_count, nullptr);
    run_work();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
}

void phase_hook(void* state) {
    main_thread = GetCurrentThreadId();
    const auto start = qpc_now();
    precompute(state);
    const auto ready = qpc_now();
    precompute_qpc.fetch_add(static_cast<std::uint64_t>(ready - start), std::memory_order_relaxed);
    phase_active = estate_count > 0 || strength_gate || reform_gate;
    original_phase(state);
    phase_active = false;
    phase_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
    phases.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<int> logged{0};

std::uint64_t estate_hook(void* estate, std::uint64_t tag) {
    if (!phase_active || estate_count == 0) return original_estate(estate, tag);
    int e = 1;
    while (e < estate_count && estates[e] != estate) ++e;
    const int id = country_id(tag);
    std::uint32_t entry = 0;
    if (e < estate_count && id >= 0 && id < max_countries) {
        entry = table[id * max_estates + e].load(std::memory_order_relaxed);
    }
    const bool have = entry != 0 && (entry >> 2) == (generation & 0x3fffffffu);
    if (mode == Mode::on && have) {
        answered.fetch_add(1, std::memory_order_relaxed);
        return (entry & 3) == 2 ? 1 : 0;
    }
    const std::uint64_t result = original_estate(estate, tag);
    if (!have) {
        missing.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    compared.fetch_add(1, std::memory_order_relaxed);
    const bool actual = static_cast<std::uint8_t>(result) != 0;
    if (actual != ((entry & 3) == 2)) {
        mismatches.fetch_add(1, std::memory_order_relaxed);
        if (logged.fetch_add(1) < 40) {
            log("MISMATCH country=%d estate=%d precomputed=%d actual=%d", id, e,
                (entry & 3) == 2 ? 1 : 0, actual ? 1 : 0);
        }
    }
    return result;
}

std::atomic<int> s_logged{0};

template <int K>
std::uint64_t* strength_hook(void* cached, std::uint64_t* out, std::uint64_t flag) {
    if (!phase_active || !strength_gate) return original_strength[K](cached, out, flag);
    const auto* country = static_cast<const std::uint8_t*>(cached) - 0x28c0;
    const int id = country_id(*reinterpret_cast<const std::uint64_t*>(country + 0x20));
    const bool have = id >= 0 && id < max_countries &&
        strength_table[id].generation.load(std::memory_order_acquire) == generation;
    if (strength_mode == Mode::on && have) {
        *out = strength_table[id].value[K];
        s_answered.fetch_add(1, std::memory_order_relaxed);
        return out;
    }
    std::uint64_t* result = original_strength[K](cached, out, flag);
    if (!have) {
        s_missing.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    s_compared.fetch_add(1, std::memory_order_relaxed);
    if (*result != strength_table[id].value[K]) {
        s_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (s_logged.fetch_add(1) < 40) {
            log("STRENGTH MISMATCH country=%d kind=%d precomputed=%llx actual=%llx", id, K,
                static_cast<unsigned long long>(strength_table[id].value[K]),
                static_cast<unsigned long long>(*result));
        }
    }
    return result;
}

std::atomic<int> r_logged{0};

template <int K>
std::uint64_t reform_hook(void* reform, std::uint64_t tag) {
    if (!phase_active || !reform_gate) return original_reform[K](reform, tag);
    const int id = country_id(tag);
    std::uint8_t code = 0;
    bool have = false;
    if (id >= 0 && id < max_countries &&
        reform_table[id].generation.load(std::memory_order_acquire) == generation) {
        const ReformEntry& r = reform_table[id];
        for (int i = 0; i < r.count; ++i) {
            if (r.reform[i] == reform) {
                code = r.code[i];
                have = K == 0 || (code & 2) != 0;
                break;
            }
        }
    }
    const bool predicted = K == 0 ? (code & 1) != 0 : (code & 4) != 0;
    if (reform_mode == Mode::on && have) {
        r_answered.fetch_add(1, std::memory_order_relaxed);
        return predicted ? 1 : 0;
    }
    const std::uint64_t result = original_reform[K](reform, tag);
    if (!have) {
        r_missing.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    r_compared.fetch_add(1, std::memory_order_relaxed);
    const bool actual = static_cast<std::uint8_t>(result) != 0;
    if (actual != predicted) {
        r_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (r_logged.fetch_add(1) < 40) {
            log("REFORM MISMATCH country=%d kind=%d reform=%p precomputed=%d actual=%d", id, K,
                reform, predicted ? 1 : 0, actual ? 1 : 0);
        }
    }
    return result;
}

// ---- colony range precompute (main thread drives everything except compute_colony)
inline unsigned colony_hash(const void* country, const void* province) noexcept {
    const auto x = reinterpret_cast<std::uintptr_t>(country) * 0x9E3779B97F4A7C15ull ^
        reinterpret_cast<std::uintptr_t>(province) * 0xC2B2AE3D27D4EB4Full;
    return static_cast<unsigned>(x >> 40) & (colony_slots - 1);
}

void colony_reset_entry(ColonyEntry& e) noexcept {
    e.country = nullptr;
    e.province = nullptr;
    e.seen_phase = 0;
    e.answer.store(0, std::memory_order_relaxed);
}

void colony_clear() noexcept {
    for (int i = 0; i < colony_used_count; ++i) colony_reset_entry(colony_table[colony_used[i]]);
    colony_used_count = 0;
}

// Finds or inserts; returns -1 when the table is too full.
int colony_slot(void* country, void* province) noexcept {
    unsigned h = colony_hash(country, province);
    for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (colony_slots - 1)) {
        ColonyEntry& e = colony_table[h];
        if (e.country == country && e.province == province) return static_cast<int>(h);
        if (!e.country) {
            if (colony_used_count >= colony_slots / 2) return -1;
            e.country = country;
            e.province = province;
            e.seen_phase = 0;
            e.answer.store(0, std::memory_order_relaxed);
            colony_used[colony_used_count++] = static_cast<int>(h);
            return static_cast<int>(h);
        }
    }
    return -1;
}

void colony_province_phase(void* state) {
    const auto* s = static_cast<const std::uint8_t*>(state);
    const void* key[3] = {state, *reinterpret_cast<void* const*>(s + 0x1ca8),
        *reinterpret_cast<void* const*>(s + 0x1d08)};
    if (key[0] != colony_key_state[0] || key[1] != colony_key_state[1] ||
        key[2] != colony_key_state[2]) {
        colony_clear();  // another game was loaded: yesterday's pointers are meaningless
        for (int i = 0; i < 3; ++i) colony_key_state[i] = key[i];
    }
    colony_state = state;
    main_thread = GetCurrentThreadId();
    original_province_phase(state);
    colony_active = false;
    c_phases.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t colony_begin(void* timer) {
    const std::uint64_t result = original_timer(timer);
    if (!colony_state || GetCurrentThreadId() != main_thread) return result;
    const auto start = qpc_now();
    ++colony_generation;
    if (colony_generation >= 0x7fffffffu) colony_generation = 1;
    // Pairs asked yesterday become today's jobs; everything else is dropped.
    // Rebuild the table from scratch so that probe chains stay intact.
    int kept = 0;
    for (int i = 0; i < colony_used_count; ++i) {
        ColonyEntry& e = colony_table[colony_used[i]];
        if (e.seen_phase != 0 && e.seen_phase + 1 == colony_generation) {
            colony_keep[kept++] = {e.country, e.province};
        }
        colony_reset_entry(e);
    }
    colony_used_count = 0;
    colony_job_count = 0;
    for (int i = 0; i < kept; ++i) {
        const int slot = colony_slot(colony_keep[i].country, colony_keep[i].province);
        if (slot >= 0) colony_jobs[colony_job_count++] = slot;
    }
    if (colony_job_count > 0) {
        job_kind.store(1, std::memory_order_release);
        next_country.store(0);
        ReleaseSemaphore(job_semaphore, worker_count, nullptr);
        run_work();
        for (int spins = 0; active.load() != 0; ++spins) {
            if (spins < 4000) YieldProcessor(); else SwitchToThread();
        }
        job_kind.store(0, std::memory_order_release);
        c_jobs.fetch_add(static_cast<std::uint64_t>(colony_job_count), std::memory_order_relaxed);
    }
    colony_active = true;
    c_precompute_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start),
        std::memory_order_relaxed);
    return result;
}

std::atomic<int> c_logged{0};

std::uint64_t colony_hook(void* country, void* province, int* percent) {
    if (!colony_active || GetCurrentThreadId() != main_thread || !percent || *percent != 1000) {
        c_other.fetch_add(1, std::memory_order_relaxed);
        return original_colony(country, province, percent);
    }
    const int slot = colony_slot(country, province);
    std::uint32_t entry = 0;
    if (slot >= 0) {
        ColonyEntry& e = colony_table[slot];
        e.seen_phase = colony_generation;
        entry = e.answer.load(std::memory_order_acquire);
    }
    const bool have = entry != 0 && (entry >> 1) == colony_generation;
    if (colony_mode == Mode::on && have) {
        c_answered.fetch_add(1, std::memory_order_relaxed);
        return (entry & 1) ? 1 : 0;
    }
    const auto start = qpc_now();
    const std::uint64_t result = original_colony(country, province, percent);
    c_original_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start),
        std::memory_order_relaxed);
    if (!have) {
        c_missing.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    c_compared.fetch_add(1, std::memory_order_relaxed);
    const bool actual = static_cast<std::uint8_t>(result) != 0;
    if (actual != ((entry & 1) != 0)) {
        c_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (c_logged.fetch_add(1) < 40) {
            log("COLONY MISMATCH country=%p province=%p precomputed=%u actual=%u", country,
                province, entry & 1, actual ? 1u : 0u);
        }
    }
    return result;
}

// ---- casus belli cache --------------------------------------------------------
// 140442720 (UpdateCasusBellisGainLostCache, after event options and other
// changes) builds two trigger scopes on its stack (A: ROOT = country, FROM -> B;
// B: the target; 0xd0 bytes each, destructor 1400947f0) and calls 140440da0:
//   1. CB types (database 1401f9210, list +0xe8..+0xf0) whose prerequisites
//      (+0x40, count +0x4c, vfunc +0x30 with A) hold -> candidates;
//   2. for every other country passing a few filters (alive, not the special tags,
//      visibility through overlords, not in the exclusion list +0x1548), B is set
//      to that country and, for every candidate not masked in the bitset, the
//      trigger at cb+0xb0 is evaluated with A (Trigger_AND 140b98200); a true
//      result appends the target tag to cache vector [cb+0x10].
// Step 2 only reads game state. cb=on evaluates the targets on the worker pool,
// each worker with its own fresh copies of A and B, then appends the results in
// the original order. cb=shadow evaluates in parallel, lets the game run as usual
// and compares the appended lists.
constexpr std::uintptr_t cb_call_rva = 0x442baf;
constexpr std::uint8_t cb_bytes[] = {0xe8, 0xec, 0xe1, 0xff, 0xff};
constexpr std::uintptr_t cb_function_rva = 0x440da0;
constexpr std::uintptr_t cb_db_rva = 0x1f9210;
constexpr std::uintptr_t trigger_and_rva = 0xb98200;
constexpr std::uintptr_t vector_push_rva = 0xdfde0;
constexpr std::uintptr_t scope_dtor_rva = 0x947f0;
constexpr std::uintptr_t special_tag_rva[3] = {0x233d8a4, 0x233d8d8, 0x233d8dc};
constexpr std::uintptr_t country_by_id_rva = 0x233d8d0;  // ptr; +0x118 = country*[id]
using CbFn = void (*)(void*, void*, void*, void*, void*, int*);
using DbFn = std::uint8_t* (*)();
using TriggerFn = std::uint64_t (*)(void*, void*);
using PrereqFn = std::uint64_t (*)(void*, void*);
using PushFn = void (*)(void*, void*, void*);
using ScopeDtorFn = void (*)(void*);
CbFn original_cb = nullptr;
DbFn cb_db = nullptr;
TriggerFn trigger_and = nullptr;
PushFn vector_push = nullptr;
ScopeDtorFn scope_dtor = nullptr;
Mode cb_mode = Mode::off;
constexpr int cb_max_candidates = 2048;
constexpr int scope_size = 0xd0;
constexpr int scope_data = 0x68;  // fields copied; 0x68.. are vectors / lazy child scopes
std::uint8_t* cb_candidates[cb_max_candidates];
int cb_candidate_count = 0;
std::uint64_t cb_target_tag[max_countries];
int cb_target_count = 0;
constexpr std::size_t cb_result_size = 1u << 22;
std::uint8_t cb_result[cb_result_size];  // [target][candidate]: 0 no, 1 yes, 2 index out of range
alignas(16) std::uint8_t cb_template[2][scope_size];
const int* cb_info = nullptr;
std::atomic<int> cb_next{1 << 30};
std::atomic<std::uint64_t> cb_calls{0}, cb_parallel{0}, cb_targets{0}, cb_evals{0},
    cb_compared{0}, cb_mismatches{0}, cb_fallback{0}, cb_qpc{0}, cb_original_qpc{0}, cb_prepare_qpc{0};
std::atomic<int> cb_logged{0};

inline std::uint64_t u64(const void* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline bool tag_present(std::uint64_t tag) noexcept {
    return static_cast<char>(tag >> 56) != 0 && static_cast<char>(tag >> 48) != 7;
}

// bit (id * stride + cb index) of the boost::dynamic_bitset described by info:
// info[0] stride, (info+2) block pointer, (info+8) bit count. -1 = out of range.
inline int cb_masked(const int* info, std::uint64_t tag, const std::uint8_t* cb) noexcept {
    const int pos = static_cast<int>(static_cast<short>(tag >> 32)) * info[0] +
        *reinterpret_cast<const int*>(cb + 0x10);
    const std::uint64_t bits = u64(info + 8);
    const auto upos = static_cast<std::uint64_t>(static_cast<std::int64_t>(pos));
    if (upos >= bits) return -1;
    const auto* blocks = *reinterpret_cast<const std::uint32_t* const*>(info + 2);
    return static_cast<int>((blocks[upos >> 5] >> (pos & 31)) & 1);
}

void make_scopes(std::uint8_t* a, std::uint8_t* b) noexcept {
    std::memcpy(a, cb_template[0], scope_data);
    std::memset(a + scope_data, 0, scope_size - scope_data);
    std::memcpy(b, cb_template[1], scope_data);
    std::memset(b + scope_data, 0, scope_size - scope_data);
    void* pa = a;
    void* pb = b;
    std::memcpy(a + 0x10, &pa, 8);
    std::memcpy(a + 0x18, &pb, 8);
    std::memcpy(a + 0x20, &pa, 8);
    std::memcpy(b + 0x10, &pb, 8);
    std::memcpy(b + 0x18, &pb, 8);
    std::memcpy(b + 0x20, &pb, 8);
}

int cb_target(int index, std::uint8_t* a, std::uint8_t* b) noexcept {
    int evals = 0;
    const std::uint64_t tag = cb_target_tag[index];
    const int zero = 0;
    std::memcpy(b + 0x28, &zero, 4);
    std::memcpy(b + 0x2c, &tag, 8);
    std::uint8_t* row = cb_result + static_cast<std::size_t>(index) * cb_candidate_count;
    for (int k = 0; k < cb_candidate_count; ++k) {
        std::uint8_t* cb = cb_candidates[k];
        const int masked = cb_masked(cb_info, tag, cb);
        if (masked != 0) {
            row[k] = masked < 0 ? 2 : 0;
            continue;
        }
        row[k] = static_cast<std::uint8_t>(trigger_and(cb + 0xb0, a)) != 0 ? 1 : 0;
        ++evals;
    }
    return evals;
}

void run_cb_work() noexcept {
    int index = cb_next.fetch_add(1, std::memory_order_relaxed);
    if (index >= cb_target_count) return;
    alignas(16) std::uint8_t a[scope_size];
    alignas(16) std::uint8_t b[scope_size];
    make_scopes(a, b);
    int evals = 0;
    do {
        evals += cb_target(index, a, b);
        index = cb_next.fetch_add(1, std::memory_order_relaxed);
    } while (index < cb_target_count);
    cb_evals.fetch_add(static_cast<std::uint64_t>(evals), std::memory_order_relaxed);
    scope_dtor(a);
    scope_dtor(b);
}

// candidates and targets exactly as 140440da0 selects them; false = leave it to the game
bool cb_prepare(std::uint8_t* country, std::uint8_t* scope_a, std::uint8_t* state) noexcept {
    cb_candidate_count = 0;
    cb_target_count = 0;
    std::uint8_t* db = cb_db();
    auto* const* it = *reinterpret_cast<std::uint8_t* const* const*>(db + 0xe8);
    auto* const* end = *reinterpret_cast<std::uint8_t* const* const*>(db + 0xf0);
    for (; it != end; ++it) {
        std::uint8_t* cb = *it;
        auto* const* prereq = *reinterpret_cast<std::uint8_t* const* const*>(cb + 0x40);
        const int prereq_count = *reinterpret_cast<const int*>(cb + 0x4c);
        bool ok = true;
        for (int j = 0; j < prereq_count && ok; ++j) {
            std::uint8_t* pr = prereq[j];
            const auto fn = *reinterpret_cast<const PrereqFn*>(
                *reinterpret_cast<std::uint8_t* const*>(pr) + 0x30);
            ok = static_cast<std::uint8_t>(fn(pr, scope_a)) != 0;
        }
        if (!ok) continue;
        if (cb_candidate_count == cb_max_candidates) return false;
        cb_candidates[cb_candidate_count++] = cb;
    }
    if (cb_candidate_count == 0) return true;
    const std::uint64_t own = u64(country + 0x20);
    const int special[3] = {*reinterpret_cast<const int*>(image + eu4build::rva(special_tag_rva[0])),
        *reinterpret_cast<const int*>(image + eu4build::rva(special_tag_rva[1])),
        *reinterpret_cast<const int*>(image + eu4build::rva(special_tag_rva[2]))};
    auto* const* by_id = *reinterpret_cast<std::uint8_t* const* const*>(
        *reinterpret_cast<std::uint8_t* const*>(image + eu4build::rva(country_by_id_rva)) + 0x118);
    auto* const* list = *reinterpret_cast<std::uint8_t* const* const*>(state + 0x1d08);
    const int count = *reinterpret_cast<const int*>(state + 0x1d14);
    const auto* exclusion = *reinterpret_cast<const int* const*>(country + 0x1548);
    const int exclusion_count = *reinterpret_cast<const int*>(country + 0x1554);
    for (int i = 0; i < count; ++i) {
        std::uint8_t* t = list[i];
        if (t == country) continue;
        if (!(*reinterpret_cast<const int*>(t + 0x1344) != 0 || t[0x26] == 7)) continue;
        const std::uint64_t tag = u64(t + 0x20);
        if (static_cast<char>(tag >> 56) == 0) continue;
        if (static_cast<char>(tag >> 48) == 7) continue;
        const int id = static_cast<short>(tag >> 32);
        if (id == special[0] || id == special[1] || id == special[2]) continue;
        const std::uint8_t* x = *reinterpret_cast<std::uint8_t* const*>(t + 0x14b0);
        auto visible = [&](std::uint64_t who) {
            if ((x[0x959] & 0x10) == 0 || !tag_present(who)) return true;
            const auto w = static_cast<std::uint32_t>(static_cast<int>(static_cast<short>(who >> 32)));
            const auto* bits = *reinterpret_cast<const std::uint32_t* const*>(x + 0x5e0);
            return ((bits[w >> 5] >> (w & 31)) & 1) != 0;
        };
        bool ok = visible(own);
        std::uint64_t link = u64(country + 0x18f0);
        while (static_cast<char>(link >> 56) != 0) {
            const std::uint8_t* o = by_id[static_cast<short>(link >> 32)];
            ok = visible(u64(o + 0x20)) || ok;
            link = u64(o + 0x18f0);
        }
        if (!ok) continue;
        bool excluded = false;
        for (int e = 0; e < exclusion_count && !excluded; ++e) excluded = exclusion[e * 4] == id;
        if (excluded) continue;
        if (cb_target_count == max_countries) return false;
        cb_target_tag[cb_target_count++] = tag;
    }
    return static_cast<std::size_t>(cb_target_count) * cb_candidate_count <= cb_result_size;
}

void cb_run_parallel() noexcept {
    job_kind.store(2, std::memory_order_release);
    cb_next.store(0);
    job_gen.fetch_add(1, std::memory_order_release);
    const int asleep = sleeping.load();
    if (asleep > 0) ReleaseSemaphore(job_semaphore, asleep < worker_count ? asleep : worker_count, nullptr);
    run_cb_work();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
    // a worker that wakes late (stale token or job_gen) must not claim targets while the
    // next call is still filling the tables
    cb_next.store(1 << 30);
}

inline std::uint8_t* cb_vector(void* cache, const std::uint8_t* cb) noexcept {
    return *reinterpret_cast<std::uint8_t* const*>(cache) +
        static_cast<std::uint64_t>(*reinterpret_cast<const std::uint32_t*>(cb + 0x10)) * 0x18;
}

void cb_hook(void* cache, void* country_ptr, void* scope_a, void* scope_b, void* state, int* info) {
    if (cb_mode == Mode::off || main_thread == 0 || GetCurrentThreadId() != main_thread) {
        original_cb(cache, country_ptr, scope_a, scope_b, state, info);
        return;
    }
    const auto start = qpc_now();
    cb_calls.fetch_add(1, std::memory_order_relaxed);
    auto* country = static_cast<std::uint8_t*>(country_ptr);
    std::memcpy(cb_template[0], scope_a, scope_data);
    std::memcpy(cb_template[1], scope_b, scope_data);
    cb_info = info;
    if (!cb_prepare(country, static_cast<std::uint8_t*>(scope_a), static_cast<std::uint8_t*>(state))) {
        cb_fallback.fetch_add(1, std::memory_order_relaxed);
        original_cb(cache, country_ptr, scope_a, scope_b, state, info);
        return;
    }
    if (cb_candidate_count == 0) {
        if (cb_mode == Mode::shadow) original_cb(cache, country_ptr, scope_a, scope_b, state, info);
        return;
    }
    cb_prepare_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
    if (cb_target_count > 0) cb_run_parallel();
    cb_parallel.fetch_add(1, std::memory_order_relaxed);
    cb_targets.fetch_add(static_cast<std::uint64_t>(cb_target_count), std::memory_order_relaxed);
    const std::size_t cells = static_cast<std::size_t>(cb_target_count) * cb_candidate_count;
    bool out_of_range = false;
    for (std::size_t i = 0; i < cells && !out_of_range; ++i) out_of_range = cb_result[i] == 2;
    const auto evaluated = qpc_now();
    cb_qpc.fetch_add(static_cast<std::uint64_t>(evaluated - start), std::memory_order_relaxed);
    if (cb_mode == Mode::on && !out_of_range) {
        for (int i = 0; i < cb_target_count; ++i) {
            const std::uint8_t* row = cb_result + static_cast<std::size_t>(i) * cb_candidate_count;
            for (int k = 0; k < cb_candidate_count; ++k) {
                if (row[k] != 1) continue;
                std::uint8_t* vec = cb_vector(cache, cb_candidates[k]);
                auto* end = *reinterpret_cast<std::uint64_t**>(vec + 8);
                if (end == *reinterpret_cast<std::uint64_t**>(vec + 0x10)) {
                    std::uint64_t value = cb_target_tag[i];
                    vector_push(vec, end, &value);
                } else {
                    *end = cb_target_tag[i];
                    *reinterpret_cast<std::uint64_t**>(vec + 8) = end + 1;
                }
            }
        }
        if (cb_target_count > 0) {
            const int zero = 0;
            auto* b = static_cast<std::uint8_t*>(scope_b);
            std::memcpy(b + 0x28, &zero, 4);
            std::memcpy(b + 0x2c, &cb_target_tag[cb_target_count - 1], 8);
        }
        return;
    }
    // shadow (or a bit index the game would assert on): the game does the work
    static std::uint64_t before[cb_max_candidates];
    for (int k = 0; k < cb_candidate_count; ++k) {
        const std::uint8_t* vec = cb_vector(cache, cb_candidates[k]);
        before[k] = (u64(vec + 8) - u64(vec)) / 8;
    }
    original_cb(cache, country_ptr, scope_a, scope_b, state, info);
    cb_original_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - evaluated),
        std::memory_order_relaxed);
    if (out_of_range) {
        cb_fallback.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int k = 0; k < cb_candidate_count; ++k) {
        const std::uint8_t* vec = cb_vector(cache, cb_candidates[k]);
        const auto* data = *reinterpret_cast<const std::uint64_t* const*>(vec);
        const std::uint64_t after = (u64(vec + 8) - u64(vec)) / 8;
        std::uint64_t pos = before[k];
        bool same = true;
        for (int i = 0; i < cb_target_count && same; ++i) {
            if (cb_result[static_cast<std::size_t>(i) * cb_candidate_count + k] != 1) continue;
            same = pos < after && data[pos] == cb_target_tag[i];
            ++pos;
        }
        same = same && pos == after;
        cb_compared.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            cb_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (cb_logged.fetch_add(1) < 30) {
                log("CB MISMATCH cb_index=%u before=%llu game_added=%llu targets=%d",
                    *reinterpret_cast<const std::uint32_t*>(cb_candidates[k] + 0x10),
                    static_cast<unsigned long long>(before[k]),
                    static_cast<unsigned long long>(after - before[k]), cb_target_count);
            }
        }
    }
}

// ---- province modifiers (monthly) --------------------------------------------
// GameState_Monthly walks all provinces (state+0x1cd8, count +0x1ce4) and calls the
// province monthly update 1409f35c0 for each; that calls CalculateModifier 1409f9500,
// which builds a fresh 0x2468-byte cache (ctor 14095c560, back pointer to the
// province), move-assigns it over province+0x988 (1409f95d0), destroys the temporary
// (1409d58c0) and fills the cache in place (140960330) when the game state allows it.
// provmod=on builds and fills a cache for every province on the worker pool before the
// first province update of the month; at the call the ready cache is move-assigned
// over province+0x988. provmod=shadow lets the game compute and compares all 26
// modifier tables of the two caches.
constexpr std::uintptr_t pm_loop_call_rva = 0x75d9eb;
constexpr std::uint8_t pm_loop_bytes[] = {0xe8, 0xd0, 0x5b, 0x29, 0x00};
constexpr std::uintptr_t pm_monthly_rva = 0x9f35c0;
constexpr std::uintptr_t pm_calc_call_rva = 0x9f38d4;
constexpr std::uint8_t pm_calc_bytes[] = {0xe8, 0x27, 0x5c, 0x00, 0x00};
constexpr std::uintptr_t pm_calc_rva = 0x9f9500;
constexpr std::uintptr_t pm_ctor_rva = 0x95c560;
constexpr std::uintptr_t pm_assign_rva = 0x9f95d0;
constexpr std::uintptr_t pm_dtor_rva = 0x9d58c0;
constexpr std::uintptr_t pm_compute_rva = 0x960330;
constexpr std::uintptr_t gamestate_rva = 0x233fe78;
constexpr std::size_t pm_cache_offset = 0x988;
constexpr std::size_t pm_cache_size = 0x2468;
constexpr std::size_t pm_slot_size = (pm_cache_size + 15) & ~std::size_t{15};
using PmFn = void (*)(void*);
using PmCtorFn = void* (*)(void*, void*);
using PmAssignFn = void* (*)(void*, void*);
PmFn original_pm_monthly = nullptr;
PmFn original_pm_calc = nullptr;
PmCtorFn pm_ctor = nullptr;
PmAssignFn pm_assign = nullptr;
PmFn pm_dtor = nullptr;
PmFn pm_compute = nullptr;
Mode pm_mode = Mode::off;
std::uint8_t* pm_buffer = nullptr;
int pm_capacity = 0;
int pm_count = 0;
std::uint8_t* const* pm_provinces = nullptr;
std::uint8_t* pm_state = nullptr;   // per slot: 0 empty, 1 built, 2 built + computed
int* pm_slot_by_id = nullptr;
int pm_id_capacity = 0;
bool pm_ready = false;
bool pm_serial = false;
std::atomic<int> pm_next{1 << 30};
std::atomic<std::uint64_t> pm_months{0}, pm_built{0}, pm_used{0}, pm_compared{0},
    pm_mismatches{0}, pm_fallback{0}, pm_precompute_qpc{0}, pm_original_qpc{0}, pm_assign_qpc{0};
std::atomic<int> pm_logged{0};

inline std::uint8_t* pm_slot(int i) noexcept { return pm_buffer + static_cast<std::size_t>(i) * pm_slot_size; }

inline bool pm_game_allows() noexcept {
    const auto* gs = *reinterpret_cast<std::uint8_t* const*>(image + eu4build::rva(gamestate_rva));
    return gs != nullptr && gs[0x7485] == 0 && gs[0x2520] != 0;
}

void run_pm_work() noexcept {
    for (;;) {
        const int i = pm_next.fetch_add(1, std::memory_order_relaxed);
        if (i >= pm_count) return;
        std::uint8_t* province = pm_provinces[i];
        if (*reinterpret_cast<const int*>(province + 0x20) == 0) continue;
        std::uint8_t* slot = pm_slot(i);
        pm_ctor(slot, province);
        pm_compute(slot);
        pm_state[i] = 2;
    }
}

void pm_release_all() noexcept {
    for (int i = 0; i < pm_count; ++i) {
        if (pm_state[i] != 0) {
            pm_dtor(pm_slot(i));
            pm_state[i] = 0;
        }
    }
    pm_ready = false;
}

bool pm_precompute(std::uint8_t* state) noexcept {
    const int count = *reinterpret_cast<const int*>(state + 0x1ce4);
    auto* const* list = *reinterpret_cast<std::uint8_t* const* const*>(state + 0x1cd8);
    if (count <= 0 || !pm_game_allows()) return false;
    if (count > pm_capacity) {
        if (pm_buffer) VirtualFree(pm_buffer, 0, MEM_RELEASE);
        delete[] pm_state;
        pm_capacity = count + 256;
        pm_buffer = static_cast<std::uint8_t*>(VirtualAlloc(nullptr,
            static_cast<std::size_t>(pm_capacity) * pm_slot_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        pm_state = new (std::nothrow) std::uint8_t[pm_capacity]();
        if (!pm_buffer || !pm_state) {
            pm_capacity = 0;
            return false;
        }
    }
    int max_id = 0;
    for (int i = 0; i < count; ++i) {
        const int id = *reinterpret_cast<const int*>(list[i] + 0x20);
        if (id > max_id) max_id = id;
    }
    if (max_id + 1 > pm_id_capacity) {
        delete[] pm_slot_by_id;
        pm_id_capacity = max_id + 1024;
        pm_slot_by_id = new (std::nothrow) int[pm_id_capacity];
        if (!pm_slot_by_id) {
            pm_id_capacity = 0;
            return false;
        }
    }
    for (int i = 0; i < pm_id_capacity; ++i) pm_slot_by_id[i] = -1;
    for (int i = 0; i < count; ++i) {
        const int id = *reinterpret_cast<const int*>(list[i] + 0x20);
        if (id >= 0) pm_slot_by_id[id] = i;
    }
    pm_provinces = list;
    pm_count = count;
    pm_next.store(0);
    if (pm_serial) {
        run_pm_work();
        pm_next.store(1 << 30);
        int built = 0;
        for (int i = 0; i < count; ++i) built += pm_state[i] != 0;
        pm_built.fetch_add(static_cast<std::uint64_t>(built), std::memory_order_relaxed);
        return true;
    }
    job_kind.store(3, std::memory_order_release);
    job_gen.fetch_add(1, std::memory_order_release);
    const int asleep = sleeping.load();
    if (asleep > 0) ReleaseSemaphore(job_semaphore, asleep < worker_count ? asleep : worker_count, nullptr);
    run_pm_work();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
    pm_next.store(1 << 30);
    int built = 0;
    for (int i = 0; i < count; ++i) built += pm_state[i] != 0;
    pm_built.fetch_add(static_cast<std::uint64_t>(built), std::memory_order_relaxed);
    return true;
}

// containers (dword offsets) that 1409f95d0 moves with 14032a150 / 140329f70
int pm_compare(const std::uint8_t* a, const std::uint8_t* b) noexcept {
    static const int words[] = {4, 0x36, 0x6e, 0xcc, 0x12a, 0x188, 0x1e6, 0x244, 0x2a2, 0x300,
        0x35e, 0x3bc, 0x41a, 0x478, 0x4d6, 0x534, 0x592, 0x5f0, 0x64e, 0x6ac, 0x70a, 0x768,
        0x7c6, 0x824, 0x854, 0x8b2};
    if (*reinterpret_cast<const int*>(a) != *reinterpret_cast<const int*>(b)) return 100;
    if (std::memcmp(a + 0x2440, b + 0x2440, 0x11) != 0) return 101;
    if (std::memcmp(a + 0x2454, b + 0x2454, 0x10) != 0) return 102;
    for (int k = 0; k < static_cast<int>(sizeof(words) / sizeof(words[0])); ++k) {
        const std::uint8_t* ca = a + words[k] * 4;
        const std::uint8_t* cb = b + words[k] * 4;
        if (*reinterpret_cast<const int*>(ca + 8) != *reinterpret_cast<const int*>(cb + 8) ||
            std::memcmp(ca + 0x10, cb + 0x10, 3) != 0) return k + 1;
        const int na = *reinterpret_cast<const int*>(ca + 0x64);
        const int nb = *reinterpret_cast<const int*>(cb + 0x64);
        if (na != nb || na < 0) return k + 1;
        const auto* pa = *reinterpret_cast<const std::uint8_t* const*>(ca + 0x58);
        const auto* pb = *reinterpret_cast<const std::uint8_t* const*>(cb + 0x58);
        if (na > 0 && (!pa || !pb || std::memcmp(pa, pb, static_cast<std::size_t>(na) * 8) != 0)) return k + 1;
    }
    return 0;
}

void pm_calc_hook(void* province_ptr) {
    auto* province = static_cast<std::uint8_t*>(province_ptr);
    int slot = -1;
    if (pm_ready && GetCurrentThreadId() == main_thread) {
        const int id = *reinterpret_cast<const int*>(province + 0x20);
        if (id >= 0 && id < pm_id_capacity) slot = pm_slot_by_id[id];
        if (slot >= 0 && (pm_state[slot] != 2 || pm_provinces[slot] != province)) slot = -1;
    }
    if (slot < 0) {
        if (pm_ready) pm_fallback.fetch_add(1, std::memory_order_relaxed);
        original_pm_calc(province_ptr);
        return;
    }
    std::uint8_t* cache = pm_slot(slot);
    const auto start = qpc_now();
    if (pm_mode == Mode::on && pm_game_allows()) {
        pm_assign(province + pm_cache_offset, cache);
        pm_dtor(cache);
        pm_state[slot] = 0;
        pm_used.fetch_add(1, std::memory_order_relaxed);
        pm_assign_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
        return;
    }
    original_pm_calc(province_ptr);
    pm_original_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
    const int diff = pm_compare(province + pm_cache_offset, cache);
    pm_compared.fetch_add(1, std::memory_order_relaxed);
    if (diff != 0) {
        pm_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (pm_logged.fetch_add(1) < 40) {
            const std::uint8_t* a = province + pm_cache_offset + 0x10;
            const std::uint8_t* b = cache + 0x10;
            const int na = *reinterpret_cast<const int*>(a + 0x64);
            const int nb = *reinterpret_cast<const int*>(b + 0x64);
            const auto* pa = *reinterpret_cast<const int* const*>(a + 0x58);
            const auto* pb = *reinterpret_cast<const int* const*>(b + 0x58);
            int first = -1;
            for (int j = 0; j < na && j < nb; ++j) {
                if (pa[2 * j] != pb[2 * j] || pa[2 * j + 1] != pb[2 * j + 1]) { first = j; break; }
            }
            log("PROVMOD MISMATCH province=%d part=%d dense=%d/%d count=%d/%d first=%d game=(%d,%d) "
                "pre=(%d,%d)", *reinterpret_cast<const int*>(province + 0x20), diff, a[0x10], b[0x10],
                na, nb, first, first >= 0 ? pa[2 * first] : 0, first >= 0 ? pa[2 * first + 1] : 0,
                first >= 0 ? pb[2 * first] : 0, first >= 0 ? pb[2 * first + 1] : 0);
        }
    }
    pm_dtor(cache);
    pm_state[slot] = 0;
}

void pm_monthly_hook(void* province_ptr) {
    auto* province = static_cast<std::uint8_t*>(province_ptr);
    auto* state = *reinterpret_cast<std::uint8_t**>(image + eu4build::rva(gamestate_rva));
    const bool on_main = main_thread != 0 && GetCurrentThreadId() == main_thread;
    if (on_main && state != nullptr) {
        const int count = *reinterpret_cast<const int*>(state + 0x1ce4);
        auto* const* list = *reinterpret_cast<std::uint8_t* const* const*>(state + 0x1cd8);
        if (count > 0 && list[0] == province) {
            if (pm_ready) pm_release_all();
            const auto start = qpc_now();
            pm_ready = pm_precompute(state);
            if (pm_ready) {
                pm_months.fetch_add(1, std::memory_order_relaxed);
                pm_precompute_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start),
                    std::memory_order_relaxed);
            }
        }
        original_pm_monthly(province_ptr);
        if (pm_ready && count > 0 && list[count - 1] == province) pm_release_all();
        return;
    }
    original_pm_monthly(province_ptr);
}

// ---- path edge check statistics (diagnostic, pathstat=on) ---------------------
// 14096f860 = vfunc +8 of the movement evaluators (vtables 141c3afa0 / 141c3cda0 /
// 141c41ac0): (this, unit, to, from, to, adjacency, options). Counts calls on the main
// thread and how many repeat an identical (vtable, unit, from, to, options) key on the
// same day.
constexpr std::uintptr_t ps_slots_rva[3] = {0x1c3afa0, 0x1c3cda0, 0x1c41ac0};
constexpr std::uintptr_t ps_function_rva = 0x96f860;
using PathCheckFn = std::uint64_t (*)(void*, void*, void*, void*, void*, void*, char*);
PathCheckFn original_path_check = nullptr;
bool ps_on = false;
constexpr unsigned ps_table_size = 1u << 21;
std::uint64_t* ps_table = nullptr;
int ps_day = -1;
std::uint64_t ps_calls = 0, ps_repeats = 0, ps_days = 0, ps_qpc = 0, ps_other = 0;

std::uint64_t path_check_hook(void* self, void* unit, void* to3, void* from, void* to, void* adj, char* opts) {
    if (GetCurrentThreadId() != main_thread || !ps_table) {
        ++ps_other;
        return original_path_check(self, unit, to3, from, to, adj, opts);
    }
    const auto* gs = *reinterpret_cast<std::uint8_t* const*>(image + eu4build::rva(gamestate_rva));
    const int day = gs ? *reinterpret_cast<const int*>(gs + 0x1dd0) : 0;
    if (day != ps_day) {
        std::memset(ps_table, 0, sizeof(std::uint64_t) * ps_table_size);
        ps_day = day;
        ++ps_days;
    }
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; h ^= h >> 29; };
    mix(*reinterpret_cast<std::uint64_t*>(self));
    mix(reinterpret_cast<std::uint64_t>(unit));
    mix(reinterpret_cast<std::uint64_t>(from));
    mix(reinterpret_cast<std::uint64_t>(to));
    mix(reinterpret_cast<std::uint64_t>(adj));
    std::uint64_t o1, o2;
    std::memcpy(&o1, opts, 8);
    std::memcpy(&o2, opts + 8, 8);
    mix(o1);
    mix(o2);
    if (h == 0) h = 1;
    unsigned i = static_cast<unsigned>(h) & (ps_table_size - 1);
    bool found = false;
    for (int probe = 0; probe < 32; ++probe, i = (i + 1) & (ps_table_size - 1)) {
        if (ps_table[i] == h) { found = true; break; }
        if (ps_table[i] == 0) { ps_table[i] = h; break; }
    }
    ++ps_calls;
    ps_repeats += found ? 1 : 0;
    const auto start = qpc_now();
    const auto result = original_path_check(self, unit, to3, from, to, adj, opts);
    ps_qpc += static_cast<std::uint64_t>(qpc_now() - start);
    return result;
}

// ---- relpf: prefetch for the monthly relation update 1402f9c50 (relpf=on|off)
// Country_MonthlyUpdate walks a vector of diplomatic entries of the country (caller loop in
// 1402f5xxx, iterator = pointer into an array of entry pointers at [rsp+0x60], end at
// [rsp+0x70]) and calls 1402f9c50(country, entry->key) for each. 1402f9c50 reads and writes
// other->relations(+0x1510, 72-byte records)[own tag]; the first load of that record is a
// cache miss (81% of the function in the monthly profile). The hook prefetches the records of
// the next entries (pipelined: entry key, country line, record) and then calls the game's
// function unchanged. Only reads within [iterator, end) and checked country indices.
constexpr std::uintptr_t relpf_function_rva = 0x2f9c50;
constexpr std::uintptr_t relpf_call_rva[2] = {0x2f6041, 0x2f60d4};
constexpr std::uintptr_t relpf_global_rva = 0x233d8d0;  // gamestate*; +0x118/+0x120 countries
constexpr std::uintptr_t relpf_bytes_rva[2] = {0x2f6032, 0x2f60bd};
constexpr std::uint8_t relpf_bytes0[] = {0xe8, 0xd9, 0x3f, 0xda, 0xff, 0x48, 0x8b, 0x10, 0x48, 0x8b,
    0x52, 0x20, 0x49, 0x8b, 0xcc, 0xe8};
constexpr std::uint8_t relpf_bytes1[] = {0x48, 0x8d, 0x4c, 0x24, 0x60, 0xe8, 0x49, 0x3f, 0xda, 0xff,
    0x48, 0x8b, 0x18, 0x48, 0x8b, 0x5b, 0x20, 0x48, 0x8b, 0xd3, 0x49, 0x8b, 0xcc, 0xe8};
// the function itself: rsi = other->relations, rdi = rsi + own_tag * 72
constexpr std::uintptr_t relpf_check_rva = 0x2f9c8a;
constexpr std::uint8_t relpf_check_bytes[] = {0x4d, 0x8b, 0x2c, 0xc6, 0x41, 0x0f, 0xbf, 0xc3, 0x49,
    0x8b, 0xb5, 0x10, 0x15, 0x00, 0x00, 0x48, 0x8d, 0x04, 0xc0, 0x48, 0x8d, 0x3c, 0xc6};
using RelFn = void (*)(void* country, std::uint64_t key);
RelFn original_rel = nullptr;
Mode relpf_mode = Mode::off;
std::atomic<std::uint64_t> relpf_calls{0}, relpf_prefetches{0};

// The keys come from the game's own list and the game indexes the table with them unchecked
// (1402f9c8a) when it reaches them; only negative tags are rejected here.
inline std::uint8_t* relpf_country(std::uint64_t key, void* const* begin) {
    const std::int16_t tag = static_cast<std::int16_t>(key >> 32);
    if (tag < 0) return nullptr;
    return static_cast<std::uint8_t*>(begin[tag]);
}

extern "C" void relpf_hook_c(void* country, std::uint64_t key, const std::uint8_t* caller_rsp) {
    std::uint64_t done = 0;
    void* const* it = *reinterpret_cast<void* const* const*>(caller_rsp + 0x60);
    void* const* end = *reinterpret_cast<void* const* const*>(caller_rsp + 0x70);
    const auto* gs = *reinterpret_cast<const std::uint8_t* const*>(image + eu4build::rva(relpf_global_rva));
    if (gs && it && end > it && end - it < (1 << 20)) {
        void* const* cbegin = *reinterpret_cast<void* const* const*>(gs + 0x118);
        const std::int16_t own = static_cast<std::int16_t>(
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(country) + 0x20) >> 32);
        const std::size_t left = static_cast<std::size_t>(end - it);
        if (cbegin && own >= 0) {
            // stage 1: entry key line, 8 ahead
            if (left > 8 && it[8]) __builtin_prefetch(static_cast<const std::uint8_t*>(it[8]) + 0x20);
            // stage 2: other country's relations pointer line, 4 ahead
            if (left > 4 && it[4]) {
                const auto key4 = *reinterpret_cast<const std::uint64_t*>(
                    static_cast<const std::uint8_t*>(it[4]) + 0x20);
                if (auto* c = relpf_country(key4, cbegin)) __builtin_prefetch(c + 0x1510);
            }
            // stage 3: the relation record itself, 1 and 2 ahead
            for (std::size_t k = 1; k <= 2 && k < left; ++k) {
                if (!it[k]) continue;
                const auto keyk = *reinterpret_cast<const std::uint64_t*>(
                    static_cast<const std::uint8_t*>(it[k]) + 0x20);
                auto* c = relpf_country(keyk, cbegin);
                if (!c) continue;
                const auto* rel = *reinterpret_cast<const std::uint8_t* const*>(c + 0x1510);
                if (!rel) continue;
                const auto* record = rel + static_cast<std::size_t>(own) * 72;
                __builtin_prefetch(record, 1);
                __builtin_prefetch(record + 0x21, 1);
                ++done;
            }
        }
    }
    relpf_calls.fetch_add(1, std::memory_order_relaxed);
    if (done) relpf_prefetches.fetch_add(done, std::memory_order_relaxed);
    original_rel(country, key);
}

// rcx = country, rdx = key; r8 = caller's rsp at the call (rsp + 8 here)
extern "C" void relpf_thunk();
asm(".text\n.globl relpf_thunk\nrelpf_thunk:\n    leaq 8(%rsp), %r8\n    jmp relpf_hook_c\n");


// ---- pmpf: prefetch for the monthly province modifier recalculation (pmpf=on|off)
// GameState_Monthly calls 1409f35c0(province) for every province (call 0x75d9eb; rbx = the
// current slot in the province pointer array, rsi = end). Each one ends in CalculateModifier
// 1409f9500, which move-assigns a fresh cache into province+0x988 (1409f95d0 frees the old
// strings/vectors) and refills it. The old cache (9 KB) and its heap blocks are touched once
// a month, so the frees stall on cache misses. The hook prefetches, a few provinces ahead,
// the province and its cache (+3), the cache's heap buffers (+2) and the strings inside its
// vectors of strings and the shared-pointer control blocks (+1), then calls the game's
// function unchanged. Only loads from the province and from live, shape-checked containers.
constexpr std::size_t pmpf_cache = 0x988;
constexpr std::size_t pmpf_province_bytes = 0x2e00;
constexpr std::size_t pmpf_top_string = 0x198;
constexpr std::size_t pmpf_alists[] = {0x10, 0xD8, 0x2090};
constexpr std::size_t pmpf_blist_first = 0x1B8, pmpf_blist_size = 0x178, pmpf_blist_count = 21;
constexpr std::size_t pmpf_blist_extra[] = {0x2150, 0x22C8};
constexpr std::uint8_t pmpf_site_bytes[] = {0x48, 0x8b, 0x0b, 0xe8};  // mov rcx,[rbx]; call
Mode pmpf_mode = Mode::off;
std::atomic<std::uint64_t> pmpf_calls{0}, pmpf_lines{0};

inline std::uint64_t pmpf_rd(const std::uint8_t* p, std::size_t at) {
    std::uint64_t v;
    std::memcpy(&v, p + at, 8);
    return v;
}
inline bool pmpf_vec(const std::uint8_t* v, std::size_t elem, std::uint64_t& b, std::uint64_t& e) {
    b = pmpf_rd(v, 0);
    e = pmpf_rd(v, 8);
    const std::uint64_t c = pmpf_rd(v, 0x10);
    return b != 0 && b <= e && e <= c && (e - b) % elem == 0 && e - b < (1ull << 24);
}
inline void pmpf_lines_of(std::uint64_t b, std::uint64_t e, std::uint64_t& n) {
    for (std::uint64_t a = b & ~63ull; a < e; a += 64, ++n) {
        __builtin_prefetch(reinterpret_cast<const void*>(a), 1);
    }
}
inline void pmpf_string(const std::uint8_t* s, std::uint64_t& n) {
    const std::uint64_t size = pmpf_rd(s, 0x10), cap = pmpf_rd(s, 0x18);
    if (cap < 16 || size > cap || cap >= (1ull << 32)) return;
    const std::uint64_t ptr = pmpf_rd(s, 0);
    if (!ptr) return;
    __builtin_prefetch(reinterpret_cast<const void*>(ptr), 1);
    if (cap + 1 >= 0x1000) __builtin_prefetch(reinterpret_cast<const void*>(ptr - 8));
    ++n;
}

// stage 2: heap buffers referenced directly by the cache
void pmpf_buffers(const std::uint8_t* c, std::uint64_t& n) {
    std::uint64_t b, e;
    auto list = [&](std::size_t at) {
        pmpf_string(c + at + 0x18, n);
        if (pmpf_vec(c + at + 0x38, 8, b, e)) pmpf_lines_of(b, e, n);
        if (pmpf_vec(c + at + 0xA8, 16, b, e)) pmpf_lines_of(b, e, n);
    };
    auto blist = [&](std::size_t at) {
        list(at);
        if (pmpf_vec(c + at + 0xC8, 4, b, e)) pmpf_lines_of(b, e, n);
        if (pmpf_vec(c + at + 0xE0, 32, b, e)) pmpf_lines_of(b, e, n);
        if (pmpf_vec(c + at + 0xF8, 32, b, e)) pmpf_lines_of(b, e, n);
        pmpf_string(c + at + 0x110, n);
        pmpf_string(c + at + 0x130, n);
        pmpf_string(c + at + 0x158, n);
    };
    pmpf_string(c + pmpf_top_string, n);
    for (auto at : pmpf_alists) list(at);
    for (std::size_t k = 0; k < pmpf_blist_count; ++k) blist(pmpf_blist_first + k * pmpf_blist_size);
    for (auto at : pmpf_blist_extra) blist(at);
}

// stage 3: strings inside the vectors of strings, shared-pointer control blocks
void pmpf_inner(const std::uint8_t* c, std::uint64_t& n) {
    std::uint64_t b, e;
    auto shared = [&](std::size_t at) {
        if (!pmpf_vec(c + at + 0xA8, 16, b, e)) return;
        for (std::uint64_t a = b; a < e; a += 16) {
            const std::uint64_t ctrl = pmpf_rd(reinterpret_cast<const std::uint8_t*>(a), 8);
            if (ctrl) {
                __builtin_prefetch(reinterpret_cast<const void*>(ctrl), 1);
                ++n;
            }
        }
    };
    auto strings = [&](std::size_t at) {
        if (!pmpf_vec(c + at, 32, b, e)) return;
        for (std::uint64_t a = b; a < e; a += 32) {
            pmpf_string(reinterpret_cast<const std::uint8_t*>(a), n);
        }
    };
    auto blist = [&](std::size_t at) {
        shared(at);
        strings(at + 0xE0);
        strings(at + 0xF8);
    };
    for (auto at : pmpf_alists) shared(at);
    for (std::size_t k = 0; k < pmpf_blist_count; ++k) blist(pmpf_blist_first + k * pmpf_blist_size);
    for (auto at : pmpf_blist_extra) blist(at);
}

extern "C" void pmpf_hook_c(void* province, void* const* slot, void* const* end) {
    std::uint64_t n = 0;
    if (slot && end > slot && end - slot < (1 << 20)) {
        const std::size_t left = static_cast<std::size_t>(end - slot);
        if (left > 3 && slot[3]) {
            const auto* p = static_cast<const std::uint8_t*>(slot[3]);
            for (std::size_t a = 0; a < pmpf_province_bytes; a += 64, ++n) __builtin_prefetch(p + a, 1);
        }
        if (left > 2 && slot[2]) pmpf_buffers(static_cast<const std::uint8_t*>(slot[2]) + pmpf_cache, n);
        if (left > 1 && slot[1]) pmpf_inner(static_cast<const std::uint8_t*>(slot[1]) + pmpf_cache, n);
    }
    pmpf_calls.fetch_add(1, std::memory_order_relaxed);
    pmpf_lines.fetch_add(n, std::memory_order_relaxed);
    original_pm_monthly(province);
}

// rcx = province; rdx = rbx (current slot), r8 = rsi (end)
extern "C" void pmpf_thunk();
asm(".text\n.globl pmpf_thunk\npmpf_thunk:\n    movq %rbx, %rdx\n    movq %rsi, %r8\n    jmp pmpf_hook_c\n");


// ---- merc: mercenary company availability precomputed for every country (merc=off|shadow|on)
// Country_MonthlyUpdate 1402f36c0 calls 1404109f0(country) (call 0x2F63DD). It checks the
// country's companies and then every company template (list at 140902860()+0x20, plus the
// random-new-world list gamestate+0x2588 when gamestate+0x2260) with 1408fcc00(template,
// tag, 0): builds a scope on its stack and ANDs the template's triggers (~28 us per country,
// ~4.5% of the monthly tick). At the first 1404109f0 call of a game date all countries are
// evaluated on the pool; each country's own call then answers from that table (calls
// 0x410B0D and 0x4112E2). The answer was computed before earlier countries of the loop ran,
// so it is an assumption: shadow compares it with the game's own evaluation.
constexpr std::uintptr_t merc_update_call_rva = 0x2F63DD;
constexpr std::uint8_t merc_update_site[] = {0x49, 0x8b, 0xcc, 0xe8};  // mov rcx,r12; call
constexpr std::uintptr_t merc_update_rva = 0x4109F0;
constexpr std::uintptr_t merc_check_call_rva[2] = {0x410B0D, 0x4112E2};
constexpr std::uintptr_t merc_check_rva = 0x8FCC00;
constexpr std::uintptr_t merc_db_rva = 0x902860;
constexpr int merc_max_templates = 1024;
constexpr int merc_hash_size = 4096;
using MercUpdateFn = void (*)(void*);
using MercCheckFn = std::uint64_t (*)(void*, std::uint64_t, void*);
using MercDbFn = std::uint8_t* (*)();
Mode merc_mode = Mode::off;
MercUpdateFn original_merc_update = nullptr;
MercCheckFn original_merc_check = nullptr;
MercDbFn merc_db = nullptr;
void* merc_templates[merc_max_templates];
int merc_template_count = 0;
const void* merc_keys[merc_hash_size];
short merc_values[merc_hash_size];
std::uint8_t merc_answer[max_countries][merc_max_templates];  // 0 none, 1 false, 2 true
std::atomic<std::uint32_t> merc_country_gen[max_countries];
std::uint32_t merc_generation = 0;
int merc_date = -0x7fffffff;
std::atomic<int> merc_next{1 << 30};
std::uint64_t merc_current_tag = 0;
bool merc_inside = false;
std::atomic<std::uint64_t> merc_batches{0}, merc_evals{0}, merc_compared{0}, merc_mismatches{0},
    merc_missing{0}, merc_answered{0}, merc_batch_qpc{0}, merc_update_qpc{0}, merc_updates{0};

inline unsigned merc_hash(const void* p) noexcept {
    return static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B1u) &
        (merc_hash_size - 1);
}
int merc_index(const void* t) noexcept {
    for (unsigned h = merc_hash(t);; h = (h + 1) & (merc_hash_size - 1)) {
        if (merc_keys[h] == t) return merc_values[h];
        if (!merc_keys[h]) return -1;
    }
}
bool merc_add_template(void* t) noexcept {
    if (!t) return true;
    for (unsigned h = merc_hash(t);; h = (h + 1) & (merc_hash_size - 1)) {
        if (merc_keys[h] == t) return true;
        if (!merc_keys[h]) {
            if (merc_template_count >= merc_max_templates) return false;
            merc_keys[h] = t;
            merc_values[h] = static_cast<short>(merc_template_count);
            merc_templates[merc_template_count++] = t;
            return true;
        }
    }
}
bool merc_add_list(const std::uint8_t* list) noexcept {
    auto* const* items = *reinterpret_cast<void* const* const*>(list + 8);
    const int count = *reinterpret_cast<const int*>(list + 0x14);
    if (count < 0 || count > merc_max_templates || (count && !items)) return false;
    for (int i = 0; i < count; ++i) {
        if (!merc_add_template(items[i])) return false;
    }
    return true;
}

void merc_compute(int index) noexcept {
    const auto* country = static_cast<const std::uint8_t*>(country_array[index]);
    if (!country) return;
    if (*reinterpret_cast<const int*>(country + 0x1344) == 0 && country[0x26] != 7) return;
    const std::uint64_t tag = *reinterpret_cast<const std::uint64_t*>(country + 0x20);
    const int id = country_id(tag);
    if (id < 0 || id >= max_countries) return;
    std::uint8_t* row = merc_answer[id];
    for (int t = 0; t < merc_template_count; ++t) {
        row[t] = static_cast<std::uint8_t>(original_merc_check(merc_templates[t], tag, nullptr)) != 0 ? 2 : 1;
    }
    merc_evals.fetch_add(static_cast<std::uint64_t>(merc_template_count), std::memory_order_relaxed);
    merc_country_gen[id].store(merc_generation, std::memory_order_release);
}

void run_merc_work() noexcept {
    for (;;) {
        const int index = merc_next.fetch_add(1, std::memory_order_relaxed);
        if (index >= country_total) return;
        merc_compute(index);
    }
}

bool merc_batch(const std::uint8_t* gs) noexcept {
    std::memset(merc_keys, 0, sizeof(merc_keys));
    merc_template_count = 0;
    const std::uint8_t* db = merc_db();
    if (!db || !merc_add_list(db + 0x20)) return false;
    if (*reinterpret_cast<const std::uint64_t*>(gs + 0x2260) != 0 && !merc_add_list(gs + 0x2588)) return false;
    country_array = *reinterpret_cast<void** const*>(gs + 0x1d08);
    country_total = *reinterpret_cast<const int*>(gs + 0x1d14);
    if (!country_array || country_total <= 0 || country_total > max_countries) return false;
    if (++merc_generation == 0) ++merc_generation;
    job_kind.store(4, std::memory_order_release);
    merc_next.store(0);
    job_gen.fetch_add(1, std::memory_order_release);
    const int asleep = sleeping.load();
    if (asleep > 0) ReleaseSemaphore(job_semaphore, asleep < worker_count ? asleep : worker_count, nullptr);
    run_merc_work();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
    merc_next.store(1 << 30);
    merc_batches.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void merc_update_hook(void* country) {
    if (merc_mode == Mode::off || main_thread == 0 || GetCurrentThreadId() != main_thread || merc_inside) {
        original_merc_update(country);
        return;
    }
    const auto start = qpc_now();
    const auto* gs = *reinterpret_cast<const std::uint8_t* const*>(image + eu4build::rva(game_flags_rva));
    if (gs) {
        const int date = *reinterpret_cast<const int*>(gs + 0x1dd0);
        if (date != merc_date) {
            merc_date = date;
            const auto b0 = qpc_now();
            if (!merc_batch(gs)) merc_template_count = 0;
            merc_batch_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - b0), std::memory_order_relaxed);
        }
    }
    merc_current_tag = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(country) + 0x20);
    merc_inside = true;
    original_merc_update(country);
    merc_inside = false;
    const int id = country_id(merc_current_tag);
    if (id >= 0 && id < max_countries) merc_country_gen[id].store(0, std::memory_order_relaxed);  // used once
    merc_updates.fetch_add(1, std::memory_order_relaxed);
    merc_update_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
}

std::uint64_t merc_check_hook(void* templ, std::uint64_t tag, void* text) {
    if (!merc_inside || text || tag != merc_current_tag || GetCurrentThreadId() != main_thread) {
        return original_merc_check(templ, tag, text);
    }
    const int id = country_id(tag);
    const int t = merc_index(templ);
    if (id < 0 || id >= max_countries || t < 0 ||
        merc_country_gen[id].load(std::memory_order_acquire) != merc_generation || merc_answer[id][t] == 0) {
        merc_missing.fetch_add(1, std::memory_order_relaxed);
        return original_merc_check(templ, tag, text);
    }
    const bool cached = merc_answer[id][t] == 2;
    if (merc_mode == Mode::shadow) {
        const std::uint64_t game = original_merc_check(templ, tag, text);
        merc_compared.fetch_add(1, std::memory_order_relaxed);
        if ((static_cast<std::uint8_t>(game) != 0) != cached) {
            const auto m = merc_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (m < 20) log("merc MISMATCH country=%d template=%d cached=%d game=%d", id, t, cached ? 1 : 0,
                static_cast<std::uint8_t>(game) != 0 ? 1 : 0);
        }
        return game;
    }
    merc_answered.fetch_add(1, std::memory_order_relaxed);
    return cached ? 1 : 0;
}

// ---- hist: province-history scans precomputed for every country (hist=off|shadow|on)
// Country_MonthlyUpdate calls 140302ee0(country, 0) (call 0x2F7943) and 140303b40(country)
// (call 0x2F7959). The first walks the country's province list +0x1378 and, for provinces
// owned by someone else, calls 1409fa230(province, &out, province+0x30, tag, &since, &best)
// (call 0x30309B): a recursive walk of the province history tree for the latest
// owner/controller entry of another tag. The second walks list +0x1390 and calls
// 1409fa090(province, province+0x30, tag) (call 0x303C51): the history node for the tag.
// Both only read the history tree, the province owner and the tag (no current date).
// At the first of these calls on a game date every country's lists are evaluated on the
// pool; the country's own pass answers from that table when the arguments match.
// History appended by earlier countries of the same loop would be missed: shadow compares.
constexpr std::uintptr_t hist_a_call_rva = 0x2F7943;  // call 140302ee0, after mov rcx,r12
constexpr std::uintptr_t hist_b_call_rva = 0x2F7959;  // call 140303b40, after mov rcx,r12
constexpr std::uintptr_t hist_a_rva = 0x302EE0;
constexpr std::uintptr_t hist_b_rva = 0x303B40;
constexpr std::uintptr_t hist_walk_call_rva = 0x30309B;  // call 1409fa230
constexpr std::uintptr_t hist_find_call_rva = 0x303C51;  // call 1409fa090
constexpr std::uintptr_t hist_walk_rva = 0x9FA230;
constexpr std::uintptr_t hist_find_rva = 0x9FA090;
constexpr std::uintptr_t hist_year_rva = 0x0CFD70;
constexpr std::uint8_t hist_site[] = {0x49, 0x8b, 0xcc, 0xe8};
constexpr int hist_none = 0x29c77f8;
constexpr int hist_slab = 1 << 19;
using HistAFn = std::uint64_t (*)(void*, std::uint64_t);
using HistBFn = std::uint64_t (*)(void*);
using HistWalkFn = int* (*)(void*, int*, void*, std::uint64_t, int*, int*);
using HistFindFn = void* (*)(void*, void*, std::uint64_t);
using HistYearFn = int (*)(const int*);
Mode hist_mode = Mode::off;
HistAFn original_hist_a = nullptr;
HistBFn original_hist_b = nullptr;
HistWalkFn original_hist_walk = nullptr;
HistFindFn original_hist_find = nullptr;
HistYearFn hist_year = nullptr;
struct HistA { const void* province; int since; int value; };  // value valid if since != 1
struct HistB { const void* province; void* node; };
HistA* hist_a_slab = nullptr;
HistB* hist_b_slab = nullptr;
std::atomic<int> hist_a_used{0}, hist_b_used{0};
struct HistCountry { int a_start, a_count, b_start, b_count; std::atomic<std::uint32_t> gen; };
HistCountry hist_country[max_countries];
int hist_cursor[max_countries];  // main thread: next expected entry (a or b pass)
std::uint32_t hist_generation = 0;
int hist_date = -0x7fffffff;
std::atomic<int> hist_next{1 << 30};
std::uint64_t hist_tag = 0;
int hist_pass = 0;  // 1 = inside 140302ee0, 2 = inside 140303b40
std::atomic<std::uint64_t> hist_batches{0}, hist_walks{0}, hist_finds{0}, hist_compared{0},
    hist_mismatches{0}, hist_missing{0}, hist_answered{0}, hist_batch_qpc{0}, hist_overflow{0};

inline const std::uint8_t* hist_province_base(const std::uint8_t* gs) noexcept {
    return *reinterpret_cast<const std::uint8_t* const*>(gs + 0x1ca8);
}

int hist_since(const std::uint8_t* province, short tag) noexcept {
    auto* const* begin = *reinterpret_cast<void* const* const*>(province + 0x88);
    auto* const* p = *reinterpret_cast<void* const* const*>(province + 0x90);
    while (p != begin) {
        --p;
        const auto* e = static_cast<const std::uint8_t*>(*p);
        if (static_cast<short>(*reinterpret_cast<const std::uint64_t*>(e + 0x58) >> 32) == tag) {
            int d = *reinterpret_cast<const int*>(e + 0x10);
            if (hist_year(&d) == 1) d += 0x2238;
            return d;
        }
    }
    return hist_none;
}

void hist_compute(int index, const std::uint8_t* gs) noexcept {
    const auto* country = static_cast<const std::uint8_t*>(country_array[index]);
    if (!country) return;
    const std::uint64_t tag = *reinterpret_cast<const std::uint64_t*>(country + 0x20);
    const int id = country_id(tag);
    if (id < 0 || id >= max_countries) return;
    const short t = static_cast<short>(tag >> 32);
    const auto* base = hist_province_base(gs);
    HistCountry& hc = hist_country[id];
    hc.a_count = hc.b_count = 0;
    int n = 0;
    for (auto* node = *reinterpret_cast<const int* const*>(country + 0x1378); node;
         node = *reinterpret_cast<const int* const*>(node + 4)) ++n;
    int start = hist_a_used.fetch_add(n, std::memory_order_relaxed);
    if (start + n <= hist_slab) {
        int k = 0;
        for (auto* node = *reinterpret_cast<const int* const*>(country + 0x1378); node;
             node = *reinterpret_cast<const int* const*>(node + 4), ++k) {
            auto* province = const_cast<std::uint8_t*>(base + static_cast<std::int64_t>(*node) * 0x2e10);
            HistA& e = hist_a_slab[start + k];
            e.province = province;
            e.since = 1;
            if (static_cast<short>(*reinterpret_cast<const std::uint64_t*>(province + 0x468) >> 32) == t) continue;
            int since = hist_since(province, t);
            int best = hist_none, out = 0;
            original_hist_walk(province, &out, province + 0x30, tag, &since, &best);
            e.since = since;
            e.value = out;
            hist_walks.fetch_add(1, std::memory_order_relaxed);
        }
        hc.a_start = start;
        hc.a_count = n;
    } else {
        hist_overflow.fetch_add(1, std::memory_order_relaxed);
    }
    n = 0;
    for (auto* node = *reinterpret_cast<const int* const*>(country + 0x1390); node;
         node = *reinterpret_cast<const int* const*>(node + 4)) ++n;
    start = hist_b_used.fetch_add(n, std::memory_order_relaxed);
    if (start + n <= hist_slab) {
        int k = 0;
        for (auto* node = *reinterpret_cast<const int* const*>(country + 0x1390); node;
             node = *reinterpret_cast<const int* const*>(node + 4), ++k) {
            auto* province = const_cast<std::uint8_t*>(base + static_cast<std::int64_t>(*node) * 0x2e10);
            HistB& e = hist_b_slab[start + k];
            e.province = province;
            e.node = original_hist_find(province, province + 0x30, tag);
            hist_finds.fetch_add(1, std::memory_order_relaxed);
        }
        hc.b_start = start;
        hc.b_count = n;
    } else {
        hist_overflow.fetch_add(1, std::memory_order_relaxed);
    }
    hc.gen.store(hist_generation, std::memory_order_release);
}

const std::uint8_t* hist_gs = nullptr;
void run_hist_work() noexcept {
    for (;;) {
        const int index = hist_next.fetch_add(1, std::memory_order_relaxed);
        if (index >= country_total) return;
        hist_compute(index, hist_gs);
    }
}

void hist_batch(const std::uint8_t* gs) noexcept {
    country_array = *reinterpret_cast<void** const*>(gs + 0x1d08);
    country_total = *reinterpret_cast<const int*>(gs + 0x1d14);
    if (!country_array || country_total <= 0 || country_total > max_countries || !hist_province_base(gs)) return;
    if (++hist_generation == 0) ++hist_generation;
    hist_gs = gs;
    hist_a_used.store(0);
    hist_b_used.store(0);
    job_kind.store(5, std::memory_order_release);
    hist_next.store(0);
    job_gen.fetch_add(1, std::memory_order_release);
    const int asleep = sleeping.load();
    if (asleep > 0) ReleaseSemaphore(job_semaphore, asleep < worker_count ? asleep : worker_count, nullptr);
    run_hist_work();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
    hist_next.store(1 << 30);
    hist_batches.fetch_add(1, std::memory_order_relaxed);
}

void hist_enter(void* country, int pass) noexcept {
    const auto* gs = *reinterpret_cast<const std::uint8_t* const*>(image + eu4build::rva(game_flags_rva));
    if (!gs) return;
    const int date = *reinterpret_cast<const int*>(gs + 0x1dd0);
    if (date != hist_date) {
        hist_date = date;
        const auto t0 = qpc_now();
        hist_batch(gs);
        hist_batch_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t0), std::memory_order_relaxed);
    }
    hist_tag = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(country) + 0x20);
    const int id = country_id(hist_tag);
    if (id >= 0 && id < max_countries) hist_cursor[id] = 0;
    hist_pass = pass;
}

inline bool hist_usable() noexcept {
    return hist_mode != Mode::off && main_thread != 0 && GetCurrentThreadId() == main_thread && hist_pass == 0;
}

std::uint64_t hist_a_hook(void* country, std::uint64_t flag) {
    if (!hist_usable()) return original_hist_a(country, flag);
    hist_enter(country, 1);
    const std::uint64_t r = original_hist_a(country, flag);
    hist_pass = 0;
    return r;
}

std::uint64_t hist_b_hook(void* country) {
    if (!hist_usable()) return original_hist_b(country);
    hist_enter(country, 2);
    const std::uint64_t r = original_hist_b(country);
    hist_pass = 0;
    return r;
}

// entry of the current country for this province (cursor first, then a scan)
template <class E> E* hist_lookup(E* slab, int start, int count, int id, const void* province) noexcept {
    int& cur = hist_cursor[id];
    if (cur < count && slab[start + cur].province == province) return &slab[start + cur++];
    for (int k = 0; k < count; ++k) {
        if (slab[start + k].province == province) {
            cur = k + 1;
            return &slab[start + k];
        }
    }
    return nullptr;
}

int* hist_walk_hook(void* province, int* out, void* root, std::uint64_t tag, int* since, int* best) {
    const int id = country_id(tag);
    if (hist_pass != 1 || tag != hist_tag || id < 0 || id >= max_countries || GetCurrentThreadId() != main_thread ||
        root != static_cast<std::uint8_t*>(province) + 0x30 || !since || !best || *best != hist_none ||
        hist_country[id].gen.load(std::memory_order_acquire) != hist_generation) {
        return original_hist_walk(province, out, root, tag, since, best);
    }
    const HistCountry& hc = hist_country[id];
    const HistA* e = hist_lookup(hist_a_slab, hc.a_start, hc.a_count, id, province);
    if (!e || e->since == 1 || e->since != *since) {
        hist_missing.fetch_add(1, std::memory_order_relaxed);
        return original_hist_walk(province, out, root, tag, since, best);
    }
    if (hist_mode == Mode::shadow) {
        int* r = original_hist_walk(province, out, root, tag, since, best);
        hist_compared.fetch_add(1, std::memory_order_relaxed);
        if (*r != e->value) {
            const auto m = hist_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (m < 20) log("hist walk MISMATCH country=%d cached=%d game=%d", id, e->value, *r);
        }
        return r;
    }
    hist_answered.fetch_add(1, std::memory_order_relaxed);
    *best = e->value;
    *out = e->value;
    return out;
}

void* hist_find_hook(void* province, void* root, std::uint64_t tag) {
    const int id = country_id(tag);
    if (hist_pass != 2 || tag != hist_tag || id < 0 || id >= max_countries || GetCurrentThreadId() != main_thread ||
        root != static_cast<std::uint8_t*>(province) + 0x30 ||
        hist_country[id].gen.load(std::memory_order_acquire) != hist_generation) {
        return original_hist_find(province, root, tag);
    }
    const HistCountry& hc = hist_country[id];
    const HistB* e = hist_lookup(hist_b_slab, hc.b_start, hc.b_count, id, province);
    if (!e) {
        hist_missing.fetch_add(1, std::memory_order_relaxed);
        return original_hist_find(province, root, tag);
    }
    if (hist_mode == Mode::shadow) {
        void* r = original_hist_find(province, root, tag);
        hist_compared.fetch_add(1, std::memory_order_relaxed);
        if (r != e->node) {
            const auto m = hist_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (m < 20) log("hist find MISMATCH country=%d cached=%p game=%p", id, e->node, r);
        }
        return r;
    }
    hist_answered.fetch_add(1, std::memory_order_relaxed);
    return e->node;
}

// ---- maint: unit maintenance sums for the monthly AI budget (maint=off|shadow|on)
// Country_MonthlyUpdate calls the AI strategy 14018f040(country+0x3d8) (call 0x2F7C38).
// Its budget estimates call 14034a7f0 -> 14034af20(country, int out[5]) (call 0x34A88C):
// a sum of per-unit maintenance over the country's units (pure read). At the first AI
// strategy call of a game date the sums for all countries are computed on the pool; inside
// the country's own strategy call the hook answers from that table. Shadow compares.
constexpr std::uintptr_t maint_strategy_call_rva = 0x2F7C38;
constexpr std::uint8_t maint_strategy_site[] = {0x49, 0x8d, 0x8c, 0x24, 0xd8, 0x03, 0x00, 0x00, 0xe8};
constexpr std::uintptr_t maint_strategy_rva = 0x18F040;
constexpr std::uintptr_t maint_sum_call_rva = 0x34A88C;
constexpr std::uint8_t maint_sum_site[] = {0x48, 0x8d, 0x54, 0x24, 0x20, 0xe8};
constexpr std::uintptr_t maint_sum_rva = 0x34AF20;
using MaintStrategyFn = void (*)(void*);
using MaintSumFn = int* (*)(void*, int*);
Mode maint_mode = Mode::off;
MaintStrategyFn original_maint_strategy = nullptr;
MaintSumFn original_maint_sum = nullptr;
struct MaintEntry { int v[5]; std::atomic<std::uint32_t> gen; };
MaintEntry maint_table[max_countries];
std::uint32_t maint_generation = 0;
int maint_date = -0x7fffffff;
std::atomic<int> maint_next{1 << 30};
std::uint64_t maint_tag = 0;
bool maint_inside = false;
std::atomic<std::uint64_t> maint_batches{0}, maint_computed{0}, maint_compared{0}, maint_mismatches{0},
    maint_missing{0}, maint_answered{0}, maint_batch_qpc{0};

void maint_compute(int index) noexcept {
    auto* country = static_cast<std::uint8_t*>(country_array[index]);
    if (!country || *reinterpret_cast<const int*>(country + 0x1344) == 0) return;
    const int id = country_id(*reinterpret_cast<const std::uint64_t*>(country + 0x20));
    if (id < 0 || id >= max_countries) return;
    MaintEntry& e = maint_table[id];
    int out[5] = {};
    original_maint_sum(country, out);
    std::memcpy(e.v, out, sizeof(out));
    maint_computed.fetch_add(1, std::memory_order_relaxed);
    e.gen.store(maint_generation, std::memory_order_release);
}

void run_maint_work() noexcept {
    for (;;) {
        const int index = maint_next.fetch_add(1, std::memory_order_relaxed);
        if (index >= country_total) return;
        maint_compute(index);
    }
}

void maint_batch(const std::uint8_t* gs) noexcept {
    country_array = *reinterpret_cast<void** const*>(gs + 0x1d08);
    country_total = *reinterpret_cast<const int*>(gs + 0x1d14);
    if (!country_array || country_total <= 0 || country_total > max_countries) return;
    if (++maint_generation == 0) ++maint_generation;
    job_kind.store(6, std::memory_order_release);
    maint_next.store(0);
    job_gen.fetch_add(1, std::memory_order_release);
    const int asleep = sleeping.load();
    if (asleep > 0) ReleaseSemaphore(job_semaphore, asleep < worker_count ? asleep : worker_count, nullptr);
    run_maint_work();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
    maint_next.store(1 << 30);
    maint_batches.fetch_add(1, std::memory_order_relaxed);
}

void maint_strategy_hook(void* ai) {
    if (maint_mode == Mode::off || main_thread == 0 || GetCurrentThreadId() != main_thread || maint_inside) {
        original_maint_strategy(ai);
        return;
    }
    const auto* gs = *reinterpret_cast<const std::uint8_t* const*>(image + eu4build::rva(game_flags_rva));
    if (gs) {
        const int date = *reinterpret_cast<const int*>(gs + 0x1dd0);
        if (date != maint_date) {
            maint_date = date;
            const auto t0 = qpc_now();
            maint_batch(gs);
            maint_batch_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t0), std::memory_order_relaxed);
        }
    }
    maint_tag = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(ai) - 0x3d8 + 0x20);
    maint_inside = true;
    original_maint_strategy(ai);
    maint_inside = false;
    const int id = country_id(maint_tag);
    if (id >= 0 && id < max_countries) maint_table[id].gen.store(0, std::memory_order_relaxed);
}

int* maint_sum_hook(void* country, int* out) {
    const std::uint64_t tag = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(country) + 0x20);
    const int id = country_id(tag);
    if (!maint_inside || tag != maint_tag || id < 0 || id >= max_countries || GetCurrentThreadId() != main_thread) {
        return original_maint_sum(country, out);
    }
    const MaintEntry& e = maint_table[id];
    if (e.gen.load(std::memory_order_acquire) != maint_generation) {
        maint_missing.fetch_add(1, std::memory_order_relaxed);
        return original_maint_sum(country, out);
    }
    if (maint_mode == Mode::shadow) {
        int* r = original_maint_sum(country, out);
        maint_compared.fetch_add(1, std::memory_order_relaxed);
        if (std::memcmp(r, e.v, sizeof(e.v)) != 0) {
            const auto m = maint_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (m < 20) log("maint MISMATCH country=%d cached=%d,%d,%d,%d,%d game=%d,%d,%d,%d,%d", id, e.v[0], e.v[1],
                e.v[2], e.v[3], e.v[4], r[0], r[1], r[2], r[3], r[4]);
        }
        return r;
    }
    maint_answered.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(out, e.v, sizeof(e.v));
    return out;
}

// ---- relguard: crash guard for a game bug in relation removal (relguard=on|off, default on)
// 140a5e130(relation) removes a diplomatic relation object and dereferences it at once.
// 140a61900 (relation.cpp) calls it with nullptr when the inner lookup finds nothing
// (call 0xA619E7: xor ecx,ecx; call), and the game crashes with an access violation at
// 0xA5E14F. Seen in Anbennar 1490 (error.log "!pRelation CDiplomacy::RemActive" right before).
// The guard redirects every call of 140a5e130 to a wrapper that skips a null argument.
// With a non-null argument it calls the original unchanged.
constexpr std::uintptr_t relguard_target_rva = 0xA5E130;
constexpr std::uintptr_t relguard_calls[] = {0x2A27DB, 0x4E50AA, 0x4E6622, 0x501C24, 0x50DA39,
    0x53F230, 0x54014C, 0x54175E, 0xA5F729, 0xA619E7, 0xA61AED};
using RelRemoveFn = void (*)(void*);
bool relguard_on = true;
RelRemoveFn original_rel_remove = nullptr;
std::atomic<std::uint64_t> relguard_calls_seen{0}, relguard_skipped{0};

void relguard_hook(void* relation) {
    relguard_calls_seen.fetch_add(1, std::memory_order_relaxed);
    if (!relation) {
        const auto n = relguard_skipped.fetch_add(1, std::memory_order_relaxed);
        if (n < 20) log("relguard: skipped null relation removal (game bug path), count=%llu",
            static_cast<unsigned long long>(n + 1));
        return;
    }
    original_rel_remove(relation);
}

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        const auto f = static_cast<std::uint64_t>(qpc_frequency);
        log("stats mode=%s phases=%llu computed=%llu compared=%llu mismatches=%llu "
            "missing=%llu answered=%llu precompute_ms=%llu loop_ms=%llu workers=%d",
            mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "off",
            static_cast<unsigned long long>(phases.load()),
            static_cast<unsigned long long>(computed.load()),
            static_cast<unsigned long long>(compared.load()),
            static_cast<unsigned long long>(mismatches.load()),
            static_cast<unsigned long long>(missing.load()),
            static_cast<unsigned long long>(answered.load()),
            static_cast<unsigned long long>(precompute_qpc.load() * 1000 / f),
            static_cast<unsigned long long>(phase_qpc.load() * 1000 / f), worker_count);
        log("strength compared=%llu mismatches=%llu missing=%llu answered=%llu",
            static_cast<unsigned long long>(s_compared.load()),
            static_cast<unsigned long long>(s_mismatches.load()),
            static_cast<unsigned long long>(s_missing.load()),
            static_cast<unsigned long long>(s_answered.load()));
        if (reform_mode != Mode::off) {
            log("reforms mode=%s computed=%llu compared=%llu mismatches=%llu missing=%llu "
                "answered=%llu", reform_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(r_computed.load()),
                static_cast<unsigned long long>(r_compared.load()),
                static_cast<unsigned long long>(r_mismatches.load()),
                static_cast<unsigned long long>(r_missing.load()),
                static_cast<unsigned long long>(r_answered.load()));
        }
        if (pmpf_mode != Mode::off) {
            log("pmpf calls=%llu prefetched_lines=%llu",
                static_cast<unsigned long long>(pmpf_calls.load()),
                static_cast<unsigned long long>(pmpf_lines.load()));
        }
        if (relguard_on) {
            log("relguard calls=%llu skipped=%llu", static_cast<unsigned long long>(relguard_calls_seen.load()),
                static_cast<unsigned long long>(relguard_skipped.load()));
        }
        if (maint_mode != Mode::off) {
            log("maint mode=%s batches=%llu computed=%llu compared=%llu mismatches=%llu missing=%llu "
                "answered=%llu batch_ms=%llu", maint_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(maint_batches.load()),
                static_cast<unsigned long long>(maint_computed.load()),
                static_cast<unsigned long long>(maint_compared.load()),
                static_cast<unsigned long long>(maint_mismatches.load()),
                static_cast<unsigned long long>(maint_missing.load()),
                static_cast<unsigned long long>(maint_answered.load()),
                static_cast<unsigned long long>(maint_batch_qpc.load() * 1000 / f));
        }
        if (hist_mode != Mode::off) {
            log("hist mode=%s batches=%llu walks=%llu finds=%llu compared=%llu mismatches=%llu "
                "missing=%llu answered=%llu overflow=%llu batch_ms=%llu",
                hist_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(hist_batches.load()),
                static_cast<unsigned long long>(hist_walks.load()),
                static_cast<unsigned long long>(hist_finds.load()),
                static_cast<unsigned long long>(hist_compared.load()),
                static_cast<unsigned long long>(hist_mismatches.load()),
                static_cast<unsigned long long>(hist_missing.load()),
                static_cast<unsigned long long>(hist_answered.load()),
                static_cast<unsigned long long>(hist_overflow.load()),
                static_cast<unsigned long long>(hist_batch_qpc.load() * 1000 / f));
        }
        if (merc_mode != Mode::off) {
            log("merc mode=%s batches=%llu updates=%llu evals=%llu compared=%llu mismatches=%llu "
                "missing=%llu answered=%llu batch_ms=%llu update_ms=%llu templates=%d",
                merc_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(merc_batches.load()),
                static_cast<unsigned long long>(merc_updates.load()),
                static_cast<unsigned long long>(merc_evals.load()),
                static_cast<unsigned long long>(merc_compared.load()),
                static_cast<unsigned long long>(merc_mismatches.load()),
                static_cast<unsigned long long>(merc_missing.load()),
                static_cast<unsigned long long>(merc_answered.load()),
                static_cast<unsigned long long>(merc_batch_qpc.load() * 1000 / f),
                static_cast<unsigned long long>(merc_update_qpc.load() * 1000 / f), merc_template_count);
        }
        if (relpf_mode != Mode::off) {
            log("relpf calls=%llu prefetched=%llu",
                static_cast<unsigned long long>(relpf_calls.load()),
                static_cast<unsigned long long>(relpf_prefetches.load()));
        }
        if (ps_on) {
            log("pathstat days=%llu calls=%llu repeats=%llu other_threads=%llu check_ms=%llu",
                static_cast<unsigned long long>(ps_days), static_cast<unsigned long long>(ps_calls),
                static_cast<unsigned long long>(ps_repeats), static_cast<unsigned long long>(ps_other),
                static_cast<unsigned long long>(ps_qpc * 1000 / f));
        }
        if (pm_mode != Mode::off) {
            log("provmod mode=%s months=%llu built=%llu used=%llu compared=%llu mismatches=%llu "
                "fallback=%llu precompute_ms=%llu original_ms=%llu assign_ms=%llu",
                pm_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(pm_months.load()),
                static_cast<unsigned long long>(pm_built.load()),
                static_cast<unsigned long long>(pm_used.load()),
                static_cast<unsigned long long>(pm_compared.load()),
                static_cast<unsigned long long>(pm_mismatches.load()),
                static_cast<unsigned long long>(pm_fallback.load()),
                static_cast<unsigned long long>(pm_precompute_qpc.load() * 1000 / f),
                static_cast<unsigned long long>(pm_original_qpc.load() * 1000 / f),
                static_cast<unsigned long long>(pm_assign_qpc.load() * 1000 / f));
        }
        if (cb_mode != Mode::off) {
            log("cb mode=%s calls=%llu parallel=%llu targets=%llu evals=%llu compared=%llu "
                "mismatches=%llu fallback=%llu cb_ms=%llu prepare_ms=%llu original_ms=%llu",
                cb_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(cb_calls.load()),
                static_cast<unsigned long long>(cb_parallel.load()),
                static_cast<unsigned long long>(cb_targets.load()),
                static_cast<unsigned long long>(cb_evals.load()),
                static_cast<unsigned long long>(cb_compared.load()),
                static_cast<unsigned long long>(cb_mismatches.load()),
                static_cast<unsigned long long>(cb_fallback.load()),
                static_cast<unsigned long long>(cb_qpc.load() * 1000 / f),
                static_cast<unsigned long long>(cb_prepare_qpc.load() * 1000 / f),
                static_cast<unsigned long long>(cb_original_qpc.load() * 1000 / f));
        }
        if (colony_mode != Mode::off) {
            log("colony mode=%s phases=%llu jobs=%llu compared=%llu mismatches=%llu missing=%llu "
                "answered=%llu other=%llu precompute_ms=%llu original_ms=%llu",
                colony_mode == Mode::on ? "on" : "shadow",
                static_cast<unsigned long long>(c_phases.load()),
                static_cast<unsigned long long>(c_jobs.load()),
                static_cast<unsigned long long>(c_compared.load()),
                static_cast<unsigned long long>(c_mismatches.load()),
                static_cast<unsigned long long>(c_missing.load()),
                static_cast<unsigned long long>(c_answered.load()),
                static_cast<unsigned long long>(c_other.load()),
                static_cast<unsigned long long>(c_precompute_qpc.load() * 1000 / f),
                static_cast<unsigned long long>(c_original_qpc.load() * 1000 / f));
        }
    }
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_country_patch.log";
    const auto ini = directory + L"\\eu4_country_patch.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"country", L"estates", L"off", value, 32, ini.c_str());
    mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"strength", L"off", value, 32, ini.c_str());
    strength_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"colony", L"off", value, 32, ini.c_str());
    colony_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"reforms", L"off", value, 32, ini.c_str());
    reform_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"cb", L"off", value, 32, ini.c_str());
    cb_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"provmod", L"off", value, 32, ini.c_str());
    pm_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"pmpf", L"off", value, 32, ini.c_str());
    pmpf_mode = _wcsicmp(value, L"on") == 0 ? Mode::on : Mode::off;
    GetPrivateProfileStringW(L"country", L"maint", L"off", value, 32, ini.c_str());
    maint_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"hist", L"off", value, 32, ini.c_str());
    hist_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"merc", L"off", value, 32, ini.c_str());
    merc_mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    GetPrivateProfileStringW(L"country", L"relpf", L"off", value, 32, ini.c_str());
    relpf_mode = _wcsicmp(value, L"on") == 0 ? Mode::on : Mode::off;
    pm_serial = GetPrivateProfileIntW(L"country", L"provmod_serial", 0, ini.c_str()) != 0;
    ps_on = GetPrivateProfileIntW(L"country", L"pathstat", 0, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"country", L"relguard", L"on", value, 32, ini.c_str());
    relguard_on = _wcsicmp(value, L"off") != 0;
    if (mode == Mode::off && strength_mode == Mode::off && colony_mode == Mode::off &&
        reform_mode == Mode::off && cb_mode == Mode::off && pm_mode == Mode::off && !ps_on &&
        relpf_mode == Mode::off && pmpf_mode == Mode::off && merc_mode == Mode::off && hist_mode == Mode::off && maint_mode == Mode::off && !relguard_on) {
        log("disabled by configuration; no bytes changed");
        installation_status.store(3);
        return 0;
    }
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!eu4build::code_matches(image, phase_site_rva, phase_bytes, sizeof(phase_bytes)) ||
        !eu4build::code_matches(image, estate_site_rva, estate_bytes, sizeof(estate_bytes)) ||
        !eu4build::code_matches(image, strength_call_rva[0], strength_site_bytes[0], 5) ||
        !eu4build::code_matches(image, strength_call_rva[1], strength_site_bytes[1], 5) ||
        !eu4build::code_matches(image, strength_call_rva[2], strength_site_bytes[2], 5) ||
        !eu4build::code_matches(image, colony_call_rva, colony_bytes, 5) ||
        !eu4build::code_matches(image, colony_begin_rva, colony_begin_bytes, 5) ||
        !eu4build::code_matches(image, colony_phase_rva, colony_phase_bytes, 5) ||
        !eu4build::code_matches(image, reform_call_rva[0], reform_bytes[0], 5) ||
        !eu4build::code_matches(image, reform_call_rva[1], reform_bytes[1], 5)) {
        log("REFUSED: bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    int workers = static_cast<int>(info.dwNumberOfProcessors) - 4;
    workers = GetPrivateProfileIntW(L"country", L"workers", workers, ini.c_str());
    if (workers < 1) workers = 1;
    if (workers > max_workers) workers = max_workers;
    if (cb_mode != Mode::off) {
        const int spin_us = static_cast<int>(GetPrivateProfileIntW(L"country", L"cb_spin_us", 200, ini.c_str()));
        hot_spin_qpc = qpc_frequency * (spin_us < 0 ? 0 : spin_us) / 1000000;
    }
    job_semaphore = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    if (!job_semaphore) {
        installation_status.store(-6);
        return 6;
    }
    for (int i = 0; i < workers; ++i) {
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
            ++worker_count;
        }
    }

    original_phase = reinterpret_cast<PhaseFn>(image + eu4build::rva(phase_function_rva));
    original_estate = reinterpret_cast<EstateFn>(image + eu4build::rva(estate_function_rva));
    for (int k = 0; k < 3; ++k) {
        original_strength[k] = reinterpret_cast<StrengthFn>(image + eu4build::rva(strength_function_rva[k]));
    }
    char error[160]{};
    original_colony = reinterpret_cast<ColonyFn>(image + eu4build::rva(colony_function_rva));
    original_timer = reinterpret_cast<TimerFn>(image + eu4build::rva(colony_begin_function_rva));
    original_province_phase = reinterpret_cast<PhaseFn>(image + eu4build::rva(colony_phase_function_rva));
    if (colony_mode != Mode::off) {
        speedpatch::CallPatch patches[3];
        const std::uintptr_t sites[3] = {colony_phase_rva, colony_begin_rva, colony_call_rva};
        const void* targets[3] = {reinterpret_cast<const void*>(original_province_phase),
            reinterpret_cast<const void*>(original_timer),
            reinterpret_cast<const void*>(original_colony)};
        const void* hooks[3] = {reinterpret_cast<const void*>(&colony_province_phase),
            reinterpret_cast<const void*>(&colony_begin),
            reinterpret_cast<const void*>(&colony_hook)};
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(sites[k])),
                targets[k], hooks[k], patches[k], error, sizeof(error));
        }
        for (int k = 0; k < 3 && ok; ++k) {
            ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        }
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("colony REFUSED: %s", error);
            colony_mode = Mode::off;
        }
    }
    original_reform[0] = reinterpret_cast<ReformFn>(image + eu4build::rva(reform_function_rva[0]));
    original_reform[1] = reinterpret_cast<ReformFn>(image + eu4build::rva(reform_function_rva[1]));
    if (reform_mode != Mode::off) {
        speedpatch::CallPatch patches[2];
        const void* hooks[2] = {reinterpret_cast<const void*>(&reform_hook<0>),
            reinterpret_cast<const void*>(&reform_hook<1>)};
        bool ok = true;
        for (int k = 0; k < 2 && ok; ++k) {
            ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(reform_call_rva[k])),
                reinterpret_cast<const void*>(original_reform[k]), hooks[k], patches[k], error,
                sizeof(error));
        }
        for (int k = 0; k < 2 && ok; ++k) ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("reforms REFUSED: %s", error);
            reform_mode = Mode::off;
        }
    }
    if (pmpf_mode != Mode::off && pm_mode != Mode::off) {
        log("pmpf REFUSED: provmod uses the same call");
        pmpf_mode = Mode::off;
    }
    if (pmpf_mode != Mode::off) {
        original_pm_monthly = reinterpret_cast<PmFn>(image + eu4build::rva(pm_monthly_rva));
        speedpatch::CallPatch patch;
        bool ok = eu4build::code_matches(image, pm_loop_call_rva - 3, pmpf_site_bytes, sizeof(pmpf_site_bytes));
        if (!ok) std::snprintf(error, sizeof(error), "bytes differ");
        ok = ok && speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(pm_loop_call_rva)),
            reinterpret_cast<const void*>(original_pm_monthly),
            reinterpret_cast<const void*>(&pmpf_thunk), patch, error, sizeof(error));
        ok = ok && speedpatch::commit_call(patch, error, sizeof(error));
        if (!ok) {
            speedpatch::discard_call(patch);
            log("pmpf REFUSED: %s", error);
            pmpf_mode = Mode::off;
        }
    }
    if (relpf_mode != Mode::off) {
        original_rel = reinterpret_cast<RelFn>(image + eu4build::rva(relpf_function_rva));
        bool ok = eu4build::code_matches(image, relpf_bytes_rva[0], relpf_bytes0, sizeof(relpf_bytes0)) &&
            eu4build::code_matches(image, relpf_bytes_rva[1], relpf_bytes1, sizeof(relpf_bytes1)) &&
            eu4build::code_matches(image, relpf_check_rva, relpf_check_bytes, sizeof(relpf_check_bytes));
        if (!ok) std::snprintf(error, sizeof(error), "bytes differ");
        speedpatch::CallPatch patches[2];
        for (int k = 0; k < 2 && ok; ++k) {
            ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(relpf_call_rva[k])),
                reinterpret_cast<const void*>(original_rel),
                reinterpret_cast<const void*>(&relpf_thunk), patches[k], error, sizeof(error));
        }
        for (int k = 0; k < 2 && ok; ++k) ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("relpf REFUSED: %s", error);
            relpf_mode = Mode::off;
        }
    }
    if (merc_mode != Mode::off) {
        original_merc_update = reinterpret_cast<MercUpdateFn>(image + eu4build::rva(merc_update_rva));
        original_merc_check = reinterpret_cast<MercCheckFn>(image + eu4build::rva(merc_check_rva));
        merc_db = reinterpret_cast<MercDbFn>(image + eu4build::rva(merc_db_rva));
        bool ok = eu4build::code_matches(image, merc_update_call_rva - 3, merc_update_site, sizeof(merc_update_site));
        if (!ok) std::snprintf(error, sizeof(error), "bytes differ");
        speedpatch::CallPatch patches[3];
        ok = ok && speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(merc_update_call_rva)),
            reinterpret_cast<const void*>(original_merc_update),
            reinterpret_cast<const void*>(&merc_update_hook), patches[0], error, sizeof(error));
        for (int k = 0; k < 2 && ok; ++k) {
            ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(merc_check_call_rva[k])),
                reinterpret_cast<const void*>(original_merc_check),
                reinterpret_cast<const void*>(&merc_check_hook), patches[k + 1], error, sizeof(error));
        }
        // the check hooks first: they pass through until the update hook runs
        for (int k = 1; k < 3 && ok; ++k) ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        ok = ok && speedpatch::commit_call(patches[0], error, sizeof(error));
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("merc REFUSED: %s", error);
            merc_mode = Mode::off;
        }
    }
    if (hist_mode != Mode::off) {
        original_hist_a = reinterpret_cast<HistAFn>(image + eu4build::rva(hist_a_rva));
        original_hist_b = reinterpret_cast<HistBFn>(image + eu4build::rva(hist_b_rva));
        original_hist_walk = reinterpret_cast<HistWalkFn>(image + eu4build::rva(hist_walk_rva));
        original_hist_find = reinterpret_cast<HistFindFn>(image + eu4build::rva(hist_find_rva));
        hist_year = reinterpret_cast<HistYearFn>(image + eu4build::rva(hist_year_rva));
        hist_a_slab = new (std::nothrow) HistA[hist_slab];
        hist_b_slab = new (std::nothrow) HistB[hist_slab];
        bool ok = hist_a_slab && hist_b_slab &&
            eu4build::code_matches(image, hist_a_call_rva - 3, hist_site, sizeof(hist_site)) &&
            eu4build::code_matches(image, hist_b_call_rva - 3, hist_site, sizeof(hist_site));
        if (!ok) std::snprintf(error, sizeof(error), "bytes differ or no memory");
        speedpatch::CallPatch patches[4];
        const std::uintptr_t sites[4] = {hist_walk_call_rva, hist_find_call_rva, hist_a_call_rva, hist_b_call_rva};
        const void* targets[4] = {reinterpret_cast<const void*>(original_hist_walk),
            reinterpret_cast<const void*>(original_hist_find), reinterpret_cast<const void*>(original_hist_a),
            reinterpret_cast<const void*>(original_hist_b)};
        const void* hooks[4] = {reinterpret_cast<const void*>(&hist_walk_hook),
            reinterpret_cast<const void*>(&hist_find_hook), reinterpret_cast<const void*>(&hist_a_hook),
            reinterpret_cast<const void*>(&hist_b_hook)};
        for (int k = 0; k < 4 && ok; ++k) {
            ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(sites[k])), targets[k], hooks[k],
                patches[k], error, sizeof(error));
        }
        for (int k = 0; k < 4 && ok; ++k) ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("hist REFUSED: %s", error);
            hist_mode = Mode::off;
        }
    }
    if (maint_mode != Mode::off) {
        original_maint_strategy = reinterpret_cast<MaintStrategyFn>(image + eu4build::rva(maint_strategy_rva));
        original_maint_sum = reinterpret_cast<MaintSumFn>(image + eu4build::rva(maint_sum_rva));
        bool ok = eu4build::code_matches(image, maint_strategy_call_rva - 8, maint_strategy_site, sizeof(maint_strategy_site)) &&
            eu4build::code_matches(image, maint_sum_call_rva - 5, maint_sum_site, sizeof(maint_sum_site));
        if (!ok) std::snprintf(error, sizeof(error), "bytes differ");
        speedpatch::CallPatch patches[2];
        ok = ok && speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(maint_sum_call_rva)),
            reinterpret_cast<const void*>(original_maint_sum), reinterpret_cast<const void*>(&maint_sum_hook),
            patches[0], error, sizeof(error));
        ok = ok && speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(maint_strategy_call_rva)),
            reinterpret_cast<const void*>(original_maint_strategy),
            reinterpret_cast<const void*>(&maint_strategy_hook), patches[1], error, sizeof(error));
        for (int k = 0; k < 2 && ok; ++k) ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("maint REFUSED: %s", error);
            maint_mode = Mode::off;
        }
    }
    if (relguard_on) {
        original_rel_remove = reinterpret_cast<RelRemoveFn>(image + eu4build::rva(relguard_target_rva));
        constexpr int n = static_cast<int>(sizeof(relguard_calls) / sizeof(relguard_calls[0]));
        speedpatch::CallPatch patches[n];
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {
            ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(relguard_calls[k])),
                reinterpret_cast<const void*>(original_rel_remove), reinterpret_cast<const void*>(&relguard_hook),
                patches[k], error, sizeof(error));
        }
        for (int k = 0; k < n && ok; ++k) ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("relguard REFUSED: %s", error);
            relguard_on = false;
        }
    }
    if (ps_on) {
        original_path_check = reinterpret_cast<PathCheckFn>(image + eu4build::rva(ps_function_rva));
        ps_table = new (std::nothrow) std::uint64_t[ps_table_size]();
        int patched = 0;
        for (auto rva : ps_slots_rva) {
            if (!eu4build::rva(rva)) continue;
            auto* slot = reinterpret_cast<std::uintptr_t*>(image + eu4build::rva(rva));
            if (*slot != image + eu4build::rva(ps_function_rva)) continue;
            DWORD old = 0, ignored = 0;
            if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) continue;
            *slot = reinterpret_cast<std::uintptr_t>(&path_check_hook);
            VirtualProtect(slot, 8, old, &ignored);
            ++patched;
        }
        log("pathstat slots patched=%d", patched);
    }
    if (pm_mode != Mode::off) {
        original_pm_monthly = reinterpret_cast<PmFn>(image + eu4build::rva(pm_monthly_rva));
        original_pm_calc = reinterpret_cast<PmFn>(image + eu4build::rva(pm_calc_rva));
        pm_ctor = reinterpret_cast<PmCtorFn>(image + eu4build::rva(pm_ctor_rva));
        pm_assign = reinterpret_cast<PmAssignFn>(image + eu4build::rva(pm_assign_rva));
        pm_dtor = reinterpret_cast<PmFn>(image + eu4build::rva(pm_dtor_rva));
        pm_compute = reinterpret_cast<PmFn>(image + eu4build::rva(pm_compute_rva));
        speedpatch::CallPatch loop_patch;
        speedpatch::CallPatch calc_patch;
        bool ok = eu4build::code_matches(image, pm_loop_call_rva, pm_loop_bytes, 5) &&
            eu4build::code_matches(image, pm_calc_call_rva, pm_calc_bytes, 5);
        ok = ok && speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(pm_calc_call_rva)),
            reinterpret_cast<const void*>(original_pm_calc), reinterpret_cast<const void*>(&pm_calc_hook),
            calc_patch, error, sizeof(error));
        ok = ok && speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(pm_loop_call_rva)),
            reinterpret_cast<const void*>(original_pm_monthly), reinterpret_cast<const void*>(&pm_monthly_hook),
            loop_patch, error, sizeof(error));
        ok = ok && speedpatch::commit_call(calc_patch, error, sizeof(error));
        ok = ok && speedpatch::commit_call(loop_patch, error, sizeof(error));
        if (!ok) {
            speedpatch::discard_call(calc_patch);
            speedpatch::discard_call(loop_patch);
            log("provmod REFUSED: %s", error[0] ? error : "bytes differ");
            pm_mode = Mode::off;
        }
    }
    if (cb_mode != Mode::off) {
        original_cb = reinterpret_cast<CbFn>(image + eu4build::rva(cb_function_rva));
        cb_db = reinterpret_cast<DbFn>(image + eu4build::rva(cb_db_rva));
        trigger_and = reinterpret_cast<TriggerFn>(image + eu4build::rva(trigger_and_rva));
        vector_push = reinterpret_cast<PushFn>(image + eu4build::rva(vector_push_rva));
        scope_dtor = reinterpret_cast<ScopeDtorFn>(image + eu4build::rva(scope_dtor_rva));
        speedpatch::CallPatch patch;
        if (!eu4build::code_matches(image, cb_call_rva, cb_bytes, 5) ||
            !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(cb_call_rva)),
                reinterpret_cast<const void*>(original_cb), reinterpret_cast<const void*>(&cb_hook),
                patch, error, sizeof(error)) ||
            !speedpatch::commit_call(patch, error, sizeof(error))) {
            speedpatch::discard_call(patch);
            log("cb REFUSED: %s", error[0] ? error : "bytes differ");
            cb_mode = Mode::off;
        }
    }
    if (strength_mode != Mode::off) {
        const void* hooks[strength_kinds] = {reinterpret_cast<const void*>(&strength_hook<0>),
            reinterpret_cast<const void*>(&strength_hook<1>)};
        speedpatch::CallPatch patches[strength_kinds];
        bool ok = true;
        for (int k = 0; k < strength_kinds && ok; ++k) {
            ok = speedpatch::prepare_call(
                reinterpret_cast<std::uint8_t*>(image + eu4build::rva(strength_call_rva[k])),
                reinterpret_cast<const void*>(original_strength[k]), hooks[k], patches[k],
                error, sizeof(error));
        }
        for (int k = 0; k < strength_kinds && ok; ++k) {
            ok = speedpatch::commit_call(patches[k], error, sizeof(error));
        }
        if (!ok) {
            for (auto& patch : patches) speedpatch::discard_call(patch);
            log("strength REFUSED: %s", error);
            strength_mode = Mode::off;
        }
    }
    speedpatch::CallPatch phase;
    speedpatch::CallPatch estate;
    if ((mode != Mode::off &&
            !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(estate_call_rva)),
                reinterpret_cast<const void*>(original_estate),
                reinterpret_cast<const void*>(&estate_hook), estate, error, sizeof(error))) ||
        !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(phase_call_rva)),
            reinterpret_cast<const void*>(original_phase),
            reinterpret_cast<const void*>(&phase_hook), phase, error, sizeof(error)) ||
        (mode != Mode::off && !speedpatch::commit_call(estate, error, sizeof(error))) ||
        !speedpatch::commit_call(phase, error, sizeof(error))) {
        speedpatch::discard_call(phase);
        speedpatch::discard_call(estate);
        log("REFUSED: %s", error);
        installation_status.store(-5);
        return 5;
    }
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    installation_status.store(mode == Mode::on ? 1 : 2);
    auto name = [](Mode m) { return m == Mode::on ? "on" : m == Mode::shadow ? "shadow" : "off"; };
    log("initialized version=0.6.0-experimental estates=%s strength=%s colony=%s reforms=%s "
        "cb=%s provmod=%s relpf=%s pmpf=%s merc=%s hist=%s maint=%s relguard=%s workers=%d", name(mode), name(strength_mode), name(colony_mode),
        name(reform_mode), name(cb_mode), name(pm_mode), name(relpf_mode), name(pmpf_mode), name(merc_mode), name(hist_mode), name(maint_mode), relguard_on ? "on" : "off", worker_count);
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
        installation_status.store(-7);
        return 7;
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4CountryPatchStatus() {
    return installation_status.load();
}
