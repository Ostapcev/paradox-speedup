// HOI4 Load Patch 0.1.0, HOI4 1.19.4.2 x64 — faster game start-up without changing behaviour.
//
// Module "registry": every CTrigger registers itself in a global vector of pointers
// (DAT_1433de538, {T** data; int cap; int size}, guarded by SRW lock 1433de540 taken through
// 140591610). The destructor 1405917b0 and the move constructor / move assignment
// 140591470 / 140591b60 unregister through 140597e80, which scans the WHOLE vector:
//   for p in vector: if *p == x { *p = *--last; (re-check p) } else ++p;  size = last - begin
// The vector reaches ~1.4 million entries during the database load, so this is quadratic
// (~1.9 s of main-thread time); half of the calls look for pointers that are not there.
//
// mode=on: the vector stays byte-for-byte what the game would have. Beside it we keep a hash
// index pointer -> position for positions [0, indexed) and check every hit (data[p] == x).
// The rest of the game only appends (597 inline registration sites), except the validation
// with clear 140596db0 and the shrink 140597d00: their call sites are hooked and mark the
// index dirty; a size below the size after our last call also resets it. New entries are
// indexed once (tail [indexed, size)). With the index covering [0, size), a miss means the
// pointer is absent. Entries are unique (one registration per object).
// Checks: mode=shadow runs an exact copy of the game loop and compares every prediction
// (found position / absent); mode=on re-checks every 1024th "absent" answer with a full scan
// and on a mismatch disables itself (the game's algorithm from then on).
// mode=timing: original, with call counts and time. mode=off: no bytes changed.

#include <windows.h>

#include "hoi4_build.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <cwchar>
#include <cwctype>

#include "call_patch.hpp"

namespace {

enum class Mode { off, timing, shadow, on };

constexpr std::uintptr_t lock_rva = 0x33de540;
constexpr std::uintptr_t acquire_rva = 0x591610;   // (char* taken) -> takes the lock if needed
constexpr std::uintptr_t release_rva = 0x2556b20;  // (SRWLOCK*)
constexpr std::uintptr_t remove_rva = 0x597e80;
constexpr std::uintptr_t validate_rva = 0x596db0;  // validate all + clear
constexpr std::uintptr_t shrink_rva = 0x597d00;

struct Site { std::uintptr_t site_rva; std::uint8_t bytes[12]; std::size_t size; std::uintptr_t call_rva; const char* name; };
constexpr Site sites[] = {
    {0x591503, {0x48, 0x8b, 0xd6, 0x48, 0x8b, 0xcf, 0xe8, 0x72, 0x69, 0x00, 0x00}, 11, 0x591509, "move_ctor"},
    {0x59188c, {0x49, 0x8b, 0xd6, 0xe8, 0xec, 0x65, 0x00, 0x00}, 8, 0x59188f, "dtor"},
    {0x591bac, {0x48, 0x8b, 0xd6, 0x48, 0x8b, 0xcf, 0xe8, 0xc9, 0x62, 0x00, 0x00}, 11, 0x591bb2, "move_assign"},
};
constexpr Site clear_sites[] = {
    {0x57ff0a, {0x48, 0x8b, 0x0d, 0x27, 0xe6, 0xe5, 0x02, 0xe8, 0x9a, 0x6e, 0x01, 0x00}, 12, 0x57ff11, "validate_clear1"},
    {0xaefafb, {0x48, 0x8b, 0x0d, 0x36, 0xea, 0x8e, 0x02, 0xe8, 0xa9, 0x72, 0xaa, 0xff}, 12, 0xaefb02, "validate_clear2"},
    {0x18d1bf, {0x48, 0x8b, 0x0d, 0x72, 0x13, 0x25, 0x03, 0xe8, 0x35, 0xab, 0x40, 0x00}, 12, 0x18d1c6, "shrink"},
};
constexpr std::uint8_t remove_prefix[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20,
    0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0xe8};

struct Registry { std::uint64_t* data; std::int32_t capacity; std::int32_t size; };
using RemoveFn = bool (*)(Registry*, std::uint64_t);
using AcquireFn = char* (*)(char*);
using ReleaseFn = void (*)(void*);
using VoidFn = void (*)(void*);

Mode mode = Mode::off;
std::uintptr_t image = 0;
std::wstring log_path;
std::mutex log_mutex;
std::int64_t qpc_frequency = 1;
RemoveFn original_remove = nullptr;
AcquireFn acquire = nullptr;
ReleaseFn release = nullptr;
VoidFn original_validate = nullptr, original_shrink = nullptr;

void log(const char* format, ...) noexcept {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> lock(log_mutex);
    FILE* file = _wfopen(log_path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "[%lu:%lu] %s\n", GetCurrentProcessId(), GetTickCount(), text);
    std::fclose(file);
}

inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double ms(std::uint64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpc_frequency);
}

