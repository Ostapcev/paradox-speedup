#pragma once

#include "equivalence.hpp"

#include <cstddef>
#include <cstdint>

namespace knowledgepatch {

enum PairFlag : std::uint8_t {
    pair_religion = 1U << 0,
    pair_core = 1U << 1,
    pair_claim = 1U << 2,
    pair_visible = 1U << 3,
    pair_visibility_unknown = 1U << 4,
    pair_valid = 1U << 5,
};

// Phase 1 only calls the World's read-only adapter. In particular, no game
// relation byte is written here. Phase 2 either compares or applies these bits.
template <typename World>
std::uint8_t compute_pair_flags(const World& world, std::size_t country_index,
    const OwnerBits& inverse, std::uint64_t& invalid_ids) {
    if (!world.country_is_valid_pair(country_index)) return 0;
    (void)inverse;
    std::uint8_t flags = pair_valid;
    const Tag current_tag = world.current_tag();
    const Tag overlord_tag = world.other_overlord_tag(country_index);
    const bool visibility_impossible = overlord_tag == current_tag;
    bool visible = false;
    bool unknown = false;

    world.for_each_owned(country_index, [&](int province_id) {
        if (!world.valid_province(province_id)) {
            ++invalid_ids;
            return true;
        }
        if (!(flags & pair_religion) && world.same_religion(province_id)) {
            flags |= pair_religion;
        }
        // Country core/claim lists are rebuilt independently and can lag behind
        // the province tag containers. The original loop reads these containers
        // directly, so workers must do the same rather than trust the inverse.
        if (!(flags & pair_core)) {
            world.for_each_core_tag(province_id, [&](Tag tag) {
                if (tag == current_tag) flags |= pair_core;
                return !(flags & pair_core);
            });
        }
        if (!(flags & pair_claim)) {
            world.for_each_claim_tag(province_id, [&](Tag tag) {
                if (tag == current_tag) flags |= pair_claim;
                return !(flags & pair_claim);
            });
        }
        if (!visible && !unknown && !visibility_impossible) {
            // These are the same predicates and short-circuit order as the
            // original province loop. A virtual special case is deliberately
            // left to the sequential fallback instead of run on a worker.
            if ((world.other_requires_first_distance(country_index) &&
                 !world.in_other_range(province_id, country_index)) ||
                !world.in_current_range(province_id)) {
                return true;
            }
            if (world.special_visibility_is_virtual(province_id)) {
                unknown = true;
                flags |= pair_visibility_unknown;
                return true;
            }
            if (world.colonial_visibility(province_id)) {
                visible = true;
                flags |= pair_visible;
            }
        }
        constexpr std::uint8_t all = pair_religion | pair_core | pair_claim | pair_visible;
        return (flags & all) != all;
    });
    return flags;
}

}  // namespace knowledgepatch
