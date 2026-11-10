#include "parallel_pool.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

struct Context {
    std::array<std::uint64_t, 8> per_worker{};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> workers{0};
};

void job(void* opaque, std::size_t worker) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    ++context.per_worker[worker];
    context.calls.fetch_add(1, std::memory_order_relaxed);
    context.workers.fetch_or(std::uint64_t{1} << worker, std::memory_order_relaxed);
    std::this_thread::yield();
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

}  // namespace

int main() {
    knowledgepatch::ParallelPool pool;
    Context context;
    pool.run(&job, &context);
    require(context.calls.load() == 0, "unstarted pool must not run jobs");
    require(pool.start(0) && pool.size() == 1, "zero workers must select the calling thread");
    pool.run(&job, &context);
    require(context.calls.load() == 1 && context.per_worker[0] == 1, "single-worker run");
    require(!pool.start(2), "starting an active pool must fail");
    pool.stop();
    pool.stop();
    require(pool.size() == 0, "stop must be idempotent");

    require(pool.start(4) && pool.size() == 4, "four-worker pool must start");
    for (int iteration = 0; iteration < 250; ++iteration) pool.run(&job, &context);
    require(context.calls.load() == 1001, "every run must execute every worker exactly once");
    require(context.workers.load() == 0x0f, "all four workers must participate");
    for (std::size_t index = 0; index < 4; ++index) {
        require(context.per_worker[index] == (index == 0 ? 251 : 250), "balanced per-run worker counts");
    }

    // Concurrent callers are serialized by run_mutex_; no generation may be lost.
    std::atomic<int> ready{0};
    std::vector<std::thread> callers;
    for (int index = 0; index < 3; ++index) {
        callers.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) != 3) std::this_thread::yield();
            for (int run = 0; run < 20; ++run) pool.run(&job, &context);
        });
    }
    for (auto& caller : callers) caller.join();
    require(context.calls.load() == 1241, "concurrent run callers must not lose generations");
    pool.stop();
    require(pool.start(2) && pool.size() == 2, "pool must restart after stop");
    pool.run(&job, &context);
    require(context.calls.load() == 1243, "restart must execute the new generation");
    pool.stop();
    std::puts("parallel pool tests passed: idle, single, four workers, concurrent runs, stop/restart");
    return 0;
}
