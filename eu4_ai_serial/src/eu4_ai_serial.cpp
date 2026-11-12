// EU4 AI Serial 0.1.0 experimental, for EU4 1.37.5.0 Windows x64.
//
// The daily country AI (14077d8a0) hands one task per country to the engine's
// parallel-for (submit 1401798a0, main-thread wait 140179e90). Profiling showed
// the main thread spins in the wait about twice as long as the AI work takes on
// all threads together. This plugin can run those tasks one by one on the main
// thread instead, calling the same functor the engine calls.
//
// Modes (eu4_ai_serial.ini, [ai] mode=):
//   serial   - run the daily AI tasks on the main thread, no submit, no wait
//   timing   - keep the engine's parallel path, only measure submit + wait
//   tasks    - like timing, plus the start/end/thread of every country task
//   disabled - change nothing (default)
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
#include "heavy_sampler.hpp"
#include "unit_flag_cache.hpp"
#include "search_memo.hpp"
#include "country_ratio_cache.hpp"
#include "province_score_cache.hpp"
#include "path_cache.hpp"
#include "tree_probe.hpp"
#include "tree_walk.hpp"
#include "eu4_build.hpp"

namespace {

enum class Mode { serial, timing, tasks, disabled };

constexpr wchar_t version[] = L"0.1.0-experimental";

// RVAs in eu4.exe 1.37.5 x64.
constexpr std::uintptr_t submit_function_rva = 0x1798a0;   // parallel-for submit
constexpr std::uintptr_t wait_function_rva = 0x179e90;     // main thread joins and waits
constexpr std::uintptr_t submit_call_rva = 0x77d9b0;       // call submit in daily AI 14077d8a0
constexpr std::uintptr_t wait_call_rva = 0x77d9ee;         // call wait (mode 2) in 14077d8a0
constexpr std::size_t context_mode_offset = 0x20;          // [rbp-0x79], switch after submit
constexpr std::size_t function_impl_offset = 0x38;         // MSVC std::function _Impl pointer
constexpr int skip_wait_mode = 3;                          // not 0/1/2: switch does nothing
constexpr std::uintptr_t task_vtable_slot_rva = 0x1cafc38;  // lambda vtable 141cafc28, _Do_call
constexpr std::uintptr_t task_function_rva = 0x786150;      // _Do_call -> 1400b0fa0
constexpr int max_tasks = 1024;

struct Signature {
    std::uintptr_t rva;
    const char* bytes;  // hex, no spaces
    const char* what;
};

// Bytes around both call sites and the mode switch must match exactly.
constexpr Signature signatures[] = {
    {0x77d958, "c7458702000000", "mode = 2 before submit"},
    {0x77d9a4, "458b4e144533c0488d4c2430e8ebbe9fff90488b4def", "submit call (r9d=count, r8d=0)"},
    {0x77d9d8, "8b4d8785c97422", "switch on context mode"},
    {0x77d9e9, "488d4c2430e89dc49fffeb39", "wait call for mode 2"},
};

using SubmitFn = std::uint64_t (*)(void* context, void* unused, int begin, int end,
    void* function, void* on_done);
using WaitFn = std::uint64_t (*)(void* context);
using TaskFn = void (*)(void* impl, int* index);

std::atomic<Mode> mode{Mode::disabled};
std::atomic<LONG> installation_status{0};
SubmitFn original_submit = nullptr;
WaitFn original_wait = nullptr;
std::int64_t qpc_frequency = 1;
std::wstring log_path;
std::mutex log_mutex;

// Only the main thread runs 14077d8a0, so plain counters are enough.
std::uint64_t calls = 0;
std::uint64_t tasks = 0;
std::uint64_t submit_ticks = 0;
std::uint64_t wait_ticks = 0;
std::uint64_t serial_ticks = 0;
std::uint64_t fallbacks = 0;
std::uint64_t max_call_ticks = 0;
std::uint64_t submit_begin = 0;
TaskFn original_task = nullptr;

// Per-call task records, written by workers (each index by one thread only).
std::uint64_t task_start[max_tasks];
std::uint64_t task_end[max_tasks];
DWORD task_thread[max_tasks];
int call_begin = 0;
int call_end = 0;

// Aggregates over calls (main thread only).
struct TaskTotals {
    std::uint64_t calls = 0;
    std::uint64_t wall = 0;        // submit start -> last task end
    std::uint64_t work = 0;        // sum of task durations
    std::uint64_t longest = 0;     // sum over calls of the longest task
    std::uint64_t ideal = 0;       // sum of max(work / threads, longest)
    std::uint64_t first_lag = 0;   // submit start -> first task start
    std::uint64_t last_start = 0;  // submit start -> start of the task that ends last
    std::uint64_t threads = 0;     // sum of distinct threads per call
    std::uint64_t index_work[max_tasks]{};
    std::uint32_t index_longest[max_tasks]{};
    std::uint32_t index_last[max_tasks]{};
} totals;
const void* index_country[max_tasks]{};
std::uint32_t index_country_changes = 0;

std::uint64_t now() noexcept;
bool sample_heavy = false;  // [ai] sample_heavy=1: stack-sample task 0 (tasks mode only)
bool tree_probe = false;    // [ai] tree_probe=1: time AI node updates in task 0 (tasks mode)

// [ai] sample_monthly=1: stack-sample the main thread during GameState_Monthly 14075d690
// (call at 140758bc3 in GameState_DailyTick) and log the duration of every monthly tick.
constexpr std::uintptr_t monthly_function_rva = 0x75d690;
constexpr std::uintptr_t monthly_call_rva = 0x758bc3;
using MonthlyFn = std::uint64_t (*)(void* gamestate, std::uint64_t flag);
MonthlyFn original_monthly = nullptr;
bool sample_monthly = false;
std::uint64_t monthly_calls = 0;
std::uint64_t monthly_ticks_total = 0;
void log(const char* format, ...) noexcept;

std::uint64_t monthly_hook(void* gamestate, std::uint64_t flag) {
    if (sample_monthly) heavy::begin();
    const std::uint64_t start = now();
    const std::uint64_t result = original_monthly(gamestate, flag);
    const std::uint64_t ticks = now() - start;
    if (sample_monthly) heavy::end();
    ++monthly_calls;
    monthly_ticks_total += ticks;
    log("monthly tick %llu: %.1f ms (avg %.1f)", static_cast<unsigned long long>(monthly_calls),
        ticks * 1000.0 / qpc_frequency, monthly_ticks_total * 1000.0 / qpc_frequency / monthly_calls);
    return result;
}

void task_hook(void* impl, int* index) {
    const int i = *index;
    const bool sample = sample_heavy && i == 0;
    const bool probe = tree_probe && i == 0;
    if (sample) heavy::begin();
    if (probe) treeprobe::begin_heavy();
    if (tree_probe) treeprobe::begin_task(i);
    const auto start = now();
    original_task(impl, index);
    const auto end = now();
    if (tree_probe) treeprobe::end_task();
    if (probe) treeprobe::end_heavy();
    if (sample) heavy::end();
    if (i >= 0 && i < max_tasks) {
        task_start[i] = start;
        task_end[i] = end;
        task_thread[i] = GetCurrentThreadId();
    }
}

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

std::uint64_t now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

unsigned long long micros(std::uint64_t ticks) noexcept {
    const auto frequency = static_cast<std::uint64_t>(qpc_frequency);
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

void count_call(std::uint64_t call_ticks) noexcept {
    max_call_ticks = (std::max)(max_call_ticks, call_ticks);
    ++calls;
    if (calls == 1 || calls % 512 == 0) {
        const auto total = submit_ticks + wait_ticks + serial_ticks;
        log("stats mode=%s calls=%llu tasks=%llu avg_tasks=%llu total_ms=%llu avg_us=%llu "
            "max_us=%llu submit_ms=%llu wait_ms=%llu serial_ms=%llu fallbacks=%llu",
            mode.load(std::memory_order_relaxed) == Mode::serial ? "serial" :
                mode.load(std::memory_order_relaxed) == Mode::tasks ? "tasks" : "timing",
            static_cast<unsigned long long>(calls), static_cast<unsigned long long>(tasks),
            static_cast<unsigned long long>(tasks / calls), micros(total) / 1000,
            micros(total) / calls, micros(max_call_ticks), micros(submit_ticks) / 1000,
            micros(wait_ticks) / 1000, micros(serial_ticks) / 1000,
            static_cast<unsigned long long>(fallbacks));
        max_call_ticks = 0;
        const auto cache = flagcache::stats();
        log("unit_flag_cache=%s hits=%llu misses=%llu mismatches=%llu bypassed=%llu",
            flagcache::mode_name(), static_cast<unsigned long long>(cache.hits),
            static_cast<unsigned long long>(cache.misses),
            static_cast<unsigned long long>(cache.mismatches),
            static_cast<unsigned long long>(cache.bypassed));
        const auto memo = searchmemo::stats();
        log("search_memo=%s scopes=%llu flag_hits=%llu flag_misses=%llu flag_outside=%llu "
            "speed_hits=%llu speed_misses=%llu fort_hits=%llu fort_misses=%llu verifications=%llu mismatches=%llu disabled=%d",
            searchmemo::mode_name(), static_cast<unsigned long long>(memo.scopes),
            static_cast<unsigned long long>(memo.flag_hits),
            static_cast<unsigned long long>(memo.flag_misses),
            static_cast<unsigned long long>(memo.flag_outside),
            static_cast<unsigned long long>(memo.speed_hits),
            static_cast<unsigned long long>(memo.speed_misses),
            static_cast<unsigned long long>(searchmemo::fort_hits()),
            static_cast<unsigned long long>(searchmemo::fort_misses()),
            static_cast<unsigned long long>(memo.verifications),
            static_cast<unsigned long long>(memo.mismatches), memo.disabled ? 1 : 0);
        const auto ratio = ratiocache::stats();
        log("country_ratio_cache=%s hits=%llu misses=%llu mismatches=%llu bypassed=%llu",
            ratiocache::mode_name(), static_cast<unsigned long long>(ratio.hits),
            static_cast<unsigned long long>(ratio.misses),
            static_cast<unsigned long long>(ratio.mismatches),
            static_cast<unsigned long long>(ratio.bypassed));
        const auto score = scorecache::stats();
        log("province_score_cache=%s hits=%llu misses=%llu mismatches=%llu bypassed=%llu",
            scorecache::mode_name(), static_cast<unsigned long long>(score.hits),
            static_cast<unsigned long long>(score.misses),
            static_cast<unsigned long long>(score.mismatches),
            static_cast<unsigned long long>(score.bypassed));
        const auto path = pathcache::stats();
        log("path_cache=%s hits=%llu misses=%llu mismatches=%llu bypassed=%llu",
            pathcache::mode_name(), static_cast<unsigned long long>(path.hits),
            static_cast<unsigned long long>(path.misses),
            static_cast<unsigned long long>(path.mismatches),
            static_cast<unsigned long long>(path.bypassed));
        if (tree_probe) treeprobe::report(&log);
        treewalk::report(&log);
    }
}

void log_task_totals() noexcept {
    const auto& t = totals;
    if (!t.calls) return;
    log("tasks calls=%llu avg_wall_us=%llu avg_work_us=%llu avg_longest_us=%llu avg_ideal_us=%llu "
        "avg_first_lag_us=%llu avg_last_start_us=%llu avg_threads_x10=%llu country_changes=%u",
        static_cast<unsigned long long>(t.calls), micros(t.wall) / t.calls,
        micros(t.work) / t.calls, micros(t.longest) / t.calls, micros(t.ideal) / t.calls,
        micros(t.first_lag) / t.calls, micros(t.last_start) / t.calls,
        static_cast<unsigned long long>(t.threads * 10 / t.calls), index_country_changes);
    static int order[max_tasks];
    for (int i = 0; i < max_tasks; ++i) order[i] = i;
    std::partial_sort(order, order + 20, order + max_tasks,
        [](int a, int b) { return totals.index_work[a] > totals.index_work[b]; });
    char line[1300];
    int used = std::snprintf(line, sizeof(line),
        "top_tasks (index:avg_us:times_longest:times_last:country)");
    for (int k = 0; k < 20 && used > 0 && used < static_cast<int>(sizeof(line)) - 64; ++k) {
        const int i = order[k];
        used += std::snprintf(line + used, sizeof(line) - used, " %d:%llu:%u:%u:%p", i,
            micros(t.index_work[i]) / t.calls, t.index_longest[i], t.index_last[i],
            index_country[i]);
    }
    log("%s", line);
}

void summarize_tasks() noexcept {
    const int begin = (std::max)(call_begin, 0);
    const int end = (std::min)(call_end, max_tasks);
    if (end <= begin) return;
    std::uint64_t first = ~0ull, last = 0, work = 0, longest = 0;
    int longest_index = begin, last_index = begin;
    DWORD seen[64];
    int distinct = 0;
    for (int i = begin; i < end; ++i) {
        if (!task_start[i]) return;  // a task was not recorded; skip this call
    }
    for (int i = begin; i < end; ++i) {
        const auto duration = task_end[i] - task_start[i];
        work += duration;
        totals.index_work[i] += duration;
        if (duration > longest) { longest = duration; longest_index = i; }
        if (task_end[i] > last) { last = task_end[i]; last_index = i; }
        first = (std::min)(first, task_start[i]);
        bool known = false;
        for (int k = 0; k < distinct; ++k) known = known || seen[k] == task_thread[i];
        if (!known && distinct < 64) seen[distinct++] = task_thread[i];
    }
    ++totals.calls;
    totals.wall += last - submit_begin;
    totals.work += work;
    totals.longest += longest;
    totals.ideal += (std::max)(work / static_cast<std::uint64_t>(distinct), longest);
    totals.first_lag += first > submit_begin ? first - submit_begin : 0;
    totals.last_start += task_start[last_index] - submit_begin;
    totals.threads += static_cast<std::uint64_t>(distinct);
    ++totals.index_longest[longest_index];
    if (tree_probe) treeprobe::phase_end(longest_index, longest);
    ++totals.index_last[last_index];
    if (totals.calls % 128 == 0) log_task_totals();
}

std::uint64_t submit_hook(void* context, void* unused, int begin, int end,
    void* function, void* on_done) {
    flagcache::begin_phase();
    const auto count = end > begin ? end - begin : 0;
    tasks += static_cast<std::uint64_t>(count);
    call_begin = begin;
    call_end = end;
    if (mode.load(std::memory_order_relaxed) == Mode::tasks) {
        for (int i = (std::max)(begin, 0); i < end && i < max_tasks; ++i) task_start[i] = 0;
        // Country pointer of each index, to see whether indices keep their country.
        auto* impl = function ? *reinterpret_cast<std::uint8_t**>(
            static_cast<std::uint8_t*>(function) + function_impl_offset) : nullptr;
        if (impl) {
            auto* array = *reinterpret_cast<std::uint8_t**>(impl + 0x10);
            auto* items = *reinterpret_cast<void* const**>(array + 8);
            for (int i = (std::max)(begin, 0); i < end && i < max_tasks; ++i) {
                if (index_country[i] && index_country[i] != items[i]) ++index_country_changes;
                index_country[i] = items[i];
            }
        }
    }

    if (mode.load(std::memory_order_relaxed) == Mode::serial) {
        auto* impl = function ? *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(function) + function_impl_offset) : nullptr;
        const auto* done_impl = on_done ? *reinterpret_cast<void* const*>(
            static_cast<const std::uint8_t*>(on_done) + function_impl_offset) : nullptr;
        // Only the shape seen in 1.37.5: a callable task and no completion callback.
        if (impl && !done_impl) {
            const auto task = reinterpret_cast<TaskFn>((*reinterpret_cast<void***>(impl))[2]);
            const auto start = now();
            for (int index = begin; index < end; ++index) {
                int argument = index;
                task(impl, &argument);
            }
            const auto elapsed = now() - start;
            serial_ticks += elapsed;
            flagcache::end_phase();
            *reinterpret_cast<int*>(static_cast<std::uint8_t*>(context) + context_mode_offset) =
                skip_wait_mode;
            count_call(elapsed);
            return 0;
        }
        ++fallbacks;
    }

    const auto start = now();
    const auto value = original_submit(context, unused, begin, end, function, on_done);
    const auto elapsed = now() - start;
    submit_ticks += elapsed;
    submit_begin = start;
    const int engine_mode = *reinterpret_cast<int*>(
        static_cast<std::uint8_t*>(context) + context_mode_offset);
    if (engine_mode != 2) {  // the wait hook will not run
        flagcache::end_phase();
        count_call(elapsed);
    }
    return value;
}

std::uint64_t wait_hook(void* context) {
    const auto start = now();
    const auto value = original_wait(context);
    flagcache::end_phase();
    const auto end = now();
    wait_ticks += end - start;
    if (mode.load(std::memory_order_relaxed) == Mode::tasks) summarize_tasks();
    count_call(end - submit_begin);
    return value;
}

bool paths_for_module(HMODULE module, std::wstring& directory) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    directory = path;
    log_path = directory + L"\\eu4_ai_serial.log";
    return true;
}

