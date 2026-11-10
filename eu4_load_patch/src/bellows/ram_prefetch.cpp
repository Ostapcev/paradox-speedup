// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/ram_prefetch.cpp; see ../../LICENSE.bellows.txt.
// Prefetch of ZIP archive members into RAM during start-up (ported from the
// lab's startup_ram_vfs.inc + startup_ram_zip_backend.cpp; lab docs:
// docs/startup-ram-prototype.md).
//
// The game reads ~2.7 k members (~1.5 GB) of the DLC archives through the
// PhysFS ZIP archiver, most of them on its main thread (inflate on the
// critical path). Worker threads read and decode the members the game will
// need — in first-use order from an access profile — into immutable buffers;
// when the game opens such a member, its Io is wrapped and served from the
// buffer. Hooks: OpenArchive / OpenRead / CloseArchive slots of the ZIP
// archiver table (Anvil swap_pointer on the declared slots).
//
// Safety (as in the lab): a buffer is published only after the local header
// matches the central directory and the CRC-32 of the whole output matches;
// at open, the game's own entry (method, CRC, sizes) must match too, otherwise
// the module turns itself off. Anything not ready, not in the profile or not
// supported goes through the game's original Io. "verify" additionally reads
// the original bytes for every read and compares. Prefetch stops when the game
// is ready; Ios already wrapped keep their buffer until the game destroys them.
//
// Differences from the lab: prefetch starts when the plugin loads (not when
// the game mounts each archive); a buffer leaves the memory budget once the
// game takes it (a second open of the same member uses the original path);
// DEFLATE is decoded with the game's own tinfl_decompress (pure miniz code,
// state owned by the worker) instead of zlib; CRC-32 from ntdll.
//
// Profile: <data>/profiles/<key>.zip.txt (learned: "<archive>\t<member>" in
// first-use order, archives in the game folder stored as "@\..."), otherwise
// the shipped <plugin>/profiles/<build>.zip.txt.
#include "modules.hpp"

#include "access_profile.hpp"
#include "ram_prefetch_core.hpp"
#include "zip_reader.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace profile = sb::profile;
namespace prefetch = sb::prefetch;

std::uint64_t g_budgetBytes = 1024ULL << 20;  // set from config before install
unsigned g_workers = sb::profile::kWorkers;  // set when the workers start
constexpr std::size_t kTinflStateBytes = 64 * 1024;  // miniz tinfl_decompressor is ~11 KB
constexpr std::size_t kMaxProfileEntries = 20000;
constexpr unsigned kTinflNonWrappingOutput = 4;  // TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF

// ---- PhysFS ABI (EU4 1.37.5, validated in the lab) --------------------------
struct Io {
    unsigned version;
    unsigned padding;
    void* opaque;
    std::int64_t (*read)(Io*, void*, std::uint64_t);
    std::int64_t (*write)(Io*, const void*, std::uint64_t);
    int (*seek)(Io*, std::uint64_t);
    std::int64_t (*tell)(Io*);
    std::int64_t (*length)(Io*);
    Io* (*duplicate)(Io*);
    int (*flush)(Io*);
    void (*destroy)(Io*);
};
static_assert(sizeof(Io) == 0x50 && offsetof(Io, read) == 0x10, "PhysFS Io ABI");

using OpenArchiveFn = void* (*)(Io*, const char*, int, int*);
using OpenReadFn = Io* (*)(void*, const char*);
using CloseArchiveFn = void (*)(void*);
using TinflFn = int (*)(void*, const std::uint8_t*, std::size_t*, std::uint8_t*, std::uint8_t*, std::size_t*, unsigned);

