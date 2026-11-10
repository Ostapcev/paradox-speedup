#include "profile.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <vector>

namespace modifierpatch {
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

    constexpr std::array<std::uint8_t, 8> prologue{
        0x40, 0x55, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x48};
    constexpr std::array<std::uint8_t, 7> continuation{
        0x4C, 0x63, 0x4A, 0x64, 0x45, 0x33, 0xC0};
    constexpr std::array<std::uint8_t, 20> add_search{
        0x39, 0x38, 0x0F, 0x84, 0x96, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC1, 0x48, 0x83, 0xC0, 0x08, 0x48, 0x3B, 0xCA, 0x7C, 0xEC};
    constexpr std::array<std::uint8_t, 20> subtract_search{
        0x39, 0x39, 0x0F, 0x84, 0x95, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC0, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x3B, 0xC2, 0x7C, 0xEC};
    constexpr std::array<std::uint8_t, 15> unit_parameters{
        0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56,
        0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
    constexpr std::array<std::uint8_t, 10> country_recalculation{
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20};
    constexpr std::array<std::uint8_t, 15> unit_modifier_build{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10};
    constexpr std::array<std::uint8_t, 11> unit_combat_value{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55};
    constexpr std::array<std::uint8_t, 11> modifier_copy{
        0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02};
    constexpr std::array<std::uint8_t, 10> modifier_scale{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10};
    constexpr std::array<std::uint8_t, 9> country_cache_reset{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
    constexpr std::array<std::uint8_t, 11> country_cache_rebuild{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55};
    if (!expect(image, add_function_rva, prologue, result, "add modifier prologue mismatch") ||
        !expect(image, add_function_rva + 8, continuation, result,
            "add modifier continuation mismatch") ||
        !expect(image, add_search_rva, add_search, result, "add modifier search mismatch") ||
        !expect(image, subtract_function_rva, prologue, result,
            "subtract modifier prologue mismatch") ||
        !expect(image, subtract_function_rva + 8, continuation, result,
            "subtract modifier continuation mismatch") ||
        !expect(image, subtract_search_rva, subtract_search, result,
            "subtract modifier search mismatch") ||
        !expect(image, unit_parameters_rva, unit_parameters, result,
            "unit parameters prologue mismatch") ||
        !expect(image, country_recalculation_rva, country_recalculation, result,
            "country recalculation prologue mismatch") ||
        !expect(image, unit_modifier_build_rva, unit_modifier_build, result,
            "unit modifier build prologue mismatch") ||
        !expect(image, unit_combat_value_rva, unit_combat_value, result,
            "unit combat value prologue mismatch") ||
        !expect(image, modifier_copy_rva, modifier_copy, result,
            "modifier copy prologue mismatch") ||
        !expect(image, modifier_scale_rva, modifier_scale, result,
            "modifier scale prologue mismatch") ||
        !expect(image, country_cache_reset_rva, country_cache_reset, result,
            "country cache reset prologue mismatch") ||
        !expect(image, country_cache_rebuild_rva, country_cache_rebuild, result,
            "country cache rebuild prologue mismatch")) {
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
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return output;
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

}  // namespace modifierpatch
