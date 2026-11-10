#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

extern "C" int vector_original(const std::uint64_t*, const std::uint64_t*, std::uint16_t);
extern "C" int vector_optimized(const std::uint64_t*, const std::uint64_t*, std::uint16_t);
extern "C" int vector_unrolled(const std::uint64_t*, const std::uint64_t*, std::uint16_t);
extern "C" int linked_original(const void*, std::uint16_t);
extern "C" int linked_optimized(const void*, std::uint16_t);

struct Node {
    std::uint64_t tag;
    std::uint64_t payload;
    Node* next;
    std::uint64_t padding;
};

static std::uint64_t make_tag(std::mt19937_64& random) {
    return random();
}

static bool equivalence_tests() {
    std::mt19937_64 random(0x1375);
    for (unsigned trial = 0; trial < 5000; ++trial) {
        const auto count = trial % 65;
        std::vector<std::uint64_t> entries(count);
        std::vector<Node> nodes(count);
        for (std::size_t index = 0; index < count; ++index) {
            entries[index] = make_tag(random);
            nodes[index].tag = entries[index];
            nodes[index].payload = random();
            nodes[index].next = index + 1 < count ? &nodes[index + 1] : nullptr;
            nodes[index].padding = random();
        }
        const auto target = static_cast<std::uint16_t>(random());
        if (count && trial % 3 == 0) {
            const auto position = random() % count;
            entries[position] = (entries[position] & 0x0000ffffffffffffULL) |
                (static_cast<std::uint64_t>(target) << 32);
            nodes[position].tag = entries[position];
        }
        const auto original = vector_original(entries.data(), entries.data() + count, target);
        const auto optimized = vector_optimized(entries.data(), entries.data() + count, target);
        const auto unrolled = vector_unrolled(entries.data(), entries.data() + count, target);
        const auto linked_before = linked_original(count ? nodes.data() : nullptr, target);
        const auto linked_after = linked_optimized(count ? nodes.data() : nullptr, target);
        if (original != optimized || original != unrolled ||
            linked_before != linked_after || original != linked_before) {
            std::fprintf(stderr, "Mismatch at trial %u: vector %d/%d/%d linked %d/%d\n",
                trial, original, optimized, unrolled, linked_before, linked_after);
            return false;
        }
    }
    return true;
}

template <typename Function>
static double benchmark(Function function, unsigned repeats, std::size_t count) {
    std::array<std::uint64_t, 32> entries{};
    for (unsigned index = 0; index < entries.size(); ++index) {
        entries[index] = (static_cast<std::uint64_t>(0x1000 + index) << 32) |
        (static_cast<std::uint64_t>(index) * 0x0101010101010101ULL);
    }
    const auto begin = std::chrono::steady_clock::now();
    unsigned result = 0;
    for (unsigned iteration = 0; iteration < repeats; ++iteration) {
        result += function(entries.data(), entries.data() + count, 0x7fff);
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (result != 0) std::fprintf(stderr, "Unexpected benchmark result: %u\n", result);
    return elapsed;
}

int main() {
    if (!equivalence_tests()) return 1;
    constexpr unsigned repeats = 5000000;
    std::printf("PASS: 5000 randomized equivalence cases for vector and linked scans\n");
    std::printf("Vector absent-tag scan, 5M calls per row:\n");
    std::printf(" entries | original | direct cmp | unrolled | unrolled speedup\n");
    for (const std::size_t count : {1, 2, 4, 8, 16, 32}) {
        const auto original = benchmark(vector_original, repeats, count);
        const auto optimized = benchmark(vector_optimized, repeats, count);
        const auto unrolled = benchmark(vector_unrolled, repeats, count);
        std::printf("%8zu | %.4f s | %.4f s   | %.4f s  | %.2fx\n",
            count, original, optimized, unrolled, original / unrolled);
    }
    return 0;
}
