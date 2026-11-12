// EU4 Modifier Patch 0.6.0-fast, for EU4 1.37.5.0 Windows x64.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "caller_stats.hpp"
#include "function_patch.hpp"
#include "function_stats.hpp"
#include "measurement.hpp"
#include "profile.hpp"
#include "search.hpp"

namespace {

using MergeFunction = void* (*)(void* destination, void* source);
using UnitParametersFunction = std::uint64_t (*)(void* unit);
using CountryRecalculationFunction = void (*)(void* cache);
using CountryCacheResetFunction = void (*)(void* cache);
using CountryCacheRebuildFunction = void* (*)(void* cache, void* country);
using UnitCombatValueFunction = int (*)(void* unit, void* modifiers, void* explanation);

constexpr wchar_t version[] = L"0.6.0-fast";
std::atomic<LONG> installation_status{0};
bool optimization_enabled = false;
bool measurement_enabled = false;
const wchar_t* mode_name = L"disabled";
MergeFunction original_add = nullptr;
MergeFunction original_subtract = nullptr;
UnitParametersFunction original_unit_parameters = nullptr;
CountryRecalculationFunction original_country_recalculation = nullptr;
CountryCacheResetFunction original_country_cache_reset = nullptr;
CountryCacheRebuildFunction original_country_cache_rebuild = nullptr;
MergeFunction original_unit_modifier_build = nullptr;
UnitCombatValueFunction original_unit_combat_value = nullptr;
MergeFunction original_modifier_copy = nullptr;
MergeFunction original_modifier_scale = nullptr;
std::int64_t qpc_frequency = 1;
std::wstring log_path;
std::mutex log_mutex;

std::atomic<std::uint64_t> total_calls{0};
std::atomic<std::uint64_t> add_calls{0};
std::atomic<std::uint64_t> subtract_calls{0};
std::atomic<std::uint64_t> dynamic_calls{0};
std::atomic<std::uint64_t> alias_calls{0};
std::atomic<std::uint64_t> invalid_measurements{0};
std::atomic<std::uint64_t> destination_entries{0};
std::atomic<std::uint64_t> source_entries{0};
std::atomic<std::uint64_t> dynamic_inputs{0};
std::atomic<std::uint64_t> direct_lookups{0};
std::atomic<std::uint64_t> comparisons{0};
std::atomic<std::uint64_t> appended{0};
std::atomic<std::uint64_t> max_comparisons{0};
std::atomic<std::uint32_t> max_destination{0};
std::atomic<std::uint32_t> max_source{0};
std::atomic<std::uint64_t> add_original_ticks{0};
std::atomic<std::uint64_t> subtract_original_ticks{0};
std::atomic<std::uint64_t> measurement_ticks{0};
std::array<std::atomic<std::uint64_t>, 7> comparison_buckets{};
thread_local modifierpatch::Scratch scratch;

void log(const char* format, ...) noexcept {
    if (log_path.empty()) return;
    char message[1800];
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
void update_max(std::atomic<T>& maximum, T value) noexcept {
    auto current = maximum.load(std::memory_order_relaxed);
    while (current < value && !maximum.compare_exchange_weak(current, value,
        std::memory_order_relaxed)) {
    }
}

unsigned bucket_for(std::uint64_t value) noexcept {
    if (value == 0) return 0;
    if (value <= 15) return 1;
    if (value <= 63) return 2;
    if (value <= 255) return 3;
    if (value <= 1023) return 4;
    if (value <= 4095) return 5;
    return 6;
}

void log_stats(std::uint64_t count) noexcept {
    const auto dynamic = dynamic_calls.load(std::memory_order_relaxed);
    const auto inputs = dynamic_inputs.load(std::memory_order_relaxed);
    const auto original_us = ticks_to_microseconds(
        add_original_ticks.load(std::memory_order_relaxed) +
        subtract_original_ticks.load(std::memory_order_relaxed));
    const auto measurement_us = ticks_to_microseconds(measurement_ticks.load(std::memory_order_relaxed));
    log("stats calls=%llu add=%llu subtract=%llu dynamic_calls=%llu aliases=%llu invalid=%llu "
        "original_total_ms=%llu measurement_total_ms=%llu original_avg_us=%llu "
        "destination_entries=%llu source_entries=%llu dynamic_inputs=%llu direct_lookups=%llu "
        "linear_comparisons=%llu appended=%llu comparisons_per_dynamic_call=%llu "
        "comparisons_per_dynamic_input=%llu max_call_comparisons=%llu max_destination=%u max_source=%u "
        "buckets[0,1-15,16-63,64-255,256-1023,1024-4095,4096+]=%llu,%llu,%llu,%llu,%llu,%llu,%llu",
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(add_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(subtract_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(dynamic),
        static_cast<unsigned long long>(alias_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(invalid_measurements.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(original_us / 1000),
        static_cast<unsigned long long>(measurement_us / 1000),
        static_cast<unsigned long long>(count ? original_us / count : 0),
        static_cast<unsigned long long>(destination_entries.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(source_entries.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(inputs),
        static_cast<unsigned long long>(direct_lookups.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparisons.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(appended.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(dynamic ? comparisons.load(std::memory_order_relaxed) / dynamic : 0),
        static_cast<unsigned long long>(inputs ? comparisons.load(std::memory_order_relaxed) / inputs : 0),
        static_cast<unsigned long long>(max_comparisons.load(std::memory_order_relaxed)),
        max_destination.load(std::memory_order_relaxed), max_source.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(comparison_buckets[0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparison_buckets[1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparison_buckets[2].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparison_buckets[3].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparison_buckets[4].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparison_buckets[5].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(comparison_buckets[6].load(std::memory_order_relaxed)));
}

void* profile_merge(void* destination, void* source, const void* caller,
    MergeFunction original, bool addition) {
    if (optimization_enabled) return original(destination, source);
    LARGE_INTEGER measure_begin{};
    LARGE_INTEGER measure_end{};
    modifierpatch::Measurement measurement;
    if (measurement_enabled) {
        QueryPerformanceCounter(&measure_begin);
        try {
            measurement = modifierpatch::measure_merge(destination, source, scratch);
        } catch (...) {
            measurement.valid = false;
        }
        QueryPerformanceCounter(&measure_end);
    }

    LARGE_INTEGER original_begin{};
    LARGE_INTEGER original_end{};
    QueryPerformanceCounter(&original_begin);
    void* result = original(destination, source);
    QueryPerformanceCounter(&original_end);

    const auto original_elapsed = static_cast<std::uint64_t>(original_end.QuadPart - original_begin.QuadPart);
    (addition ? add_original_ticks : subtract_original_ticks).fetch_add(
        original_elapsed, std::memory_order_relaxed);
    (addition ? add_calls : subtract_calls).fetch_add(1, std::memory_order_relaxed);
    if (measurement_enabled) {
        measurement_ticks.fetch_add(static_cast<std::uint64_t>(measure_end.QuadPart - measure_begin.QuadPart),
            std::memory_order_relaxed);
        modifierpatch::record_caller(caller, measurement, original_elapsed);
        if (measurement.alias) alias_calls.fetch_add(1, std::memory_order_relaxed);
        if (!measurement.valid) {
            invalid_measurements.fetch_add(1, std::memory_order_relaxed);
        } else if (!measurement.alias) {
            destination_entries.fetch_add(static_cast<std::uint32_t>(measurement.destination_count),
                std::memory_order_relaxed);
            source_entries.fetch_add(static_cast<std::uint32_t>(measurement.source_count),
                std::memory_order_relaxed);
            dynamic_inputs.fetch_add(measurement.dynamic_inputs, std::memory_order_relaxed);
            direct_lookups.fetch_add(measurement.direct_lookups, std::memory_order_relaxed);
            comparisons.fetch_add(measurement.comparisons, std::memory_order_relaxed);
            appended.fetch_add(measurement.appended, std::memory_order_relaxed);
            update_max(max_comparisons, measurement.comparisons);
            update_max(max_destination, static_cast<std::uint32_t>(measurement.destination_count));
            update_max(max_source, static_cast<std::uint32_t>(measurement.source_count));
            comparison_buckets[bucket_for(measurement.comparisons)].fetch_add(1, std::memory_order_relaxed);
            if (measurement.dynamic) dynamic_calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto count = total_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 4096 == 0) log_stats(count);
    if (count == 1 || count % 65536 == 0) {
        try {
            if (measurement_enabled) {
                for (const auto& line : modifierpatch::caller_report(false)) log("%s", line.c_str());
                for (const auto& line : modifierpatch::caller_report(true)) log("%s", line.c_str());
            }
            for (const auto& line : modifierpatch::function_report(false)) log("%s", line.c_str());
            for (const auto& line : modifierpatch::function_report(true)) log("%s", line.c_str());
        } catch (...) {
            log("caller/function report allocation failed");
        }
    }
    return result;
}

void merge_report() noexcept;
#include "merge_part.inc"
#include "eu4_build.hpp"

void* add_hook(void* destination, void* source, const void* caller) {
    if (merge_mode != MergeMode::off) return merge_hook(destination, source);
    return profile_merge(destination, source, caller, original_add, true);
}

void* subtract_hook(void* destination, void* source, const void* caller) {
    return profile_merge(destination, source, caller, original_subtract, false);
}

std::uint64_t unit_parameters_hook(void* unit, void*, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::unit_parameters);
    const auto result = original_unit_parameters(unit);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::unit_parameters, observation);
    return result;
}

void country_recalculation_hook(void* cache, void*, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::country_recalculation);
    original_country_recalculation(cache);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::country_recalculation, observation);
}

void country_cache_reset_hook(void* cache, void*, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::country_cache_reset);
    original_country_cache_reset(cache);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::country_cache_reset, observation);
}

void* country_cache_rebuild_hook(void* cache, void* country, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::country_cache_rebuild);
    void* result = original_country_cache_rebuild(cache, country);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::country_cache_rebuild, observation);
    return result;
}

void* unit_modifier_build_hook(void* unit, void* output, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::unit_modifier_build);
    void* result = original_unit_modifier_build(unit, output);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::unit_modifier_build, observation);
    return result;
}

int unit_combat_value_hook(void* unit, void* modifiers, void* explanation, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::unit_combat_value);
    const auto result = original_unit_combat_value(unit, modifiers, explanation);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::unit_combat_value, observation);
    return result;
}

void* modifier_copy_hook(void* destination, void* source, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::modifier_copy);
    void* result = original_modifier_copy(destination, source);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::modifier_copy, observation);
    return result;
}

