// How many extra worker threads a plugin may start without slowing the game down.
//
// EU4's daily AI already runs on every core (its own parallel-for), and the day's length is
// set by the main thread. Extra threads on a CPU with few physical cores take cores from the
// game's AI tasks, share a core with the main thread (SMT), and make a laptop lower its clock.
// So the count is taken from physical cores, keeping four of them for the game:
//   4 or fewer physical cores -> 1, 5 -> 1, 6 -> 2, 7 -> 3, 8 and more -> 4 (at most `cap`).
#pragma once

#include <windows.h>

#include <cstdlib>
#include <vector>

namespace eu4cpu {

// Physical cores (0 if Windows cannot tell).
inline int physical_cores() noexcept {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size == 0) return 0;
    std::vector<unsigned char> buffer(size);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &size)) return 0;
    int cores = 0;
    for (DWORD at = 0; at < size;) {
        auto* item = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        if (item->Size == 0) break;
        if (item->Relationship == RelationProcessorCore) ++cores;
        at += item->Size;
    }
    return cores;
}

inline int logical_processors() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return static_cast<int>(info.dwNumberOfProcessors);
}

// Default number of extra threads, 1..cap.
inline int auto_workers(int cap = 4) noexcept {
    int cores = physical_cores();
    if (cores <= 0) cores = logical_processors() / 2;  // assume SMT when unknown
    int n = cores - 4;
    if (n < 1) n = 1;
    if (n > cap) n = cap;
    return n;
}

// The "workers" value of an ini section: a number (1..most), or "auto" / missing for
// auto_workers(auto_cap).
inline int read_workers(const wchar_t* section, const wchar_t* ini, int auto_cap = 4,
    int most = 32) noexcept {
    wchar_t value[16]{};
    GetPrivateProfileStringW(section, L"workers", L"auto", value, 16, ini);
    if (_wcsicmp(value, L"auto") == 0 || value[0] == 0) return auto_workers(auto_cap);
    const int n = static_cast<int>(std::wcstol(value, nullptr, 10));
    if (n < 1) return 1;
    return n > most ? most : n;
}

}  // namespace eu4cpu
