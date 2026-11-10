#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace knowledgepatch {

class ParallelPool {
public:
    using Job = void (*)(void* context, std::size_t worker_index) noexcept;

    ~ParallelPool() { stop(); }
    bool start(std::size_t total_workers);
    void run(Job job, void* context);
    void stop() noexcept;
    std::size_t size() const noexcept { return total_workers_; }

private:
    void worker(std::size_t index) noexcept;

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::mutex run_mutex_;
    std::condition_variable work_available_;
    std::condition_variable work_done_;
    Job job_ = nullptr;
    void* context_ = nullptr;
    std::uint64_t generation_ = 0;
    std::size_t remaining_ = 0;
    std::size_t total_workers_ = 0;
    bool stopping_ = false;
};

}  // namespace knowledgepatch
