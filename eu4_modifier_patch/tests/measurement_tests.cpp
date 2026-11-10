#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "measurement.hpp"

namespace {

struct Pair {
    int key;
    int value;
};

struct Container {
    std::array<std::uint8_t, 0x58> fields{};
    Pair* entries = nullptr;
    std::int32_t capacity = 0;
    std::int32_t count = 0;
};
static_assert(offsetof(Container, entries) == 0x58, "entries offset");
static_assert(offsetof(Container, capacity) == 0x60, "capacity offset");
static_assert(offsetof(Container, count) == 0x64, "count offset");

modifierpatch::Measurement reference(const Container& destination, const Container& source) {
    modifierpatch::Measurement result;
    result.destination_count = destination.count;
    result.source_count = source.count;
    std::vector<int> keys;
    for (std::int32_t index = 0; index < destination.count; ++index) {
        keys.push_back(destination.entries[index].key);
    }
    const bool dense = destination.fields[0x10] != 0;
    for (std::int32_t index = 0; index < source.count; ++index) {
        const int key = source.entries[index].key;
        if (key == modifierpatch::modifier_sentinel) continue;
        if (dense && key < modifierpatch::modifier_sentinel) {
            ++result.direct_lookups;
            continue;
        }
        result.dynamic = true;
        ++result.dynamic_inputs;
        std::size_t position = 0;
        for (; position < keys.size(); ++position) {
            ++result.comparisons;
            if (keys[position] == key) break;
        }
        if (position == keys.size()) {
            keys.push_back(key);
            ++result.appended;
        }
    }
    return result;
}

bool equal(const modifierpatch::Measurement& left, const modifierpatch::Measurement& right) {
    return left.destination_count == right.destination_count &&
        left.source_count == right.source_count && left.comparisons == right.comparisons &&
        left.direct_lookups == right.direct_lookups && left.dynamic_inputs == right.dynamic_inputs &&
        left.appended == right.appended && left.dynamic == right.dynamic && left.valid == right.valid;
}

}  // namespace

int main() {
    std::mt19937 random(0x8EA420);
    modifierpatch::Scratch scratch;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        const auto destination_count = static_cast<std::int32_t>(random() % 81);
        const auto source_count = static_cast<std::int32_t>(random() % 51);
        std::vector<Pair> destination_entries(destination_count);
        std::vector<Pair> source_entries(source_count);
        for (auto& entry : destination_entries) {
            entry.key = static_cast<int>(random() % 901);
            if (trial % 7 == 0 && destination_count > 1 && &entry == &destination_entries.back()) {
                entry.key = destination_entries.front().key;
            }
            entry.value = static_cast<int>(random());
        }
        for (auto& entry : source_entries) {
            entry.key = static_cast<int>(random() % 901);
            if (trial % 11 == 0) entry.key = modifierpatch::modifier_sentinel;
            if (destination_count && trial % 3 == 0) {
                entry.key = destination_entries[random() % destination_count].key;
            }
            entry.value = static_cast<int>(random());
        }
        Container destination;
        Container source;
        destination.fields[0x10] = static_cast<std::uint8_t>(trial & 1);
        source.fields[0x10] = static_cast<std::uint8_t>((trial >> 1) & 1);
        destination.entries = destination_entries.data();
        destination.capacity = destination.count = destination_count;
        source.entries = source_entries.data();
        source.capacity = source.count = source_count;
        const auto expected = reference(destination, source);
        const auto actual = modifierpatch::measure_merge(&destination, &source, scratch);
        if (!equal(actual, expected)) {
            std::fprintf(stderr,
                "Measurement mismatch trial=%u expected comparisons=%llu actual=%llu "
                "expected appended=%llu actual=%llu\n", trial,
                static_cast<unsigned long long>(expected.comparisons),
                static_cast<unsigned long long>(actual.comparisons),
                static_cast<unsigned long long>(expected.appended),
                static_cast<unsigned long long>(actual.appended));
            return 1;
        }
    }
    Container same;
    const auto alias = modifierpatch::measure_merge(&same, &same, scratch);
    if (!alias.alias || !alias.valid) {
        std::fputs("Alias detection failed\n", stderr);
        return 2;
    }
    if (modifierpatch::measure_merge(nullptr, &same, scratch).valid) {
        std::fputs("Null guard failed\n", stderr);
        return 3;
    }
    std::puts("measurement tests passed: 10000 randomized merges, duplicates, sentinel, dense/sparse, alias");
    return 0;
}
