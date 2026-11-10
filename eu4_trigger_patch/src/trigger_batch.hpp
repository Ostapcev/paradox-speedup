#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "parallel_pool.hpp"

namespace triggerpatch {

struct Item {
    void* option = nullptr;
    std::uint32_t kind = 0;
    bool requested = false;
    bool evaluated = false;
    bool shadow = false;
};

using Evaluator = bool (*)(void* world, const Item& item) noexcept;

class Batch {
public:
    void reset() noexcept;
    bool add(void* option, std::uint32_t kind, bool requested);
    void run(knowledgepatch::ParallelPool& pool, void* world, Evaluator evaluator,
        bool allow_parallel);
    Item* find(void* option, std::uint32_t kind) noexcept;

    std::size_t size() const noexcept { return items_.size(); }
    std::size_t requested() const noexcept { return requested_; }
    bool used_parallel() const noexcept { return used_parallel_; }

private:
    struct RunContext {
        Item* items = nullptr;
        std::size_t count = 0;
        std::size_t workers = 1;
        void* world = nullptr;
        Evaluator evaluator = nullptr;
    };
    static void work(void* opaque, std::size_t worker) noexcept;

    std::vector<Item> items_;
    std::size_t requested_ = 0;
    bool used_parallel_ = false;
};

}  // namespace triggerpatch
