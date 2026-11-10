#pragma once

#include <cstddef>
#include <cstdint>

namespace knowledgepatch {

inline constexpr std::size_t loop_site_size = 11;

struct LoopPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t original[loop_site_size]{};
    std::uint8_t replacement[loop_site_size]{};
    bool committed = false;
};

bool prepare_loop(
    std::uint8_t* site,
    const void* found,
    const void* out,
    const std::uint8_t expected[loop_site_size],
    LoopPatch& patch,
    char* error,
    std::size_t error_size) noexcept;

bool commit_loop(LoopPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_loop(LoopPatch& patch) noexcept;
void discard_loop(LoopPatch& patch) noexcept;

}  // namespace knowledgepatch