// ---- open-addressing index pointer -> position (linear probing, backward-shift delete)
struct Index {
    std::uint64_t* keys = nullptr;   // 0 = empty
    std::int32_t* values = nullptr;
    std::size_t mask = 0, count = 0;
    bool ok = true;

    static std::size_t slot_of(std::uint64_t key, std::size_t mask) noexcept {
        key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
        return static_cast<std::size_t>(key) & mask;
    }
    void clear() noexcept {
        if (keys && count) std::memset(keys, 0, (mask + 1) * sizeof(*keys));
        count = 0;
    }
    bool grow() noexcept {
        const std::size_t old_mask = mask;
        std::uint64_t* old_keys = keys;
        std::int32_t* old_values = values;
        const std::size_t size = old_keys ? (old_mask + 1) * 2 : 1u << 16;
        auto* k = static_cast<std::uint64_t*>(std::calloc(size, sizeof(std::uint64_t)));
        auto* v = static_cast<std::int32_t*>(std::malloc(size * sizeof(std::int32_t)));
        if (!k || !v) { std::free(k); std::free(v); ok = false; return false; }
        keys = k; values = v; mask = size - 1; count = 0;
        if (old_keys) {
            for (std::size_t i = 0; i <= old_mask; ++i) if (old_keys[i]) put(old_keys[i], old_values[i]);
            std::free(old_keys); std::free(old_values);
        }
        return true;
    }
    void put(std::uint64_t key, std::int32_t value) noexcept {
        if (!key || !ok) return;
        if (!keys || (count + 1) * 2 > mask + 1) { if (!grow()) return; }
        std::size_t i = slot_of(key, mask);
        while (keys[i] && keys[i] != key) i = (i + 1) & mask;
        if (!keys[i]) { keys[i] = key; ++count; }
        values[i] = value;
    }
    std::int32_t get(std::uint64_t key) const noexcept {
        if (!keys || !key) return -1;
        std::size_t i = slot_of(key, mask);
        while (keys[i]) {
            if (keys[i] == key) return values[i];
            i = (i + 1) & mask;
        }
        return -1;
    }
    void erase(std::uint64_t key) noexcept {
        if (!keys || !key) return;
        std::size_t i = slot_of(key, mask);
        while (keys[i] && keys[i] != key) i = (i + 1) & mask;
        if (!keys[i]) return;
        --count;
        std::size_t j = i;
        for (;;) {  // backward shift
            keys[i] = 0;
            for (;;) {
                j = (j + 1) & mask;
                if (!keys[j]) return;
                const std::size_t home = slot_of(keys[j], mask);
                // keys[j] may move to i unless its home lies cyclically in (i, j]
                if (i <= j ? (home <= i || home > j) : (home <= i && home > j)) break;
            }
            keys[i] = keys[j]; values[i] = values[j];
            i = j;
        }
    }
};

// state (all under the game's registry lock)
Index index;
Registry* indexed_registry = nullptr;
std::int32_t indexed = 0;      // positions [0, indexed) are in the index
std::int32_t last_size = 0;    // size after our last operation (others only append)
std::atomic<bool> dirty{true}; // set after the game's clear (140596db0) / shrink (140597d00)
std::atomic<bool> disabled{false};
std::uint32_t absent_counter = 0;