// The rest of each slot's chain (the archiver's own function unless another
// plugin swapped the slot too), read on each call: Anvil rewrites it while the
// chain changes.
OpenArchiveFn g_openArchivePrev = nullptr;
OpenReadFn g_openReadPrev = nullptr;
CloseArchiveFn g_closeArchivePrev = nullptr;
TinflFn g_tinfl = nullptr;
// The game's ZIP entry (eu4.exe 1.37.5, lab): method, CRC-32, compressed and uncompressed size.
constexpr std::size_t g_offMethod = 0x46, g_offCrc = 0x48, g_offCompressed = 0x50, g_offUncompressed = 0x58;
bool g_verify = false;
const char* g_buildId = nullptr;
std::wstring* g_gameDir = nullptr;
std::wstring* g_dataDir = nullptr;
std::wstring* g_pluginDir = nullptr;
std::wstring* g_profileTarget = nullptr;
volatile LONG g_listSource = 0;  // 1 learned, 2 shipped base layer
std::atomic<bool> g_off{false};   // disabled after a mismatch or once the game is ready

// ---- Buffers and the prefetch backend ---------------------------------------
struct Buffer {
    std::unique_ptr<std::uint8_t[]> bytes;
    std::uint64_t size = 0;
    sb::zip::Entry meta;
};
using BufferPtr = std::shared_ptr<const Buffer>;

struct Archive {
    std::wstring path;
    std::once_flag scanOnce;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool valid = false;
    std::unordered_set<std::string> targets;
    std::unordered_map<std::string, sb::zip::Entry> entries;
};

struct Member {
    std::size_t archive = 0;
    std::string name;
    std::atomic<int> status{prefetch::Pending};
    BufferPtr buffer;  // guarded by Backend::mutex
};

struct Backend : prefetch::Budget {
    std::vector<std::unique_ptr<Archive>> archives;
    std::vector<std::unique_ptr<Member>> members;  // first-use order
    std::unordered_map<std::string, std::size_t> index;  // "<archive key>\n<member>"
    volatile LONG next = 0;
    std::vector<HANDLE> threads;
};
std::atomic<Backend*> g_backend{nullptr};

// ---- Archives opened by the game ---------------------------------------------
struct OpenedArchive {
    std::string key;     // lower-case full path, UTF-8 (backend lookup)
    std::string stored;  // profile spelling, UTF-8
};
SRWLOCK g_archiveLock = SRWLOCK_INIT;
std::unordered_map<void*, OpenedArchive>* g_opened = nullptr;

// ---- Learning ------------------------------------------------------------------
std::mutex g_learnMutex;
std::vector<std::wstring>* g_learned = nullptr;  // "<stored archive>\t<member>" in first-use order
std::unordered_set<std::wstring>* g_learnedSet = nullptr;
std::atomic<bool> g_learnStopped{false};

LONG64 g_ticksDecode = 0;
volatile LONG64 g_bytesPrefetched = 0, g_bytesServed = 0, g_failed = 0;
volatile LONG g_profileEntries = 0, g_profileArchives = 0;
std::uint64_t g_committedPeak = 0;  // copied from the backend at stop
LONG64 g_startTicks = 0;
volatile LONG64 g_finishTicks = 0;
volatile LONG g_workersRunning = 0;

thread_local std::uint8_t* t_tinflState = nullptr;

}  // namespace

extern "C" {
// requests = game opens of archive members while active, hits = served from
// RAM, misses = not ready yet (read by the game itself), skips = not in the
// profile, duplicateKeys = members prefetched, verifications / mismatches =
// byte or metadata comparisons with the game, mode = 1 on / 2 verify,
// cacheDisabled = off after a mismatch, readyDisabled = stopped at ready.
__declspec(dllexport) sb::Stats EU4SB_RamPrefetchStats = SB_STATS_INIT;
}

