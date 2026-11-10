// Plumbing for the ported Bellows modules (see modules.hpp).
#include "modules.hpp"

#include <cstdarg>
#include <cstdio>

void eu4load_log_line(const char* line);  // eu4_load_patch.cpp

namespace sb {

double TicksToMs(LONG64 ticks) {
    static const LONG64 frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency);
}

void Log(AnvilLogLevel level, const char* format, ...) {
    char line[1500];
    const int prefix = std::snprintf(line, sizeof(line), "%s", level == ANVIL_LOG_WARNING ? "WARNING " : "");
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line + prefix, sizeof(line) - static_cast<std::size_t>(prefix), format, arguments);
    va_end(arguments);
    eu4load_log_line(line);
}

void LogStats(const char* name, const Stats& s) {
    Log(ANVIL_LOG_INFO,
        "%s: requests %lld, hits %lld, misses %lld, skips %lld, mismatches %lld, prefetched %lld, verifications %lld, "
        "mode %ld, off at ready %ld, off after mismatch %ld",
        name, s.requests, s.hits, s.misses, s.skips, s.mismatches, s.duplicateKeys, s.verifications, s.mode,
        s.readyDisabled, s.cacheDisabled);
}

bool SwapSlot(std::uintptr_t slot, void* expected, void* replacement) {
    auto* p = reinterpret_cast<void* volatile*>(slot);
    DWORD old = 0;
    if (!VirtualProtect(const_cast<void**>(p), sizeof(void*), PAGE_READWRITE, &old)) return false;
    const bool swapped = InterlockedCompareExchangePointer(p, replacement, expected) == expected;
    DWORD ignored = 0;
    VirtualProtect(const_cast<void**>(p), sizeof(void*), old, &ignored);
    return swapped;
}

void* ReplaceImport(std::uintptr_t image, const char* dll, const char* name, void* replacement) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + static_cast<std::uintptr_t>(dos->e_lfanew));
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;
    for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(image + d->Name), dll) != 0 || d->OriginalFirstThunk == 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(image + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + d->FirstThunk);
        for (std::size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) != 0) continue;
            auto* slot = reinterpret_cast<void* volatile*>(&slots[i].u1.Function);
            void* previous = *slot;
            if (!SwapSlot(reinterpret_cast<std::uintptr_t>(slot), previous, replacement)) return nullptr;
            return previous;
        }
    }
    return nullptr;
}

}  // namespace sb
