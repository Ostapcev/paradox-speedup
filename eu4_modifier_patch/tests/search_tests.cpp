#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "function_patch.hpp"
#include "search.hpp"

extern "C" std::uint64_t add_search_fixture(const void*, std::uint64_t, std::uint32_t);
extern "C" std::uint64_t subtract_search_fixture(const void*, std::uint64_t, std::uint32_t);
extern "C" void add_search_fixture_found();
extern "C" void add_search_fixture_not_found();
extern "C" void subtract_search_fixture_found();
extern "C" void subtract_search_fixture_not_found();
extern "C" void add_search_loop();
extern "C" void subtract_search_loop();
extern "C" const void* search_fixture_expected_pointer;

namespace {

struct Entry {
    std::int32_t key;
    std::int32_t value;
};

std::uint64_t reference(const std::vector<Entry>& entries, std::uint32_t key) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (static_cast<std::uint32_t>(entries[index].key) == key) return index;
    }
    return entries.size();
}

using SearchFixture = std::uint64_t (*)(const void*, std::uint64_t, std::uint32_t);

bool guard_page_test(bool relays = false) {
    const auto page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return false;
    DWORD ignored = 0;
    const bool protected_page = VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &ignored) != 0;
    bool valid = protected_page;
    for (std::size_t count = 1; valid && count <= 17; ++count) {
        auto* entries = reinterpret_cast<Entry*>(page + 4096 - count * sizeof(Entry));
        for (std::size_t index = 0; index < count; ++index) {
            entries[index] = Entry{static_cast<int>(100 + index), 0};
        }
        if (relays) {
            search_fixture_expected_pointer = entries + count - 1;
            valid = add_search_fixture(entries, count, 100 + count - 1) == count - 1 &&
                subtract_search_fixture(entries, count, 100 + count - 1) == count - 1;
            search_fixture_expected_pointer = entries + count;
            valid = valid && add_search_fixture(entries, count, 0x12345678) == count &&
                subtract_search_fixture(entries, count, 0x12345678) == count;
        } else {
            valid = modifierpatch::find_modifier_key(entries, count, 100 + count - 1) == count - 1 &&
                modifierpatch::find_modifier_key(entries, count, 0x12345678) == count;
        }
    }
    VirtualFree(page, 0, MEM_RELEASE);
    return valid;
}

double benchmark_add(SearchFixture fixture, const std::vector<Entry>& entries,
    std::uint32_t key, std::uint64_t repetitions) {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER begin{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&frequency);
    search_fixture_expected_pointer = entries.data() + entries.size();
    volatile std::uint64_t sink = 0;
    QueryPerformanceCounter(&begin);
    for (std::uint64_t iteration = 0; iteration < repetitions; ++iteration) {
        sink += fixture(entries.data(), entries.size(), key);
    }
    QueryPerformanceCounter(&end);
    return static_cast<double>(end.QuadPart - begin.QuadPart) * 1000000000.0 /
        (static_cast<double>(frequency.QuadPart) * static_cast<double>(repetitions));
}

}  // namespace

