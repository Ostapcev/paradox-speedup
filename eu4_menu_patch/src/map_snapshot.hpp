#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace hamweard {

// One SAdjacency of a province template: 0x24 bytes in both game builds.
inline constexpr std::size_t adjacency_size = 0x24;

// Whether a province's neighbours after a world reset are the ones saved before
// the first canal, only with other costs. The fields that name a link are the
// neighbour (+0), the source (+4), the passage (+8) and the type (+0x1C).
// CalculateNeighborPaths rewrites the two costs (+0xC, +0x10) and nothing else.
inline bool same_links(std::span<const std::uint8_t> saved, std::span<const std::uint8_t> current) noexcept {
    if (saved.size() != current.size() || saved.size() % adjacency_size != 0) return false;
    for (std::size_t i = 0; i < saved.size(); i += adjacency_size) {
        if (std::memcmp(saved.data() + i, current.data() + i, 0xC) != 0) return false;
        if (std::memcmp(saved.data() + i + 0x1C, current.data() + i + 0x1C, 4) != 0) return false;
    }
    return true;
}

// The parts of the game's map a canal leaves changed after a world reset: each
// province's neighbours and the naval distances.
struct MapView {
    std::vector<std::span<std::uint8_t>> neighbours;
    std::span<std::uint8_t> naval;
};

// The map as it was before the first canal.
class MapSnapshot {
    std::vector<std::vector<std::uint8_t>> neighbours_;
    std::vector<std::uint8_t> naval_;
    bool taken_ = false;

public:
    bool taken() const noexcept { return taken_; }

    // Only the first call counts: later canals change the map further.
    void take(const MapView& map) {
        if (taken_ || map.neighbours.empty()) return;
        neighbours_.reserve(map.neighbours.size());
        for (const auto& links : map.neighbours) neighbours_.emplace_back(links.begin(), links.end());
        naval_.assign(map.naval.begin(), map.naval.end());
        taken_ = true;
    }

    // Puts the costs and the naval distances back. False, with nothing written,
    // when the map no longer has the links it had: the caller has to restart.
    bool restore(const MapView& map) const {
        if (!taken_ || map.neighbours.size() != neighbours_.size() || map.naval.size() != naval_.size()) return false;
        for (std::size_t i = 0; i < neighbours_.size(); ++i)
            if (!same_links(neighbours_[i], map.neighbours[i])) return false;
        for (std::size_t i = 0; i < neighbours_.size(); ++i)
            if (!neighbours_[i].empty()) std::memcpy(map.neighbours[i].data(), neighbours_[i].data(), neighbours_[i].size());
        if (!naval_.empty()) std::memcpy(map.naval.data(), naval_.data(), naval_.size());
        return true;
    }
};

}  // namespace hamweard
