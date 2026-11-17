// stellaris_build.hpp — which stellaris.exe build is running (from its PE header).
//
// The plugins hook fixed addresses (RVAs). They are verified for these builds only:
//   4.5.4 (1b4f) "Cygnus": TimeDateStamp 0x6af23393, SizeOfImage 0x37fa000
//     SHA-256 10d2613a392fe8ac7982701899dc2e43ebf4c1c721ea1baeba6c3fb25db36eb1
// Every hook site is still compared byte for byte before it is changed.
// A new game patch moves almost every address: add the build here only after the
// RVAs of every plugin have been checked against it.
#pragma once

#include <windows.h>

#include <cstdint>

namespace stellarisbuild {

struct Build {
    std::uint32_t stamp;
    std::uint32_t image_size;
    const char* name;
};

inline constexpr Build kBuilds[] = {
    {0x6af23393u, 0x37fa000u, "4.5.4.1b4f"},
};

// Returns the build name, or nullptr for an unknown build. stamp/size get the header values.
inline const char* detect(unsigned* stamp = nullptr, unsigned* size = nullptr) noexcept {
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const std::uint32_t s = nt->FileHeader.TimeDateStamp;
    const std::uint32_t z = nt->OptionalHeader.SizeOfImage;
    if (stamp) *stamp = s;
    if (size) *size = z;
    for (const Build& build : kBuilds)
        if (build.stamp == s && build.image_size == z) return build.name;
    return nullptr;
}

}  // namespace stellarisbuild
