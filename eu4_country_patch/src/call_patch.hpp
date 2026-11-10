#pragma once

#include <cstddef>
#include <cstdint>

namespace speedpatch {

struct CallPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t original[5]{};
    std::uint8_t replacement[5]{};
    bool committed = false;
};

bool prepare_call(
    std::uint8_t* site,
    const void* expected_target,
    const void* hook,
    CallPatch& patch,
    char* error,
    std::size_t error_size) noexcept;

bool commit_call(CallPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_call(CallPatch& patch) noexcept;
void discard_call(CallPatch& patch) noexcept;

}  // namespace speedpatch
