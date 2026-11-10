#pragma once
// Stand-in for the Bellows/Anvil plumbing (common.hpp, modules.hpp, benchmark.hpp) that the
// ported ram_prefetch.cpp and open_warmup.cpp use. Hooks are installed by the functions at the
// end of those files with plain pointer swaps; the log goes to eu4_load_patch.log.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

enum AnvilLogLevel { ANVIL_LOG_INFO = 1, ANVIL_LOG_WARNING = 2 };

namespace sb {

struct Stats {
    std::uint32_t structSize;
    std::uint32_t layoutVersion;
    volatile LONG64 requests;
    volatile LONG64 hits;
    volatile LONG64 misses;
    volatile LONG64 skips;
    volatile LONG64 mismatches;
    volatile LONG64 duplicateKeys;
    volatile LONG64 verifications;
    volatile LONG readyDisabled;
    volatile LONG cacheDisabled;
    volatile LONG mode;
    volatile LONG initialized;
};
#define SB_STATS_INIT {sizeof(::sb::Stats), 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

inline void Increment(volatile LONG64* value) noexcept { InterlockedIncrement64(value); }

enum class Mode { Off, On, Verify };

// 1 once App_InitGame returned (eu4_load_patch's init hook sets it).
inline volatile LONG g_gameReady = 0;
inline bool ReadyReached() noexcept { return g_gameReady != 0; }

template <typename T> inline T Read(const void* base, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}

inline LONG64 Ticks() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double TicksToMs(LONG64 ticks);
void Log(AnvilLogLevel level, const char* format, ...);
void LogStats(const char* name, const Stats& stats);

// ram_prefetch.cpp
void SetRamPrefetchBudget(std::uint64_t megabytes);
bool InstallRamPrefetch(std::uintptr_t image, const std::wstring& plugin_dir, Mode mode);
void FinishRamPrefetch();
void ReportRamPrefetch();
// open_warmup.cpp
bool InstallOpenWarmup(std::uintptr_t image, const std::wstring& plugin_dir, Mode mode);
void FinishOpenWarmup();
void ReportOpenWarmup();

// Swaps a pointer-sized slot when it still holds `expected`.
bool SwapSlot(std::uintptr_t slot, void* expected, void* replacement);
// Replaces an import of the main image; returns the previous pointer or nullptr.
void* ReplaceImport(std::uintptr_t image, const char* dll, const char* name, void* replacement);

inline constexpr const char* kBuildId = "eu4-1.37.5-steam-win64";

}  // namespace sb