int main() {
    std::mt19937 random(0x8EA4E0);
    std::vector<Entry> entries(80);
    for (unsigned trial = 0; trial < 20000; ++trial) {
        const auto count = random() % 81;
        entries.resize(count);
        for (auto& entry : entries) {
            entry.key = static_cast<int>(random() % 1000);
            entry.value = static_cast<int>(random());
        }
        if (count > 3 && trial % 5 == 0) entries[count - 1].key = entries[1].key;
        const auto key = trial % 3 ? random() % 1000 :
            (count ? static_cast<std::uint32_t>(entries[random() % count].key) : 0);
        const auto expected = reference(entries, key);
        if (modifierpatch::find_modifier_key(entries.data(), count, key) != expected) {
            std::fprintf(stderr, "SSE2 search mismatch at trial %u\n", trial);
            return 1;
        }
    }
    if (!guard_page_test()) {
        std::fputs("SSE2 search crossed a protected page or guard setup failed\n", stderr);
        return 2;
    }

    constexpr std::uint64_t benchmark_counts[]{1, 4, 8, 16, 32, 64, 128, 256, 708};
    double original_ns[std::size(benchmark_counts)]{};
    for (std::size_t index = 0; index < std::size(benchmark_counts); ++index) {
        entries.resize(benchmark_counts[index]);
        for (std::size_t entry = 0; entry < entries.size(); ++entry) {
            entries[entry] = Entry{static_cast<int>(entry + 1), 0};
        }
        const auto repetitions = (std::max<std::uint64_t>)(20000, 8000000 / entries.size());
        original_ns[index] = benchmark_add(&add_search_fixture, entries, 0x7fffffff, repetitions);
    }

    modifierpatch::add_search_found_target = reinterpret_cast<void*>(&add_search_fixture_found);
    modifierpatch::add_search_not_found_target = reinterpret_cast<void*>(&add_search_fixture_not_found);
    modifierpatch::subtract_search_found_target = reinterpret_cast<void*>(&subtract_search_fixture_found);
    modifierpatch::subtract_search_not_found_target = reinterpret_cast<void*>(&subtract_search_fixture_not_found);

    constexpr modifierpatch::SearchBytes add_expected{
        0x39, 0x38, 0x0F, 0x84, 0x96, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC1, 0x48, 0x83, 0xC0, 0x08, 0x48, 0x3B, 0xCA, 0x7C, 0xEC};
    constexpr modifierpatch::SearchBytes subtract_expected{
        0x39, 0x39, 0x0F, 0x84, 0x95, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC0, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x3B, 0xC2, 0x7C, 0xEC};
    modifierpatch::SearchPatch add_patch;
    modifierpatch::SearchPatch subtract_patch;
    char error[256]{};
    if (!modifierpatch::prepare_search(reinterpret_cast<std::uint8_t*>(&add_search_loop),
            add_expected, 20, reinterpret_cast<const void*>(&modifierpatch::add_search_relay),
            add_patch, error, sizeof(error)) ||
        !modifierpatch::prepare_search(reinterpret_cast<std::uint8_t*>(&subtract_search_loop),
            subtract_expected, 20, reinterpret_cast<const void*>(&modifierpatch::subtract_search_relay),
            subtract_patch, error, sizeof(error))) {
        std::fprintf(stderr, "Search patch preparation failed: %s\n", error);
        return 3;
    }
    if (!modifierpatch::commit_search(add_patch, error, sizeof(error)) ||
        !modifierpatch::commit_search(subtract_patch, error, sizeof(error))) {
        std::fprintf(stderr, "Search patch commit failed: %s\n", error);
        modifierpatch::discard_search(add_patch);
        modifierpatch::discard_search(subtract_patch);
        return 4;
    }

    for (unsigned trial = 0; trial < 20000; ++trial) {
        const auto count = random() % 81;
        entries.resize(count);
        for (auto& entry : entries) {
            entry.key = static_cast<int>(random() % 1000);
            entry.value = static_cast<int>(random());
        }
        const auto key = trial % 2 ? random() % 1000 :
            (count ? static_cast<std::uint32_t>(entries[random() % count].key) : 0);
        const auto expected = reference(entries, key);
        search_fixture_expected_pointer = entries.data() + expected;
        if (add_search_fixture(entries.data(), count, key) != expected ||
            subtract_search_fixture(entries.data(), count, key) != expected) {
            std::fprintf(stderr, "Relay/register mismatch at trial %u\n", trial);
            modifierpatch::discard_search(add_patch);
            modifierpatch::discard_search(subtract_patch);
            return 5;
        }
    }
    if (!guard_page_test(true)) {
        std::fputs("Search relay crossed a protected page or returned wrong registers\n", stderr);
        modifierpatch::discard_search(add_patch);
        modifierpatch::discard_search(subtract_patch);
        return 7;
    }
    for (std::size_t index = 0; index < std::size(benchmark_counts); ++index) {
        entries.resize(benchmark_counts[index]);
        for (std::size_t entry = 0; entry < entries.size(); ++entry) {
            entries[entry] = Entry{static_cast<int>(entry + 1), 0};
        }
        const auto repetitions = (std::max<std::uint64_t>)(20000, 8000000 / entries.size());
        const auto optimized_ns = benchmark_add(&add_search_fixture, entries, 0x7fffffff, repetitions);
        std::printf("search benchmark count=%llu original_ns=%.2f sse2_ns=%.2f speedup=%.3fx\n",
            static_cast<unsigned long long>(entries.size()), original_ns[index], optimized_ns,
            original_ns[index] / optimized_ns);
    }
    modifierpatch::rollback_search(add_patch);
    modifierpatch::rollback_search(subtract_patch);
    if (std::memcmp(reinterpret_cast<const void*>(&add_search_loop), add_expected.data(), 20) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(&subtract_search_loop), subtract_expected.data(), 20) != 0) {
        std::fputs("Search rollback did not restore original bytes\n", stderr);
        return 6;
    }
    modifierpatch::discard_search(add_patch);
    modifierpatch::discard_search(subtract_patch);
    std::puts("search tests passed: 40000 randomized cases, first match, registers, rollback, guard page");
    return 0;
}
