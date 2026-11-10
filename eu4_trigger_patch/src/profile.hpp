#pragma once

#include <cstddef>
#include <cstdint>

namespace triggerpatch {

inline constexpr std::uintptr_t batch_entry_rva = 0x97FA13;
inline constexpr std::uintptr_t batch_entry_continuation_rva = 0x97FA1A;
inline constexpr std::uintptr_t first_call_rva = 0x97FAB2;
inline constexpr std::uintptr_t second_call_rva = 0x97FB97;
inline constexpr std::uintptr_t finish_call_rva = 0x97FC5F;
inline constexpr std::uintptr_t trigger_eval_rva = 0xB98200;
inline constexpr std::uintptr_t context_construct_rva = 0xACAF40;
inline constexpr std::uintptr_t context_destroy_rva = 0x947F0;
inline constexpr std::uintptr_t finish_target_rva = 0x974D00;
inline constexpr std::uintptr_t options_pointer_rva = 0x242BD80;

struct ProfileResult {
    bool compatible = false;
    const char* error = "not checked";
    std::size_t checked_sites = 0;
};

ProfileResult check_eu4_image(std::uintptr_t image) noexcept;

}  // namespace triggerpatch