std::atomic<std::uint64_t> calls{0}, removed{0}, not_found{0}, index_hits{0}, tail_hits{0};
std::atomic<std::uint64_t> absent_answers{0}, resets{0}, full_scans{0}, mismatches{0};
std::atomic<std::uint64_t> total_qpc{0}, size_sum{0};
std::atomic<std::int32_t> max_size{0};
std::atomic<int> mismatch_logged{0};

void reset_index(Registry* registry) noexcept {
    index.clear();
    indexed_registry = registry;
    indexed = 0;
    resets.fetch_add(1, std::memory_order_relaxed);
}

void index_tail(Registry* r) noexcept {
    for (std::int32_t i = indexed; i < r->size; ++i) index.put(r->data[i], i);
    indexed = r->size;
}

// position of x, or -2 = certainly absent (index covers [0, size)), or -1 = unknown
std::int32_t find(Registry* r, std::uint64_t x) noexcept {
    // anything but appends since our last call => start over (a new data pointer after growth
    // keeps positions, that alone is fine)
    if (indexed_registry != r || dirty.exchange(false) || r->size < last_size) reset_index(r);
    std::int32_t p = index.get(x);
    if (p >= 0 && p < indexed && r->data[p] == x) { index_hits.fetch_add(1, std::memory_order_relaxed); return p; }
    if (indexed < r->size) {
        index_tail(r);
        p = index.get(x);
        if (p >= 0 && p < indexed && r->data[p] == x) { tail_hits.fetch_add(1, std::memory_order_relaxed); return p; }
    }
    if (!index.ok) return -1;
    return indexed == r->size ? -2 : -1;
}

// after "x at p removed" (data[p] = old last, size - 1)
void note_removed(Registry* r, std::uint64_t x, std::int32_t p) noexcept {
    index.erase(x);
    if (p < r->size) index.put(r->data[p], p);
    if (indexed > r->size) indexed = r->size;
}

// exact copy of the game's loop (removes all occurrences)
bool original_scan(Registry* r, std::uint64_t x) noexcept {
    std::uint64_t* p = r->data;
    std::uint64_t* last = r->data + r->size;
    while (p != last) {
        if (*p == x) { --last; *p = *last; } else { ++p; }
    }
    const auto remaining = static_cast<std::int32_t>(last - r->data);
    if (r->size - remaining < 1) return false;
    r->size = remaining;
    return true;
}

bool contains(const Registry* r, std::uint64_t x) noexcept {
    for (std::int32_t i = 0; i < r->size; ++i) if (r->data[i] == x) return true;
    return false;
}

void report_mismatch(const char* what, std::int32_t p, std::int32_t size, const Registry* r, bool result) noexcept {
    mismatches.fetch_add(1, std::memory_order_relaxed);
    if (mismatch_logged.fetch_add(1) < 20) {
        log("MISMATCH (%s) p=%d size %d -> %d result=%d", what, p, size, r->size, result);
    }
}