namespace {

std::wstring LowerFull(std::wstring path) {
    path = profile::Normalize(std::move(path));
    if (!profile::IsAbsolute(path)) path = *g_gameDir + L"\\" + path;
    wchar_t buffer[1024];
    const DWORD length = GetFullPathNameW(path.c_str(), 1024, buffer, nullptr);
    if (length > 0 && length < 1024) path.assign(buffer, length);
    return profile::Lower(path);
}

bool Inflate(const std::uint8_t* input, std::size_t inputSize, std::uint8_t* output, std::size_t outputSize) {
    if (t_tinflState == nullptr) return false;
    std::memset(t_tinflState, 0, kTinflStateBytes);  // tinfl_init: m_state = 0
    std::size_t in = inputSize;
    std::size_t out = outputSize;
    const int status = g_tinfl(t_tinflState, input, &in, output, output, &out, kTinflNonWrappingOutput);
    return status == 0 && in == inputSize && out == outputSize;  // TINFL_STATUS_DONE
}

void Scan(Archive& a) {
    a.file = CreateFileW(a.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (a.file == INVALID_HANDLE_VALUE) return;
    try {
        a.valid = sb::zip::ParseCentralDirectory(a.file, a.targets, a.entries);
    } catch (...) {
        a.valid = false;
    }
}

// Whether the member of a line of the profile this start read is gone. The workers may have stopped
// before its archive, so the scan runs here when it has not run yet. A line the backend does not
// know stays.
bool LineGone(const std::wstring& line) {
    Backend* b = g_backend.load();
    std::wstring storedArchive, storedMember;
    if (b == nullptr || !prefetch::SplitLine(line, storedArchive, storedMember)) return false;
    const std::wstring key = LowerFull(profile::FromStored(storedArchive, profile::Lower(*g_gameDir)));
    const std::string member = profile::Narrow(storedMember);
    const auto found = b->index.find(profile::Narrow(key) + "\n" + member);
    if (found == b->index.end()) return false;
    Archive& a = *b->archives[b->members[found->second]->archive];
    std::call_once(a.scanOnce, [&a] { Scan(a); });
    return prefetch::MemberGone(a.file == INVALID_HANDLE_VALUE && profile::FileGone(a.path), a.valid,
                                a.entries.count(member) != 0);
}

void Prefetch(Backend& b, Member& m) {
    Archive& a = *b.archives[m.archive];
    std::call_once(a.scanOnce, [&a] { Scan(a); });
    const auto found = a.valid ? a.entries.find(m.name) : a.entries.end();
    if (found == a.entries.end() || found->second.unsupported || found->second.duplicate ||
        !prefetch::Reserve(b, found->second.uncompressedSize, g_budgetBytes)) {
        prefetch::Publish(m.status, false, false);
        InterlockedIncrement64(&g_failed);
        return;
    }
    const sb::zip::Entry& meta = found->second;
    bool ok = false;
    auto buffer = std::make_shared<Buffer>();
    try {
        const LONG64 start = sb::Ticks();
        buffer->bytes.reset(new std::uint8_t[static_cast<std::size_t>(meta.uncompressedSize) + 1]);
        buffer->size = meta.uncompressedSize;
        buffer->meta = meta;
        ok = sb::zip::ReadMember(a.file, m.name, meta, &Inflate, buffer->bytes.get());
        InterlockedExchangeAdd64(&g_ticksDecode, sb::Ticks() - start);
    } catch (...) {
        ok = false;
    }
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        published = prefetch::Publish(m.status, ok, b.stopping.load());
        if (published) m.buffer = std::move(buffer);
    }
    if (published) {
        sb::Increment(&EU4SB_RamPrefetchStats.duplicateKeys);
        InterlockedExchangeAdd64(&g_bytesPrefetched, static_cast<LONG64>(meta.uncompressedSize));
    } else {
        if (!ok) InterlockedIncrement64(&g_failed);
        prefetch::Release(b, meta.uncompressedSize);
    }
}

DWORD WINAPI Worker(LPVOID) {
    Backend& b = *g_backend.load();
    std::unique_ptr<std::uint8_t[]> state(new (std::nothrow) std::uint8_t[kTinflStateBytes]);
    t_tinflState = state.get();
    const LONG count = static_cast<LONG>(b.members.size());
    for (;;) {
        if (b.stopping.load()) break;
        const LONG index = InterlockedIncrement(&b.next) - 1;
        if (index >= count) break;
        Member& m = *b.members[static_cast<std::size_t>(index)];
        if (!prefetch::Claim(m.status)) continue;  // abandoned before we got here
        Prefetch(b, m);
    }
    t_tinflState = nullptr;
    if (InterlockedDecrement(&g_workersRunning) == 0) InterlockedExchange64(&g_finishTicks, sb::Ticks());
    return 0;
}

// Returns the member's buffer when it is ready; otherwise the game reads it itself.
BufferPtr Take(const std::string& archiveKey, const char* member) {
    Backend* b = g_backend.load();
    if (b == nullptr || b->stopping.load()) return nullptr;
    const auto found = b->index.find(archiveKey + "\n" + member);
    if (found == b->index.end()) {
        sb::Increment(&EU4SB_RamPrefetchStats.skips);
        return nullptr;
    }
    Member& m = *b->members[found->second];
    BufferPtr buffer;
    {
        std::lock_guard<std::mutex> lock(b->mutex);
        // Not ready: Take drops it from the queue (or discards the running read) so no work is wasted.
        if (prefetch::Take(m.status)) buffer = std::move(m.buffer);
    }
    if (buffer) {
        prefetch::Release(*b, buffer->size);  // owned by the game's Io from now on
        return buffer;
    }
    sb::Increment(&EU4SB_RamPrefetchStats.misses);
    return nullptr;
}

void StopBackend() {
    Backend* b = g_backend.load();
    if (b == nullptr || !prefetch::Stop(*b)) return;
    if (!b->threads.empty()) {
        WaitForMultipleObjects(static_cast<DWORD>(b->threads.size()), b->threads.data(), TRUE, 10000);
    }
    std::lock_guard<std::mutex> lock(b->mutex);
    g_committedPeak = b->committedPeak;
    for (auto& m : b->members) {
        if (m->buffer) {
            b->committed -= m->buffer->size;
            m->buffer.reset();  // buffers already handed to the game stay alive in their Io
        }
    }
}

bool Active() {
    if (g_off.load()) return false;
    if (sb::ReadyReached()) {
        if (!g_off.exchange(true)) InterlockedExchange(&EU4SB_RamPrefetchStats.readyDisabled, 1);
        return false;
    }
    return true;
}

void Disable() {
    g_off.store(true);
    InterlockedExchange(&EU4SB_RamPrefetchStats.cacheDisabled, 1);
}

// ---- The wrapped Io ------------------------------------------------------------
struct Wrapped {
    Io io;
    Io* real;
    BufferPtr buffer;
    std::uint64_t position = 0;
    bool verify = false;
};
Wrapped* W(Io* io) { return static_cast<Wrapped*>(io->opaque); }
Io* Wrap(Io* real, BufferPtr buffer, bool verify);

std::int64_t WRead(Io* io, void* destination, std::uint64_t length) {
    Wrapped* w = W(io);
    if (!w->buffer) return w->real->read(w->real, destination, length);
    const std::uint64_t count = prefetch::ReadCount(w->buffer->size, w->position, length);
    if (w->verify) {
        const DWORD before = GetLastError();
        const std::int64_t n = w->real->read(w->real, destination, length);
        const DWORD after = GetLastError();
        sb::Increment(&EU4SB_RamPrefetchStats.verifications);
        if (!prefetch::SameRead(n, destination, w->buffer->bytes.get(), w->position, count)) {
            sb::Increment(&EU4SB_RamPrefetchStats.mismatches);
            Disable();
            w->buffer.reset();  // the original's bytes are already in the caller's buffer
            SetLastError(after);
            return n;
        }
        w->position += count;
        SetLastError(before);
        return n;
    }
    if (count != 0) std::memcpy(destination, w->buffer->bytes.get() + w->position, static_cast<std::size_t>(count));
    w->position += count;
    InterlockedExchangeAdd64(&g_bytesServed, static_cast<LONG64>(count));
    return static_cast<std::int64_t>(count);
}

std::int64_t WWrite(Io* io, const void* source, std::uint64_t length) {
    Wrapped* w = W(io);
    return w->real->write(w->real, source, length);
}

int WSeek(Io* io, std::uint64_t position) {
    Wrapped* w = W(io);
    // The game's ZIP seek rejects positions past the member size; keep that answer.
    if (!w->buffer || position > w->buffer->size) return w->real->seek(w->real, position);
    if (w->verify && !w->real->seek(w->real, position)) return 0;
    w->position = position;
    return 1;
}

std::int64_t WTell(Io* io) {
    Wrapped* w = W(io);
    return w->buffer ? static_cast<std::int64_t>(w->position) : w->real->tell(w->real);
}

std::int64_t WLength(Io* io) {
    Wrapped* w = W(io);
    return w->buffer ? static_cast<std::int64_t>(w->buffer->size) : w->real->length(w->real);
}

Io* WDuplicate(Io* io) {
    Wrapped* w = W(io);
    Io* copy = w->real->duplicate(w->real);
    const DWORD error = GetLastError();
    if (copy != nullptr) {
        try {
            copy = Wrap(copy, w->buffer, w->verify);
        } catch (...) {
        }
    }
    SetLastError(error);
    return copy;
}

int WFlush(Io* io) {
    Wrapped* w = W(io);
    return w->real->flush(w->real);
}

void WDestroy(Io* io) {
    Wrapped* w = W(io);
    w->real->destroy(w->real);
    const DWORD error = GetLastError();
    delete w;
    SetLastError(error);
}

Io* Wrap(Io* real, BufferPtr buffer, bool verify) {
    auto* w = new Wrapped{};
    w->real = real;
    w->buffer = std::move(buffer);
    w->verify = verify;
    w->io = {0, 0, w, WRead, WWrite, WSeek, WTell, WLength, WDuplicate, WFlush, WDestroy};
    return &w->io;
}

void Learn(const std::string& storedArchive, const char* member) {
    if (g_learnStopped.load() || g_learned == nullptr) return;
    try {
        std::wstring line = profile::Widen(storedArchive) + L"\t" + profile::Widen(member);
        std::lock_guard<std::mutex> lock(g_learnMutex);
        if (g_learnedSet->insert(line).second) g_learned->push_back(std::move(line));
    } catch (...) {
    }
}

}  // namespace

