#include <cstdint>
#include <cstdio>

#include "parallel_patch.hpp"

extern "C" {
void* trigger_original_target = nullptr;
void* trigger_finish_original = nullptr;
void trigger_first_hook();
void trigger_second_hook();
void trigger_finish_hook();
void trigger_entry_fixture(void* args);
extern const std::uint8_t trigger_entry_fixture_site[];
extern const std::uint8_t trigger_entry_fixture_continuation[];
extern const std::uint8_t trigger_batch_entry_begin[];
extern const std::uint8_t trigger_batch_entry_end[];
extern const std::uint8_t trigger_batch_entry_continuation_displacement[];

using Hook = std::uint64_t (*)(void*, void*);
static void* expected_option = nullptr;
static void* seen_trigger = nullptr;
static void* seen_context = nullptr;
static void* seen_root = nullptr;
static void* seen_filter = nullptr;
static void* seen_other = nullptr;
static unsigned first_calls = 0;
static unsigned second_calls = 0;
static unsigned finish_calls = 0;
static unsigned end_calls = 0;
static std::uint8_t seen_result = 0;
static std::uint64_t seen_started = 0;

std::uint64_t trigger_qpc_now() noexcept { return 123456789; }
std::uintptr_t trigger_batch_begin(void* root, void* filter, void* other) noexcept {
    seen_root = root;
    seen_filter = filter;
    seen_other = other;
    return 0x1122334455667788ULL;
}

std::uint64_t fake_evaluate(void* trigger, void* context) {
    seen_trigger = trigger;
    seen_context = context;
    return 0x123456789ABCDEF0ULL;
}

std::uint64_t fake_finish(void*) {
    ++finish_calls;
    return 0xFEDCBA9876543210ULL;
}

void trigger_first_verify(void* option, std::uint8_t result, std::uint64_t started) noexcept {
    ++first_calls;
    if (option != expected_option) std::fprintf(stderr, "bad first option\n");
    seen_result = result;
    seen_started = started;
}

void trigger_second_verify(void* option, std::uint8_t result, std::uint64_t started) noexcept {
    ++second_calls;
    if (option != expected_option) std::fprintf(stderr, "bad second option\n");
    seen_result = result;
    seen_started = started;
}

void trigger_batch_end() noexcept { ++end_calls; }
}

struct EntryArgs {
    std::uint64_t root, filter, other;
    std::uint64_t rax, rcx, rdx, r8, r9, r10, r11;
    std::uint64_t out_rax, out_rcx, out_rdx, out_r8, out_r9, out_r10, out_r11;
    std::uint64_t out_r15, out_rsp;
};

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main() {
    alignas(16) unsigned char option[0x100]{};
    alignas(16) unsigned char context[0x200]{};
    expected_option = option;
    trigger_original_target = reinterpret_cast<void*>(&fake_evaluate);
    trigger_finish_original = reinterpret_cast<void*>(&fake_finish);

    const auto first = reinterpret_cast<Hook>(&trigger_first_hook)(option + 0x30, context);
    if (first != 0x123456789ABCDEF0ULL) return fail("first RAX changed");
    if (first_calls != 1 || second_calls != 0) return fail("first verify count");
    if (seen_result != 0xF0 || seen_started != 123456789) return fail("first verify arguments");
    if (seen_trigger != option + 0x30 || seen_context != context) return fail("first original arguments");

    const auto second = reinterpret_cast<Hook>(&trigger_second_hook)(option + 0x30, context);
    if (second != 0x123456789ABCDEF0ULL) return fail("second RAX changed");
    if (second_calls != 1 || seen_result != 0xF0) return fail("second verify");

    using Finish = std::uint64_t (*)();
    const auto finished = reinterpret_cast<Finish>(&trigger_finish_hook)();
    if (finished != 0xFEDCBA9876543210ULL) return fail("finish RAX changed");
    if (finish_calls != 1 || end_calls != 1) return fail("finish calls");

    constexpr std::uint8_t expected[7]{0x4C, 0x8B, 0x3D, 0x66, 0xC3, 0xAA, 0x01};
    const knowledgepatch::ParallelHookImage image{
        trigger_batch_entry_begin, trigger_batch_entry_end, nullptr,
        trigger_batch_entry_continuation_displacement, nullptr};
    knowledgepatch::ParallelPatch patch;
    char error[180]{};
    if (!knowledgepatch::prepare_parallel_patch(
            const_cast<std::uint8_t*>(trigger_entry_fixture_site),
            trigger_entry_fixture_continuation, nullptr, expected, sizeof(expected),
            image, 0, patch, error, sizeof(error))) return fail(error);
    if (!knowledgepatch::commit_parallel_patch(patch, error, sizeof(error))) return fail(error);
    EntryArgs args{};
    args.root = 0x1001; args.filter = 0x1002; args.other = 0x1003;
    args.rax = 0x2001; args.rcx = 0x2002; args.rdx = 0x2003;
    args.r8 = 0x2004; args.r9 = 0x2005; args.r10 = 0x2006; args.r11 = 0x2007;
    trigger_entry_fixture(&args);
    if (seen_root != reinterpret_cast<void*>(0x1001) ||
        seen_filter != reinterpret_cast<void*>(0x1002) ||
        seen_other != reinterpret_cast<void*>(0x1003)) return fail("entry C++ arguments");
    if (args.out_rax != args.rax || args.out_rcx != args.rcx || args.out_rdx != args.rdx ||
        args.out_r8 != args.r8 || args.out_r9 != args.r9 || args.out_r10 != args.r10 ||
        args.out_r11 != args.r11) return fail("entry volatile register preservation");
    if (args.out_r15 != 0x1122334455667788ULL) return fail("entry R15 restoration");
    if (!args.out_rsp || args.out_rsp % 16 != 0) return fail("entry stack alignment");
    knowledgepatch::discard_parallel_patch(patch);

    std::puts("trigger hook tests passed, including executable batch entry");
    return 0;
}
