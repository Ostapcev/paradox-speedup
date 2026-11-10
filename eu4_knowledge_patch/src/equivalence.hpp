#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace knowledgepatch {

using Tag = std::uint16_t;
constexpr std::size_t tag_count = 65536;
constexpr std::size_t tag_words = tag_count / 64;

struct TagBits {
    std::array<std::uint64_t, tag_words> words{};

    void clear() noexcept { words.fill(0); }
    void set(Tag tag) noexcept { words[tag >> 6] |= std::uint64_t{1} << (tag & 63); }
    bool get(Tag tag) const noexcept {
        return (words[tag >> 6] & (std::uint64_t{1} << (tag & 63))) != 0;
    }
};

struct OwnerBits {
    TagBits cores;
    TagBits claims;
};

struct ComparisonStats {
    std::uint64_t country_pairs = 0;
    std::uint64_t inverse_core_entries = 0;
    std::uint64_t inverse_claim_entries = 0;
    std::uint64_t verification_province_visits = 0;
    std::uint64_t core_tag_comparisons = 0;
    std::uint64_t claim_tag_comparisons = 0;
    std::uint64_t invalid_province_ids = 0;
};

struct Mismatch {
    std::size_t country_index = 0;
    Tag current_tag = 0;
    Tag other_tag = 0;
    bool core_expected = false;
    bool core_inverse = false;
    bool claim_expected = false;
    bool claim_inverse = false;
    int core_expected_province = -1;
    int core_inverse_province = -1;
    int claim_expected_province = -1;
    int claim_inverse_province = -1;
};

struct Comparison {
    ComparisonStats stats;
    std::uint64_t mismatched_pairs = 0;
    bool has_first_mismatch = false;
    Mismatch first;
};

struct NoPhaseClock {
    void inverse_finished() noexcept {}
};

// World is a read-only adapter. Every callback returns true to continue or
// false to stop early. The algorithm never writes through the adapter.
template <typename World, typename PhaseClock>
Comparison compare_inverse(const World& world, PhaseClock& clock) {
    Comparison result;
    OwnerBits inverse;
    inverse.cores.clear();
    inverse.claims.clear();
    const Tag current_tag = world.current_tag();

    world.for_each_current_core([&](int province_id) {
        ++result.stats.inverse_core_entries;
        if (world.valid_province(province_id)) {
            inverse.cores.set(world.province_owner(province_id));
        } else {
            ++result.stats.invalid_province_ids;
        }
        return true;
    });
    world.for_each_current_claim([&](int province_id) {
        ++result.stats.inverse_claim_entries;
        if (world.valid_province(province_id)) {
            inverse.claims.set(world.province_owner(province_id));
        } else {
            ++result.stats.invalid_province_ids;
        }
        return true;
    });
    clock.inverse_finished();

    for (std::size_t country_index = 0; country_index < world.country_count(); ++country_index) {
        ++result.stats.country_pairs;
        const Tag other_tag = world.country_tag(country_index);
        bool core_expected = false;
        bool claim_expected = false;
        int core_expected_province = -1;
        int claim_expected_province = -1;

        world.for_each_owned(country_index, [&](int province_id) {
            ++result.stats.verification_province_visits;
            if (!world.valid_province(province_id)) {
                ++result.stats.invalid_province_ids;
                return true;
            }
            if (!core_expected) {
                world.for_each_core_tag(province_id, [&](Tag tag) {
                    ++result.stats.core_tag_comparisons;
                    if (tag == current_tag) {
                        core_expected = true;
                        core_expected_province = province_id;
                        return false;
                    }
                    return true;
                });
            }
            if (!claim_expected) {
                world.for_each_claim_tag(province_id, [&](Tag tag) {
                    ++result.stats.claim_tag_comparisons;
                    if (tag == current_tag) {
                        claim_expected = true;
                        claim_expected_province = province_id;
                        return false;
                    }
                    return true;
                });
            }
            return !(core_expected && claim_expected);
        });

        const bool core_inverse = inverse.cores.get(other_tag);
        const bool claim_inverse = inverse.claims.get(other_tag);
        if (core_expected == core_inverse && claim_expected == claim_inverse) continue;

        ++result.mismatched_pairs;
        if (result.has_first_mismatch) continue;
        result.has_first_mismatch = true;
        auto& first = result.first;
        first.country_index = country_index;
        first.current_tag = current_tag;
        first.other_tag = other_tag;
        first.core_expected = core_expected;
        first.core_inverse = core_inverse;
        first.claim_expected = claim_expected;
        first.claim_inverse = claim_inverse;
        first.core_expected_province = core_expected_province;
        first.claim_expected_province = claim_expected_province;

        if (core_inverse) {
            world.for_each_current_core([&](int province_id) {
                if (world.valid_province(province_id) &&
                    world.province_owner(province_id) == other_tag) {
                    first.core_inverse_province = province_id;
                    return false;
                }
                return true;
            });
        }
        if (claim_inverse) {
            world.for_each_current_claim([&](int province_id) {
                if (world.valid_province(province_id) &&
                    world.province_owner(province_id) == other_tag) {
                    first.claim_inverse_province = province_id;
                    return false;
                }
                return true;
            });
        }
    }
    return result;
}

template <typename World>
Comparison compare_inverse(const World& world) {
    NoPhaseClock clock;
    return compare_inverse(world, clock);
}

}  // namespace knowledgepatch
