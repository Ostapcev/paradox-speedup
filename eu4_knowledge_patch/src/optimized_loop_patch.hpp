#pragma once

#include <cstddef>
#include <cstdint>

namespace knowledgepatch {

inline constexpr std::size_t loop_site_size = 11;  // Maximum patch storage.
inline constexpr std::size_t core_loop_site_size = 11;
inline constexpr std::size_t claim_loop_site_size = 10;

struct LoopHookImage {
    const std::uint8_t* begin;
    const std::uint8_t* end;
    const std::uint8_t* tls_displacement;
    const std::uint8_t* found_displacement;
    const std::uint8_t* out_displacement;
    const std::uint8_t* continuation_displacement;
};

struct LoopPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t original[loop_site_size]{};
    std::uint8_t replacement[loop_site_size]{};
    std::size_t size = 0;
    bool committed = false;
};

bool prepare_loop(std::uint8_t* site, const void* continuation,
    const void* found, const void* out, const std::uint8_t* expected, std::size_t site_size,
    const LoopHookImage& image, std::uint32_t tls_byte_offset, LoopPatch& patch,
    char* error, std::size_t error_size) noexcept;
bool commit_loop(LoopPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_loop(LoopPatch& patch) noexcept;
void discard_loop(LoopPatch& patch) noexcept;

}  // namespace knowledgepatch
