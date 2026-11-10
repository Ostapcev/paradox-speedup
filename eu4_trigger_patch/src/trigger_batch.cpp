#include "trigger_batch.hpp"

#include <algorithm>

namespace triggerpatch {

void Batch::reset() noexcept {
    items_.clear();
    requested_ = 0;
    used_parallel_ = false;
}

bool Batch::add(void* option, std::uint32_t kind, bool requested) {
    if (!option || kind > 1) return false;
    items_.push_back(Item{option, kind, requested, false, false});
    if (requested) ++requested_;
    return true;
}

Item* Batch::find(void* option, std::uint32_t kind) noexcept {
    const auto found = std::find_if(items_.begin(), items_.end(), [&](const Item& item) {
        return item.option == option && item.kind == kind;
    });
    return found == items_.end() ? nullptr : &*found;
}

void Batch::work(void* opaque, std::size_t worker) noexcept {
    auto& context = *static_cast<RunContext*>(opaque);
    for (std::size_t index = worker; index < context.count; index += context.workers) {
        auto& item = context.items[index];
        if (!item.requested) continue;
        item.shadow = context.evaluator(context.world, item);
        item.evaluated = true;
    }
}

void Batch::run(knowledgepatch::ParallelPool& pool, void* world, Evaluator evaluator,
    bool allow_parallel) {
    if (!evaluator || items_.empty()) return;
    used_parallel_ = allow_parallel && requested_ >= 8 && pool.size() > 1;
    RunContext context{items_.data(), items_.size(),
        used_parallel_ ? pool.size() : std::size_t{1}, world, evaluator};
    if (used_parallel_) {
        pool.run(&Batch::work, &context);
    } else {
        Batch::work(&context, 0);
    }
}

}  // namespace triggerpatch
