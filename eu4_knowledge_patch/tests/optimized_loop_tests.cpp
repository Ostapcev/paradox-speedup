#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "equivalence.hpp"
#include "optimized_loop_patch.hpp"

extern "C" {
int core_original(void* args);
int core_fixture(void* args);
int claim_original(void* args);
int claim_fixture(void* args);
extern const std::uint8_t core_fixture_site[];
extern const std::uint8_t core_fixture_continuation[];
extern const std::uint8_t core_fixture_found[];
extern const std::uint8_t core_fixture_out[];
extern const std::uint8_t claim_fixture_site[];
extern const std::uint8_t claim_fixture_continuation[];
extern const std::uint8_t claim_fixture_found[];
extern const std::uint8_t claim_fixture_out[];
extern const std::uint8_t knowledge_optimized_core_begin[];
extern const std::uint8_t knowledge_optimized_core_end[];
extern const std::uint8_t knowledge_optimized_core_tls[];
extern const std::uint8_t knowledge_optimized_core_found_displacement[];
extern const std::uint8_t knowledge_optimized_core_out_displacement[];
extern const std::uint8_t knowledge_optimized_core_continuation_displacement[];
extern const std::uint8_t knowledge_optimized_claim_begin[];
extern const std::uint8_t knowledge_optimized_claim_end[];
extern const std::uint8_t knowledge_optimized_claim_tls[];
extern const std::uint8_t knowledge_optimized_claim_found_displacement[];
extern const std::uint8_t knowledge_optimized_claim_out_displacement[];
extern const std::uint8_t knowledge_optimized_claim_continuation_displacement[];
}

namespace {

struct Snapshot {
    const void* country;
    knowledgepatch::TagBits cores;
    knowledgepatch::TagBits claims;
};
static_assert(offsetof(Snapshot, cores) == 8, "core offset");
static_assert(offsetof(Snapshot, claims) == 0x2008, "claim offset");

struct Args {
    const void* current;
    const void* other;
    const void* entries;
    const void* end;
    std::uint64_t wanted;
};

struct Node {
    std::uint64_t packed = 0;
    std::uint64_t unused = 0;
    Node* next = nullptr;
};

constexpr std::uint8_t core_expected[knowledgepatch::core_loop_site_size] = {
    0x48, 0x8B, 0x0A, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x41, 0x3B, 0xC8};
constexpr std::uint8_t claim_expected[knowledgepatch::claim_loop_site_size] = {
    0x48, 0x8B, 0x08, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x3B, 0xCA};

const knowledgepatch::LoopHookImage core_image{
    knowledge_optimized_core_begin, knowledge_optimized_core_end,
    knowledge_optimized_core_tls, knowledge_optimized_core_found_displacement,
    knowledge_optimized_core_out_displacement,
    knowledge_optimized_core_continuation_displacement};
const knowledgepatch::LoopHookImage claim_image{
    knowledge_optimized_claim_begin, knowledge_optimized_claim_end,
    knowledge_optimized_claim_tls, knowledge_optimized_claim_found_displacement,
    knowledge_optimized_claim_out_displacement,
    knowledge_optimized_claim_continuation_displacement};

}  // namespace