// ---- Archiver hooks ------------------------------------------------------------
extern "C" void* SB_ZipOpenArchive(Io* io, const char* name, int forWriting, int* claimed) {
    void* archive = g_openArchivePrev(io, name, forWriting, claimed);
    const DWORD error = GetLastError();
    if (archive != nullptr && name != nullptr && Active()) {
        try {
            const std::wstring full = LowerFull(profile::Widen(name));
            OpenedArchive opened{profile::Narrow(full), profile::Narrow(profile::ToStored(full, profile::Lower(*g_gameDir)))};
            AcquireSRWLockExclusive(&g_archiveLock);
            (*g_opened)[archive] = std::move(opened);
            ReleaseSRWLockExclusive(&g_archiveLock);
        } catch (...) {
        }
    }
    SetLastError(error);
    return archive;
}

extern "C" Io* SB_ZipOpenRead(void* archive, const char* name) {
    Io* io = g_openReadPrev(archive, name);
    const DWORD error = GetLastError();
    if (io != nullptr && Active()) {
        try {
            OpenedArchive opened;
            AcquireSRWLockShared(&g_archiveLock);
            const auto found = g_opened->find(archive);
            const bool known = found != g_opened->end();
            if (known) opened = found->second;
            ReleaseSRWLockShared(&g_archiveLock);
            // ZIPfileinfo->entry->name: the resolved entry (after ZIP symlinks).
            const auto* entry = io->opaque ? *static_cast<const std::uint8_t* const*>(io->opaque) : nullptr;
            const char* member = entry ? *reinterpret_cast<const char* const*>(entry) : nullptr;
            if (known && member != nullptr) {
                sb::Increment(&EU4SB_RamPrefetchStats.requests);
                Learn(opened.stored, member);
                if (BufferPtr buffer = Take(opened.key, member)) {
                    if (!prefetch::SameEntry(buffer->meta, sb::Read<std::uint16_t>(entry, g_offMethod),
                                             sb::Read<std::uint32_t>(entry, g_offCrc),
                                             sb::Read<std::uint64_t>(entry, g_offCompressed),
                                             sb::Read<std::uint64_t>(entry, g_offUncompressed))) {
                        sb::Increment(&EU4SB_RamPrefetchStats.mismatches);
                        Disable();
                    } else {
                        io = Wrap(io, std::move(buffer), g_verify);
                        sb::Increment(&EU4SB_RamPrefetchStats.hits);
                    }
                }
            }
        } catch (...) {
        }
    }
    SetLastError(error);
    return io;
}

