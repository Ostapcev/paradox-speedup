#pragma once

#include <cstddef>
#include <cstdint>

namespace speedpatch {

struct CallPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t original[8]{};
    std::uint8_t replacement[8]{};
    std::uint8_t size = 0;
    bool committed = false;
};

// Patch a call site whose current bytes must equal `expected` (size 5..8).
// The replacement is `call stub` (E8 rel32) padded with NOPs; the stub is
// `mov rax, hook; jmp rax` allocated within rel32 range of the site.
bool prepare_call_bytes(
    std::uint8_t* site,
    const std::uint8_t* expected,
    std::uint8_t size,
    const void* hook,
    CallPatch& patch,
    char* error,
    std::size_t error_size) noexcept;

bool commit_call(CallPatch& patch, char* error, std::size_t error_size) noexcept;
void rollback_call(CallPatch& patch) noexcept;
void discard_call(CallPatch& patch) noexcept;

}  // namespace speedpatch
