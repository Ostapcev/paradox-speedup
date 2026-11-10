#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "parallel_pool.hpp"
#include "trigger_batch.hpp"

namespace {
std::atomic<unsigned> calls{0};

bool fake_evaluate(void* world, const triggerpatch::Item& item) noexcept {
    ++calls;
    const auto salt = *static_cast<unsigned*>(world);
    return ((reinterpret_cast<std::uintptr_t>(item.option) ^ salt ^ item.kind) & 1) != 0;
}

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}
}

int main() {
    knowledgepatch::ParallelPool pool;
    if (!pool.start(4)) return fail("pool start");
    triggerpatch::Batch batch;
    unsigned salt = 7;
    std::vector<std::uintptr_t> options(40);
    std::size_t expected_calls = 0;
    for (std::size_t index = 0; index < options.size(); ++index) {
        options[index] = 0x1000 + index * 0x10;
        const bool requested = index % 3 != 0;
        if (!batch.add(reinterpret_cast<void*>(options[index]),
                static_cast<std::uint32_t>(index % 2), requested)) {
            return fail("add");
        }
        if (requested) ++expected_calls;
    }
    calls.store(0);
    batch.run(pool, &salt, &fake_evaluate, true);
    if (!batch.used_parallel()) return fail("parallel threshold");
    if (calls.load() != expected_calls) return fail("evaluation count");
    for (std::size_t index = 0; index < options.size(); ++index) {
        auto* item = batch.find(reinterpret_cast<void*>(options[index]), index % 2);
        if (!item) return fail("find");
        if (item->evaluated != (index % 3 != 0)) return fail("evaluated flag");
        if (item->evaluated &&
            item->shadow != fake_evaluate(&salt, *item)) return fail("result mismatch");
    }

    batch.run(pool, &salt, &fake_evaluate, false);
    if (batch.used_parallel()) return fail("sequential fallback");
    triggerpatch::Batch small;
    for (std::size_t index = 0; index < 7; ++index) {
        small.add(reinterpret_cast<void*>(0x5000 + index * 0x10), 0, true);
    }
    small.run(pool, &salt, &fake_evaluate, true);
    if (small.used_parallel()) return fail("small batches must stay sequential");
    pool.stop();
    std::puts("trigger batch tests passed");
    return 0;
}
