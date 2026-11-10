#include "two_phase.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

using knowledgepatch::OwnerBits;
using knowledgepatch::Tag;

struct Province {
    Tag owner = 0;
    bool same_religion = false;
    bool other_range = false;
    bool current_range = false;
    bool special = false;
    bool colonial = false;
    std::vector<Tag> cores;
    std::vector<Tag> claims;
};

struct Country {
    Tag tag = 0;
    Tag overlord = 0;
    bool first_distance = false;
    std::vector<int> owned;
    bool active = true;
    std::uint8_t kind = 0;
    bool special = false;
};

struct World {
    Tag current = 1;
    bool special_group = false;
    std::vector<int> current_cores;
    std::vector<int> current_claims;
    std::vector<Country> countries;
    std::vector<Province> provinces;
    mutable std::uint64_t visits = 0;
    mutable std::uint64_t range_calls = 0;

    Tag current_tag() const { return current; }
    Tag country_tag(std::size_t index) const { return countries[index].tag; }
    bool country_is_valid_pair(std::size_t index) const {
        const auto& country = countries[index];
        return country.tag != current && country.active && country.kind != 7 && !country.special;
    }
    Tag other_overlord_tag(std::size_t index) const { return countries[index].overlord; }
    bool other_requires_first_distance(std::size_t index) const {
        return countries[index].first_distance;
    }
    bool valid_province(int id) const {
        return id >= 0 && static_cast<std::size_t>(id) < provinces.size();
    }
    Tag province_owner(int id) const { return provinces[static_cast<std::size_t>(id)].owner; }
    bool same_religion(int id) const { return provinces[static_cast<std::size_t>(id)].same_religion; }
    bool in_other_range(int id, std::size_t) const {
        ++range_calls;
        return provinces[static_cast<std::size_t>(id)].other_range;
    }
    bool in_current_range(int id) const {
        ++range_calls;
        return provinces[static_cast<std::size_t>(id)].current_range;
    }
    bool special_visibility_is_virtual(int id) const {
        return special_group && provinces[static_cast<std::size_t>(id)].special;
    }
    bool colonial_visibility(int id) const { return provinces[static_cast<std::size_t>(id)].colonial; }

