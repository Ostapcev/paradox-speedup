// Diagnostic: time the AI node updates (vtable slot +0x60) of the 13 AI node
// types whose +0x58 is the tree walk 1400e4890, inside the heaviest daily AI task
// (task index 0). Also counts how many nodes of each type update per heavy task.
#include "tree_probe.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include "eu4_build.hpp"

namespace treeprobe {
namespace {

// vtable addresses holding 1400e4890 at +0x58 (vtable = address - 0x58)
constexpr std::uintptr_t walk_slot_rvas[] = {0x1c3b030, 0x1c3ce10, 0x1c3d3c0, 0x1c3df78,
    0x1c3edd8, 0x1c3f140, 0x1c3f6b8, 0x1c40090, 0x1c40490, 0x1c408c8, 0x1c42e20, 0x1c43160,
    0x1c436e8};
constexpr int kinds = sizeof(walk_slot_rvas) / sizeof(walk_slot_rvas[0]);
constexpr std::uintptr_t walk_rva = 0xe4890;

using UpdateFn = void (*)(void* node, void* arg);
UpdateFn originals[kinds]{};
std::uintptr_t update_rvas[kinds]{};
std::uintptr_t image_base = 0;

thread_local bool heavy = false;
thread_local int current_task = -1;
constexpr int max_tasks = 1024;
std::uint64_t task_kind[max_tasks][kinds];  // top-level node time per task (one writer per task)
std::uint64_t longest_kind[kinds];
std::uint64_t longest_total = 0, phases = 0;
thread_local int depth = 0;
std::atomic<std::uint64_t> ticks[kinds]{};
std::atomic<std::uint64_t> self_ticks[kinds]{};
std::atomic<std::uint64_t> calls[kinds]{};
std::atomic<std::uint64_t> heavy_tasks{0};
std::atomic<std::uint64_t> heavy_task_ticks{0};
thread_local std::uint64_t child_ticks = 0;
thread_local std::uint64_t heavy_start = 0;

inline std::uint64_t qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}

template <int K>
void update_hook(void* node, void* arg) {
    if (current_task >= 0 && depth == 0) {
        ++depth;
        const auto start = qpc();
        originals[K](node, arg);
        task_kind[current_task][K] += qpc() - start;
        --depth;
        if (!heavy) return;
        return;
    }
    if (!heavy) {
        originals[K](node, arg);
        return;
    }
    const auto saved_child = child_ticks;
    child_ticks = 0;
    ++depth;
    const auto start = qpc();
    originals[K](node, arg);
    const auto spent = qpc() - start;
    --depth;
    ticks[K].fetch_add(spent, std::memory_order_relaxed);
    self_ticks[K].fetch_add(spent - child_ticks, std::memory_order_relaxed);
    calls[K].fetch_add(1, std::memory_order_relaxed);
    child_ticks = saved_child + spent;
}

template <int... I>
struct Hooks {
    static constexpr UpdateFn table[] = {&update_hook<I>...};
};
using AllHooks = Hooks<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12>;

}  // namespace

bool install(std::uintptr_t image, char* error, std::size_t error_size) {
    image_base = image;
    for (int k = 0; k < kinds; ++k) {
        auto** walk = reinterpret_cast<void**>(image + eu4build::rva(walk_slot_rvas[k]));
        if (*walk != reinterpret_cast<void*>(image + eu4build::rva(walk_rva))) {
            std::snprintf(error, error_size, "tree probe: slot 0x%llx is not the walk",
                static_cast<unsigned long long>(walk_slot_rvas[k]));
            return false;
        }
    }
    for (int k = 0; k < kinds; ++k) {
        auto** slot = reinterpret_cast<void**>(image + eu4build::rva(walk_slot_rvas[k] + 8));  // +0x60
        originals[k] = reinterpret_cast<UpdateFn>(*slot);
        update_rvas[k] = reinterpret_cast<std::uintptr_t>(*slot) - image;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            std::snprintf(error, error_size, "tree probe: VirtualProtect %lu", GetLastError());
            return false;
        }
        *slot = reinterpret_cast<void*>(AllHooks::table[k]);
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    return true;
}

void begin_task(int index) {
    if (index < 0 || index >= max_tasks) return;
    current_task = index;
    for (auto& v : task_kind[index]) v = 0;
}

void end_task() { current_task = -1; }

void phase_end(int longest_index, std::uint64_t longest_ticks) {
    if (longest_index < 0 || longest_index >= max_tasks) return;
    for (int k = 0; k < kinds; ++k) longest_kind[k] += task_kind[longest_index][k];
    longest_total += longest_ticks;
    ++phases;
}

void begin_heavy() {
    heavy = true;
    heavy_start = qpc();
}

void end_heavy() {
    heavy = false;
    heavy_tasks.fetch_add(1, std::memory_order_relaxed);
    heavy_task_ticks.fetch_add(qpc() - heavy_start, std::memory_order_relaxed);
}

void report(void (*log)(const char*, ...) noexcept) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const double us = 1e6 / static_cast<double>(f.QuadPart);
    if (phases) {
        char line[900];
        int at = std::snprintf(line, sizeof(line), "tree_probe longest_task avg_us=%.0f by kind:",
            longest_total * us / phases);
        double covered = 0;
        for (int k = 0; k < kinds; ++k) {
            const double v = longest_kind[k] * us / phases;
            covered += v;
            if (v >= 1) at += std::snprintf(line + at, sizeof(line) - at, " %llx=%.0f",
                static_cast<unsigned long long>(0x140000000 + update_rvas[k]), v);
        }
        std::snprintf(line + at, sizeof(line) - at, " other=%.0f",
            longest_total * us / phases - covered);
        log("%s", line);
    }
    const auto tasks = heavy_tasks.load();
    if (!tasks) return;
    log("tree_probe heavy_tasks=%llu avg_task_us=%.0f", static_cast<unsigned long long>(tasks),
        heavy_task_ticks.load() * us / tasks);
    for (int k = 0; k < kinds; ++k) {
        const auto c = calls[k].load();
        if (!c) continue;
        log("tree_probe kind=%d vtable=%llx update=%llx calls_per_task=%.2f us_per_task=%.0f "
            "self_us_per_task=%.0f us_per_call=%.0f", k,
            static_cast<unsigned long long>(0x140000000 + walk_slot_rvas[k] - 0x58),
            static_cast<unsigned long long>(0x140000000 + update_rvas[k]),
            static_cast<double>(c) / tasks, ticks[k].load() * us / tasks,
            self_ticks[k].load() * us / tasks, ticks[k].load() * us / c);
    }
}

}  // namespace treeprobe
