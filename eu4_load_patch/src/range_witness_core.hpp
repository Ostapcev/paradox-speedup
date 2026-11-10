// Copied from Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/range_witness_core.hpp; see LICENSE.bellows.txt.
#pragma once
// range_witness: decision logic, independent of the game's memory so it can be unit-tested
// with a fake Game (tests/test_range_witness.cpp). See range_witness.cpp for the game side.
//
// The country range check 0x3526B0 walks, in order, the country's own provinces and then the
// provinces of the countries it has a qualifying relation with (list +0x1510), and stops at
// the first province whose scaled distance to the target is below the threshold. Its caller
// 0x3520D0 only asks "value <= threshold". The engine remembers, per (country, target
// province, threshold), the province that ended the last walk (the witness, with the related
// country it came from). If the witness still qualifies and still lies below the threshold,
// "in range" is the answer the original would give: the original stops at the first such
// province and any such province makes the answer true. Otherwise the original runs and the
// witness is looked up again; when none exists, the lookup is retried only after a while.
// Verify also keeps the first mismatches of each kind (Mismatch) for the module to write out.

#include <cstdint>
#include <unordered_map>

namespace sb {

struct WitnessKey {
    std::uintptr_t country;
    std::int32_t target;
    std::int32_t threshold;
    bool operator==(const WitnessKey& o) const {
        return country == o.country && target == o.target && threshold == o.threshold;
    }
};

struct WitnessKeyHash {
    std::size_t operator()(const WitnessKey& k) const noexcept {
        std::uint64_t h = k.country * 0x9E3779B97F4A7C15ull;
        h ^= (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.target)) << 32) ^
             static_cast<std::uint32_t>(k.threshold);
        h *= 0xBF58476D1CE4E5B9ull;
        return static_cast<std::size_t>(h ^ (h >> 31));
    }
};

// Where the walk stopped: in the country's own list (related == -1) or in the list of the
// related country at index `related`.
struct Witness {
    std::int32_t related = -1;
    std::int32_t province = -1;
    std::int32_t retry_day = 0;  // province == -1: no witness; look again from this day
    bool found() const { return province >= 0; }
};

struct WitnessStats {
    std::int64_t calls = 0;               // range checks seen
    std::int64_t answered = 0;            // answered from the witness (mode on)
    std::int64_t answered_related = 0;    // ... of which from a related country's province
    std::int64_t originals = 0;           // original walks run
    std::int64_t witness_valid = 0;       // witness still in range (on and verify)
    std::int64_t witness_lost = 0;        // witness present but no longer usable
    std::int64_t acquired = 0;            // new witness found after an in-range original
    std::int64_t not_acquired = 0;        // in range, but no province below the threshold found
    std::int64_t lookups_skipped = 0;     // no witness known, lookup not due yet
    std::int64_t out_of_range = 0;        // original said out of range
    std::int64_t verify_agree = 0;        // verify: valid witness and original in range
    std::int64_t verify_disagree = 0;     // verify: valid witness but original out of range
    std::int64_t verify_value_same = 0;   // verify: original stopped at the witness, same value
    std::int64_t verify_value_differs = 0;
};

// One verify mismatch, copied when it is detected and written out later by the module.
enum class MismatchKind : std::uint8_t { Disagree, ValueDiffers };

struct Mismatch {
    MismatchKind kind = MismatchKind::Disagree;
    std::int32_t day = 0;
    std::uintptr_t country = 0;
    std::int32_t country_id = -1;
    std::int32_t target = -1;
    std::int32_t threshold = 0;
    Witness witness;                    // related index (-1: own list) and province
    std::int32_t witness_owner_id = -1;  // country whose list holds the witness
    std::int64_t witness_value = 0;      // the module's formula, before rounding
    std::int32_t original_value = -1;    // what the original wrote (-1: it returned nullptr)
    Witness first_below;                 // where the walk stops by the module's formula
};

// Game must provide:
//   int* Original(void* country, int* out, void* target, void* arg4, int* threshold);
//   const std::int32_t* ListBegin(void* owner);  const std::int32_t* ListEnd(void* owner);
//   std::int32_t RelatedCount(void* country);
//   void* Related(void* country, std::int32_t index);   // nullptr when the relation does not qualify
//   bool Eligible(void* owner, std::int32_t province);  // the walk's filter
//   bool Value(void* owner, bool related, std::int32_t province, void* target, std::int64_t* value);
//   std::int32_t TargetId(void* target);
//   std::int32_t CountryId(void* country);
//   std::int32_t Day();
template <class Game>
class WitnessEngine {
public:
    static constexpr std::int32_t kRetryDays = 30;
    static constexpr std::size_t kMismatchesPerKind = 32;  // kept per kind; later ones are only counted

    WitnessStats stats;

