#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "parallel_patch.hpp"
#include "two_phase.hpp"

extern "C" {
int apply_original(void* args);
int apply_fixture(void* args);
void verify_original(void* args);
void verify_fixture(void* args);
int timing_fixture(void* args);
extern const std::uint8_t apply_fixture_site[];
extern const std::uint8_t apply_fixture_fallback[];
extern const std::uint8_t apply_fixture_commit[];
extern const std::uint8_t timing_fixture_site[];
extern const std::uint8_t timing_fixture_fallback[];
extern const std::uint8_t verify_fixture_site[];
extern const std::uint8_t verify_fixture_continuation[];
extern const std::uint8_t knowledge_parallel_apply_begin[];
extern const std::uint8_t knowledge_parallel_apply_end[];
extern const std::uint8_t knowledge_parallel_apply_tls[];
extern const std::uint8_t knowledge_parallel_apply_commit_displacement[];
extern const std::uint8_t knowledge_parallel_apply_fallback_displacement[];
extern const std::uint8_t knowledge_parallel_verify_begin[];
extern const std::uint8_t knowledge_parallel_verify_end[];
extern const std::uint8_t knowledge_parallel_verify_continuation_displacement[];
extern const std::uint8_t knowledge_parallel_timing_begin[];
extern const std::uint8_t knowledge_parallel_timing_end[];
extern const std::uint8_t knowledge_parallel_timing_continuation_displacement[];
}

namespace {

struct Snapshot {
    const void* country;
    knowledgepatch::TagBits cores;
    knowledgepatch::TagBits claims;
    std::array<std::uint8_t, knowledgepatch::tag_count> pair_flags;
};
static_assert(offsetof(Snapshot, pair_flags) == 0x4008, "parallel flag offset");

struct ApplyArgs {
    const void* current;
    const void* other;
    void* relation;
    std::uint8_t expected;
    std::uint8_t padding[7];
    std::uint64_t initial_rax;
    std::uint64_t initial_rdx;
    void* scratch;
    std::uint64_t observed_rax;
    std::uint64_t observed_rdx;
    std::uint64_t observed_rsp;
};

struct VerifyArgs {
    const void* current;
    const void* other;
    const void* relation;
    std::uint64_t rax;
    std::uint64_t rcx;
    std::uint64_t rdx;
    std::uint64_t r8;
    std::uint64_t r9;
    std::uint64_t r11;
    std::uint64_t out_rax;
    std::uint64_t out_rcx;
    std::uint64_t out_rdx;
    std::uint64_t out_r8;
    std::uint64_t out_r9;
    std::uint64_t out_r11;
    std::uint64_t out_rdi;
    std::uint64_t out_r10;
    std::uint64_t out_r13;
    std::uint64_t observed_rsp;
};

constexpr std::uint8_t apply_expected[23] = {
    0x45, 0x32, 0xFF, 0x32, 0xC9, 0x45, 0x32, 0xC0,
    0x44, 0x88, 0x85, 0x40, 0x02, 0x00, 0x00, 0x45,
    0x32, 0xC9, 0x44, 0x88, 0x4C, 0x24, 0x38};
constexpr std::uint8_t verify_expected[9] = {
    0x4C, 0x8B, 0x75, 0x80, 0x48, 0x8B, 0x74, 0x24, 0x60};

const knowledgepatch::ParallelHookImage apply_image{
    knowledge_parallel_apply_begin, knowledge_parallel_apply_end,
    knowledge_parallel_apply_tls, knowledge_parallel_apply_commit_displacement,
    knowledge_parallel_apply_fallback_displacement};
const knowledgepatch::ParallelHookImage verify_image{
    knowledge_parallel_verify_begin, knowledge_parallel_verify_end, nullptr,
    knowledge_parallel_verify_continuation_displacement, nullptr};
const knowledgepatch::ParallelHookImage timing_image{
    knowledge_parallel_timing_begin, knowledge_parallel_timing_end, nullptr,
    knowledge_parallel_timing_continuation_displacement, nullptr};

int timing_calls = 0;
int verifier_calls = 0;
const void* verifier_current = nullptr;
const void* verifier_other = nullptr;
const void* verifier_relation = nullptr;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

void compare_verify(const VerifyArgs& actual, const VerifyArgs& expected) {
    require(actual.out_rax == expected.out_rax, "verifier changed RAX");
    require(actual.out_rcx == expected.out_rcx, "verifier changed RCX");
    require(actual.out_rdx == expected.out_rdx, "verifier changed RDX");
    require(actual.out_r8 == expected.out_r8, "verifier changed R8");
    require(actual.out_r9 == expected.out_r9, "verifier changed R9");
    require(actual.out_r11 == expected.out_r11, "verifier changed R11");
    require(actual.out_rdi == expected.out_rdi, "verifier changed RDI");
    require(actual.out_r10 == expected.out_r10, "verifier changed R10");
    require(actual.out_r13 == expected.out_r13, "verifier changed R13");
    require(actual.observed_rsp == expected.observed_rsp, "verifier changed RSP");
}

}  // namespace

