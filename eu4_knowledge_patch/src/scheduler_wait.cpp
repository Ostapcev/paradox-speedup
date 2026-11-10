#include "scheduler_wait.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace knowledgepatch {
namespace {

std::atomic<std::uint64_t> wait_calls{0};
std::atomic<std::uint64_t> wait_iterations{0};
std::atomic<std::uint64_t> wait_yields{0};
std::atomic<std::uint64_t> wait_max_iterations{0};

std::uintptr_t read_pointer(std::uintptr_t address) noexcept {
    return *reinterpret_cast<const volatile std::uintptr_t*>(address);
}

std::int32_t read_int(std::uintptr_t address) noexcept {
    return *reinterpret_cast<const volatile std::int32_t*>(address);
}

bool scheduler_complete(const void* state) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(state);
    // This is the exact exit predicate at EU4 0x179eb3..0x179ede.
    // state+0x38: optional completion callback, state+0x58: callback status,
    // state+0x48: completed-count pointer, state+0xa8: final index.
    const auto callback = read_pointer(base + 0x38);
    const auto callback_finished = callback == 0 || read_int(read_pointer(base + 0x58)) != 0;
    const auto completed = read_int(read_pointer(base + 0x48));
    return callback_finished && completed >= read_int(base + 0xA8);
}

}  // namespace

SchedulerWaitStats scheduler_wait_stats() noexcept {
    return {wait_calls.load(std::memory_order_relaxed),
        wait_iterations.load(std::memory_order_relaxed),
        wait_yields.load(std::memory_order_relaxed),
        wait_max_iterations.load(std::memory_order_relaxed)};
}

void reset_scheduler_wait_stats() noexcept {
    wait_calls.store(0, std::memory_order_relaxed);
    wait_iterations.store(0, std::memory_order_relaxed);
    wait_yields.store(0, std::memory_order_relaxed);
    wait_max_iterations.store(0, std::memory_order_relaxed);
}

}  // namespace knowledgepatch

extern "C" void knowledge_scheduler_efficient_wait(void* state) noexcept {
    using namespace knowledgepatch;
    wait_calls.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t local_iterations = 0;
    std::uint64_t local_yields = 0;
    while (!scheduler_complete(state)) {
        ++local_iterations;
        _mm_pause();
        if ((local_iterations & 0x3F) == 0) {
            ++local_yields;
            SwitchToThread();
        }
    }
    wait_iterations.fetch_add(local_iterations, std::memory_order_relaxed);
    wait_yields.fetch_add(local_yields, std::memory_order_relaxed);
    auto previous = wait_max_iterations.load(std::memory_order_relaxed);
    while (previous < local_iterations &&
        !wait_max_iterations.compare_exchange_weak(previous, local_iterations,
            std::memory_order_relaxed)) {
    }
}
