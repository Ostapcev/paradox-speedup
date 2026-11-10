#pragma once

#include <cstdint>

namespace knowledgepatch {

struct SchedulerWaitStats {
    std::uint64_t calls = 0;
    std::uint64_t iterations = 0;
    std::uint64_t yields = 0;
    std::uint64_t max_iterations = 0;
};

SchedulerWaitStats scheduler_wait_stats() noexcept;
void reset_scheduler_wait_stats() noexcept;

}  // namespace knowledgepatch

extern "C" void knowledge_scheduler_efficient_wait(void* state) noexcept;