    template <typename F> void for_each_core_tag(int id, F&& fn) const {
        for (const Tag tag : provinces[static_cast<std::size_t>(id)].cores) {
            if (!fn(tag)) return;
        }
    }
    template <typename F> void for_each_claim_tag(int id, F&& fn) const {
        for (const Tag tag : provinces[static_cast<std::size_t>(id)].claims) {
            if (!fn(tag)) return;
        }
    }
    template <typename F> void for_each_owned(std::size_t index, F&& fn) const {
        for (const int id : countries[index].owned) {
            ++visits;
            if (!fn(id)) return;
        }
    }
    template <typename F> void for_each_current_core(F&& fn) const {
        for (const int id : current_cores) if (!fn(id)) return;
    }
    template <typename F> void for_each_current_claim(F&& fn) const {
        for (const int id : current_claims) if (!fn(id)) return;
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

OwnerBits make_inverse(const World& world) {
    OwnerBits inverse;
    inverse.cores.clear();
    inverse.claims.clear();
    world.for_each_current_core([&](int id) {
        if (world.valid_province(id)) inverse.cores.set(world.province_owner(id));
        return true;
    });
    world.for_each_current_claim([&](int id) {
        if (world.valid_province(id)) inverse.claims.set(world.province_owner(id));
        return true;
    });
    return inverse;
}

std::uint8_t reference(const World& world, std::size_t index) {
    using namespace knowledgepatch;
    if (!world.country_is_valid_pair(index)) return 0;
    std::uint8_t flags = pair_valid;
    const auto& country = world.countries[index];
    for (const int id : country.owned) {
        if (!world.valid_province(id)) continue;
        const auto& province = world.provinces[static_cast<std::size_t>(id)];
        if (province.same_religion) flags |= pair_religion;
        for (const Tag tag : province.cores) if (tag == world.current) flags |= pair_core;
        for (const Tag tag : province.claims) if (tag == world.current) flags |= pair_claim;
        if ((flags & pair_visible) || (flags & pair_visibility_unknown) ||
            country.overlord == world.current) {
            continue;
        }
        if (country.first_distance && !province.other_range) continue;
        if (!province.current_range) continue;
        if (world.special_group && province.special) {
            flags |= pair_visibility_unknown;
        } else if (province.colonial) {
            flags |= pair_visible;
        }
    }
    return flags;
}

void synchronize_inverse_lists(World& world) {
    world.current_cores.clear();
    world.current_claims.clear();
    for (std::size_t id = 0; id < world.provinces.size(); ++id) {
        const auto& province = world.provinces[id];
        for (const Tag tag : province.cores) {
            if (tag == world.current) world.current_cores.push_back(static_cast<int>(id));
        }
        for (const Tag tag : province.claims) {
            if (tag == world.current) world.current_claims.push_back(static_cast<int>(id));
        }
    }
}

void check_country(const World& world, std::size_t index, const char* message) {
    const auto inverse = make_inverse(world);
    std::uint64_t invalid = 0;
    const auto actual = knowledgepatch::compute_pair_flags(world, index, inverse, invalid);
    const auto expected = reference(world, index);
    constexpr std::uint8_t fixed = knowledgepatch::pair_religion | knowledgepatch::pair_core |
        knowledgepatch::pair_claim | knowledgepatch::pair_valid | knowledgepatch::pair_visibility_unknown;
    require((actual & fixed) == (expected & fixed), message);
    if (!(expected & knowledgepatch::pair_visibility_unknown)) {
        require((actual & knowledgepatch::pair_visible) == (expected & knowledgepatch::pair_visible),
            "known visibility differs from original oracle");
    }
}

void targeted_cases() {
    using namespace knowledgepatch;
    World world;
    world.current = 7;
    world.countries = {{9, 1, true, {0, -1}}};
    world.provinces.resize(1);
    auto& province = world.provinces[0];
    province.owner = 9;
    province.same_religion = true;
    province.other_range = true;
    province.current_range = true;
    province.colonial = true;
    province.cores = {7, 7};
    province.claims = {7};
    synchronize_inverse_lists(world);
    const auto inverse = make_inverse(world);
    std::uint64_t invalid = 0;
    const auto flags = compute_pair_flags(world, 0, inverse, invalid);
    require((flags & 0x0f) == 0x0f, "all four flags must be computed");
    require(world.visits == 1, "all-flags short circuit must skip later provinces");
    require(invalid == 0, "short-circuited invalid id must not be visited");

    world.special_group = true;
    province.special = true;
    world.visits = 0;
    invalid = 0;
    const auto unknown = compute_pair_flags(world, 0, inverse, invalid);
    require((unknown & pair_visibility_unknown) && !(unknown & pair_visible),
        "virtual special case must request sequential fallback");

    world.countries[0].overlord = world.current;
    world.countries[0].owned = {0};
    world.special_group = false;
    province.special = false;
    world.range_calls = 0;
    invalid = 0;
    const auto impossible = compute_pair_flags(world, 0, inverse, invalid);
    require(!(impossible & pair_visible) && !(impossible & pair_visibility_unknown),
        "same-overlord visibility must be false without fallback");
    require(world.range_calls == 0, "impossible visibility must not call range functions");

    world.countries[0].owned = {-1, 99, -1};
    invalid = 0;
    (void)compute_pair_flags(world, 0, inverse, invalid);
    require(invalid == 3, "every visited invalid province id must be contained and counted");

    world.current = 65535;
    world.countries[0].tag = 65535;
    world.countries[0].overlord = 0;
    world.provinces[0].owner = 65535;
    synchronize_inverse_lists(world);
    check_country(world, 0, "tag 65535 and duplicate core/claim entries");

    // Regression for the startup crash: skipped countries must not even have
    // their province list inspected, regardless of what that list contains.
    world.countries[0].owned = {-1, 99};
    world.countries[0].active = false;
    world.visits = 0;
    require(compute_pair_flags(world, 0, make_inverse(world), invalid) == 0 && world.visits == 0,
        "inactive country must be skipped before province access");
    world.countries[0].active = true;
    world.countries[0].kind = 7;
    require(compute_pair_flags(world, 0, make_inverse(world), invalid) == 0 && world.visits == 0,
        "country kind 7 must be skipped before province access");
    world.countries[0].kind = 0;
    world.countries[0].special = true;
    require(compute_pair_flags(world, 0, make_inverse(world), invalid) == 0 && world.visits == 0,
        "special tag must be skipped before province access");
    world.countries[0].special = false;
    world.countries[0].tag = world.current;
    require(compute_pair_flags(world, 0, make_inverse(world), invalid) == 0 && world.visits == 0,
        "current country must be skipped before province access");

    // Regression for live mismatches: the country's cached lists can be either
    // stale-empty or stale-nonempty. Province tag containers are the oracle.
    World stale;
    stale.current = 7;
    stale.countries = {{9, 1, false, {0}}};
    stale.provinces.resize(1);
    stale.provinces[0].owner = 9;
    stale.provinces[0].cores = {7};
    stale.provinces[0].claims = {7};
    OwnerBits stale_inverse;
    stale_inverse.cores.clear();
    stale_inverse.claims.clear();
    invalid = 0;
    const auto missing = compute_pair_flags(stale, 0, stale_inverse, invalid);
    require((missing & (pair_core | pair_claim)) == (pair_core | pair_claim),
        "stale-empty inverse must not hide province core/claim tags");

    stale.provinces[0].cores.clear();
    stale.provinces[0].claims.clear();
    stale_inverse.cores.set(9);
    stale_inverse.claims.set(9);
    const auto spurious = compute_pair_flags(stale, 0, stale_inverse, invalid);
    require(!(spurious & (pair_core | pair_claim)),
        "stale-nonempty inverse must not invent province core/claim tags");
}

void randomized_oracle() {
    std::mt19937 random(0x941DC7);
    for (int iteration = 0; iteration < 3000; ++iteration) {
        World world;
        world.current = static_cast<Tag>(random());
        world.special_group = random() % 2 != 0;
        const std::size_t country_count = 1 + random() % 12;
        const std::size_t province_count = 1 + random() % 35;
        world.countries.resize(country_count);
        world.provinces.resize(province_count);
        for (std::size_t index = 0; index < country_count; ++index) {
            auto& country = world.countries[index];
            country.tag = static_cast<Tag>(index * 7919 + 1);
            country.overlord = random() % 4 == 0 ? world.current : static_cast<Tag>(random());
            country.first_distance = random() % 2 != 0;
            country.active = random() % 13 != 0;
            country.kind = random() % 17 == 0 ? 7 : 0;
            country.special = random() % 19 == 0;
            if (random() % 23 == 0) country.tag = world.current;
        }
        for (std::size_t id = 0; id < province_count; ++id) {
            auto& province = world.provinces[id];
            const std::size_t owner = random() % country_count;
            province.owner = world.countries[owner].tag;
            world.countries[owner].owned.push_back(static_cast<int>(id));
            if (random() % 5 == 0) world.countries[owner].owned.push_back(static_cast<int>(id));
            province.same_religion = random() % 2 != 0;
            province.other_range = random() % 3 != 0;
            province.current_range = random() % 3 != 0;
            province.special = random() % 5 == 0;
            province.colonial = random() % 2 != 0;
            if (random() % 3 == 0) province.cores.push_back(world.current);
            if (random() % 4 == 0) province.claims.push_back(world.current);
            if (random() % 11 == 0) province.cores.push_back(static_cast<Tag>(random()));
        }
        for (auto& country : world.countries) {
            if (random() % 3 == 0) country.owned.push_back(-1);
            if (random() % 7 == 0) country.owned.push_back(static_cast<int>(province_count + 100));
        }
        synchronize_inverse_lists(world);
        const auto inverse = make_inverse(world);
        for (std::size_t index = 0; index < country_count; ++index) {
            std::uint64_t invalid = 0;
            const auto actual = knowledgepatch::compute_pair_flags(world, index, inverse, invalid);
            const auto expected = reference(world, index);
            constexpr std::uint8_t fixed = knowledgepatch::pair_religion | knowledgepatch::pair_core |
                knowledgepatch::pair_claim | knowledgepatch::pair_valid |
                knowledgepatch::pair_visibility_unknown;
            if ((actual & fixed) != (expected & fixed) ||
                (!(expected & knowledgepatch::pair_visibility_unknown) &&
                 (actual & knowledgepatch::pair_visible) != (expected & knowledgepatch::pair_visible))) {
                std::fprintf(stderr, "FAILED: random oracle iteration=%d country=%zu actual=%02x expected=%02x\n",
                    iteration, index, actual, expected);
                std::exit(2);
            }
        }
    }
}

}  // namespace

int main() {
    targeted_cases();
    randomized_oracle();
    std::puts("two-phase tests passed: short circuit, duplicates, invalid ids, fallback, tag range, 3000 random worlds");
    return 0;
}
