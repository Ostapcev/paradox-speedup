#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace modifierpatch {

constexpr std::uintptr_t add_function_rva = 0x8EA420;
constexpr std::uintptr_t subtract_function_rva = 0x8EA730;
constexpr std::uintptr_t add_search_rva = 0x8EA4E0;
constexpr std::uintptr_t subtract_search_rva = 0x8EA7F1;
constexpr std::uintptr_t unit_parameters_rva = 0xD54600;
constexpr std::uintptr_t country_recalculation_rva = 0x93D130;
constexpr std::uintptr_t unit_modifier_build_rva = 0xAE7230;
constexpr std::uintptr_t unit_combat_value_rva = 0xAEBEB0;
constexpr std::uintptr_t modifier_copy_rva = 0x8EA2D0;
constexpr std::uintptr_t modifier_scale_rva = 0x8EA920;
constexpr std::uintptr_t country_cache_reset_rva = 0x93AE10;
constexpr std::uintptr_t country_cache_rebuild_rva = 0x939EF0;
constexpr wchar_t expected_exe_sha256[] =
    L"9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a";

struct ProfileResult {
    bool compatible = false;
    const char* error = "not checked";
    std::size_t checked_sites = 0;
};

ProfileResult check_eu4_image(std::uintptr_t image) noexcept;
std::wstring executable_sha256(const wchar_t* path);

}  // namespace modifierpatch
