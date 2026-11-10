#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace knowledgepatch {

constexpr std::uintptr_t refresh_knowledge_rva = 0x93FE30;
constexpr std::uintptr_t shadow_call_rva = 0x93FF47;
constexpr std::uintptr_t shadow_target_rva = 0x2FC810;
constexpr std::uintptr_t core_loop_rva = 0x941E80;
constexpr std::uintptr_t core_continuation_rva = 0x941E8B;
constexpr std::uintptr_t core_found_rva = 0x941E98;
constexpr std::uintptr_t core_out_rva = 0x941E9B;
constexpr std::uintptr_t claim_loop_rva = 0x941EC0;
constexpr std::uintptr_t claim_continuation_rva = 0x941ECA;
constexpr std::uintptr_t claim_found_rva = 0x941EDA;
constexpr std::uintptr_t claim_out_rva = 0x941EDF;
constexpr std::uintptr_t parallel_apply_rva = 0x941DC7;
constexpr std::uintptr_t parallel_apply_fallback_rva = 0x941DDE;
constexpr std::uintptr_t parallel_apply_commit_rva = 0x941FD7;
constexpr std::uintptr_t parallel_verify_rva = 0x942047;
constexpr std::uintptr_t parallel_verify_continuation_rva = 0x942050;
constexpr std::uintptr_t scheduler_wait_rva = 0x179EE4;
constexpr std::uintptr_t scheduler_wait_resume_rva = 0x179EF1;
constexpr std::uintptr_t scheduler_wait_top_rva = 0x179EB3;
constexpr std::uintptr_t gamestate_pointer_rva = 0x233FE78;
constexpr wchar_t expected_exe_sha256[] =
    L"9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a";

struct ProfileResult {
    bool compatible = false;
    const char* error = "not checked";
    std::size_t checked_sites = 0;
};

ProfileResult check_eu4_image(std::uintptr_t image) noexcept;
std::wstring executable_sha256(const wchar_t* path);

}  // namespace knowledgepatch