Mode read_mode(const std::wstring& directory) {
    const auto ini = directory + L"\\eu4_ai_serial.ini";
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"ai", L"mode", L"disabled", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    if (_wcsicmp(value, L"serial") == 0) return Mode::serial;
    if (_wcsicmp(value, L"timing") == 0) return Mode::timing;
    if (_wcsicmp(value, L"tasks") == 0) return Mode::tasks;
    return Mode::disabled;
}

int hex_value(char symbol) noexcept {
    if (symbol >= '0' && symbol <= '9') return symbol - '0';
    if (symbol >= 'a' && symbol <= 'f') return symbol - 'a' + 10;
    return -1;
}

bool signatures_match(std::uintptr_t image, char* error, std::size_t error_size) noexcept {
    for (const auto& signature : signatures) {
        const auto length = std::strlen(signature.bytes) / 2;
        std::uint8_t expected[128]{};
        if (length > sizeof(expected)) {
            std::snprintf(error, error_size, "signature too long (%s)", signature.what);
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const int high = hex_value(signature.bytes[i * 2]);
            const int low = hex_value(signature.bytes[i * 2 + 1]);
            if (high < 0 || low < 0) {
                std::snprintf(error, error_size, "bad signature (%s)", signature.what);
                return false;
            }
            expected[i] = static_cast<std::uint8_t>(high * 16 + low);
        }
        if (!eu4build::code_matches(image, signature.rva, expected, length)) {
            std::snprintf(error, error_size, "bytes differ at rva 0x%llx (%s)",
                static_cast<unsigned long long>(signature.rva), signature.what);
            return false;
        }
    }
    return true;
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
        installation_status.store(3);
        return 0;
    }

    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char error[160]{};
    if (!signatures_match(image, error, sizeof(error))) {
        log("REFUSED: %s; no bytes changed", error);
        installation_status.store(-4);
        return 4;
    }

    mode.store(selected_mode);
    {
        const auto ini = directory + L"\\eu4_ai_serial.ini";
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"ai", L"unit_flag_cache", L"off", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        const auto cache_mode = _wcsicmp(value, L"on") == 0 ? flagcache::Mode::on :
            _wcsicmp(value, L"shadow") == 0 ? flagcache::Mode::shadow : flagcache::Mode::off;
        GetPrivateProfileStringW(L"ai", L"search_memo", L"off", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        const auto memo_mode = _wcsicmp(value, L"on") == 0 ? searchmemo::Mode::on :
            _wcsicmp(value, L"shadow") == 0 ? searchmemo::Mode::shadow : searchmemo::Mode::off;
        const bool memo_owns_site = memo_mode != searchmemo::Mode::off;
        if (!flagcache::install(image, cache_mode, error, sizeof(error), !memo_owns_site)) {
            log("REFUSED: %s; no bytes changed", error);
            installation_status.store(-10);
            return 10;
        }
        log("unit_flag_cache=%s", flagcache::mode_name());
        if (memo_owns_site && !searchmemo::install(image, memo_mode, error, sizeof(error))) {
            log("search_memo FAILED: %s; unit_flag_cache takes the call site", error);
            if (!flagcache::install(image, cache_mode, error, sizeof(error))) {
                log("REFUSED: %s", error);
                installation_status.store(-15);
                return 15;
            }
        }
        log("search_memo=%s (Bellows army_search_memo port) fort_memo=%d", searchmemo::mode_name(), searchmemo::fort_active() ? 1 : 0);
        GetPrivateProfileStringW(L"ai", L"country_ratio_cache", L"off", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        const auto ratio_mode = _wcsicmp(value, L"on") == 0 ? flagcache::Mode::on :
            _wcsicmp(value, L"shadow") == 0 ? flagcache::Mode::shadow : flagcache::Mode::off;
        if (!ratiocache::install(image, ratio_mode, error, sizeof(error))) {
            log("REFUSED: %s", error);
            installation_status.store(-11);
            return 11;
        }
        log("country_ratio_cache=%s", ratiocache::mode_name());
        GetPrivateProfileStringW(L"ai", L"province_score_cache", L"off", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        const auto score_mode = _wcsicmp(value, L"on") == 0 ? flagcache::Mode::on :
            _wcsicmp(value, L"shadow") == 0 ? flagcache::Mode::shadow : flagcache::Mode::off;
        if (!scorecache::install(image, score_mode, error, sizeof(error))) {
            log("REFUSED: %s", error);
            installation_status.store(-12);
            return 12;
        }
        log("province_score_cache=%s", scorecache::mode_name());
        GetPrivateProfileStringW(L"ai", L"path_cache", L"off", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        const auto path_mode = _wcsicmp(value, L"on") == 0 ? flagcache::Mode::on :
            _wcsicmp(value, L"shadow") == 0 ? flagcache::Mode::shadow : flagcache::Mode::off;
        if (!pathcache::install(image, path_mode, error, sizeof(error))) {
            log("REFUSED: %s", error);
            installation_status.store(-13);
            return 13;
        }
        log("path_cache=%s", pathcache::mode_name());
        if (GetPrivateProfileIntW(L"ai", L"tree_probe", 0, ini.c_str()) != 0) {
            tree_probe = treeprobe::install(image, error, sizeof(error));
            log("tree_probe %s %s", tree_probe ? "installed" : "FAILED", tree_probe ? "" : error);
        }
        GetPrivateProfileStringW(L"ai", L"tree_walk", L"off", value,
            static_cast<DWORD>(std::size(value)), ini.c_str());
        const auto walk_mode = _wcsicmp(value, L"probe") == 0 ? treewalk::Mode::probe :
            _wcsicmp(value, L"serial") == 0 ? treewalk::Mode::serial :
            _wcsicmp(value, L"parallel") == 0 ? treewalk::Mode::parallel : treewalk::Mode::off;
        const int walk_helpers = static_cast<int>(GetPrivateProfileIntW(L"ai", L"tree_workers", 4,
            ini.c_str()));
        if (!treewalk::install(image, walk_mode, walk_helpers, error, sizeof(error))) {
            log("REFUSED: %s", error);
            installation_status.store(-14);
            return 14;
        }
        log("tree_walk=%s", treewalk::mode_name());
    }
    original_submit = reinterpret_cast<SubmitFn>(image + eu4build::rva(submit_function_rva));
    original_wait = reinterpret_cast<WaitFn>(image + eu4build::rva(wait_function_rva));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

    speedpatch::CallPatch submit;
    speedpatch::CallPatch wait;
    bool prepared = speedpatch::prepare_call(
        reinterpret_cast<std::uint8_t*>(image + eu4build::rva(submit_call_rva)),
        reinterpret_cast<const void*>(original_submit),
        reinterpret_cast<const void*>(&submit_hook), submit, error, sizeof(error));
    if (prepared) {
        prepared = speedpatch::prepare_call(
            reinterpret_cast<std::uint8_t*>(image + eu4build::rva(wait_call_rva)),
            reinterpret_cast<const void*>(original_wait),
            reinterpret_cast<const void*>(&wait_hook), wait, error, sizeof(error));
    }
    if (!prepared) {
        log("REFUSED: %s; no bytes changed", error);
        speedpatch::discard_call(submit);
        speedpatch::discard_call(wait);
        installation_status.store(-5);
        return 5;
    }
    if (selected_mode == Mode::tasks) {
        const auto ini = directory + L"\\eu4_ai_serial.ini";
        if (GetPrivateProfileIntW(L"ai", L"sample_heavy", 0, ini.c_str()) != 0) {
            sample_heavy = heavy::start(directory + L"\\eu4_ai_serial_heavy_stacks.csv");
            log("heavy task sampler %s", sample_heavy ? "started" : "FAILED to start");
        }
        auto* slot = reinterpret_cast<void**>(image + eu4build::rva(task_vtable_slot_rva));
        if (*slot != reinterpret_cast<void*>(image + eu4build::rva(task_function_rva))) {
            log("REFUSED: task vtable slot differs; no bytes changed");
            speedpatch::discard_call(submit);
            speedpatch::discard_call(wait);
            installation_status.store(-8);
            return 8;
        }
        original_task = reinterpret_cast<TaskFn>(image + eu4build::rva(task_function_rva));
        DWORD old_protection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protection)) {
            log("REFUSED: cannot make the task vtable slot writable");
            speedpatch::discard_call(submit);
            speedpatch::discard_call(wait);
            installation_status.store(-9);
            return 9;
        }
        *slot = reinterpret_cast<void*>(&task_hook);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
    }
    speedpatch::CallPatch monthly;
    // sample_monthly=1: stacks + duration of every monthly tick; 2: duration only
    const UINT monthly_option = GetPrivateProfileIntW(L"ai", L"sample_monthly", 0,
        (directory + L"\\eu4_ai_serial.ini").c_str());
    if (monthly_option != 0) {
        original_monthly = reinterpret_cast<MonthlyFn>(image + eu4build::rva(monthly_function_rva));
        if (speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(monthly_call_rva)),
                reinterpret_cast<const void*>(original_monthly),
                reinterpret_cast<const void*>(&monthly_hook), monthly, error, sizeof(error))) {
            sample_monthly = monthly_option == 1 &&
                heavy::start(directory + L"\\eu4_ai_serial_monthly_stacks.csv", 1);
            if (speedpatch::commit_call(monthly, error, sizeof(error))) {
                log("monthly sampler %s (call 140758bc3 changed)",
                    sample_monthly ? "started" : "off, timing only");
            } else {
                log("monthly sampler REFUSED: %s", error);
                sample_monthly = false;
            }
        } else {
            log("monthly sampler REFUSED: %s", error);
            speedpatch::discard_call(monthly);
        }
    }
    // The wait hook first: until submit is redirected it only measures.
    if (!speedpatch::commit_call(wait, error, sizeof(error)) ||
        !speedpatch::commit_call(submit, error, sizeof(error))) {
        log("REFUSED: %s; rolling back", error);
        speedpatch::discard_call(submit);
        speedpatch::discard_call(wait);
        installation_status.store(-6);
        return 6;
    }

    installation_status.store(selected_mode == Mode::serial ? 1 : 2);
    log("initialized version=%ls mode=%s; two CALL sites in 14077d8a0 changed", version,
        selected_mode == Mode::serial ? "serial" :
            selected_mode == Mode::tasks ? "tasks (task vtable slot changed too)" : "timing");
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

extern "C" __declspec(dllexport) LONG EU4AiSerialStatus() {
    return installation_status.load();
}