void* modifier_scale_hook(void* destination, void* factor, const void*) {
    const auto observation = modifierpatch::enter_function(
        modifierpatch::ProfileFunction::modifier_scale);
    void* result = original_modifier_scale(destination, factor);
    modifierpatch::leave_function(modifierpatch::ProfileFunction::modifier_scale, observation);
    return result;
}

bool paths_for_module(HMODULE module, std::wstring& directory) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    directory = path;
    log_path = directory + L"\\eu4_modifier_patch.log";
    return true;
}

bool profile_enabled(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_modifier_patch.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"modifier", L"mode", L"disabled", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    optimization_enabled = false;
    measurement_enabled = false;
    mode_name = L"disabled";
    wchar_t merge_value[32]{};
    GetPrivateProfileStringW(L"modifier", L"merge", L"off", merge_value,
        static_cast<DWORD>(std::size(merge_value)), ini.c_str());
    merge_mode = _wcsicmp(merge_value, L"on") == 0 ? MergeMode::on
        : _wcsicmp(merge_value, L"shadow") == 0 ? MergeMode::shadow : MergeMode::off;
    if (_wcsicmp(value, L"optimize") == 0) {
        optimization_enabled = true;
        mode_name = L"optimize";
        return true;
    }
    if (_wcsicmp(value, L"profile") == 0) {
        measurement_enabled = true;
        mode_name = L"profile";
        return true;
    }
    if (_wcsicmp(value, L"baseline") == 0) {
        mode_name = L"baseline";
        return true;
    }
    return false;
}

