#pragma once

#include <cstddef>
#include <cstdint>

namespace knowledgepatch {

inline constexpr std::size_t parallel_patch_capacity = 32;

struct ParallelHookImage {
    const std::uint8_t* begin;
    const std::uint8_t* end;
    const std::uint8_t* tls_displacement;  // Nullable for the verifier.
    const std::uint8_t* continuation_displacement;
    const std::uint8_t* secondary_displacement;  // Nullable; used by apply/fallback.
};

struct ParallelPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t original[parallel_patch_capacity]{};
    std::uint8_t replacement[parallel_patch_capacity]{};
    std::size_t size = 0;
    bool committed = false;
};

bool prepare_parallel_patch(std::uint8_t* site, const void* continuation,
    const void* secondary, const std::uint8_t* expected, std::size_t site_size,
    const ParallelHookImage& image,
    std::uint32_t tls_byte_offset, ParallelPatch& patch, char* error,
    std::size_t error_size) noexcept;
bool commit_parallel_patch(ParallelPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_parallel_patch(ParallelPatch& patch) noexcept;
void discard_parallel_patch(ParallelPatch& patch) noexcept;

}  // namespace knowledgepatch
