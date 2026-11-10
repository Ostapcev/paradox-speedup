#include "profile.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <vector>

namespace knowledgepatch {
namespace {

template <std::size_t N>
bool expect(std::uintptr_t image, std::uintptr_t rva,
    const std::array<std::uint8_t, N>& expected, ProfileResult& result,
    const char* error) noexcept {
    if (std::memcmp(reinterpret_cast<const void*>(image + rva), expected.data(), N) != 0) {
        result.error = error;
        return false;
    }
    ++result.checked_sites;
    return true;
}

bool expect_call(std::uintptr_t image, std::uintptr_t site_rva,
    std::uintptr_t target_rva, ProfileResult& result, const char* error) noexcept {
    const auto* site = reinterpret_cast<const std::uint8_t*>(image + site_rva);
    if (site[0] != 0xE8) {
        result.error = error;
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    if (image + site_rva + 5 + displacement != image + target_rva) {
        result.error = error;
        return false;
    }
    ++result.checked_sites;
    return true;
}

}  // namespace

ProfileResult check_eu4_image(std::uintptr_t image) noexcept {
    ProfileResult result;
    if (!image) {
        result.error = "EU4 image base is null";
        return result;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        result.error = "invalid DOS header";
        return result;
    }
    const auto nt = static_cast<std::uintptr_t>(dos->e_lfanew);
    const auto* signature = reinterpret_cast<const DWORD*>(image + nt);
    const auto* file = reinterpret_cast<const IMAGE_FILE_HEADER*>(image + nt + 4);
    const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(image + nt + 24);
    if (*signature != IMAGE_NT_SIGNATURE || file->Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (file->Characteristics & IMAGE_FILE_DLL) || file->SizeOfOptionalHeader < sizeof(*optional) ||
        optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional->SizeOfImage != 0x25E0000) {
        result.error = "not the expected Windows x64 EU4 image layout";
        return result;
    }
    ++result.checked_sites;

    const auto section_table = image + nt + 24 + file->SizeOfOptionalHeader;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(section_table);
    bool text_found = false;
    for (unsigned index = 0; index < file->NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (section.VirtualAddress == 0x1000 && section.Misc.VirtualSize >= 0x1B64D78 &&
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
            (section.Characteristics & IMAGE_SCN_MEM_READ)) {
            text_found = true;
            break;
        }
    }
    if (!text_found) {
        result.error = "the .text section differs from EU4 1.37.5";
        return result;
    }
    ++result.checked_sites;

    constexpr std::array<std::uint8_t, 16> refresh_prologue{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    constexpr std::array<std::uint8_t, 16> target_prologue{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
        0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xB9};
    constexpr std::array<std::uint8_t, 8> continuation{
        0x48, 0x8B, 0x10, 0x48, 0x8B, 0xC8, 0xFF, 0x52};
    constexpr std::array<std::uint8_t, 11> core_loop{
        0x48, 0x8B, 0x0A, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x41, 0x3B, 0xC8};
    constexpr std::array<std::uint8_t, 4> core_continuation{0x74, 0x0B, 0x48, 0x83};
    constexpr std::array<std::uint8_t, 2> core_found{0x41, 0xB5};
    constexpr std::array<std::uint8_t, 4> core_out{0x44, 0x0F, 0xB6, 0x85};
    constexpr std::array<std::uint8_t, 10> claim_loop{
        0x48, 0x8B, 0x08, 0x48, 0xC1, 0xE9, 0x20, 0x66, 0x3B, 0xCA};
    constexpr std::array<std::uint8_t, 4> claim_continuation{0x74, 0x0E, 0x48, 0x8B};
    constexpr std::array<std::uint8_t, 3> claim_found{0xC6, 0x44, 0x24};
    constexpr std::array<std::uint8_t, 2> claim_out{0x45, 0x84};
    constexpr std::array<std::uint8_t, 23> parallel_apply{
        0x45, 0x32, 0xFF, 0x32, 0xC9, 0x45, 0x32, 0xC0,
        0x44, 0x88, 0x85, 0x40, 0x02, 0x00, 0x00, 0x45,
        0x32, 0xC9, 0x44, 0x88, 0x4C, 0x24, 0x38};
    constexpr std::array<std::uint8_t, 8> parallel_fallback{
        0x4D, 0x8B, 0xA4, 0x24, 0x38, 0x13, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 16> parallel_commit{
        0x41, 0xF6, 0xDF, 0x1A, 0xD2, 0x80, 0xE2, 0x10,
        0x41, 0x0F, 0xB6, 0x45, 0x45, 0x24, 0xEF, 0x0A};
    constexpr std::array<std::uint8_t, 9> parallel_verify{
        0x4C, 0x8B, 0x75, 0x80, 0x48, 0x8B, 0x74, 0x24, 0x60};
    constexpr std::array<std::uint8_t, 8> parallel_verify_continuation{
        0x4C, 0x8B, 0x64, 0x24, 0x40, 0x48, 0x8B, 0x5C};
    constexpr std::array<std::uint8_t, 13> scheduler_wait{
        0x0F, 0x57, 0xC0, 0xF3, 0x0F, 0x7F, 0x44,
        0x24, 0x20, 0x48, 0x8B, 0x5F, 0x18};
    constexpr std::array<std::uint8_t, 3> scheduler_wait_resume{0x48, 0x85, 0xDB};
    constexpr std::array<std::uint8_t, 4> scheduler_wait_top{0x48, 0x8B, 0x47, 0x10};

    if (!expect(image, refresh_knowledge_rva, refresh_prologue, result,
            "RefreshKnowledge prologue mismatch") ||
        !expect(image, shadow_target_rva, target_prologue, result,
            "shadow target prologue mismatch") ||
        !expect_call(image, shadow_call_rva, shadow_target_rva, result,
            "shadow CALL mismatch or another patch is already active") ||
        !expect(image, shadow_call_rva + 5, continuation, result,
            "shadow continuation mismatch") ||
        !expect(image, core_loop_rva, core_loop, result, "core loop signature mismatch") ||
        !expect(image, core_continuation_rva, core_continuation, result,
            "core continuation mismatch") ||
        !expect(image, core_found_rva, core_found, result, "core found block mismatch") ||
        !expect(image, core_out_rva, core_out, result, "core out block mismatch") ||
        !expect(image, claim_loop_rva, claim_loop, result, "claim loop signature mismatch") ||
        !expect(image, claim_continuation_rva, claim_continuation, result,
            "claim continuation mismatch") ||
        !expect(image, claim_found_rva, claim_found, result, "claim found block mismatch") ||
        !expect(image, claim_out_rva, claim_out, result, "claim out block mismatch") ||
        !expect(image, parallel_apply_rva, parallel_apply, result,
            "parallel apply signature mismatch") ||
        !expect(image, parallel_apply_fallback_rva, parallel_fallback, result,
            "parallel fallback continuation mismatch") ||
        !expect(image, parallel_apply_commit_rva, parallel_commit, result,
            "parallel apply continuation mismatch") ||
        !expect(image, parallel_verify_rva, parallel_verify, result,
            "parallel verifier signature mismatch") ||
        !expect(image, parallel_verify_continuation_rva, parallel_verify_continuation, result,
            "parallel verifier continuation mismatch") ||
        !expect(image, scheduler_wait_rva, scheduler_wait, result,
            "scheduler wait signature mismatch") ||
        !expect(image, scheduler_wait_resume_rva, scheduler_wait_resume, result,
            "scheduler wait continuation mismatch") ||
        !expect(image, scheduler_wait_top_rva, scheduler_wait_top, result,
            "scheduler loop head mismatch")) {
        return result;
    }
    result.compatible = true;
    result.error = "compatible";
    return result;
}

std::wstring executable_sha256(const wchar_t* path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<UCHAR> object;
    std::wstring output;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return output;
    }
    DWORD object_size = 0;
    DWORD returned = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0) < 0 ||
        returned != sizeof(object_size) || !object_size) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return output;
    }
    object.resize(object_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return output;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return output;
    }
    std::array<UCHAR, 65536> buffer{};
    bool valid = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            valid = false;
            break;
        }
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
            valid = false;
            break;
        }
    }
    std::array<UCHAR, 32> digest{};
    if (valid && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
        constexpr wchar_t digits[] = L"0123456789abcdef";
        output.reserve(64);
        for (const auto byte : digest) {
            output.push_back(digits[byte >> 4]);
            output.push_back(digits[byte & 15]);
        }
    }
    CloseHandle(file);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return output;
}

}  // namespace knowledgepatch