extern "C" void SB_ZipCloseArchive(void* archive) {
    const DWORD error = GetLastError();
    AcquireSRWLockExclusive(&g_archiveLock);
    g_opened->erase(archive);
    ReleaseSRWLockExclusive(&g_archiveLock);
    SetLastError(error);
    g_closeArchivePrev(archive);
}

namespace {

// Reads the profile and starts the workers (off the game's main thread).
DWORD WINAPI Starter(LPVOID) {
    try {
        const std::wstring user = profile::UserDirectory();
        std::string json;
        if (user.empty() || !profile::ReadWhole(user + L"\\dlc_load.json", json, 1 << 20)) {
            sb::Log(ANVIL_LOG_WARNING, "ram_prefetch: dlc_load.json not found; prefetch and learning off");
            return 0;
        }
        const std::wstring dir = *g_dataDir + L"\\profiles";
        CreateDirectoryW(dir.c_str(), nullptr);
        auto* target = new std::wstring(dir + L"\\" + profile::ProfileKey(g_buildId, json) + L".zip.txt");
        std::string text;
        std::vector<std::wstring> lines;
        LONG source = 0;
        if (profile::ReadWhole(*target, text) && !(lines = profile::ParseList(text)).empty()) {
            source = 1;
        } else if (profile::ReadWhole(*g_pluginDir + L"\\profiles\\" + profile::Widen(g_buildId) + L".zip.txt", text) &&
                   !(lines = profile::ParseList(text)).empty()) {
            source = 2;
        }
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_profileTarget), target);
        InterlockedExchange(&g_listSource, source);
        if (lines.empty()) return 0;

