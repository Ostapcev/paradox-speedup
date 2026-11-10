// In-process stack sampler for the heaviest daily AI task (index 0).
//
// While a worker runs task 0, a dedicated thread suspends it every ~200 us,
// unwinds its stack with RtlVirtualUnwind and counts the stacks. Nothing is
// allocated while the worker is suspended (it may hold the heap lock).
#include "heavy_sampler.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace heavy {
namespace {

constexpr int max_depth = 64;
constexpr double interval_us = 200.0;

std::atomic<HANDLE> target{nullptr};   // duplicated handle of the worker running task 0
std::atomic<std::uint64_t> generation{0};
HANDLE wake_event = nullptr;
std::wstring output_path;
std::int64_t frequency = 1;

std::unordered_map<std::string, std::uint64_t> stacks;
std::map<std::uintptr_t, std::wstring> module_names;  // module base -> file name
std::uint64_t samples = 0;
std::uint64_t tasks_seen = 0;
std::uint64_t last_written_tasks = 0;
std::uint64_t write_every = 128;

thread_local HANDLE own_handle = nullptr;

std::wstring module_of(std::uintptr_t address, std::uintptr_t& base) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) || !module) {
        base = 0;
        return L"?";
    }
    base = reinterpret_cast<std::uintptr_t>(module);
    auto found = module_names.find(base);
    if (found != module_names.end()) return found->second;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    std::wstring result = name ? name + 1 : path;
    module_names[base] = result;
    return result;
}

// Unwinds a suspended thread. Returns the number of frames, leaf first.
int capture(HANDLE thread, std::uint64_t* frames) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(thread, &context)) return 0;
    int depth = 0;
    while (depth < max_depth && context.Rip) {
        frames[depth++] = context.Rip;
        DWORD64 image_base = 0;
        auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!entry) {
            // Leaf function without unwind data: return address is at the stack top.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context,
                &handler_data, &establisher, nullptr);
        }
    }
    return depth;
}

void record(const std::uint64_t* frames, int depth) {
    // Root first, like eu4_sampler stacks.csv: "module+0xRVA;...".
    std::string key;
    key.reserve(depth * 24);
    for (int i = depth - 1; i >= 0; --i) {
        // Return addresses point after the call; step back one byte so the
        // frame resolves inside the calling function (not for the leaf).
        const std::uintptr_t address = i == 0 ? frames[i] : frames[i] - 1;
        std::uintptr_t base = 0;
        const auto module = module_of(address, base);
        char part[160];
        std::snprintf(part, sizeof(part), "%ls+0x%llx", module.c_str(),
            static_cast<unsigned long long>(address - base));
        if (!key.empty()) key += ';';
        key += part;
    }
    ++stacks[key];
    ++samples;
}

void write_output() {
    const auto temporary = output_path + L".tmp";
    FILE* file = _wfopen(temporary.c_str(), L"wb");
    if (!file) return;
    std::fprintf(file, "# heavy_tasks=%llu samples=%llu interval_us=%.0f\n",
        static_cast<unsigned long long>(tasks_seen), static_cast<unsigned long long>(samples),
        interval_us);
    std::fprintf(file, "thread_id,estimated_cpu_ms,samples,frames\n");
    for (const auto& [key, count] : stacks) {
        std::fprintf(file, "0,%.3f,%llu,%s\n", count * interval_us / 1000.0,
            static_cast<unsigned long long>(count), key.c_str());
    }
    std::fclose(file);
    MoveFileExW(temporary.c_str(), output_path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void wait_interval() {
    LARGE_INTEGER start{};
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&start);
    const auto ticks = static_cast<std::int64_t>(interval_us * frequency / 1e6);
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

DWORD WINAPI sampler_thread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::uint64_t frames[max_depth];
    std::uint64_t seen_generation = 0;
    for (;;) {
        WaitForSingleObject(wake_event, INFINITE);
        const auto current = generation.load();
        if (current != seen_generation) {
            seen_generation = current;
            ++tasks_seen;
        }
        for (;;) {
            HANDLE thread = target.load();
            if (!thread || generation.load() != seen_generation) break;
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) break;
            int depth = 0;
            // Re-check after suspension: the task may have just finished.
            if (target.load() == thread && generation.load() == seen_generation) {
                depth = capture(thread, frames);
            }
            ResumeThread(thread);
            if (depth > 0) record(frames, depth);
            wait_interval();
        }
        if (tasks_seen - last_written_tasks >= write_every) {
            last_written_tasks = tasks_seen;
            write_output();
        }
    }
}

}  // namespace

bool start(const std::wstring& path, int every) {
    output_path = path;
    write_every = every > 0 ? static_cast<std::uint64_t>(every) : 1;
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    frequency = value.QuadPart > 0 ? value.QuadPart : 1;
    wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_event) return false;
    HANDLE thread = CreateThread(nullptr, 0, sampler_thread, nullptr, 0, nullptr);
    if (!thread) return false;
    CloseHandle(thread);
    return true;
}

void begin() {
    if (!wake_event) return;
    if (!own_handle) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
            &own_handle, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    }
    if (!own_handle) return;
    generation.fetch_add(1);
    target.store(own_handle);
    SetEvent(wake_event);
}

void end() {
    target.store(nullptr);
    // The sampler re-checks target while this thread is suspended, so once
    // target is cleared no new sample of this thread can start.
}

}  // namespace heavy
