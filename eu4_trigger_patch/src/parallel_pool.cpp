#include "parallel_pool.hpp"

#include <algorithm>

namespace knowledgepatch {

bool ParallelPool::start(std::size_t total_workers) {
    std::lock_guard<std::mutex> lock(run_mutex_);
    if (total_workers_ || !threads_.empty()) return false;
    total_workers = (std::max)(std::size_t{1}, total_workers);
    stopping_ = false;
    remaining_ = 0;
    job_ = nullptr;
    context_ = nullptr;
    try {
        threads_.reserve(total_workers - 1);
        for (std::size_t index = 1; index < total_workers; ++index) {
            threads_.emplace_back([this, index]() { worker(index); });
        }
    } catch (...) {
        // run_mutex_ is already held here. Do not call stop(): it would try to
        // acquire the same mutex again while recovering from thread creation.
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopping_ = true;
            ++generation_;
        }
        work_available_.notify_all();
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
        threads_.clear();
        total_workers_ = 0;
        remaining_ = 0;
        job_ = nullptr;
        context_ = nullptr;
        return false;
    }
    total_workers_ = total_workers;
    return true;
}

void ParallelPool::run(Job job, void* context) {
    std::lock_guard<std::mutex> serialize(run_mutex_);
    if (!job || total_workers_ == 0) return;
    if (total_workers_ == 1) {
        job(context, 0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = job;
        context_ = context;
        remaining_ = threads_.size();
        ++generation_;
    }
    work_available_.notify_all();
    job(context, 0);
    std::unique_lock<std::mutex> lock(mutex_);
    work_done_.wait(lock, [this]() { return remaining_ == 0; });
    job_ = nullptr;
    context_ = nullptr;
}

void ParallelPool::worker(std::size_t index) noexcept {
    std::uint64_t observed = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_available_.wait(lock, [this, observed]() { return stopping_ || generation_ != observed; });
        if (stopping_) return;
        observed = generation_;
        const auto job = job_;
        auto* context = context_;
        lock.unlock();
        if (job) job(context, index);
        lock.lock();
        if (remaining_ > 0 && --remaining_ == 0) work_done_.notify_one();
    }
}

void ParallelPool::stop() noexcept {
    std::lock_guard<std::mutex> serialize(run_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        ++generation_;
    }
    work_available_.notify_all();
    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();
    total_workers_ = 0;
    remaining_ = 0;
    job_ = nullptr;
    context_ = nullptr;
}

}  // namespace knowledgepatch
