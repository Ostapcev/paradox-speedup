#include "profile.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstring>

namespace triggerpatch {
namespace {

template <std::size_t N>
bool expect(std::uintptr_t image, std::uintptr_t rva,
    const std::array<std::uint8_t, N>& bytes, ProfileResult& result,
    const char* error) noexcept {
    if (std::memcmp(reinterpret_cast<const void*>(image + rva), bytes.data(), N) != 0) {
        result.error = error;
        return false;
    }
    ++result.checked_sites;
    return true;
}

bool expect_call(std::uintptr_t image, std::uintptr_t site, std::uintptr_t target,
    ProfileResult& result, const char* error) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(image + site);
    std::int32_t displacement = 0;
    if (bytes[0] != 0xE8) {
        result.error = error;
        return false;
    }
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    if (image + site + 5 + displacement != image + target) {
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
        result.error = "not the expected Windows x64 EU4 1.37.5 image";
        return result;
    }
    ++result.checked_sites;

    constexpr std::array<std::uint8_t, 7> entry{
        0x4C, 0x8B, 0x3D, 0x66, 0xC3, 0xAA, 0x01};
    constexpr std::array<std::uint8_t, 7> entry_next{0x49, 0x8B, 0x3F, 0x4D, 0x8B, 0x67, 0x08};
    constexpr std::array<std::uint8_t, 16> trigger{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
    constexpr std::array<std::uint8_t, 16> construct{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C};
    if (!expect(image, batch_entry_rva, entry, result, "batch entry signature mismatch") ||
        !expect(image, batch_entry_continuation_rva, entry_next, result,
            "batch continuation signature mismatch") ||
        !expect(image, trigger_eval_rva, trigger, result, "Trigger_AND prologue mismatch") ||
        !expect(image, context_construct_rva, construct, result,
            "trigger context constructor mismatch") ||
        !expect_call(image, first_call_rva, trigger_eval_rva, result,
            "first trigger CALL mismatch or another patch is active") ||
        !expect_call(image, second_call_rva, trigger_eval_rva, result,
            "second trigger CALL mismatch or another patch is active") ||
        !expect_call(image, finish_call_rva, finish_target_rva, result,
            "batch finish CALL mismatch or another patch is active")) {
        return result;
    }
    result.compatible = true;
    result.error = "compatible";
    return result;
}

}  // namespace triggerpatch
