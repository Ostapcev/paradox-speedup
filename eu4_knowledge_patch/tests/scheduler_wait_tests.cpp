#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "parallel_patch.hpp"
#include "scheduler_wait.hpp"

using knowledgepatch::reset_scheduler_wait_stats;
using knowledgepatch::scheduler_wait_stats;

extern "C" {
void scheduler_fixture(void* args);
extern const std::uint8_t scheduler_fixture_site[];
extern const std::uint8_t scheduler_fixture_resume[];
extern const std::uint8_t scheduler_fixture_top[];
extern const std::uint8_t knowledge_scheduler_wait_begin[];
extern const std::uint8_t knowledge_scheduler_wait_end[];
extern const std::uint8_t knowledge_scheduler_wait_resume_displacement[];
extern const std::uint8_t knowledge_scheduler_wait_top_displacement[];
}

namespace {

struct FixtureArgs {
    std::uint64_t unused0 = 0;
    std::uint64_t unused8 = 0;
    void* state = nullptr;
    std::uint64_t control = 0;
    std::uint64_t top = 0;
    std::uint64_t rbx = 0;
    std::uint64_t zero0 = 0;
    std::uint64_t zero1 = 0;
    std::uint64_t rsp = 0;
    std::uint64_t rdi = 0;
    std::uint64_t r12 = 0;
};
static_assert(offsetof(FixtureArgs, state) == 0x10, "fixture state offset");
static_assert(offsetof(FixtureArgs, control) == 0x18, "fixture control offset");

constexpr std::uint8_t expected[13] = {
    0x0F, 0x57, 0xC0, 0xF3, 0x0F, 0x7F, 0x44,
    0x24, 0x20, 0x48, 0x8B, 0x5F, 0x18};

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

void put_pointer(std::array<std::uint8_t, 0xC0>& state, std::size_t offset,
    const void* value) {
    const auto pointer = reinterpret_cast<std::uintptr_t>(value);
    std::memcpy(state.data() + offset, &pointer, sizeof(pointer));
}

void put_int(std::array<std::uint8_t, 0xC0>& state, std::size_t offset, int value) {
    std::memcpy(state.data() + offset, &value, sizeof(value));
}

void test_completion_without_callback() {
    std::array<std::uint8_t, 0xC0> state{};
    int completed = 1;
    put_pointer(state, 0x48, &completed);
    put_int(state, 0xA8, 4);
    std::thread worker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        completed = 4;
    });
    knowledge_scheduler_efficient_wait(state.data());
    worker.join();
    require(completed == 4, "completed-count wait returned too early");
}

void test_completion_callback() {
    std::array<std::uint8_t, 0xC0> state{};
    int completed = 4;
    int callback_finished = 0;
    int callback_present = 1;
    put_pointer(state, 0x38, &callback_present);
    put_pointer(state, 0x48, &completed);
    put_pointer(state, 0x58, &callback_finished);
    put_int(state, 0xA8, 4);
    std::thread worker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        callback_finished = 1;
    });
    knowledge_scheduler_efficient_wait(state.data());
    worker.join();
    require(callback_finished == 1, "completion-callback wait returned too early");
}

}  // namespace

int main() {
    reset_scheduler_wait_stats();
    test_completion_without_callback();
    test_completion_callback();
    auto stats = scheduler_wait_stats();
    require(stats.calls == 2 && stats.iterations > 0, "wait counters did not advance");

    const knowledgepatch::ParallelHookImage image{
        knowledge_scheduler_wait_begin, knowledge_scheduler_wait_end, nullptr,
        knowledge_scheduler_wait_resume_displacement,
        knowledge_scheduler_wait_top_displacement};
    knowledgepatch::ParallelPatch patch;
    char error[256]{};
    std::uint8_t wrong[sizeof(expected)];
    std::memcpy(wrong, expected, sizeof(wrong));
    wrong[0] = 0;
    require(!knowledgepatch::prepare_parallel_patch(
        const_cast<std::uint8_t*>(scheduler_fixture_site), scheduler_fixture_resume,
        scheduler_fixture_top, wrong, sizeof(wrong), image, 0, patch,
        error, sizeof(error)), "scheduler signature guard accepted wrong bytes");
    require(knowledgepatch::prepare_parallel_patch(
        const_cast<std::uint8_t*>(scheduler_fixture_site), scheduler_fixture_resume,
        scheduler_fixture_top, expected, sizeof(expected), image, 0, patch,
        error, sizeof(error)), error);

    std::array<std::uint8_t, 0xC0> state{};
    int completed = 4;
    put_pointer(state, 0x48, &completed);
    put_int(state, 0xA8, 4);
    FixtureArgs original;
    original.state = state.data();
    original.control = 0x123456789ABCDEF0ULL;
    scheduler_fixture(&original);
    require(original.top == 0 && original.rbx == original.control &&
        original.zero0 == 0 && original.zero1 == 0, "original fixture behavior");

    require(knowledgepatch::commit_parallel_patch(patch, error, sizeof(error)), error);
    FixtureArgs fallback;
    fallback.state = state.data();
    fallback.control = original.control;
    scheduler_fixture(&fallback);
    require(fallback.top == 0 && fallback.rbx == original.rbx &&
        fallback.zero0 == 0 && fallback.zero1 == 0, "hook changed non-waiting path");
    require(fallback.rsp == original.rsp &&
        fallback.rdi == reinterpret_cast<std::uint64_t>(&fallback) &&
        fallback.r12 == reinterpret_cast<std::uint64_t>(&fallback),
        "hook changed registers or stack alignment");

    state[0x50] = 1;
    const auto before = scheduler_wait_stats().calls;
    FixtureArgs waiting;
    waiting.state = state.data();
    waiting.control = original.control;
    scheduler_fixture(&waiting);
    stats = scheduler_wait_stats();
    require(waiting.top == 1 && waiting.zero0 == 0 && waiting.zero1 == 0,
        "drained branch did not return to the original loop head");
    require(waiting.rsp == original.rsp &&
        waiting.rdi == reinterpret_cast<std::uint64_t>(&waiting) &&
        waiting.r12 == reinterpret_cast<std::uint64_t>(&waiting),
        "wait branch changed nonvolatile registers or RSP");
    require(stats.calls == before + 1, "drained branch did not call efficient wait");

    knowledgepatch::rollback_parallel_patch(patch);
    require(std::memcmp(scheduler_fixture_site, expected, sizeof(expected)) == 0,
        "rollback did not restore scheduler bytes");
    FixtureArgs restored;
    restored.state = state.data();
    restored.control = original.control;
    scheduler_fixture(&restored);
    require(restored.top == 0 && restored.rbx == restored.control,
        "scheduler hook remained active after rollback");
    knowledgepatch::discard_parallel_patch(patch);
    std::puts("scheduler wait tests passed: completion predicate, registers, stack, fallback, rollback");
    return 0;
}