extern "C" void knowledge_parallel_timing_start() noexcept {
    ++timing_calls;
}

extern "C" void knowledge_parallel_verify(
    void* current, void* other, void* relation) noexcept {
    ++verifier_calls;
    verifier_current = current;
    verifier_other = other;
    verifier_relation = relation;
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    const auto* exception = info->ExceptionRecord;
    const auto* context = info->ContextRecord;
    std::fprintf(stderr,
        "EXCEPTION code=%08lx address=%p rip=%llx rsp=%llx rax=%llx rcx=%llx rdx=%llx\n",
        exception->ExceptionCode, exception->ExceptionAddress, context->Rip,
        context->Rsp, context->Rax, context->Rcx, context->Rdx);
    ExitProcess(200);
}

int main() {
    AddVectoredExceptionHandler(1, crash_handler);
    const DWORD slot = TlsAlloc();
    require(slot != TLS_OUT_OF_INDEXES && slot < 64, "direct TLS slot unavailable");
    const auto tls_offset = static_cast<std::uint32_t>(0x1480 + slot * 8);
    knowledgepatch::ParallelPatch apply_patch;
    knowledgepatch::ParallelPatch verify_patch;
    knowledgepatch::ParallelPatch timing_patch;
    char error[256]{};
    const auto cleanup = [&]() {
        knowledgepatch::discard_parallel_patch(timing_patch);
        knowledgepatch::discard_parallel_patch(verify_patch);
        knowledgepatch::discard_parallel_patch(apply_patch);
        TlsFree(slot);
    };

    std::uint8_t wrong[sizeof(apply_expected)];
    std::memcpy(wrong, apply_expected, sizeof(wrong));
    wrong[0] = 0;
    require(!knowledgepatch::prepare_parallel_patch(
        const_cast<std::uint8_t*>(apply_fixture_site), apply_fixture_commit,
        apply_fixture_fallback, wrong, sizeof(wrong), apply_image, tls_offset,
        apply_patch, error, sizeof(error)), "apply signature guard accepted wrong bytes");
    require(knowledgepatch::prepare_parallel_patch(
        const_cast<std::uint8_t*>(apply_fixture_site), apply_fixture_commit,
        apply_fixture_fallback, apply_expected, sizeof(apply_expected), apply_image,
        tls_offset, apply_patch, error, sizeof(error)), error);
    require(knowledgepatch::prepare_parallel_patch(
        const_cast<std::uint8_t*>(verify_fixture_site), verify_fixture_continuation,
        nullptr, verify_expected, sizeof(verify_expected), verify_image, tls_offset,
        verify_patch, error, sizeof(error)), error);
    require(knowledgepatch::prepare_parallel_patch(
        const_cast<std::uint8_t*>(timing_fixture_site), timing_fixture_fallback,
        nullptr, apply_expected, sizeof(apply_expected), timing_image, tls_offset,
        timing_patch, error, sizeof(error)), error);
    require(knowledgepatch::commit_parallel_patch(apply_patch, error, sizeof(error)) &&
        knowledgepatch::commit_parallel_patch(verify_patch, error, sizeof(error)) &&
        knowledgepatch::commit_parallel_patch(timing_patch, error, sizeof(error)), error);

    Snapshot snapshot{};
    int current = 0;
    int stale = 0;
    std::array<std::uint8_t, 0x30> other{};
    std::array<std::uint8_t, 0x50> original_relation{};
    std::array<std::uint8_t, 0x50> fixture_relation{};
    std::array<std::uint8_t, 0x248> scratch{};
    std::mt19937_64 random(0x942047);
    const auto tag = static_cast<std::uint16_t>(0xF123);
    std::memcpy(other.data() + 0x24, &tag, sizeof(tag));

    for (unsigned trial = 0; trial < 10000; ++trial) {
        for (std::size_t index = 0; index < original_relation.size(); ++index) {
            original_relation[index] = static_cast<std::uint8_t>(random());
        }
        fixture_relation = original_relation;
        ApplyArgs original{&current, other.data(), original_relation.data(),
            static_cast<std::uint8_t>(random() & 0x0f), {}, random(), random(), scratch.data(), 0, 0, 0};
        ApplyArgs fixture = original;
        fixture.relation = fixture_relation.data();
        snapshot.country = &current;
        snapshot.pair_flags[tag] = knowledgepatch::pair_valid |
            static_cast<std::uint8_t>(original.expected);
        if (trial % 3 == 1) {
            snapshot.pair_flags[tag] |= knowledgepatch::pair_visibility_unknown |
                static_cast<std::uint8_t>((original.expected + 5) & 0x0f);
        } else if (trial % 3 == 2) {
            snapshot.pair_flags[tag] = static_cast<std::uint8_t>(random()) &
                static_cast<std::uint8_t>(~knowledgepatch::pair_valid);
        }
        require(TlsSetValue(slot, &snapshot) != 0, "TlsSetValue failed");
        const int expected = apply_original(&original);
        const int actual = apply_fixture(&fixture);
        if (actual != expected || fixture_relation != original_relation) {
            std::fprintf(stderr, "FAILED: apply trial=%u expected=%02x actual=%02x\n",
                trial, expected, actual);
            cleanup();
            return 2;
        }
        if (trial % 3 != 0) {
            require(fixture.observed_rax == original.observed_rax &&
                fixture.observed_rdx == original.observed_rdx,
                "fallback did not preserve live RAX/RDX");
        }
    }

    // Null TLS and a nested/stale owner must execute the original path.
    original_relation.fill(0);
    fixture_relation.fill(0);
    ApplyArgs original{&current, other.data(), original_relation.data(), 0x0d, {},
        0x11223344, 0x55667788, scratch.data(), 0, 0, 0};
    ApplyArgs fixture = original;
    fixture.relation = fixture_relation.data();
    snapshot.country = &stale;
    snapshot.pair_flags[tag] = knowledgepatch::pair_valid | 0x02;
    TlsSetValue(slot, &snapshot);
    require(apply_fixture(&fixture) == apply_original(&original) &&
        fixture_relation == original_relation, "stale-owner fallback");
    require(fixture.observed_rax == original.observed_rax &&
        fixture.observed_rdx == original.observed_rdx, "stale-owner RAX/RDX preservation");
    TlsSetValue(slot, nullptr);
    fixture_relation.fill(0);
    original_relation.fill(0);
    require(apply_fixture(&fixture) == apply_original(&original) &&
        fixture_relation == original_relation, "null-TLS fallback");

    original_relation.fill(0);
    fixture_relation.fill(0);
    const int timing_before = timing_calls;
    const int expected_timing = apply_original(&original);
    const int actual_timing = timing_fixture(&fixture);
    require(timing_calls == timing_before + 1 && actual_timing == expected_timing &&
        fixture_relation == original_relation, "timing hook changed original behavior");
    require(fixture.observed_rax == original.observed_rax &&
        fixture.observed_rdx == original.observed_rdx,
        "timing hook did not preserve live RAX/RDX");
    require(fixture.observed_rsp == original.observed_rsp,
        "timing hook did not preserve RSP");

    VerifyArgs expected_verify{&current, other.data(), original_relation.data(),
        random(), random(), random(), random(), random(), random(),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned trial = 0; trial < 1000; ++trial) {
        VerifyArgs actual = expected_verify;
        verify_original(&expected_verify);
        const int before = verifier_calls;
        verify_fixture(&actual);
        require(verifier_calls == before + 1, "verify hook was not called exactly once");
        require(verifier_current == &current && verifier_other == other.data() &&
            verifier_relation == original_relation.data(),
            "verify arguments are wrong: other must come from original RSP+0x40");
        compare_verify(actual, expected_verify);
        expected_verify.rax = random();
        expected_verify.rcx = random();
        expected_verify.rdx = random();
        expected_verify.r8 = random();
        expected_verify.r9 = random();
        expected_verify.r11 = random();
        expected_verify.out_rax = expected_verify.out_rcx = expected_verify.out_rdx = 0;
        expected_verify.out_r8 = expected_verify.out_r9 = expected_verify.out_r11 = 0;
        expected_verify.out_rdi = expected_verify.out_r10 = expected_verify.out_r13 = 0;
    }

    knowledgepatch::rollback_parallel_patch(timing_patch);
    knowledgepatch::rollback_parallel_patch(verify_patch);
    knowledgepatch::rollback_parallel_patch(apply_patch);
    require(std::memcmp(apply_fixture_site, apply_expected, sizeof(apply_expected)) == 0 &&
        std::memcmp(timing_fixture_site, apply_expected, sizeof(apply_expected)) == 0 &&
        std::memcmp(verify_fixture_site, verify_expected, sizeof(verify_expected)) == 0,
        "rollback did not restore original bytes");
    fixture_relation.fill(0);
    original_relation.fill(0);
    require(apply_fixture(&fixture) == apply_original(&original) &&
        fixture_relation == original_relation, "apply behavior after rollback");
    VerifyArgs actual = expected_verify;
    verify_original(&expected_verify);
    const int before = verifier_calls;
    verify_fixture(&actual);
    require(verifier_calls == before, "verify hook remained active after rollback");
    compare_verify(actual, expected_verify);
    cleanup();
    std::puts("parallel loop tests passed: 10000 apply/fallback cases, 1000 verify cases, registers, rollback");
    return 0;
}