bool remove_hook(Registry* r, std::uint64_t x) {
    const auto t0 = qpc_now();
    bool result;
    if (mode == Mode::timing) {
        const auto size = r->size;
        result = original_remove(r, x);
        size_sum.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
        if (size > max_size.load(std::memory_order_relaxed)) max_size.store(size, std::memory_order_relaxed);
    } else {
        char taken = 0;
        acquire(&taken);
        const auto size = r->size;
        size_sum.fetch_add(static_cast<std::uint64_t>(size), std::memory_order_relaxed);
        if (size > max_size.load(std::memory_order_relaxed)) max_size.store(size, std::memory_order_relaxed);
        if (disabled.load(std::memory_order_relaxed)) {
            result = original_scan(r, x);
        } else {
            const std::int32_t p = find(r, x);
            if (mode == Mode::shadow) {
                const std::uint64_t last = size > 0 ? r->data[size - 1] : 0;
                result = original_scan(r, x);
                if (p >= 0) {
                    if (!(result && r->size == size - 1 && (p == r->size || r->data[p] == last))) {
                        report_mismatch("found", p, size, r, result);
                        dirty = true;
                    } else {
                        note_removed(r, x, p);
                    }
                } else if (p == -2) {
                    absent_answers.fetch_add(1, std::memory_order_relaxed);
                    if (result) { report_mismatch("absent", p, size, r, result); dirty = true; }
                } else if (result) {
                    full_scans.fetch_add(1, std::memory_order_relaxed);
                    dirty = true;
                }
            } else if (p >= 0) {
                r->size = size - 1;
                r->data[p] = r->data[size - 1];
                note_removed(r, x, p);
                result = true;
            } else if (p == -2) {
                absent_answers.fetch_add(1, std::memory_order_relaxed);
                result = false;
                if ((++absent_counter & 1023) == 0 && contains(r, x)) {  // sampled self-check
                    report_mismatch("absent, sampled check: module disabled", p, size, r, true);
                    disabled = true;
                    result = original_scan(r, x);
                }
            } else {
                full_scans.fetch_add(1, std::memory_order_relaxed);
                result = original_scan(r, x);
                dirty = true;
            }
        }
        last_size = r->size;
        if (taken) release(reinterpret_cast<void*>(image + lock_rva));
    }
    calls.fetch_add(1, std::memory_order_relaxed);
    (result ? removed : not_found).fetch_add(1, std::memory_order_relaxed);
    total_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t0), std::memory_order_relaxed);
    return result;
}

void validate_hook(void* r) { original_validate(r); dirty = true; }
void shrink_hook(void* r) { original_shrink(r); dirty = true; }

void report(const char* when) noexcept {
    const auto c = calls.exchange(0);
    if (!c) return;
    log("registry %s: calls=%llu removed=%llu not_found=%llu index_hits=%llu tail_hits=%llu "
        "absent=%llu resets=%llu full_scans=%llu MISMATCH=%llu%s | time=%.1f ms avg size=%.0f "
        "max=%d index entries=%zu",
        when, static_cast<unsigned long long>(c), static_cast<unsigned long long>(removed.exchange(0)),
        static_cast<unsigned long long>(not_found.exchange(0)),
        static_cast<unsigned long long>(index_hits.exchange(0)),
        static_cast<unsigned long long>(tail_hits.exchange(0)),
        static_cast<unsigned long long>(absent_answers.exchange(0)),
        static_cast<unsigned long long>(resets.exchange(0)),
        static_cast<unsigned long long>(full_scans.exchange(0)),
        static_cast<unsigned long long>(mismatches.exchange(0)), disabled ? " DISABLED" : "",
        ms(total_qpc.exchange(0)),
        static_cast<double>(size_sum.exchange(0)) / static_cast<double>(c), max_size.load(), index.count);
}

#include "files.inc"
#include "phases.inc"
#include "tags.inc"

bool files_on = false;
bool phases_on = false;
bool tags_on = false;

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(2000);
        report("2s");
        if (files_on) files_report();
        if (phases_on) phases_report();
        if (tags_on) tags_report();
    }
}

bool hook(const Site& s, const void* target) {
    if (std::memcmp(reinterpret_cast<void*>(image + s.site_rva), s.bytes, s.size) != 0) {
        log("%s hook REFUSED (call site bytes differ)", s.name);
        return false;
    }
    char error[160]{};
    speedpatch::CallPatch patch;
    const auto offset = s.call_rva - s.site_rva;
    if (!speedpatch::prepare_call_bytes(reinterpret_cast<std::uint8_t*>(image + s.call_rva),
            s.bytes + offset, 5, target, patch, error, sizeof(error)) ||
        !speedpatch::commit_call(patch, error, sizeof(error))) {
        speedpatch::discard_call(patch);
        log("%s hook REFUSED (%s)", s.name, error);
        return false;
    }
    return true;
}

