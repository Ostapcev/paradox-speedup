// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/open_warmup.cpp; see ../../LICENSE.bellows.txt.
// Parallel first-open warm-up with a learned access profile per mod set
// (ported from the lab's startup_open_warmup.inc; lab docs: docs/startup-cold-start.md).
//
// After a reboot (and after a Defender signature update) the first open of
// every existing game file costs ~4.5 ms while a repeated open costs ~0.04 ms;
// the game's opens are serialized by PhysFS, so ~11.5 k first opens put ~50 s
// on the critical path of a cold start. Worker threads open and immediately
// close each listed file with the game's parameters, earliest-needed first,
// outside the PhysFS lock; nothing is read or changed. Workers stop when the
// game is ready.
//
// List choice (key = game build + hash of dlc_load.json):
//   1. <data>/profiles/<key>.txt — learned on previous starts with this mod set;
//   2. otherwise <plugin>/profiles/<build>.txt (shipped base layer, game files)
//      plus the "all" list of the enabled mods' folders, or the full "all"
//      list when no base layer is shipped.
// Learning: the game's CreateFileW import (through Anvil) records successful
// GENERIC_READ/OPEN_EXISTING opens until the game is ready; then the list is
// merged with the previous one (this run's first-use order first) and saved.
#include "modules.hpp"

#include "access_profile.hpp"
#include "open_warmup_core.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

namespace profile = sb::profile;
namespace warmup = sb::warmup;

constexpr std::size_t kLearnCapacity = 65536;

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using warmup::LearnRecord;

std::vector<std::wstring>* g_paths = nullptr;
volatile LONG g_next = 0;
volatile LONG g_running = 0;
std::int64_t g_startQpc = 0;
std::int64_t g_frequency = 1;
unsigned g_threads = sb::profile::kWorkers;  // set when the workers start

CreateFileWFn g_realCreateFileW = nullptr;  // the rest of the import's chain

LearnRecord* g_learn = nullptr;
volatile LONG g_learnNext = 0;
volatile LONG g_learnStopped = 0;
std::wstring* g_gameDir = nullptr;
std::wstring* g_profileTarget = nullptr;  // set by the starter thread
std::wstring* g_dataDir = nullptr;
std::wstring* g_pluginDir = nullptr;
const char* g_buildId = nullptr;
volatile LONG g_listSource = 0;  // 1 learned, 2 base + mods, 3 all

}  // namespace

extern "C" {
// Warm-up: requests = list entries, hits = opened, misses = failed opens, skips
// = not tried (ready reached), verifications = warm-up wall time in ms,
// duplicateKeys = worker threads, mode = list source (1 learned, 2 base+mods,
// 3 all), initialized = 1 when all workers finished.
__declspec(dllexport) sb::Stats EU4SB_OpenWarmupStats = SB_STATS_INIT;
// Learning: requests = game read opens recorded, hits = entries written,
// skips = opens not recorded (table full), initialized = 1 when written,
// cacheDisabled = 1 when writing failed.
__declspec(dllexport) sb::Stats EU4SB_OpenWarmupLearn = SB_STATS_INIT;
}

extern "C" HANDLE WINAPI SB_LearnCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                             DWORD disposition, DWORD flags, HANDLE templateFile) {
    const HANDLE result = g_realCreateFileW(path, access, share, security, disposition, flags, templateFile);
    const DWORD error = GetLastError();
    if (warmup::Recorded(result, path, access, disposition) && InterlockedCompareExchange(&g_learnStopped, 0, 0) == 0) {
        const LONG index = InterlockedIncrement(&g_learnNext) - 1;
        if (index >= 0 && static_cast<std::size_t>(index) < kLearnCapacity) {
            warmup::Record(g_learn[index], path);
            sb::Increment(&EU4SB_OpenWarmupLearn.requests);
        } else {
            sb::Increment(&EU4SB_OpenWarmupLearn.skips);
        }
    }
    SetLastError(error);
    return result;
}

namespace {

DWORD WINAPI Worker(LPVOID) {
    const LONG count = static_cast<LONG>(g_paths->size());
    for (;;) {
        const LONG index = InterlockedIncrement(&g_next) - 1;
        if (index >= count) break;
        if (sb::ReadyReached()) {
            sb::Increment(&EU4SB_OpenWarmupStats.skips);
            continue;
        }
        if (warmup::OpenAhead((*g_paths)[static_cast<std::size_t>(index)])) {
            sb::Increment(&EU4SB_OpenWarmupStats.hits);
        } else {
            sb::Increment(&EU4SB_OpenWarmupStats.misses);
        }
    }
    if (InterlockedDecrement(&g_running) == 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        InterlockedExchange64(&EU4SB_OpenWarmupStats.verifications, (now.QuadPart - g_startQpc) * 1000 / g_frequency);
        InterlockedExchange(&EU4SB_OpenWarmupStats.initialized, 1);
    }
    return 0;
}

void StartWorkers() {
    InterlockedExchange64(&EU4SB_OpenWarmupStats.requests, static_cast<LONG64>(g_paths->size()));
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    g_frequency = frequency.QuadPart ? frequency.QuadPart : 1;
    g_startQpc = now.QuadPart;
    g_threads = profile::WorkersFor(*g_gameDir);
    g_running = static_cast<LONG>(g_threads);
    unsigned started = 0;
    for (unsigned index = 0; index < g_threads; ++index) {
        if (HANDLE worker = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr)) {
            CloseHandle(worker);
            ++started;
        }
    }
    for (unsigned index = started; index < g_threads; ++index) InterlockedDecrement(&g_running);
    InterlockedExchange64(&EU4SB_OpenWarmupStats.duplicateKeys, static_cast<LONG64>(started));
}