        auto backend = std::make_unique<Backend>();
        std::unordered_map<std::wstring, std::size_t> archiveIndex;
        const std::wstring lowerGame = profile::Lower(*g_gameDir);
        for (const auto& line : lines) {
            std::wstring storedArchive, storedMember;
            if (!prefetch::SplitLine(line, storedArchive, storedMember)) continue;
            const std::wstring key = LowerFull(profile::FromStored(storedArchive, lowerGame));
            auto [slot, added] = archiveIndex.emplace(key, backend->archives.size());
            if (added) {
                auto archive = std::make_unique<Archive>();
                archive->path = key;
                backend->archives.push_back(std::move(archive));
            }
            const std::string member = profile::Narrow(storedMember);
            if (!backend->index.emplace(profile::Narrow(key) + "\n" + member, backend->members.size()).second) continue;
            auto m = std::make_unique<Member>();
            m->archive = slot->second;
            m->name = member;
            backend->archives[slot->second]->targets.insert(member);
            backend->members.push_back(std::move(m));
        }
        InterlockedExchange(&g_profileEntries, static_cast<LONG>(backend->members.size()));
        InterlockedExchange(&g_profileArchives, static_cast<LONG>(backend->archives.size()));
        if (backend->members.empty() || !Active()) return 0;
        Backend* published = backend.release();
        g_backend.store(published);
        g_startTicks = sb::Ticks();
        g_workers = profile::WorkersFor(*g_gameDir);
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        const std::uint64_t setting = g_budgetBytes;
        g_budgetBytes = prefetch::EffectiveBudget(setting, GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0);
        if (g_budgetBytes != setting) {
            sb::Log(ANVIL_LOG_INFO, "ram_prefetch: budget %llu MB with %llu MB free",
                    static_cast<unsigned long long>(g_budgetBytes >> 20),
                    static_cast<unsigned long long>(memory.ullAvailPhys >> 20));
        }
        g_workersRunning = static_cast<LONG>(g_workers);
        for (unsigned index = 0; index < g_workers; ++index) {
            HANDLE thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
            if (thread == nullptr) {
                InterlockedDecrement(&g_workersRunning);
                continue;
            }
            SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
            published->threads.push_back(thread);
        }
    } catch (...) {
        sb::Log(ANVIL_LOG_WARNING, "ram_prefetch: profile could not be loaded; prefetch off");
    }
    return 0;
}

}  // namespace

