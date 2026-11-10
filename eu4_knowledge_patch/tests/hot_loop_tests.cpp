#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "hot_loop_patch.hpp"

extern "C" int vector_original(const std::uint64_t*, const std::uint64_t*, std::uint16_t);
extern "C" int vector_fixture(const std::uint64_t*, const std::uint64_t*, std::uint16_t);
extern "C" const std::uint8_t vector_fixture_site[];
extern "C" const std::uint8_t vector_fixture_found[];
extern "C" const std::uint8_t vector_fixture_out[];

namespace {

constexpr std::uint8_t expected[knowledgepatch::loop_site_size] = {
    0x48, 0x8B, 0x0A,             // mov rcx, [rdx]
    0x48, 0xC1, 0xE9, 0x20,       // shr rcx, 32
    0x66, 0x44, 0x39, 0xC1        // cmp cx, r8w
};

bool equivalence_tests() {
    std::mt19937_64 random(0x941e80);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        const std::size_t count = trial % 65;
        std::vector<std::uint64_t> entries(count);
        for (auto& entry : entries) entry = random();
        const auto target = static_cast<std::uint16_t>(random());
        if (count && trial % 3 == 0) {
            const auto position = random() % count;
            entries[position] = (entries[position] & 0x0000ffffffffffffULL) |
                (static_cast<std::uint64_t>(target) << 32);
        }
        const auto before = vector_original(entries.data(), entries.data() + count, target);
        const auto after = vector_fixture(entries.data(), entries.data() + count, target);
        if (before != after) {
            std::fprintf(stderr, "Mismatch at trial %u, length %zu: %d != %d\n",
                trial, count, before, after);
            return false;
        }
    }
    return true;
}

using Scan = int (*)(const std::uint64_t*, const std::uint64_t*, std::uint16_t);

double benchmark(Scan scan, unsigned repeats, std::size_t count) {
    std::array<std::uint64_t, 32> entries{};
    for (unsigned index = 0; index < entries.size(); ++index) {
        entries[index] = (static_cast<std::uint64_t>(0x1000 + index) << 32) |
            (static_cast<std::uint64_t>(index) * 0x0101010101010101ULL);
    }
    const auto begin = std::chrono::steady_clock::now();
    unsigned result = 0;
    for (unsigned iteration = 0; iteration < repeats; ++iteration) {
        result += scan(entries.data(), entries.data() + count, 0x7fff);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (result) std::fprintf(stderr, "Unexpected benchmark result: %u\n", result);
    return elapsed;
}

}  // namespace

int main() {
    char error[256]{};
    knowledgepatch::LoopPatch patch;
    std::array<std::uint8_t, knowledgepatch::loop_site_size> wrong{};
    std::memcpy(wrong.data(), expected, wrong.size());
    wrong[0] = 0;
    auto* site = const_cast<std::uint8_t*>(vector_fixture_site);
    if (knowledgepatch::prepare_loop(site, vector_fixture_found, vector_fixture_out,
            wrong.data(), patch, error, sizeof(error))) {
        std::fprintf(stderr, "Signature guard accepted wrong bytes\n");
        return 1;
    }
    if (!knowledgepatch::prepare_loop(site, vector_fixture_found, vector_fixture_out,
            expected, patch, error, sizeof(error))) {
        std::fprintf(stderr, "prepare_loop failed: %s\n", error);
        return 2;
    }
    if (!knowledgepatch::commit_loop(patch, error, sizeof(error))) {
        std::fprintf(stderr, "commit_loop failed: %s\n", error);
        knowledgepatch::discard_loop(patch);
        return 3;
    }
    const bool equivalent = equivalence_tests();
    if (!equivalent) {
        knowledgepatch::discard_loop(patch);
        return 4;
    }

    constexpr unsigned repeats = 5000000;
    std::printf("PASS: actual patched loop, 10000 randomized cases, lengths 0..64\n");
    std::printf(" entries | original | patched  | speedup\n");
    for (const std::size_t count : {1, 2, 4, 8, 16, 32}) {
        const auto original = benchmark(vector_original, repeats, count);
        const auto optimized = benchmark(vector_fixture, repeats, count);
        std::printf("%8zu | %.4f s | %.4f s | %.2fx\n", count, original, optimized,
            original / optimized);
    }

    knowledgepatch::rollback_loop(patch);
    if (std::memcmp(vector_fixture_site, expected, sizeof(expected)) != 0 || !equivalence_tests()) {
        std::fprintf(stderr, "Rollback did not restore the original loop\n");
        knowledgepatch::discard_loop(patch);
        return 5;
    }
    knowledgepatch::discard_loop(patch);
    std::printf("PASS: rollback restored the original instructions and behavior\n");
    return 0;
}
