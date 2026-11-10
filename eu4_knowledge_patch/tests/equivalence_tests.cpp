#include "equivalence.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

namespace {

using knowledgepatch::Tag;
using knowledgepatch::compare_inverse;

struct Province {
    Tag owner = 0;
    std::vector<Tag> cores;
    std::vector<Tag> claims;
};

struct Country {
    Tag tag = 0;
    std::vector<int> owned;
};

struct World {
    Tag current = 0;
    std::vector<int> current_cores;
    std::vector<int> current_claims;
    std::vector<Country> countries;
    std::vector<Province> provinces;

    Tag current_tag() const { return current; }
    std::size_t country_count() const { return countries.size(); }
    Tag country_tag(std::size_t index) const { return countries[index].tag; }
    bool valid_province(int id) const { return id >= 0 && static_cast<std::size_t>(id) < provinces.size(); }
    Tag province_owner(int id) const { return provinces[static_cast<std::size_t>(id)].owner; }

    template <typename F> void for_each_current_core(F&& fn) const {
        for (int id : current_cores) if (!fn(id)) return;
    }
    template <typename F> void for_each_current_claim(F&& fn) const {
        for (int id : current_claims) if (!fn(id)) return;
    }
    template <typename F> void for_each_owned(std::size_t index, F&& fn) const {
        for (int id : countries[index].owned) if (!fn(id)) return;
    }
    template <typename F> void for_each_core_tag(int id, F&& fn) const {
        for (Tag tag : provinces[static_cast<std::size_t>(id)].cores) if (!fn(tag)) return;
    }
    template <typename F> void for_each_claim_tag(int id, F&& fn) const {
        for (Tag tag : provinces[static_cast<std::size_t>(id)].claims) if (!fn(tag)) return;
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

void empty_lists() {
    World world;
    world.current = 1;
    world.countries = {{1, {}}, {2, {}}};
    const auto result = compare_inverse(world);
    require(result.mismatched_pairs == 0, "empty lists must match");
    require(result.stats.country_pairs == 2, "all countries must be checked");
}

void consistent_core_claim_and_id_zero() {
    World world;
    world.current = 1;
    world.current_cores = {0, 2};
    world.current_claims = {1};
    world.provinces = {
        {2, {1}, {}},
        {3, {}, {1}},
        {2, {1}, {}},
    };
    world.countries = {{1, {}}, {2, {0, 2}}, {3, {1}}, {4, {}}};
    const auto result = compare_inverse(world);
    require(result.mismatched_pairs == 0, "core, claim, and province zero must match");
    require(result.stats.inverse_core_entries == 2, "both inverse cores must be visited");
    require(result.stats.inverse_claim_entries == 1, "inverse claim must be visited");
}

void duplicates_and_multiple_provinces() {
    World world;
    world.current = 7;
    world.current_cores = {0, 0, 1};
    world.current_claims = {1, 1};
    world.provinces = {
        {9, {7, 7}, {}},
        {9, {7}, {7, 7}},
    };
    world.countries = {{9, {0, 1}}};
    const auto result = compare_inverse(world);
    require(result.mismatched_pairs == 0, "duplicates must be idempotent");
    require(result.stats.inverse_core_entries == 3, "duplicate inverse entries must be measured");
    require(result.stats.inverse_claim_entries == 2, "duplicate claims must be measured");
}

void full_tag_range() {
    World world;
    world.current = 65535;
    world.current_cores = {0};
    world.current_claims = {1};
    world.provinces = {{0, {65535}, {}}, {65535, {}, {65535}}};
    world.countries = {{0, {0}}, {65535, {1}}};
    const auto result = compare_inverse(world);
    require(result.mismatched_pairs == 0, "tags zero and 65535 must be valid bit indices");
}

void owner_owned_desynchronization() {
    World world;
    world.current = 1;
    world.current_cores = {0};
    world.current_claims = {0};
    world.provinces = {{2, {1}, {1}}};
    world.countries = {{2, {}}};
    auto result = compare_inverse(world);
    require(result.mismatched_pairs == 1, "inverse-only result must be detected");
    require(result.has_first_mismatch && result.first.other_tag == 2, "mismatch country must be logged");
    require(!result.first.core_expected && result.first.core_inverse, "core mismatch direction");
    require(!result.first.claim_expected && result.first.claim_inverse, "claim mismatch direction");
    require(result.first.core_inverse_province == 0, "inverse core source province");
    require(result.first.claim_inverse_province == 0, "inverse claim source province");

    world.countries = {{2, {0}}};
    world.provinces[0].owner = 3;
    result = compare_inverse(world);
    require(result.mismatched_pairs == 1, "old-only result must be detected");
    require(result.first.core_expected && !result.first.core_inverse, "reverse core mismatch direction");
    require(result.first.claim_expected && !result.first.claim_inverse, "reverse claim mismatch direction");
    require(result.first.core_expected_province == 0, "old core source province");
    require(result.first.claim_expected_province == 0, "old claim source province");
}

void invalid_province_ids_are_contained() {
    World world;
    world.current = 1;
    world.current_cores = {-1, 8};
    world.countries = {{2, {-3, 9}}};
    const auto result = compare_inverse(world);
    require(result.mismatched_pairs == 0, "invalid synthetic ids must not create false claims");
    require(result.stats.invalid_province_ids == 4, "invalid ids must be counted");
}

void randomized_consistency_and_mutation() {
    std::mt19937 random(0x455534);
    for (int iteration = 0; iteration < 1000; ++iteration) {
        World world;
        world.current = static_cast<Tag>(random());
        const int province_count = 1 + static_cast<int>(random() % 30);
        const int country_count = 1 + static_cast<int>(random() % 12);
        world.provinces.resize(static_cast<std::size_t>(province_count));
        world.countries.resize(static_cast<std::size_t>(country_count));
        for (int country = 0; country < country_count; ++country) {
            world.countries[static_cast<std::size_t>(country)].tag = static_cast<Tag>(random());
        }
        for (int province = 0; province < province_count; ++province) {
            auto& data = world.provinces[static_cast<std::size_t>(province)];
            data.owner = world.countries[random() % country_count].tag;
            world.countries[static_cast<std::size_t>(data.owner % country_count)].owned.push_back(province);
            // Rebuild owned below from the actual owner, not the incidental index above.
            for (auto& country : world.countries) country.owned.clear();
            for (int prior = 0; prior <= province; ++prior) {
                const Tag owner = world.provinces[static_cast<std::size_t>(prior)].owner;
                for (auto& country : world.countries) {
                    if (country.tag == owner) country.owned.push_back(prior);
                }
            }
            if (random() % 2) {
                data.cores.push_back(world.current);
                world.current_cores.push_back(province);
            }
            if (random() % 3 == 0) {
                data.claims.push_back(world.current);
                world.current_claims.push_back(province);
            }
            if (random() % 11 == 0) data.cores.push_back(static_cast<Tag>(random()));
        }
        auto result = compare_inverse(world);
        require(result.mismatched_pairs == 0, "randomized consistent model must match");
        if (!world.current_cores.empty() && !world.countries.empty()) {
            world.countries[0].owned.clear();
            result = compare_inverse(world);
            // Clearing one owner's list can expose a mismatch, but must never crash or
            // produce an out-of-range bit access; exact direction is covered above.
            require(result.stats.country_pairs == world.countries.size(), "mutated model completes");
        }
    }
}

}  // namespace

int main() {
    empty_lists();
    consistent_core_claim_and_id_zero();
    duplicates_and_multiple_provinces();
    full_tag_range();
    owner_owned_desynchronization();
    invalid_province_ids_are_contained();
    randomized_consistency_and_mutation();
    std::puts("equivalence tests passed: empty, duplicates, owners, id 0, tag range, mismatches, random");
    return 0;
}