namespace sb {

void SetRamPrefetchBudget(std::uint64_t megabytes) { g_budgetBytes = megabytes << 20; }

bool InstallRamPrefetch(std::uintptr_t image, const std::wstring& plugin_dir, Mode mode) {
    if (mode == Mode::Off) return false;
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe)) return false;
    try {
        g_gameDir = new std::wstring(exe, length);
        g_gameDir->resize(g_gameDir->find_last_of(L'\\'));
        g_dataDir = new std::wstring(plugin_dir + L"\\eu4_load_patch_data");
        CreateDirectoryW(g_dataDir->c_str(), nullptr);
        g_pluginDir = new std::wstring(plugin_dir);
        g_opened = new std::unordered_map<void*, OpenedArchive>();
        g_learned = new std::vector<std::wstring>();
        g_learnedSet = new std::unordered_set<std::wstring>();
        g_learned->reserve(4096);
    } catch (...) {
        return false;
    }
    g_buildId = kBuildId;
    g_verify = mode == Mode::Verify;
    // eu4.exe 1.37.5 Steam: tinfl_decompress 0x1A08470; ZIP archiver table 0x1C1D890 slots
    // +0x30 openArchive (0x1A073F0), +0x40 openRead (0x1A07580), +0x70 closeArchive (0x1A073B0).
    static const std::uint8_t tinfl_bytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    if (std::memcmp(reinterpret_cast<const void*>(image + 0x1A08470), tinfl_bytes, sizeof(tinfl_bytes)) != 0) {
        Log(ANVIL_LOG_WARNING, "ram_prefetch REFUSED: tinfl bytes differ");
        return false;
    }
    g_tinfl = reinterpret_cast<TinflFn>(image + 0x1A08470);
    g_openArchivePrev = reinterpret_cast<OpenArchiveFn>(image + 0x1A073F0);
    g_openReadPrev = reinterpret_cast<OpenReadFn>(image + 0x1A07580);
    g_closeArchivePrev = reinterpret_cast<CloseArchiveFn>(image + 0x1A073B0);
    if (!SwapSlot(image + 0x1C1D8C0, reinterpret_cast<void*>(g_openArchivePrev), reinterpret_cast<void*>(&SB_ZipOpenArchive))) {
        Log(ANVIL_LOG_WARNING, "ram_prefetch REFUSED: openArchive slot differs");
        return false;
    }
    if (!SwapSlot(image + 0x1C1D8D0, reinterpret_cast<void*>(g_openReadPrev), reinterpret_cast<void*>(&SB_ZipOpenRead))) {
        SwapSlot(image + 0x1C1D8C0, reinterpret_cast<void*>(&SB_ZipOpenArchive), reinterpret_cast<void*>(g_openArchivePrev));
        Log(ANVIL_LOG_WARNING, "ram_prefetch REFUSED: openRead slot differs");
        return false;
    }
    if (!SwapSlot(image + 0x1C1D900, reinterpret_cast<void*>(g_closeArchivePrev), reinterpret_cast<void*>(&SB_ZipCloseArchive))) {
        // closeArchive only forgets the archive; without it entries may outlive it, so go back.
        SwapSlot(image + 0x1C1D8D0, reinterpret_cast<void*>(&SB_ZipOpenRead), reinterpret_cast<void*>(g_openReadPrev));
        SwapSlot(image + 0x1C1D8C0, reinterpret_cast<void*>(&SB_ZipOpenArchive), reinterpret_cast<void*>(g_openArchivePrev));
        Log(ANVIL_LOG_WARNING, "ram_prefetch REFUSED: closeArchive slot differs");
        return false;
    }
    InterlockedExchange(&EU4SB_RamPrefetchStats.mode, g_verify ? 2 : 1);
    InterlockedExchange(&EU4SB_RamPrefetchStats.initialized, 1);
    HANDLE starter = CreateThread(nullptr, 0, &Starter, nullptr, 0, nullptr);
    if (starter != nullptr) CloseHandle(starter);
    return true;
}

