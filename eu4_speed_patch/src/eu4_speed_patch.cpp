// EU4 Speed Patch 0.1.0 experimental, for EU4 1.37.5.0 Windows x64.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "call_patch.hpp"
#include "profile.hpp"

namespace {

enum class Mode { balanced, profile, disabled };

constexpr wchar_t version[] = L"0.1.0-experimental";
std::atomic<Mode> mode{Mode::disabled};
std::atomic<LONG> installation_status{0};
std::atomic<std::uint64_t> schedule_checks{0};
std::atomic<std::uint64_t> search_calls{0};
std::atomic<std::uint64_t> search_microseconds{0};
std::int64_t qpc_frequency = 1;
std::wstring log_path;
std::mutex log_mutex;

using DailyCheckFn = std::uint64_t (*)(void* army_ai);
using ArmySearchFn = std::uint64_t (*)(
    void* context,
    int maximum_steps,
    void* unit,
    void* ai_data,
    float* best_score,
    void* evaluation_state,
    std::uint8_t* out_a,
    std::uint8_t* out_b,
    std::uint8_t* out_c,
    std::uint8_t stationary,
    std::uint8_t flag,
    void* result);

DailyCheckFn original_daily_check = nullptr;
ArmySearchFn original_army_search = nullptr;

void log(const char* format, ...) noexcept {
    if (log_path.empty()) return;
    char message[1400];
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

void log_stats(std::uint64_t count) noexcept {
    const auto micros = search_microseconds.load(std::memory_order_relaxed);
    const auto checks = schedule_checks.load(std::memory_order_relaxed);
    log("stats mode=%s searches=%llu search_total_ms=%llu average_us=%llu schedule_checks=%llu",
        mode.load(std::memory_order_relaxed) == Mode::balanced ? "balanced" : "profile",
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(micros / 1000),
        static_cast<unsigned long long>(count ? micros / count : 0),
        static_cast<unsigned long long>(checks));
}

std::uint64_t daily_check_hook(void* army_ai) {
    schedule_checks.fetch_add(1, std::memory_order_relaxed);
    if (mode.load(std::memory_order_relaxed) == Mode::balanced) return 0;
    return original_daily_check(army_ai);
}

std::uint64_t army_search_hook(
    void* context,
    int maximum_steps,
    void* unit,
    void* ai_data,
    float* best_score,
    void* evaluation_state,
    std::uint8_t* out_a,
    std::uint8_t* out_b,
    std::uint8_t* out_c,
    std::uint8_t stationary,
    std::uint8_t flag,
    void* result) {
    LARGE_INTEGER begin{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&begin);
    const auto value = original_army_search(context, maximum_steps, unit, ai_data,
        best_score, evaluation_state, out_a, out_b, out_c, stationary, flag, result);
    QueryPerformanceCounter(&end);
    const auto elapsed = static_cast<std::uint64_t>(end.QuadPart - begin.QuadPart);
    const auto frequency = static_cast<std::uint64_t>(qpc_frequency);
    const auto micros = elapsed / frequency * 1000000 + elapsed % frequency * 1000000 / frequency;
    search_microseconds.fetch_add(micros, std::memory_order_relaxed);
    const auto count = search_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 256 == 0) log_stats(count);
    return value;
}

bool paths_for_module(HMODULE module, std::wstring& directory) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    directory = path;
    log_path = directory + L"\\eu4_speed_patch.log";
    return true;
}

Mode read_mode(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_speed_patch.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"speed", L"mode", L"balanced", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    if (_wcsicmp(value, L"balanced") == 0) return Mode::balanced;
    if (_wcsicmp(value, L"profile") == 0) return Mode::profile;
    if (_wcsicmp(value, L"disabled") == 0) return Mode::disabled;
    return Mode::disabled;
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
        const auto hash = speedpatch::executable_sha256(exe_path);
        if (hash.empty()) {
            log("executable SHA-256 unavailable");
        } else {
            log("executable SHA-256=%ls expected=%s", hash.c_str(),
                hash == speedpatch::expected_exe_sha256 ? "yes" : "no (signature checks decide)");
        }
    } catch (...) {
        log("executable SHA-256 failed; signature checks will still run");
    }

    const auto selected_mode = read_mode(directory);
    if (selected_mode == Mode::disabled) {
        log("disabled by configuration; no bytes changed; version=%ls", version);
        installation_status.store(3);
        return 0;
    }

    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto compatibility = speedpatch::check_eu4_image(image);
    if (!compatibility.compatible) {
        log("REFUSED: %s; no bytes changed", compatibility.error);
        installation_status.store(-4);
        return 4;
    }
    log("compatibility passed: EU4 1.37.5 x64; checked_sites=%zu", compatibility.checked_sites);

    mode.store(selected_mode);
    original_daily_check = reinterpret_cast<DailyCheckFn>(image + speedpatch::daily_check_function_rva);
    original_army_search = reinterpret_cast<ArmySearchFn>(image + speedpatch::army_search_function_rva);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

    speedpatch::CallPatch daily;
    speedpatch::CallPatch search;
    char error[160]{};
    bool prepared = speedpatch::prepare_call(
        reinterpret_cast<std::uint8_t*>(image + speedpatch::daily_check_call_rva),
        reinterpret_cast<const void*>(original_daily_check),
        reinterpret_cast<const void*>(&daily_check_hook), daily, error, sizeof(error));
    if (prepared) {
        prepared = speedpatch::prepare_call(
            reinterpret_cast<std::uint8_t*>(image + speedpatch::army_search_call_rva),
            reinterpret_cast<const void*>(original_army_search),
            reinterpret_cast<const void*>(&army_search_hook), search, error, sizeof(error));
    }
    if (!prepared) {
        log("REFUSED: %s; no bytes changed", error);
        speedpatch::discard_call(daily);
        speedpatch::discard_call(search);
        installation_status.store(-5);
        return 5;
    }

    if (!speedpatch::commit_call(daily, error, sizeof(error)) ||
        !speedpatch::commit_call(search, error, sizeof(error))) {
        log("REFUSED: %s; rolling back any installed hook", error);
        speedpatch::discard_call(search);
        speedpatch::discard_call(daily);
        installation_status.store(-6);
        return 6;
    }

    installation_status.store(selected_mode == Mode::balanced ? 1 : 2);
    log("initialized version=%ls mode=%s; only two CALL sites changed; original functions retained",
        version, selected_mode == Mode::balanced ? "balanced" : "profile");
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

extern "C" __declspec(dllexport) LONG EU4SpeedPatchStatus() {
    return installation_status.load();
}

extern "C" __declspec(dllexport) std::uint64_t EU4SpeedPatchSearchCount() {
    return search_calls.load();
}
