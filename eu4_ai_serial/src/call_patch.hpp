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
// Entry hook: target must start with the 5 bytes `prologue` (one whole instruction, no RIP use).
// The target gets `jmp stub`; `*trampoline` runs the copied bytes and jumps to target+5.
bool prepare_entry(
    std::uint8_t* target,
    const std::uint8_t* prologue,
    const void* hook,
    CallPatch& patch,
    void** trampoline,
    char* error,
    std::size_t error_size) noexcept;

void rollback_call(CallPatch& patch) noexcept;
void discard_call(CallPatch& patch) noexcept;

}  // namespace speedpatch