int main() {
    DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES || slot >= 64) {
        std::fputs("No direct TLS slot\n", stderr);
        return 1;
    }
    const auto tls_offset = static_cast<std::uint32_t>(0x1480 + slot * 8);
    knowledgepatch::LoopPatch core_patch;
    knowledgepatch::LoopPatch claim_patch;
    char error[256]{};
    auto cleanup = [&]() {
        knowledgepatch::discard_loop(claim_patch);
        knowledgepatch::discard_loop(core_patch);
        TlsFree(slot);
    };
    std::uint8_t wrong[knowledgepatch::core_loop_site_size]{};
    std::memcpy(wrong, core_expected, sizeof(wrong));
    wrong[0] = 0;
    if (knowledgepatch::prepare_loop(const_cast<std::uint8_t*>(core_fixture_site),
            core_fixture_continuation, core_fixture_found, core_fixture_out,
            wrong, sizeof(wrong), core_image, tls_offset, core_patch, error, sizeof(error))) {
        std::fputs("Signature guard accepted wrong core bytes\n", stderr);
        cleanup();
        return 2;
    }
    if (!knowledgepatch::prepare_loop(const_cast<std::uint8_t*>(core_fixture_site),
            core_fixture_continuation, core_fixture_found, core_fixture_out,
            core_expected, sizeof(core_expected), core_image, tls_offset, core_patch,
            error, sizeof(error))) {
        std::fprintf(stderr, "core prepare_loop failed: %s\n", error);
        cleanup();
        return 3;
    }
    if (!knowledgepatch::prepare_loop(const_cast<std::uint8_t*>(claim_fixture_site),
            claim_fixture_continuation, claim_fixture_found, claim_fixture_out,
            claim_expected, sizeof(claim_expected), claim_image, tls_offset, claim_patch,
            error, sizeof(error))) {
        std::fprintf(stderr, "claim prepare_loop failed: %s\n", error);
        cleanup();
        return 3;
    }
    if (!knowledgepatch::commit_loop(core_patch, error, sizeof(error)) ||
        !knowledgepatch::commit_loop(claim_patch, error, sizeof(error))) {
        std::fprintf(stderr, "commit_loop failed: %s\n", error);
        cleanup();
        return 4;
    }

    Snapshot snapshot{};
    int current_sentinel = 0;
    int wrong_sentinel = 0;
    std::array<std::uint64_t, 5> other{};
    Args args{&current_sentinel, other.data(), nullptr, nullptr, 0};
    std::mt19937_64 random(0x941EC0);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        const auto other_tag = static_cast<std::uint16_t>(random());
        other[4] = static_cast<std::uint64_t>(other_tag) << 32;
        args.wanted = static_cast<std::uint16_t>(random());
        const std::size_t count = trial % 65;
        std::vector<std::uint64_t> entries(count);
        for (auto& entry : entries) entry = random();
        if (count && trial % 3 == 0) {
            entries[random() % count] = (entries[random() % count] & 0x0000FFFFFFFFFFFFULL) |
                (args.wanted << 32);
        }
        args.entries = entries.data();
        args.end = entries.data() + count;
        const int expected = core_original(&args);
        snapshot.country = args.current;
        snapshot.cores.clear();
        if (expected) snapshot.cores.set(other_tag);
        TlsSetValue(slot, &snapshot);
        const int actual = core_fixture(&args);
        if (actual != expected) {
            std::fprintf(stderr, "Core mismatch trial=%u expected=%d actual=%d\n",
                trial, expected, actual);
            cleanup();
            return 5;
        }

        std::vector<Node> nodes(count);
        for (std::size_t index = 0; index < count; ++index) {
            nodes[index].packed = random();
            nodes[index].next = index + 1 < count ? &nodes[index + 1] : nullptr;
        }
        if (count && trial % 4 == 0) {
            nodes[random() % count].packed = args.wanted << 32;
        }
        args.entries = count ? nodes.data() : nullptr;
        const int claim_expected_result = claim_original(&args);
        snapshot.claims.clear();
        if (claim_expected_result) snapshot.claims.set(other_tag);
        TlsSetValue(slot, &snapshot);
        const int claim_actual = claim_fixture(&args);
        if (claim_actual != claim_expected_result) {
            std::fprintf(stderr, "Claim mismatch trial=%u expected=%d actual=%d\n",
                trial, claim_expected_result, claim_actual);
            cleanup();
            return 6;
        }
    }

    // A stale or nested snapshot must run the displaced original instructions,
    // even when its bit deliberately disagrees with the real list.
    other[4] = std::uint64_t{0x1234} << 32;
    std::uint64_t entry = std::uint64_t{0x9999} << 32;
    args.entries = &entry;
    args.end = &entry + 1;
    args.wanted = 0x5555;
    snapshot.cores.clear();
    snapshot.cores.set(0x1234);
    snapshot.country = &wrong_sentinel;
    TlsSetValue(slot, &snapshot);
    if (core_original(&args) != 0 || core_fixture(&args) != 0) {
        std::fputs("Core owner-mismatch fallback failed\n", stderr);
        cleanup();
        return 7;
    }
    Node node{std::uint64_t{0x9999} << 32, 0, nullptr};
    args.entries = &node;
    snapshot.claims.clear();
    snapshot.claims.set(0x1234);
    if (claim_original(&args) != 0 || claim_fixture(&args) != 0) {
        std::fputs("Claim owner-mismatch fallback failed\n", stderr);
        cleanup();
        return 8;
    }
    TlsSetValue(slot, nullptr);
    if (core_fixture(&args) != core_original(&args) ||
        claim_fixture(&args) != claim_original(&args)) {
        std::fputs("Null-TLS fallback failed\n", stderr);
        cleanup();
        return 9;
    }

    knowledgepatch::rollback_loop(claim_patch);
    knowledgepatch::rollback_loop(core_patch);
    if (std::memcmp(core_fixture_site, core_expected, sizeof(core_expected)) != 0 ||
        std::memcmp(claim_fixture_site, claim_expected, sizeof(claim_expected)) != 0 ||
        core_fixture(&args) != core_original(&args) ||
        claim_fixture(&args) != claim_original(&args)) {
        std::fputs("Rollback did not restore original loops\n", stderr);
        cleanup();
        return 10;
    }
    cleanup();
    std::puts("optimized loop tests passed: 10000 core/claim cases, null TLS, owner mismatch, rollback");
    return 0;
}
