// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/ram_prefetch_core.hpp; see ../../LICENSE.bellows.txt.
#pragma once
// ram_prefetch: the profile lines, the memory budget, the states of a member, the reads served from
// a buffer and the profile merge, apart from the archiver slots so that tests can run them
// (tests/test_ram_prefetch.cpp). See ram_prefetch.cpp for the game side.
#include "zip_reader.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace sb::prefetch {

// "<stored archive>\t<member>"; false for a line without both parts.
inline bool SplitLine(const std::wstring& line, std::wstring& archive, std::wstring& member) {
    const std::size_t tab = line.find(L'\t');
    if (tab == std::wstring::npos || tab == 0 || tab + 1 >= line.size()) return false;
    archive = line.substr(0, tab);
    member = line.substr(tab + 1);
    return true;
}

// The budget for this start: at most a quarter of the memory free now, so a small machine does not
// page the game out to hold what it would read anyway. 0 means free memory is unknown.
inline std::uint64_t EffectiveBudget(std::uint64_t setting, std::uint64_t freeBytes) {
    if (freeBytes == 0) return setting;
    return setting < freeBytes / 4 ? setting : freeBytes / 4;
}

// Unpacked bytes the game has not taken yet. The mutex also guards the members' buffers.
struct Budget {
    std::mutex mutex;
    std::condition_variable budgetCv;
    std::uint64_t committed = 0;
    std::uint64_t committedPeak = 0;
    std::atomic<bool> stopping{false};
};

// Waits until bytes fit under limit; false for a member larger than the whole limit or once stopping.
inline bool Reserve(Budget& b, std::uint64_t bytes, std::uint64_t limit) {
    if (bytes > limit) return false;
    std::unique_lock<std::mutex> lock(b.mutex);
    b.budgetCv.wait(lock, [&] { return b.stopping.load() || b.committed + bytes <= limit; });
    if (b.stopping.load()) return false;
    b.committed += bytes;
    if (b.committed > b.committedPeak) b.committedPeak = b.committed;
    return true;
}

// Waiting workers wake and get nothing; false when it was stopping already. stopping changes under
// the mutex, so a worker between its check and its wait cannot miss the wake-up.
inline bool Stop(Budget& b) {
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        if (b.stopping.exchange(true)) return false;
    }
    b.budgetCv.notify_all();
    return true;
}

inline void Release(Budget& b, std::uint64_t bytes) {
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        b.committed -= bytes;
    }
    b.budgetCv.notify_all();
}

// A member's way: Pending -> Running (a worker took it) -> Ready (published) -> Taken (the game's Io
// owns the buffer); Failed when it could not be read; Abandoned when the game opened it first.
enum Status : int { Pending, Running, Ready, Failed, Taken, Abandoned };

// A worker takes a member that is still queued.
inline bool Claim(std::atomic<int>& status) {
    int pending = Pending;
    return status.compare_exchange_strong(pending, Running);
}

// After the read: Ready when it succeeded and the backend is not stopping (the caller holds the
// budget's mutex and stores the buffer only then); otherwise Failed, unless the game abandoned it.
inline bool Publish(std::atomic<int>& status, bool ok, bool stopping) {
    int running = Running;
    if (ok && !stopping && status.compare_exchange_strong(running, Ready)) return true;
    running = Running;
    status.compare_exchange_strong(running, Failed);  // an Abandoned member stays Abandoned
    return false;
}

// The game opens the member: true when its buffer is ready (the caller then moves it out); otherwise
// the member leaves the queue, or its running read is discarded.
inline bool Take(std::atomic<int>& status) {
    int ready = Ready;
    if (status.compare_exchange_strong(ready, Taken)) return true;
    int pending = Pending;
    if (!status.compare_exchange_strong(pending, Abandoned)) {
        int running = Running;
        status.compare_exchange_strong(running, Abandoned);
    }
    return false;
}

// The game's entry for the member it opened (method, CRC-32, sizes) describes the prefetched bytes.
inline bool SameEntry(const zip::Entry& prefetched, std::uint16_t method, std::uint32_t crc32,
                      std::uint64_t compressedSize, std::uint64_t uncompressedSize) {
    return prefetched.method == method && prefetched.crc32 == crc32 && prefetched.compressedSize == compressedSize &&
           prefetched.uncompressedSize == uncompressedSize;
}

// Bytes a read of length at position returns from a member of size bytes.
inline std::uint64_t ReadCount(std::uint64_t size, std::uint64_t position, std::uint64_t length) {
    const std::uint64_t available = size - position;
    return length < available ? length : available;
}

// "verify": the game's read returned what the buffer holds at position.
inline bool SameRead(std::int64_t gameCount, const void* gameBytes, const std::uint8_t* buffer, std::uint64_t position,
                     std::uint64_t count) {
    return gameCount == static_cast<std::int64_t>(count) &&
           (count == 0 || std::memcmp(gameBytes, buffer + position, static_cast<std::size_t>(count)) == 0);
}

// A member of the previous profile is gone when its archive is, or when the archive's central
// directory was read and does not list it. An archive that could not be read says nothing.
inline bool MemberGone(bool archiveGone, bool directoryRead, bool listed) {
    return archiveGone || (directoryRead && !listed);
}

// The saved profile: this start's members in first-use order, then the previous profile's members
// not seen this time and not gone(line), while the profile has fewer than maxEntries.
template <class Gone>
inline std::vector<std::wstring> MergeProfile(std::vector<std::wstring> learned, std::vector<std::wstring> previous,
                                              std::size_t maxEntries, Gone&& gone) {
    std::unordered_set<std::wstring> seen(learned.begin(), learned.end());
    for (auto& line : previous) {
        if (learned.size() >= maxEntries) break;
        if (seen.insert(line).second && !gone(line)) learned.push_back(std::move(line));
    }
    return learned;
}

}  // namespace sb::prefetch
