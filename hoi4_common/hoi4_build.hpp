// hoi4_build.hpp — which hoi4.exe build is running (from its PE header).
//
// The plugins hook fixed addresses (RVAs). They are verified for these builds only:
//   1.19.4.2 (2da5): TimeDateStamp 0x6ae9f874, SizeOfImage 0x38a3000
//   1.19.5.0 (7c6c): TimeDateStamp 0x6aec984b, SizeOfImage 0x38a3000
// 1.19.5.0 differs from 1.19.4.2 only in version strings, RTTI names and two small parser
// functions (rva 0x124b940..0x124baec, 0x123d970, 0x1256f70); no plugin address is inside them.
// Every hook site is still compared byte for byte before it is changed.
#pragma once

#include <windows.h>

#include <cstdint>

namespace hoi4build {

struct Build {
    std::uint32_t stamp;
    std::uint32_t image_size;
    const char* name;
};

inline constexpr Build kBuilds[] = {
    {0x6ae9f874u, 0x38a3000u, "1.19.4.2.2da5"},
    {0x6aec984bu, 0x38a3000u, "1.19.5.0.7c6c"},
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

}  // namespace hoi4build
