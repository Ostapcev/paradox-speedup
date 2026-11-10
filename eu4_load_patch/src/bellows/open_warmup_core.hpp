// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/open_warmup_core.hpp; see ../../LICENSE.bellows.txt.
#pragma once
// open_warmup: the choice of the list, the opens it records while learning and the open ahead of
// the game, apart from the CreateFileW import so that tests can run them on real folders
// (tests/test_open_warmup.cpp). See open_warmup.cpp for the game side.
#include "access_profile.hpp"

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace sb::warmup {

// Where the list of a start came from (EU4SB_OpenWarmupStats.mode).
enum Source : long { None = 0, Learned = 1, BaseAndMods = 2, All = 3 };

constexpr std::size_t kPathChars = 259;

struct LearnRecord {
    volatile LONG done;
    wchar_t path[kPathChars + 1];
};

// The list a start warms up, in stored form: the learned profile of this mod set when it has
// entries; otherwise the shipped base layer (game files) followed by allList(true), the "all" list
// of the enabled mods only; without a base layer allList(false), the full "all" list.
template <class AllList>
Source ChooseList(const std::wstring& learnedPath, const std::wstring& basePath, AllList&& allList,
                  std::vector<std::wstring>& lines) {
    std::string text;
    if (profile::ReadWhole(learnedPath, text) && !(lines = profile::ParseList(text)).empty()) return Learned;
    std::string base;
    const bool hasBase = profile::ReadWhole(basePath, base) && !(lines = profile::ParseList(base)).empty();
    const auto mods = allList(hasBase);
    lines.insert(lines.end(), mods.begin(), mods.end());
    return hasBase ? BaseAndMods : All;
}

// An open the learning hook records: a successful read of an existing file.
inline bool Recorded(HANDLE result, LPCWSTR path, DWORD access, DWORD disposition) noexcept {
    return result != INVALID_HANDLE_VALUE && path != nullptr && access == GENERIC_READ && disposition == OPEN_EXISTING;
}

// Copies path into the row; the row counts (done = 1) only when the whole path fit.
inline void Record(LearnRecord& r, LPCWSTR path) noexcept {
    std::size_t length = 0;
    for (; length < kPathChars && path[length] != L'\0'; ++length) r.path[length] = path[length];
    r.path[length] = L'\0';
    if (path[length] == L'\0') InterlockedExchange(&r.done, 1);  // truncated paths are dropped
}

// The paths of the rows claimed so far that were written in full, in claim order.
inline std::vector<std::wstring> RecordedPaths(const LearnRecord* rows, LONG claimed, std::size_t capacity) {
    const std::size_t count = static_cast<std::size_t>(claimed) < capacity ? static_cast<std::size_t>(claimed) : capacity;
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (rows[index].done) paths.emplace_back(rows[index].path);
    }
    return paths;
}

// Opens one listed file as the game will and closes it at once: nothing is read or changed.
inline bool OpenAhead(const std::wstring& path) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

}  // namespace sb::warmup
