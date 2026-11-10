#include "profile.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace speedpatch {
namespace {

template <std::size_t N>
bool expect(
    std::uintptr_t image,
    std::uintptr_t rva,
    const std::array<std::uint8_t, N>& expected,
    ProfileResult& result,
    const char* error) noexcept {
    if (std::memcmp(reinterpret_cast<const void*>(image + rva), expected.data(), N) != 0) {
        result.error = error;
        return false;
    }
    ++result.checked_sites;
    return true;
}

bool expect_call(
    std::uintptr_t image,
    std::uintptr_t site_rva,
    std::uintptr_t target_rva,
    ProfileResult& result,
    const char* error) noexcept {
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

    constexpr std::array<std::uint8_t, 16> daily_prologue{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
        0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
    constexpr std::array<std::uint8_t, 16> search_prologue{
        0x4C, 0x89, 0x4C, 0x24, 0x20, 0x89, 0x54,
        0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53};
    constexpr std::array<std::uint8_t, 2> daily_continuation{0x84, 0xC0};
    constexpr std::array<std::uint8_t, 3> search_continuation{0x44, 0x8B, 0xF8};

    if (!expect(image, daily_check_function_rva, daily_prologue, result,
                "daily-search predicate prologue mismatch") ||
        !expect(image, army_search_function_rva, search_prologue, result,
                "army-search function prologue mismatch") ||
        !expect_call(image, daily_check_call_rva, daily_check_function_rva, result,
                     "daily-search CALL mismatch or another patch is already active") ||
        !expect_call(image, army_search_call_rva, army_search_function_rva, result,
                     "army-search CALL mismatch or another patch is already active") ||
        !expect(image, daily_check_call_rva + 5, daily_continuation, result,
                "daily-search continuation mismatch") ||
        !expect(image, army_search_call_rva + 5, search_continuation, result,
                "army-search continuation mismatch")) {
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

}  // namespace speedpatch
