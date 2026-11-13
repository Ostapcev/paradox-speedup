// HOI4 Supply Patch 0.3.0, HOI4 1.19.4.2 x64 (modes: off, timing, shadow, verify, on — see memo.inc / memo_on.inc).
//
// mode=timing: measures CSupplySystem hourly update without changing anything.
//   * whole supply update: call 140f45d10 at 1401f59af (CGameState::HourlyUpdate)
//   * heavy phase:         call 140f35bb0 at 140f4b8d2 (parallel-for over countries)
//   * per-country task:    call 1412ac0c0 at 140f4d0de (inside lambda 140f4d070);
//                          rcx = supply country (800-byte records at [system+0x120])
// Every 30 s it logs per-hour averages: supply wall, phase wall, sum of task times,
// longest task, worker count and the heaviest countries.
//
// mode=off: no bytes changed.

#include <windows.h>

#include "hoi4_build.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cstdlib>
#include <string>

#include "call_patch.hpp"

namespace {

constexpr std::uintptr_t supply_site_rva = 0x1f59a0;
constexpr std::uint8_t supply_bytes[] = {0x49, 0x8d, 0x8f, 0xd8, 0x03, 0x00, 0x00, 0xe8,
    0xd4, 0x7d, 0xfe, 0xff, 0x48, 0x8b, 0xc8, 0xe8, 0x5c, 0x03, 0xd5, 0x00};
constexpr std::uintptr_t supply_call_rva = 0x1f59af;
constexpr std::uintptr_t supply_function_rva = 0xf45d10;

constexpr std::uintptr_t phase_site_rva = 0xf4b8c2;
constexpr std::uint8_t phase_bytes[] = {0x4c, 0x8d, 0x4c, 0x24, 0x30, 0x45, 0x33, 0xc0, 0x48,
    0x8d, 0x55, 0xa0, 0x48, 0x8d, 0x4d, 0x18, 0xe8, 0xd9, 0xa2, 0xfe, 0xff};
constexpr std::uintptr_t phase_call_rva = 0xf4b8d2;
constexpr std::uintptr_t phase_function_rva = 0xf35bb0;

constexpr std::uintptr_t task_site_rva = 0xf4d0c5;
constexpr std::uint8_t task_bytes[] = {0x4a, 0x63, 0x04, 0x81, 0x4c, 0x8b, 0x82, 0x08, 0x01,
    0x00, 0x00, 0x48, 0x69, 0xc8, 0x20, 0x03, 0x00, 0x00, 0x4d, 0x8b, 0x04, 0xc0, 0x49, 0x03,
    0x09, 0xe8, 0xdd, 0xef, 0x35, 0x00};
constexpr std::uintptr_t task_call_rva = 0xf4d0de;
constexpr std::uintptr_t task_function_rva = 0x12ac0c0;

using SupplyFn = void (*)(void*);
using PhaseFn = void (*)(void*, void*, void*, void*, void*);
using TaskFn = void (*)(void*, void*, void*, void*);

constexpr int max_countries = 1024;
constexpr int max_threads = 64;

std::uintptr_t image = 0;
std::wstring log_path;
std::mutex log_mutex;
std::int64_t qpc_frequency = 1;
SupplyFn original_supply = nullptr;
PhaseFn original_phase = nullptr;
TaskFn original_task = nullptr;

std::atomic<std::uintptr_t> country_base{0};  // [system+0x120] seen in the task hook
std::atomic<std::uint64_t> country_qpc[max_countries];
std::atomic<std::uint32_t> country_calls[max_countries];
std::atomic<std::uint32_t> thread_ids[max_threads];

std::atomic<std::uint64_t> supply_qpc{0}, supply_runs{0};
std::atomic<std::uint64_t> phase_qpc{0}, phase_runs{0};
std::atomic<std::uint64_t> task_qpc{0}, task_runs{0}, busy_tasks{0};
std::atomic<std::uint64_t> longest_sum{0};       // sum over phases of the longest task
std::atomic<std::uint64_t> current_longest{0};   // within the running phase

// Fingerprint of the 800-byte country record after the task: every qword that does not
// look like a pointer (pointers change on reallocation and say nothing about content).
// Compared with the same country's fingerprint from the previous hour.
constexpr int record_qwords = 800 / 8;
std::uint64_t previous_print[max_countries];
std::uint64_t previous_words[max_countries][record_qwords];
bool previous_valid[max_countries];
std::atomic<std::uint64_t> same_prints{0}, changed_prints{0};
std::atomic<std::uint32_t> changed_offset[record_qwords];

// Heap/image addresses on Win64 are >= 2^40 and 8-aligned; game numbers (fixed point x100000)
// are far below 2^40. (0.2.0 used >= 0x10000, which hid most supply amounts.)
inline bool looks_like_pointer(std::uint64_t value) noexcept {
    return value >= (1ull << 40) && value < (1ull << 47) && (value & 7) == 0;
}

// Containers in the record: {T* data; int capacity; int size; ...}, size at data+0xc.
struct ContainerSpec { std::uint32_t offset; std::uint32_t stride; const char* name; };
constexpr ContainerSpec containers[] = {
    {0xb8, 0xe8, "nodes"}, {0x148, 4, "visited"}, {0x1e8, 16, "result"},
    {0x200, 24, "list200"}, {0x278, 4, "list278"}, {0x2c0, 8, "map2c0"}, {0x2f0, 8, "map2f0"},
};
constexpr int container_count = sizeof(containers) / sizeof(containers[0]);
std::uint64_t previous_content[max_countries][container_count];
std::atomic<std::uint64_t> content_changed[container_count];
std::atomic<std::uint64_t> content_bad[container_count];
std::atomic<std::uint64_t> all_same{0}, any_changed{0};

bool readable(const void* p, std::size_t n) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (const std::uint8_t* q : {b, b + n - 1}) {
        if (!VirtualQuery(q, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !info.Protect) return false;
    }
    return true;
}

// Hash of the container content; qwords that look like pointers are skipped. ~0 = unreadable.
std::uint64_t content_hash(const std::uint8_t* record, const ContainerSpec& spec) noexcept {
    const auto data = *reinterpret_cast<const std::uint8_t* const*>(record + spec.offset);
    const auto size = *reinterpret_cast<const std::int32_t*>(record + spec.offset + 0xc);
    std::uint64_t h = 1469598103934665603ull ^ static_cast<std::uint32_t>(size);
    if (size <= 0) return h;
    const std::size_t bytes = static_cast<std::size_t>(size) * spec.stride;
    if (size > 4000000 || !data || !readable(data, bytes)) return ~0ull;
    const std::size_t qwords = bytes / 8;
    const auto* q = reinterpret_cast<const std::uint64_t*>(data);
    for (std::size_t i = 0; i < qwords; ++i) {
        const std::uint64_t w = looks_like_pointer(q[i]) ? 0 : q[i];
        h = (h ^ w) * 1099511628211ull;
    }
    for (std::size_t i = qwords * 8; i < bytes; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void log(const char* format, ...) noexcept {
    char text[1024];
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

void note_thread() noexcept {
    const auto id = static_cast<std::uint32_t>(GetCurrentThreadId());
    for (auto& slot : thread_ids) {
        std::uint32_t value = slot.load(std::memory_order_relaxed);
        if (value == id) return;
        if (value == 0 && slot.compare_exchange_strong(value, id)) return;
        if (value == id) return;
    }
}

void supply_hook(void* system) {
    const auto start = qpc_now();
    original_supply(system);
    supply_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
    supply_runs.fetch_add(1, std::memory_order_relaxed);
}

void phase_hook(void* a, void* b, void* c, void* d, void* e) {
    current_longest.store(0, std::memory_order_relaxed);
    const auto start = qpc_now();
    original_phase(a, b, c, d, e);
    phase_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - start), std::memory_order_relaxed);
    phase_runs.fetch_add(1, std::memory_order_relaxed);
    longest_sum.fetch_add(current_longest.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void task_hook(void* country, void* system, void* c, void* d) {
    const auto start = qpc_now();
    original_task(country, system, c, d);
    const auto spent = static_cast<std::uint64_t>(qpc_now() - start);
    task_qpc.fetch_add(spent, std::memory_order_relaxed);
    task_runs.fetch_add(1, std::memory_order_relaxed);
    const auto base = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(system) + 0x120);
    country_base.store(base, std::memory_order_relaxed);
    const auto index = (reinterpret_cast<std::uintptr_t>(country) - base) / 800;
    if (index < max_countries) {
        country_qpc[index].fetch_add(spent, std::memory_order_relaxed);
        country_calls[index].fetch_add(1, std::memory_order_relaxed);
    }
    if (*reinterpret_cast<const int*>(reinterpret_cast<std::uintptr_t>(country) + 0x1a8) != -1) {
        busy_tasks.fetch_add(1, std::memory_order_relaxed);
        if (index < max_countries) {  // one task per country per hour: no race on index
            const auto* words = static_cast<const std::uint64_t*>(country);
            std::uint64_t print = 1469598103934665603ull;
            for (int i = 0; i < record_qwords; ++i) {
                const std::uint64_t w = looks_like_pointer(words[i]) ? 0 : words[i];
                print = (print ^ w) * 1099511628211ull;
                if (previous_valid[index] && previous_words[index][i] != w) {
                    changed_offset[i].fetch_add(1, std::memory_order_relaxed);
                }
                previous_words[index][i] = w;
            }
            if (previous_valid[index]) {
                (print == previous_print[index] ? same_prints : changed_prints)
                    .fetch_add(1, std::memory_order_relaxed);
            }
            bool changed_any = print != previous_print[index];
            for (int c = 0; c < container_count; ++c) {
                const auto h = content_hash(static_cast<const std::uint8_t*>(country), containers[c]);
                if (h == ~0ull) content_bad[c].fetch_add(1, std::memory_order_relaxed);
                if (previous_valid[index] && h != previous_content[index][c]) {
                    content_changed[c].fetch_add(1, std::memory_order_relaxed);
                    changed_any = true;
                }
                previous_content[index][c] = h;
            }
            if (previous_valid[index]) {
                (changed_any ? any_changed : all_same).fetch_add(1, std::memory_order_relaxed);
            }
            previous_print[index] = print;
            previous_valid[index] = true;
        }
    }
    std::uint64_t longest = current_longest.load(std::memory_order_relaxed);
    while (spent > longest &&
           !current_longest.compare_exchange_weak(longest, spent, std::memory_order_relaxed)) {
    }
    note_thread();
}

double ms(std::uint64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpc_frequency);
}

#include "memo.inc"
#include "memo_on.inc"

DWORD WINAPI reporter(void*) {
    std::uint64_t last_supply = 0, last_supply_runs = 0, last_phase = 0, last_phase_runs = 0,
        last_task = 0, last_task_runs = 0, last_busy = 0, last_longest = 0;
    static std::uint64_t last_country[max_countries]{};
    for (;;) {
        Sleep(30000);
        const auto s = supply_qpc.load(), sr = supply_runs.load();
        const auto p = phase_qpc.load(), pr = phase_runs.load();
        const auto t = task_qpc.load(), tr = task_runs.load(), b = busy_tasks.load();
        const auto l = longest_sum.load();
        const auto runs = pr - last_phase_runs;
        memo_report();
        on_report();
        if (runs == 0) continue;
        int threads = 0;
        for (auto& slot : thread_ids) threads += slot.load() != 0;
        log("per hour: supply=%.2f ms phase=%.2f ms tasks_sum=%.2f ms longest_task=%.2f ms "
            "tasks=%.0f busy=%.0f threads=%d hours=%llu",
            sr - last_supply_runs ? ms(s - last_supply) / static_cast<double>(sr - last_supply_runs) : 0.0,
            ms(p - last_phase) / static_cast<double>(runs),
            ms(t - last_task) / static_cast<double>(runs),
            ms(l - last_longest) / static_cast<double>(runs),
            static_cast<double>(tr - last_task_runs) / static_cast<double>(runs),
            static_cast<double>(b - last_busy) / static_cast<double>(runs), threads,
            static_cast<unsigned long long>(runs));
        // top 8 countries by time in this window
        std::uint64_t delta[max_countries];
        for (int i = 0; i < max_countries; ++i) {
            const auto now = country_qpc[i].load();
            delta[i] = now - last_country[i];
            last_country[i] = now;
        }
        char line[600];
        int length = std::snprintf(line, sizeof(line), "top countries (index: ms/hour):");
        for (int k = 0; k < 8; ++k) {
            int best = -1;
            for (int i = 0; i < max_countries; ++i) {
                if (delta[i] && (best < 0 || delta[i] > delta[best])) best = i;
            }
            if (best < 0) break;
            length += std::snprintf(line + length, sizeof(line) - length, " %d:%.2f", best,
                ms(delta[best]) / static_cast<double>(runs));
            delta[best] = 0;
        }
        log("%s", line);
        const auto same = same_prints.exchange(0), changed = changed_prints.exchange(0);
        length = std::snprintf(line, sizeof(line),
            "fingerprint vs previous hour: same=%llu changed=%llu (%.1f%% same); most changed offsets:",
            static_cast<unsigned long long>(same), static_cast<unsigned long long>(changed),
            same + changed ? 100.0 * static_cast<double>(same) / static_cast<double>(same + changed) : 0.0);
        std::uint32_t counts[record_qwords];
        for (int i = 0; i < record_qwords; ++i) counts[i] = changed_offset[i].exchange(0);
        for (int k = 0; k < 12; ++k) {
            int best = -1;
            for (int i = 0; i < record_qwords; ++i) {
                if (counts[i] && (best < 0 || counts[i] > counts[best])) best = i;
            }
            if (best < 0) break;
            length += std::snprintf(line + length, sizeof(line) - length, " +0x%x:%u", best * 8,
                counts[best]);
            counts[best] = 0;
        }
        log("%s", line);
        {
            const auto same_all = all_same.exchange(0), changed_all = any_changed.exchange(0);
            length = std::snprintf(line, sizeof(line),
                "record+containers vs previous hour: same=%llu changed=%llu (%.1f%% same); changed by container:",
                static_cast<unsigned long long>(same_all), static_cast<unsigned long long>(changed_all),
                same_all + changed_all ? 100.0 * static_cast<double>(same_all) /
                    static_cast<double>(same_all + changed_all) : 0.0);
            for (int c = 0; c < container_count; ++c) {
                length += std::snprintf(line + length, sizeof(line) - length, " %s:%llu(bad %llu)",
                    containers[c].name,
                    static_cast<unsigned long long>(content_changed[c].exchange(0)),
                    static_cast<unsigned long long>(content_bad[c].exchange(0)));
            }
            log("%s", line);
        }
        last_supply = s; last_supply_runs = sr; last_phase = p; last_phase_runs = pr;
        last_task = t; last_task_runs = tr; last_busy = b; last_longest = l;
    }
}

bool hook(std::uintptr_t site_rva, const std::uint8_t* bytes, std::size_t size,
          std::uintptr_t call_rva, const void* target, const char* name, bool check = true) {
    if (check && std::memcmp(reinterpret_cast<void*>(image + site_rva), bytes, size) != 0) {
        log("%s hook REFUSED (call site bytes differ)", name);
        return false;
    }
    char error[160]{};
    speedpatch::CallPatch patch;
    const auto offset = call_rva - site_rva;
    if (!speedpatch::prepare_call_bytes(reinterpret_cast<std::uint8_t*>(image + call_rva),
            bytes + offset, 5, target, patch, error, sizeof(error)) ||
        !speedpatch::commit_call(patch, error, sizeof(error))) {
        speedpatch::discard_call(patch);
        log("%s hook REFUSED (%s)", name, error);
        return false;
    }
    return true;
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\hoi4_supply_patch.log";
    const auto ini = directory + L"\\hoi4_supply_patch.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"hoi4.exe") != 0) return 3;

    wchar_t value[32]{};
    GetPrivateProfileStringW(L"supply", L"mode", L"off", value, 32, ini.c_str());
    const bool verify = _wcsicmp(value, L"verify") == 0;
    const bool on = _wcsicmp(value, L"on") == 0;
    const bool shadow = _wcsicmp(value, L"shadow") == 0 || verify || on;
    memo_mode = on ? MemoMode::on : verify ? MemoMode::verify : MemoMode::shadow;
    if (_wcsicmp(value, L"timing") != 0 && !shadow) {
        log("disabled by configuration (mode=off); no bytes changed");
        return 0;
    }
    unsigned exe_stamp = 0, exe_size = 0;
    const char* exe_build = hoi4build::detect(&exe_stamp, &exe_size);
    if (!exe_build) {
        log("REFUSED: unknown hoi4.exe build (stamp %08x, image size %x); no bytes changed",
            exe_stamp, exe_size);
        return 5;
    }
    log("hoi4.exe build %s", exe_build);
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    original_supply = reinterpret_cast<SupplyFn>(image + supply_function_rva);
    original_phase = reinterpret_cast<PhaseFn>(image + phase_function_rva);
    original_task = reinterpret_cast<TaskFn>(image + task_function_rva);
    const bool s = hook(supply_site_rva, supply_bytes, sizeof(supply_bytes), supply_call_rva,
        reinterpret_cast<const void*>(&supply_hook), "supply");
    const bool p = hook(phase_site_rva, phase_bytes, sizeof(phase_bytes), phase_call_rva,
        reinterpret_cast<const void*>(&phase_hook), "phase");
    bool t = false, d = false;
    if (shadow) {
        original_c610 = reinterpret_cast<DijkstraFn>(image + dijkstra_c610_rva);
        original_d990 = reinterpret_cast<DijkstraFn>(image + dijkstra_d990_rva);
        d = hook(dijkstra_site_rva, dijkstra_bytes, sizeof(dijkstra_bytes), dijkstra_c610_call_rva,
                reinterpret_cast<const void*>(&c610_hook), "c610") &&
            hook(dijkstra_site_rva, dijkstra_bytes, sizeof(dijkstra_bytes), dijkstra_d990_call_rva,
                reinterpret_cast<const void*>(&d990_hook), "d990", false);
    } else {
        t = hook(task_site_rva, task_bytes, sizeof(task_bytes), task_call_rva,
            reinterpret_cast<const void*>(&task_hook), "task");
    }
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    log("initialized version=0.3.0 mode=%s supply=%d phase=%d task=%d dijkstra=%d",
        on ? "on" : verify ? "verify" : shadow ? "shadow" : "timing", s, p, t, d);
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
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