DWORD install_checked(HMODULE module) {
    std::wstring directory;
    if (!paths_for_module(module, directory)) {
        installation_status.store(-1);
        return 1;
    }
    if (!profile_enabled(directory)) {
        log("disabled by configuration; no bytes changed; version=%ls", version);
        installation_status.store(2);
        return 0;
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
        const auto hash = modifierpatch::executable_sha256(exe_path);
        if (hash.empty()) {
            log("executable SHA-256 unavailable");
        } else {
            log("executable SHA-256=%ls expected=%s", hash.c_str(),
                hash == modifierpatch::expected_exe_sha256 ? "yes" : "no (signature checks decide)");
        }
    } catch (...) {
        log("executable SHA-256 failed; signature checks will still run");
    }

    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto compatibility = modifierpatch::check_eu4_image(image);
    if (!compatibility.compatible) {
        log("REFUSED: %s; no bytes changed", compatibility.error);
        installation_status.store(-4);
        return 4;
    }
    log("compatibility passed: EU4 1.37.5 x64; checked_sites=%zu", compatibility.checked_sites);
    if (merge_mode != MergeMode::off && !check_merge(image)) merge_mode = MergeMode::off;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    modifierpatch::initialize_caller_stats(image, 0x25E0000, qpc_frequency);
    modifierpatch::initialize_function_stats(qpc_frequency);

    constexpr modifierpatch::EntryBytes prologue{
        0x40, 0x55, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x48};
    constexpr modifierpatch::EntryBytes unit_parameters{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56,
        0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
    constexpr modifierpatch::EntryBytes country_recalculation{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20};
    constexpr modifierpatch::EntryBytes unit_modifier_build{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10};
    constexpr modifierpatch::EntryBytes unit_combat_value{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55};
    constexpr modifierpatch::EntryBytes modifier_copy{
        0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02};
    constexpr modifierpatch::EntryBytes modifier_scale{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10};
    constexpr modifierpatch::EntryBytes country_cache_reset{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
    constexpr modifierpatch::EntryBytes country_cache_rebuild{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55};
    constexpr modifierpatch::SearchBytes add_search{
        0x39, 0x38, 0x0F, 0x84, 0x96, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC1, 0x48, 0x83, 0xC0, 0x08, 0x48, 0x3B, 0xCA, 0x7C, 0xEC};
    constexpr modifierpatch::SearchBytes subtract_search{
        0x39, 0x39, 0x0F, 0x84, 0x95, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC0, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x3B, 0xC2, 0x7C, 0xEC};

    std::array<modifierpatch::FunctionPatch, 10> patches{};
    std::array<modifierpatch::SearchPatch, 2> search_patches{};
    char error[160]{};
    auto prepare = [&](std::size_t index, std::uintptr_t rva,
        const modifierpatch::EntryBytes& expected, std::size_t size, const void* hook,
        modifierpatch::CallerArgument argument = modifierpatch::CallerArgument::third) {
        return modifierpatch::prepare_function(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(rva)),
            expected, size, hook, patches[index], error, sizeof(error), argument);
    };
    auto discard_all = [&]() {
        for (auto& patch : patches) modifierpatch::discard_function(patch);
    };
    auto discard_searches = [&]() {
        for (auto& patch : search_patches) modifierpatch::discard_search(patch);
    };

    const bool diagnostic_hooks = !optimization_enabled;
    bool prepared = prepare(0, modifierpatch::add_function_rva, prologue, 8,
        reinterpret_cast<const void*>(&add_hook));
    if (prepared) prepared = prepare(1, modifierpatch::subtract_function_rva, prologue, 8,
        reinterpret_cast<const void*>(&subtract_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(2, modifierpatch::unit_parameters_rva,
        unit_parameters, 15, reinterpret_cast<const void*>(&unit_parameters_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(3, modifierpatch::country_recalculation_rva,
        country_recalculation, 10, reinterpret_cast<const void*>(&country_recalculation_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(4, modifierpatch::unit_modifier_build_rva,
        unit_modifier_build, 15, reinterpret_cast<const void*>(&unit_modifier_build_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(5, modifierpatch::unit_combat_value_rva,
        unit_combat_value, 11, reinterpret_cast<const void*>(&unit_combat_value_hook),
        modifierpatch::CallerArgument::fourth);
    if (prepared && diagnostic_hooks) prepared = prepare(6, modifierpatch::modifier_copy_rva,
        modifier_copy, 11, reinterpret_cast<const void*>(&modifier_copy_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(7, modifierpatch::modifier_scale_rva,
        modifier_scale, 10, reinterpret_cast<const void*>(&modifier_scale_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(8, modifierpatch::country_cache_reset_rva,
        country_cache_reset, 9, reinterpret_cast<const void*>(&country_cache_reset_hook));
    if (prepared && diagnostic_hooks) prepared = prepare(9, modifierpatch::country_cache_rebuild_rva,
        country_cache_rebuild, 11, reinterpret_cast<const void*>(&country_cache_rebuild_hook));
    if (prepared && optimization_enabled) {
        modifierpatch::add_search_found_target = reinterpret_cast<void*>(image + eu4build::rva(0x8EA57E));
        modifierpatch::add_search_not_found_target = reinterpret_cast<void*>(image + eu4build::rva(0x8EA4F4));
        modifierpatch::subtract_search_found_target = reinterpret_cast<void*>(image + eu4build::rva(0x8EA88E));
        modifierpatch::subtract_search_not_found_target = reinterpret_cast<void*>(image + eu4build::rva(0x8EA805));
        prepared = modifierpatch::prepare_search(
            reinterpret_cast<std::uint8_t*>(image + eu4build::rva(modifierpatch::add_search_rva)),
            add_search, 20, reinterpret_cast<const void*>(&modifierpatch::add_search_relay),
            search_patches[0], error, sizeof(error));
        if (prepared) prepared = modifierpatch::prepare_search(
            reinterpret_cast<std::uint8_t*>(image + eu4build::rva(modifierpatch::subtract_search_rva)),
            subtract_search, 20, reinterpret_cast<const void*>(&modifierpatch::subtract_search_relay),
            search_patches[1], error, sizeof(error));
    }
    if (!prepared) {
        log("REFUSED: %s; no game bytes changed", error);
        discard_searches();
        discard_all();
        installation_status.store(-5);
        return 5;
    }

    original_add = reinterpret_cast<MergeFunction>(patches[0].trampoline);
    original_subtract = reinterpret_cast<MergeFunction>(patches[1].trampoline);
    original_unit_parameters = reinterpret_cast<UnitParametersFunction>(patches[2].trampoline);
    original_country_recalculation =
        reinterpret_cast<CountryRecalculationFunction>(patches[3].trampoline);
    original_unit_modifier_build = reinterpret_cast<MergeFunction>(patches[4].trampoline);
    original_unit_combat_value = reinterpret_cast<UnitCombatValueFunction>(patches[5].trampoline);
    original_modifier_copy = reinterpret_cast<MergeFunction>(patches[6].trampoline);
    original_modifier_scale = reinterpret_cast<MergeFunction>(patches[7].trampoline);
    original_country_cache_reset =
        reinterpret_cast<CountryCacheResetFunction>(patches[8].trampoline);
    original_country_cache_rebuild =
        reinterpret_cast<CountryCacheRebuildFunction>(patches[9].trampoline);
    const auto patch_count = optimization_enabled ? std::size_t{2} : patches.size();
    bool committed = true;
    for (std::size_t index = 0; index < patch_count; ++index) {
        auto& patch = patches[index];
        if (!modifierpatch::commit_function(patch, error, sizeof(error))) {
            committed = false;
            break;
        }
    }
    if (committed && optimization_enabled) {
        for (auto& patch : search_patches) {
            if (!modifierpatch::commit_search(patch, error, sizeof(error))) {
                committed = false;
                break;
            }
        }
    }
    if (!committed) {
        log("REFUSED: %s; rolling back every installed hook", error);
        discard_searches();
        discard_all();
        installation_status.store(-6);
        return 6;
    }
    installation_status.store(1);
    log("initialized version=%ls mode=%ls hooks=%zu search_sse2=%u add_rva=0x%zx subtract_rva=0x%zx; "
        "merge behavior retained; variable-length entries=8,8,15,10,15,11,11,10,9,11",
        version, mode_name, patch_count, optimization_enabled ? 2U : 0U,
        modifierpatch::add_function_rva, modifierpatch::subtract_function_rva);
    log("merge index (Bellows modifier_merge port) mode=%s",
        merge_mode == MergeMode::on ? "on" : merge_mode == MergeMode::shadow ? "shadow" : "off");
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

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4ModifierPatchStatus() {
    return installation_status.load();
}

extern "C" __declspec(dllexport) std::uint64_t EU4ModifierPatchCallCount() {
    return total_calls.load();
}