    // verify = always run the original and compare; otherwise answer from a valid witness.
    int* Check(Game& g, void* country, int* out, void* target, void* arg4, int* threshold, bool verify) {
        ++stats.calls;
        if (country == nullptr || target == nullptr || out == nullptr || threshold == nullptr) {
            ++stats.originals;
            return g.Original(country, out, target, arg4, threshold);
        }
        const std::int32_t limit = *threshold;
        const WitnessKey key{reinterpret_cast<std::uintptr_t>(country), g.TargetId(target), limit};
        bool valid = false;
        std::int64_t witnessValue = 0;
        Witness known;
        const auto it = witnesses_.find(key);
        const bool have = it != witnesses_.end();
        if (have) known = it->second;
        if (have && known.found()) {
            valid = Qualifies(g, country, known, target, limit, &witnessValue);
            ++(valid ? stats.witness_valid : stats.witness_lost);
        }
        if (valid && !verify) {
            ++stats.answered;
            if (known.related >= 0) ++stats.answered_related;
            *out = Rounded(witnessValue);  // what the original writes when it stops at this province
            return out;
        }
        ++stats.originals;
        int* result = g.Original(country, out, target, arg4, threshold);
        const bool inRange = result != nullptr && *result <= limit;
        if (verify && valid) {
            if (inRange) {
                ++stats.verify_agree;
                // When the original stopped at the witness itself it wrote the same value.
                const Witness first = FirstBelow(g, country, target, limit);
                if (first.related == known.related && first.province == known.province) {
                    if (*result == Rounded(witnessValue)) {
                        ++stats.verify_value_same;
                    } else {
                        ++stats.verify_value_differs;
                        Record(g, MismatchKind::ValueDiffers, country, key, known, witnessValue, result, first);
                    }
                }
            } else {
                ++stats.verify_disagree;
                if (kept_[static_cast<int>(MismatchKind::Disagree)] < kMismatchesPerKind) {
                    Record(g, MismatchKind::Disagree, country, key, known, witnessValue, result,
                           FirstBelow(g, country, target, limit));
                }
            }
        }
        if (!inRange) {
            ++stats.out_of_range;
            if (have) witnesses_.erase(key);
            return result;
        }
        if (valid) return result;
        if (have && !known.found() && g.Day() < known.retry_day) {
            ++stats.lookups_skipped;
            return result;
        }
        Witness found = FirstBelow(g, country, target, limit);
        if (found.found()) {
            ++stats.acquired;
        } else {
            ++stats.not_acquired;
            found.retry_day = g.Day() + kRetryDays;
        }
        witnesses_[key] = found;
        return result;
    }

    std::size_t size() const { return witnesses_.size(); }

    // Kept mismatches in the order they were found; an entry never changes once kept.
    std::size_t mismatch_count() const { return mismatchCount_; }
    const Mismatch& mismatch(std::size_t i) const { return mismatches_[i]; }

    // The original's rounding of the value it writes: truncated to thousands.
    static int Rounded(std::int64_t value) { return static_cast<int>(value) / 1000 * 1000; }

private:
    static bool InList(Game& g, void* owner, std::int32_t province) {
        for (const std::int32_t* p = g.ListBegin(owner); p != g.ListEnd(owner); ++p) {
            if (*p == province) return true;
        }
        return false;
    }

    static bool Qualifies(Game& g, void* country, const Witness& w, void* target, std::int32_t limit,
                          std::int64_t* value) {
        void* owner = country;
        if (w.related >= 0) {
            if (w.related >= g.RelatedCount(country)) return false;
            owner = g.Related(country, w.related);
            if (owner == nullptr) return false;
        }
        return InList(g, owner, w.province) && g.Eligible(owner, w.province) &&
               g.Value(owner, w.related >= 0, w.province, target, value) && *value < limit;
    }

    // The province at which the original walk stops (own list first, then related countries).
    static Witness FirstBelow(Game& g, void* country, void* target, std::int32_t limit) {
        Witness w;
        std::int64_t v = 0;
        for (const std::int32_t* p = g.ListBegin(country); p != g.ListEnd(country); ++p) {
            if (g.Eligible(country, *p) && g.Value(country, false, *p, target, &v) && v < limit) {
                w.province = *p;
                return w;
            }
        }
        const std::int32_t related = g.RelatedCount(country);
        for (std::int32_t j = 0; j < related; ++j) {
            void* owner = g.Related(country, j);
            if (owner == nullptr) continue;
            for (const std::int32_t* p = g.ListBegin(owner); p != g.ListEnd(owner); ++p) {
                if (g.Eligible(owner, *p) && g.Value(owner, true, *p, target, &v) && v < limit) {
                    w.related = j;
                    w.province = *p;
                    return w;
                }
            }
        }
        return w;
    }

    void Record(Game& g, MismatchKind kind, void* country, const WitnessKey& key, const Witness& witness,
                std::int64_t witnessValue, const int* result, const Witness& first) {
        std::size_t& kept = kept_[static_cast<int>(kind)];
        if (kept >= kMismatchesPerKind) return;
        ++kept;
        Mismatch& m = mismatches_[mismatchCount_++];
        m.kind = kind;
        m.day = g.Day();
        m.country = key.country;
        m.country_id = g.CountryId(country);
        m.target = key.target;
        m.threshold = key.threshold;
        m.witness = witness;
        void* owner = witness.related >= 0 ? g.Related(country, witness.related) : country;
        m.witness_owner_id = owner != nullptr ? g.CountryId(owner) : -1;
        m.witness_value = witnessValue;
        m.original_value = result != nullptr ? *result : -1;
        m.first_below = first;
    }

    std::unordered_map<WitnessKey, Witness, WitnessKeyHash> witnesses_;
    Mismatch mismatches_[2 * kMismatchesPerKind];
    std::size_t mismatchCount_ = 0;
    std::size_t kept_[2] = {};
};

}  // namespace sb