// Called after the game-ready event: stop the workers, free unused buffers, save the profile.
void FinishRamPrefetch() {
    if (g_learned == nullptr) return;
    g_off.store(true);
    InterlockedExchange(&EU4SB_RamPrefetchStats.readyDisabled, 1);
    StopBackend();
    g_learnStopped.store(true);
    if (g_profileTarget == nullptr) return;
    try {
        std::vector<std::wstring> learned;
        {
            std::lock_guard<std::mutex> lock(g_learnMutex);
            learned = *g_learned;
        }
        if (learned.empty()) return;
        std::string previousText;
        std::vector<std::wstring> previous;
        if (profile::ReadWhole(*g_profileTarget, previousText)) previous = profile::ParseList(previousText);
        const auto merged = prefetch::MergeProfile(std::move(learned), std::move(previous), kMaxProfileEntries, &LineGone);
        if (!profile::WriteList(*g_profileTarget, merged)) Log(ANVIL_LOG_WARNING, "ram_prefetch: profile NOT saved");
    } catch (...) {
        Log(ANVIL_LOG_WARNING, "ram_prefetch: profile NOT saved");
    }
}

void ReportRamPrefetch() {
    if (g_learned == nullptr) return;
    LogStats("ram_prefetch", EU4SB_RamPrefetchStats);
    static const char* const kSources[] = {"no profile (learning only)", "learned profile", "shipped base layer"};
    const LONG source = g_listSource >= 0 && g_listSource <= 2 ? g_listSource : 0;
    const LONG64 finish = g_finishTicks;
    Log(ANVIL_LOG_INFO,
        "ram_prefetch: %s, %ld members in %ld archives; prefetched %.1f MB, served %.1f MB, failed %lld; decode %.0f ms "
        "(sum over %u workers), workers %s %.0f ms; peak not yet taken %.0f of %llu MB; learned %zu members",
        kSources[source], g_profileEntries, g_profileArchives, g_bytesPrefetched / 1048576.0, g_bytesServed / 1048576.0,
        g_failed, TicksToMs(g_ticksDecode), g_workers, finish ? "finished after" : "still running at stop,",
        finish ? TicksToMs(finish - g_startTicks) : 0.0, g_committedPeak / 1048576.0,
        static_cast<unsigned long long>(g_budgetBytes >> 20), g_learned->size());

    // No learned profile for this mod set yet: this start builds it.
}

}  // namespace sb
