#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "profile.hpp"

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: speed_patch_check.exe <path-to-eu4.exe>\n");
        return 2;
    }
    HMODULE mapped = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mapped) {
        std::fwprintf(stderr, L"Cannot map executable: error %lu\n", GetLastError());
        return 3;
    }
    const auto image = reinterpret_cast<std::uintptr_t>(mapped) & ~std::uintptr_t(0xFFF);
    const auto result = speedpatch::check_eu4_image(image);
    const auto hash = speedpatch::executable_sha256(argv[1]);
    std::printf("compatible=%s\nchecked_sites=%zu\nresult=%s\n",
        result.compatible ? "yes" : "no", result.checked_sites, result.error);
    std::printf("sha256=%ls\nexpected_sha256=%ls\n", hash.c_str(), speedpatch::expected_exe_sha256);
    FreeLibrary(mapped);
    return result.compatible ? 0 : 1;
}
