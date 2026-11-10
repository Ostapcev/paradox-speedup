#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: load_tests.exe <dll-path> <expected-status>\n");
        return 2;
    }
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }
    using StatusFn = LONG (*)();
    using CountFn = std::uint64_t (*)();
    const auto status = reinterpret_cast<StatusFn>(GetProcAddress(module, "EU4SpeedPatchStatus"));
    const auto count = reinterpret_cast<CountFn>(GetProcAddress(module, "EU4SpeedPatchSearchCount"));
    if (!status || !count) {
        std::fprintf(stderr, "Required exports are missing\n");
        return 4;
    }
    const auto expected = static_cast<LONG>(std::strtol(argv[2], nullptr, 10));
    LONG actual = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        actual = status();
        if (actual != 0) break;
        Sleep(50);
    }
    std::printf("status=%ld expected=%ld searches=%llu\n", actual, expected,
        static_cast<unsigned long long>(count()));
    FreeLibrary(module);
    if (actual != expected) return 5;
    std::puts("DLL load and export test passed");
    return 0;
}
