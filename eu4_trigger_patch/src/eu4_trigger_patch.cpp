// EU4 Trigger Patch 0.1.0-shadow, for EU4 1.37.5.0 Windows x64.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "call_patch.hpp"
#include "parallel_patch.hpp"
#include "parallel_pool.hpp"
#include "profile.hpp"
#include "trigger_batch.hpp"

extern "C" {
void* trigger_original_target = nullptr;
void* trigger_finish_original = nullptr;
void trigger_first_hook();
void trigger_second_hook();
void trigger_finish_hook();
extern const std::uint8_t trigger_batch_entry_begin[];
extern const std::uint8_t trigger_batch_entry_end[];
extern const std::uint8_t trigger_batch_entry_continuation_displacement[];
}

namespace {

enum class Mode { disabled, shadow, parallel_shadow };

constexpr wchar_t version[] = L"0.2.0-parallel-shadow";
std::atomic<Mode> mode{Mode::disabled};
std::atomic<LONG> installation_status{0};
std::atomic<bool> batch_active{false};
std::uintptr_t image_base = 0;
std::int64_t qpc_frequency = 1;
std::wstring log_path;
std::mutex log_mutex;
knowledgepatch::ParallelPool pool;

std::atomic<std::uint64_t> batches{0};
std::atomic<std::uint64_t> parallel_batches{0};
std::atomic<std::uint64_t> skipped_batches{0};
std::atomic<std::uint64_t> options_seen{0};
std::atomic<std::uint64_t> evaluations{0};
std::atomic<std::uint64_t> compared{0};
std::atomic<std::uint64_t> mismatches{0};
std::atomic<std::uint64_t> missing{0};
std::atomic<std::uint64_t> max_batch{0};
std::atomic<std::uint64_t> parallel_ticks{0};
std::atomic<std::uint64_t> original_ticks{0};

struct World {
    std::uintptr_t root;
    std::uintptr_t other;
};

thread_local triggerpatch::Batch local_batch;
thread_local World local_world{};
thread_local unsigned batch_nesting = 0;
thread_local unsigned shadow_evaluation_depth = 0;
thread_local bool owns_batch = false;
thread_local bool trace_batch = false;
thread_local unsigned trace_evaluations = 0;

void log(const char* format, ...) noexcept {
    if (log_path.empty()) return;
    char message[1500];
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

template <typename T>
T read_memory(std::uintptr_t address) noexcept {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart);
}

std::uint64_t ticks_to_microseconds(std::uint64_t ticks) noexcept {
    const auto frequency = static_cast<std::uint64_t>(qpc_frequency);
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

void update_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < value &&
        !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

const char* mode_name(Mode selected) noexcept {
    switch (selected) {
        case Mode::shadow: return "shadow";
        case Mode::parallel_shadow: return "parallel_shadow";
        case Mode::disabled: return "disabled";
    }
    return "unknown";
}

void log_stats() noexcept {
    const auto batch_count = batches.load(std::memory_order_relaxed);
    const auto eval_count = evaluations.load(std::memory_order_relaxed);
    const auto parallel_us = ticks_to_microseconds(parallel_ticks.load(std::memory_order_relaxed));
    const auto original_us = ticks_to_microseconds(original_ticks.load(std::memory_order_relaxed));
    log("stats mode=%s batches=%llu parallel_batches=%llu skipped_batches=%llu "
        "options=%llu evaluations=%llu compared=%llu mismatches=%llu missing=%llu "
        "max_batch=%llu workers=%zu shadow_total_ms=%llu shadow_avg_us=%llu "
        "original_eval_total_ms=%llu original_eval_avg_us=%llu",
        mode_name(mode.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(batch_count),
        static_cast<unsigned long long>(parallel_batches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(skipped_batches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(options_seen.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(eval_count),
        static_cast<unsigned long long>(compared.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(mismatches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(missing.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(max_batch.load(std::memory_order_relaxed)),
        pool.size(),
        static_cast<unsigned long long>(parallel_us / 1000),
        static_cast<unsigned long long>(batch_count ? parallel_us / batch_count : 0),
        static_cast<unsigned long long>(original_us / 1000),
        static_cast<unsigned long long>(eval_count ? original_us / eval_count : 0));
}

bool eligible(std::uintptr_t option, std::uintptr_t filter) noexcept {
    if (read_memory<std::uint8_t>(option + 0x28E) == 0) return true;
    if (!filter) return false;
    const int count = read_memory<int>(filter + 0x1A4);
    const auto entries = read_memory<std::uintptr_t>(filter + 0x198);
    if (count <= 0 || !entries) return false;
    for (int index = 0; index < count; ++index) {
        const auto entry = entries + static_cast<std::size_t>(index) * 0x10;
        if (read_memory<std::uint32_t>(entry) == 0x27 &&
            read_memory<std::uintptr_t>(entry + 8) == option) {
            return true;
        }
    }
    return false;
}

bool evaluate(void* opaque, const triggerpatch::Item& item) noexcept {
    const auto& world = *static_cast<const World*>(opaque);
    struct DepthGuard {
        DepthGuard() { ++shadow_evaluation_depth; }
        ~DepthGuard() { --shadow_evaluation_depth; }
    } guard;

    using Construct = void* (*)(void*, std::uint64_t, std::uint64_t);
    using Destroy = void (*)(void*);
    using Eval = std::uint64_t (*)(void*, void*);
    const auto construct = reinterpret_cast<Construct>(image_base + triggerpatch::context_construct_rva);
    const auto destroy = reinterpret_cast<Destroy>(image_base + triggerpatch::context_destroy_rva);
    const auto eval = reinterpret_cast<Eval>(trigger_original_target);

    const auto scope_object = item.kind == 0 ? world.root + 0x10 : world.other + 0x158;
    const bool trace = trace_batch && trace_evaluations++ < 16;
    if (trace) {
        log("trace stage=before_eval option=%p kind=%u root=%p other=%p scope_object=%p",
            item.option, item.kind, reinterpret_cast<void*>(world.root),
            reinterpret_cast<void*>(world.other), reinterpret_cast<void*>(scope_object));
    }
    if (!scope_object || !world.root) return false;
    alignas(16) std::array<std::uint8_t, 0x200> storage{};
    const auto third = read_memory<std::uint64_t>(world.root + 0x18);
    const auto scope = read_memory<std::uint64_t>(scope_object);
    auto* context = construct(storage.data(), scope, third);
    if (!context) return false;
    const auto result = eval(static_cast<std::uint8_t*>(item.option) + 0x30,
        static_cast<std::uint8_t*>(context) + 0xD0);
    destroy(static_cast<std::uint8_t*>(context) + 0xD0);
    destroy(context);
    if (trace) log("trace stage=after_eval option=%p result=%u", item.option,
        static_cast<unsigned>(result & 0xFF));
    return (result & 0xFF) != 0;
}

void verify(void* option, std::uint32_t kind, bool original, std::uint64_t started) noexcept {
    original_ticks.fetch_add(qpc_now() - started, std::memory_order_relaxed);
    if (!owns_batch || batch_nesting != 1 || shadow_evaluation_depth != 0) return;
    auto* item = local_batch.find(option, kind);
    if (!item || !item->requested || !item->evaluated) {
        missing.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    compared.fetch_add(1, std::memory_order_relaxed);
    if (item->shadow == original) return;
    const auto count = mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32) {
        log("MISMATCH option=%p kind=%u original=%u shadow=%u", option, kind,
            original ? 1U : 0U, item->shadow ? 1U : 0U);
    }
}

bool paths_for_module(HMODULE module, std::wstring& directory) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    directory = path;
    log_path = directory + L"\\eu4_trigger_patch.log";
    return true;
}

Mode read_mode(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_trigger_patch.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"trigger", L"mode", L"parallel_shadow", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    if (_wcsicmp(value, L"parallel_shadow") == 0) return Mode::parallel_shadow;
    if (_wcsicmp(value, L"shadow") == 0) return Mode::shadow;
    return Mode::disabled;
}

unsigned read_workers(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_trigger_patch.ini";
    const auto configured = GetPrivateProfileIntW(L"trigger", L"workers", 4, ini.c_str());
    const auto hardware = (std::max)(1U, std::thread::hardware_concurrency());
    return (std::max)(1U, (std::min)({configured, 4U, hardware}));
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
    const auto selected_mode = read_mode(directory);
    if (selected_mode == Mode::disabled) {
        log("disabled by configuration; no bytes changed; version=%ls", version);
        installation_status.store(2);
        return 0;
    }
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto compatibility = triggerpatch::check_eu4_image(image);
    if (!compatibility.compatible) {
        log("REFUSED: %s; no bytes changed", compatibility.error);
        installation_status.store(-4);
        return 4;
    }
    image_base = image;
    trigger_original_target = reinterpret_cast<void*>(image + triggerpatch::trigger_eval_rva);
    trigger_finish_original = reinterpret_cast<void*>(image + triggerpatch::finish_target_rva);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    const auto workers = selected_mode == Mode::parallel_shadow ? read_workers(directory) : 1U;
    if (!pool.start(workers)) {
        log("REFUSED: cannot start worker pool; no game bytes changed");
        installation_status.store(-5);
        return 5;
    }

    speedpatch::CallPatch first;
    speedpatch::CallPatch second;
    speedpatch::CallPatch finish;
    knowledgepatch::ParallelPatch entry;
    char error[180]{};
    const auto cleanup = [&]() noexcept {
        mode.store(Mode::disabled);
        speedpatch::discard_call(finish);
        speedpatch::discard_call(second);
        speedpatch::discard_call(first);
        knowledgepatch::discard_parallel_patch(entry);
        pool.stop();
    };
    constexpr std::uint8_t entry_expected[7]{0x4C, 0x8B, 0x3D, 0x66, 0xC3, 0xAA, 0x01};
    const knowledgepatch::ParallelHookImage entry_image{
        trigger_batch_entry_begin, trigger_batch_entry_end, nullptr,
        trigger_batch_entry_continuation_displacement, nullptr};
    bool prepared = knowledgepatch::prepare_parallel_patch(
        reinterpret_cast<std::uint8_t*>(image + triggerpatch::batch_entry_rva),
        reinterpret_cast<const void*>(image + triggerpatch::batch_entry_continuation_rva),
        nullptr, entry_expected, sizeof(entry_expected), entry_image, 0,
        entry, error, sizeof(error));
    if (prepared) {
        prepared = speedpatch::prepare_call(
            reinterpret_cast<std::uint8_t*>(image + triggerpatch::first_call_rva),
            trigger_original_target, reinterpret_cast<const void*>(&trigger_first_hook),
            first, error, sizeof(error));
    }
    if (prepared) {
        prepared = speedpatch::prepare_call(
            reinterpret_cast<std::uint8_t*>(image + triggerpatch::second_call_rva),
            trigger_original_target, reinterpret_cast<const void*>(&trigger_second_hook),
            second, error, sizeof(error));
    }
    if (prepared) {
        prepared = speedpatch::prepare_call(
            reinterpret_cast<std::uint8_t*>(image + triggerpatch::finish_call_rva),
            trigger_finish_original, reinterpret_cast<const void*>(&trigger_finish_hook),
            finish, error, sizeof(error));
    }
    if (!prepared) {
        log("REFUSED: %s; no game bytes changed", error);
        cleanup();
        installation_status.store(-6);
        return 6;
    }
    mode.store(selected_mode);
    bool committed = knowledgepatch::commit_parallel_patch(entry, error, sizeof(error));
    if (committed) committed = speedpatch::commit_call(first, error, sizeof(error));
    if (committed) committed = speedpatch::commit_call(second, error, sizeof(error));
    if (committed) committed = speedpatch::commit_call(finish, error, sizeof(error));
    if (!committed) {
        log("REFUSED: %s; rolling back every installed hook", error);
        cleanup();
        installation_status.store(-7);
        return 7;
    }
    installation_status.store(selected_mode == Mode::parallel_shadow ? 3 : 1);
    log("initialized version=%ls mode=%s checked_sites=%zu workers=%zu "
        "entry_rva=0x%zx first_call=0x%zx second_call=0x%zx finish_call=0x%zx; "
        "original results retained; parallel shadow threshold=8; filter read from stack",
        version, mode_name(selected_mode), compatibility.checked_sites, pool.size(),
        triggerpatch::batch_entry_rva, triggerpatch::first_call_rva,
        triggerpatch::second_call_rva, triggerpatch::finish_call_rva);
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

extern "C" std::uint64_t trigger_qpc_now() noexcept {
    return qpc_now();
}

extern "C" std::uintptr_t trigger_batch_begin(void* root, void* filter, void* other) noexcept {
    const auto options_root = read_memory<std::uintptr_t>(image_base + triggerpatch::options_pointer_rva);
    ++batch_nesting;
    if (batch_nesting != 1 || shadow_evaluation_depth != 0) return options_root;
    local_batch.reset();
    owns_batch = false;
    if (!root || !filter || !other || !options_root) {
        skipped_batches.fetch_add(1, std::memory_order_relaxed);
        return options_root;
    }
    bool expected = false;
    if (!batch_active.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        skipped_batches.fetch_add(1, std::memory_order_relaxed);
        return options_root;
    }
    owns_batch = true;
    trace_batch = batches.load(std::memory_order_relaxed) < 8;
    trace_evaluations = 0;
    if (trace_batch) {
        log("trace stage=entry root=%p filter=%p other=%p options_root=%p",
            root, filter, other, reinterpret_cast<void*>(options_root));
    }
    try {
        const auto begin = read_memory<std::uintptr_t>(options_root);
        const auto end = read_memory<std::uintptr_t>(options_root + 8);
        if (!begin || end < begin || (end - begin) % 8 != 0 || (end - begin) / 8 > 4096) {
            skipped_batches.fetch_add(1, std::memory_order_relaxed);
            return options_root;
        }
        const auto count = (end - begin) / 8;
        options_seen.fetch_add(count, std::memory_order_relaxed);
        update_max(max_batch, count);
        local_world = World{reinterpret_cast<std::uintptr_t>(root),
            reinterpret_cast<std::uintptr_t>(other)};
        const auto filter_address = reinterpret_cast<std::uintptr_t>(filter);
        for (std::uintptr_t cursor = begin; cursor != end; cursor += 8) {
            const auto option = read_memory<std::uintptr_t>(cursor);
            if (!option || !eligible(option, filter_address)) continue;
            const auto kind = read_memory<std::uint8_t>(option + 0x28F) != 0 ? 1U : 0U;
            local_batch.add(reinterpret_cast<void*>(option), kind, true);
        }
        if (trace_batch) {
            log("trace stage=collected options=%llu requested=%zu",
                static_cast<unsigned long long>(count), local_batch.requested());
        }
        const auto started = qpc_now();
        local_batch.run(pool, &local_world, &evaluate,
            mode.load(std::memory_order_relaxed) == Mode::parallel_shadow);
        parallel_ticks.fetch_add(qpc_now() - started, std::memory_order_relaxed);
        batches.fetch_add(1, std::memory_order_relaxed);
        evaluations.fetch_add(local_batch.requested(), std::memory_order_relaxed);
        if (local_batch.used_parallel()) parallel_batches.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        local_batch.reset();
        skipped_batches.fetch_add(1, std::memory_order_relaxed);
    }
    return options_root;
}

extern "C" void trigger_batch_end() noexcept {
    if (batch_nesting == 0) return;
    --batch_nesting;
    if (batch_nesting != 0) return;
    if (owns_batch) {
        const auto count = batches.load(std::memory_order_relaxed);
        if (trace_batch) log("trace stage=finish batches=%llu",
            static_cast<unsigned long long>(count));
        if (count && count % 32 == 0) log_stats();
        local_batch.reset();
        owns_batch = false;
        trace_batch = false;
        batch_active.store(false, std::memory_order_release);
    }
}

extern "C" void trigger_first_verify(void* option, std::uint8_t result,
    std::uint64_t started) noexcept {
    verify(option, 0, result != 0, started);
}

extern "C" void trigger_second_verify(void* option, std::uint8_t result,
    std::uint64_t started) noexcept {
    verify(option, 1, result != 0, started);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4TriggerPatchStatus() {
    return installation_status.load();
}

extern "C" __declspec(dllexport) std::uint64_t EU4TriggerPatchMismatchCount() {
    return mismatches.load(std::memory_order_relaxed);
}