void install(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\hoi4_load_patch.log";
    const auto ini = directory + L"\\hoi4_load_patch.ini";
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"hoi4.exe") != 0) return;

    unsigned exe_stamp = 0, exe_size = 0;
    const char* exe_build = hoi4build::detect(&exe_stamp, &exe_size);
    if (!exe_build) {
        log("REFUSED: unknown hoi4.exe build (stamp %08x, image size %x); no bytes changed",
            exe_stamp, exe_size);
        return;
    }
    log("hoi4.exe build %s", exe_build);
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"load", L"files", L"off", value, 32, ini.c_str());
    const FilesMode fm = _wcsicmp(value, L"on") == 0 ? FilesMode::on
        : _wcsicmp(value, L"shadow") == 0 ? FilesMode::shadow
        : _wcsicmp(value, L"timing") == 0 ? FilesMode::timing : FilesMode::off;
    if (fm != FilesMode::off) {
        files_on = install_files(fm);
        log("files=%ls %s", value, files_on ? "installed" : "not installed");
    }
    GetPrivateProfileStringW(L"load", L"phases", L"off", value, 32, ini.c_str());
    if (_wcsicmp(value, L"on") == 0 || _wcsicmp(value, L"physfs") == 0)
        phases_on = install_phases(_wcsicmp(value, L"physfs") == 0);
    GetPrivateProfileStringW(L"load", L"tags", L"off", value, 32, ini.c_str());
    if (_wcsicmp(value, L"on") == 0 || _wcsicmp(value, L"shadow") == 0)
        tags_on = install_tags(_wcsicmp(value, L"on") == 0 ? TagsMode::on : TagsMode::shadow);
    GetPrivateProfileStringW(L"load", L"registry", L"off", value, 32, ini.c_str());
    mode = _wcsicmp(value, L"on") == 0 ? Mode::on : _wcsicmp(value, L"shadow") == 0 ? Mode::shadow
        : _wcsicmp(value, L"timing") == 0 ? Mode::timing : Mode::off;
    if (mode == Mode::off) {
        log("registry disabled by configuration (registry=off); no bytes changed");
        if (files_on || phases_on || tags_on) {
            HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
        }
        return;
    }
    if (std::memcmp(reinterpret_cast<void*>(image + remove_rva), remove_prefix, sizeof(remove_prefix)) != 0) {
        log("registry REFUSED (140597e80 bytes differ: other game version?)");
        return;
    }
    original_remove = reinterpret_cast<RemoveFn>(image + remove_rva);
    acquire = reinterpret_cast<AcquireFn>(image + acquire_rva);
    release = reinterpret_cast<ReleaseFn>(image + release_rva);
    original_validate = reinterpret_cast<VoidFn>(image + validate_rva);
    original_shrink = reinterpret_cast<VoidFn>(image + shrink_rva);
    int cleared = 0;
    if (mode != Mode::timing) {  // the index must hear about every non-append change
        for (const auto& other : clear_sites) {  // check all before patching any
            if (std::memcmp(reinterpret_cast<void*>(image + other.site_rva), other.bytes, other.size) != 0) {
                log("registry REFUSED (%s call site bytes differ)", other.name);
                return;
            }
        }
        for (const auto& c : clear_sites) {
            cleared += hook(c, reinterpret_cast<const void*>(
                c.call_rva == 0x18d1c6 ? &shrink_hook : &validate_hook));
        }
        if (cleared != 3) {
            log("registry REFUSED after partial patch (clear sites %d/3): index stays unused", cleared);
            disabled = true;
        }
    }
    int hooked = 0;
    for (const auto& s : sites) hooked += hook(s, reinterpret_cast<const void*>(&remove_hook));
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    log("initialized version=0.1.0 registry=%s sites=%d/3 clear_sites=%d/3",
        mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "timing", hooked, cleared);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);  // synchronously: the database load starts right after start-up
    }
    return TRUE;
}
