#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace speedpatch {

constexpr std::uintptr_t daily_check_call_rva = 0x0EB74C;
constexpr std::uintptr_t daily_check_function_rva = 0x10BDB0;
constexpr std::uintptr_t army_search_call_rva = 0x0EBB5F;
constexpr std::uintptr_t army_search_function_rva = 0x1022B0;
constexpr wchar_t expected_exe_sha256[] =
    L"9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a";

struct ProfileResult {
    bool compatible = false;
    const char* error = "not checked";
    std::size_t checked_sites = 0;
};

ProfileResult check_eu4_image(std::uintptr_t image) noexcept;
std::wstring executable_sha256(const wchar_t* path);

}  // namespace speedpatch