// Builds the list (file system scan may take a while) off the game's main thread.
DWORD WINAPI Starter(LPVOID) {
    try {
        const std::wstring user = profile::UserDirectory();
        std::string json;
        if (user.empty() || !profile::ReadWhole(user + L"\\dlc_load.json", json, 1 << 20)) {
            sb::Log(ANVIL_LOG_WARNING, "open_warmup: dlc_load.json not found; warm-up skipped");
            return 0;
        }
        const std::wstring dir = *g_dataDir + L"\\profiles";
        CreateDirectoryW(dir.c_str(), nullptr);
        auto* target = new std::wstring(dir + L"\\" + profile::ProfileKey(g_buildId, json) + L".txt");
        std::vector<std::wstring> lines;
        const std::wstring basePath = *g_pluginDir + L"\\profiles\\" + profile::Widen(g_buildId) + L".txt";
        const LONG source = warmup::ChooseList(*target, basePath, [&](bool skipGame) {
            return profile::BuildAllList(*g_gameDir, user, profile::ParseEnabledMods(json), skipGame);
        }, lines);
        for (const auto& line : lines) g_paths->push_back(profile::FromStored(line, *g_gameDir));
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&g_profileTarget), target);
        InterlockedExchange(&g_listSource, source);
        InterlockedExchange(&EU4SB_OpenWarmupStats.mode, source);
        if (!g_paths->empty()) StartWorkers();
    } catch (...) {
        sb::Log(ANVIL_LOG_WARNING, "open_warmup: list could not be built; warm-up skipped");
    }
    return 0;
}

}  // namespace

namespace sb {

bool InstallOpenWarmup(std::uintptr_t image, const std::wstring& plugin_dir, Mode mode) {
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
        g_paths = new std::vector<std::wstring>();
        g_paths->reserve(16384);
    } catch (...) {
        return false;
    }
    g_buildId = kBuildId;
    g_learn = static_cast<LearnRecord*>(
        VirtualAlloc(nullptr, sizeof(LearnRecord) * kLearnCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (g_learn != nullptr) {
        g_realCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
        void* previous = g_realCreateFileW ? ReplaceImport(image, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&SB_LearnCreateFileW)) : nullptr;
        if (previous) {
            g_realCreateFileW = reinterpret_cast<CreateFileWFn>(previous);
            InterlockedExchange(&EU4SB_OpenWarmupLearn.mode, 1);
        } else {
            Log(ANVIL_LOG_WARNING, "open_warmup: learning off (CreateFileW import not replaced)");
        }
    }
    HANDLE starter = CreateThread(nullptr, 0, &Starter, nullptr, 0, nullptr);
    if (starter == nullptr) return false;
    CloseHandle(starter);
    return true;
}

// Called after the game-ready event: stop learning and save the merged profile.
void FinishOpenWarmup() {
    if (g_learn == nullptr || EU4SB_OpenWarmupLearn.mode == 0 || g_profileTarget == nullptr) return;
    InterlockedExchange(&g_learnStopped, 1);
    Sleep(200);  // let calls that claimed a row finish writing it
    const LONG claimed = InterlockedCompareExchange(&g_learnNext, 0, 0);
    try {
        const auto paths = warmup::RecordedPaths(g_learn, claimed, kLearnCapacity);
        std::string previousText;
        std::vector<std::wstring> previous;
        if (profile::ReadWhole(*g_profileTarget, previousText)) previous = profile::ParseList(previousText);
        const auto merged = profile::UniqueStored(paths, *g_gameDir, previous);
        if (!merged.empty() && profile::WriteList(*g_profileTarget, merged)) {
            InterlockedExchange64(&EU4SB_OpenWarmupLearn.hits, static_cast<LONG64>(merged.size()));
            InterlockedExchange(&EU4SB_OpenWarmupLearn.initialized, 1);
        } else {
            InterlockedExchange(&EU4SB_OpenWarmupLearn.cacheDisabled, 1);
        }
    } catch (...) {
        InterlockedExchange(&EU4SB_OpenWarmupLearn.cacheDisabled, 1);
    }
    // The table stays allocated: the hook keeps forwarding for the whole session.
}

void ReportOpenWarmup() {
    const Stats& s = EU4SB_OpenWarmupStats;
    if (s.mode == 0) return;
    static const char* const kSources[] = {"none", "learned profile", "base layer + mods", "all list"};
    Log(ANVIL_LOG_INFO, "open_warmup: %s, %lld entries, %lld opened, %lld failed, %lld not tried, %lld threads, %s %lld ms",
        kSources[s.mode >= 0 && s.mode <= 3 ? s.mode : 0], s.requests, s.hits, s.misses, s.skips, s.duplicateKeys,
        s.initialized ? "finished in" : "still running after", s.verifications);
    const Stats& l = EU4SB_OpenWarmupLearn;
    Log(ANVIL_LOG_INFO, "open_warmup learning: %lld opens recorded, profile %s (%lld entries)", l.requests,
        l.initialized ? "saved" : (l.cacheDisabled ? "NOT saved" : "not written"), l.hits);

    if (l.mode != 0) {
    }
    // The warm-up opened the game's files ahead of it: their wall time per file is the start's open cost.
    if (s.initialized && s.hits > 0) {
    }
    // No learned profile for this mod set yet: this start builds it.
}

}  // namespace sb
