// EU4 Gov Patch 0.2.0 experimental, for EU4 1.37.5.0 Windows x64.
//
// Trigger has_government_attribute (140d135c0) walks the country's reform
// stack and calls 1407a6020(reform, attribute) for every reform. That lookup
// checks the reform's `conditional` blocks (their `allow` holds only has_dlc /
// NOT / OR in the game files and is evaluated with an empty scope) and the
// attribute hash sets. For a given (reform, attribute) the answer depends only
// on static database data and the DLC set, so it is memoised here.
// It costs ~2.7% of the main thread in the original.
//
// Modes (eu4_gov_patch.ini, [gov] attr_cache=):
//   off    - change nothing (default)
//   shadow - call the original every time, compare with the remembered answer
//   on     - answer from the cache; first call per key goes to the original
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace {

enum class Mode { off, shadow, on };

// mov edx,r15d; mov rcx,r14; call 1407a6020
constexpr std::uintptr_t site1_rva = 0xd1398c;
constexpr std::uint8_t site1_bytes[] = {0x41, 0x8b, 0xd7, 0x49, 0x8b, 0xce, 0xe8, 0x89, 0x26,
    0xa9, 0xff};
constexpr std::uintptr_t call1_rva = 0xd13992;
constexpr std::uintptr_t site2_rva = 0xd13b12;
constexpr std::uint8_t site2_bytes[] = {0x41, 0x8b, 0xd7, 0x49, 0x8b, 0xce, 0xe8, 0x03, 0x25,
    0xa9, 0xff};
constexpr std::uintptr_t call2_rva = 0xd13b18;
constexpr std::uintptr_t lookup_rva = 0x7a6020;

using LookupFn = std::uint64_t (*)(void*, std::uint32_t);
LookupFn original_lookup = nullptr;
Mode mode = Mode::off;

// Lock-free memo: one 64-bit word per entry = (reform >> 3) | attr << 45 | answer << 63.
constexpr std::size_t slot_count = 1u << 16;
constexpr std::size_t probe_limit = 32;
constexpr std::uint64_t answer_bit = 1ull << 63;
std::atomic<std::uint64_t> slots[slot_count];

std::atomic<std::uint64_t> calls{0}, hits{0}, misses{0}, inserts{0}, mismatches{0},
    bypass{0}, full{0};
std::atomic<std::uint32_t> max_attr{0};
std::atomic<LONG> installation_status{0};
std::wstring log_path;
std::mutex log_mutex;

void log(const char* format, ...) noexcept {
    char text[512];
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

inline std::size_t slot_of(std::uint64_t key) noexcept {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return static_cast<std::size_t>(key) & (slot_count - 1);
}

// 1 = found false, 2 = found true, 0 = absent
int find(std::uint64_t key) noexcept {
    std::size_t index = slot_of(key);
    for (std::size_t i = 0; i < probe_limit; ++i) {
        const std::uint64_t value = slots[index].load(std::memory_order_acquire);
        if (value == 0) return 0;
        if ((value & ~answer_bit) == key) return (value & answer_bit) ? 2 : 1;
        index = (index + 1) & (slot_count - 1);
    }
    return 0;
}

void insert(std::uint64_t key, bool answer) noexcept {
    const std::uint64_t stored = key | (answer ? answer_bit : 0);
    std::size_t index = slot_of(key);
    for (std::size_t i = 0; i < probe_limit; ++i) {
        std::uint64_t value = slots[index].load(std::memory_order_acquire);
        if (value == 0) {
            if (slots[index].compare_exchange_strong(value, stored, std::memory_order_acq_rel)) {
                inserts.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if ((value & ~answer_bit) == key) return;  // another thread stored it
        index = (index + 1) & (slot_count - 1);
    }
    full.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<int> logged_mismatches{0};

// Fast path for mode=on: direct-mapped table, one relaxed 64-bit load per call,
// no counters. A miss calls the original and overwrites the slot.
constexpr std::size_t fast_count = 1u << 13;
std::atomic<std::uint64_t> fast_slots[fast_count];
std::atomic<std::uint64_t> fast_misses{0};

std::uint64_t fast_hook(void* reform, std::uint32_t attribute) {
    const auto address = reinterpret_cast<std::uintptr_t>(reform);
    const std::uint64_t key = (static_cast<std::uint64_t>(address) >> 3) |
        (static_cast<std::uint64_t>(attribute) << 45);
    if ((address & 7) == 0 && address < (1ull << 48) && attribute < (1u << 18)) {
        const std::size_t index =
            static_cast<std::size_t>((key * 0x9e3779b97f4a7c15ull) >> 51) & (fast_count - 1);
        const std::uint64_t value = fast_slots[index].load(std::memory_order_relaxed);
        if ((value & ~answer_bit) == key) return (value & answer_bit) ? 1 : 0;
        const std::uint64_t result = original_lookup(reform, attribute);
        fast_slots[index].store(key | (static_cast<std::uint8_t>(result) ? answer_bit : 0),
            std::memory_order_relaxed);
        fast_misses.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    return original_lookup(reform, attribute);
}

std::uint64_t lookup_hook(void* reform, std::uint32_t attribute) {
    calls.fetch_add(1, std::memory_order_relaxed);
    const auto address = reinterpret_cast<std::uintptr_t>(reform);
    if ((address & 7) != 0 || address >= (1ull << 48) || attribute >= (1u << 18)) {
        bypass.fetch_add(1, std::memory_order_relaxed);
        return original_lookup(reform, attribute);
    }
    std::uint32_t seen = max_attr.load(std::memory_order_relaxed);
    while (attribute > seen &&
           !max_attr.compare_exchange_weak(seen, attribute, std::memory_order_relaxed)) {
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(address) >> 3) |
        (static_cast<std::uint64_t>(attribute) << 45);
    const int cached = find(key);
    if (mode == Mode::on && cached != 0) {
        hits.fetch_add(1, std::memory_order_relaxed);
        return cached == 2 ? 1 : 0;
    }
    const std::uint64_t result = original_lookup(reform, attribute);
    const bool answer = static_cast<std::uint8_t>(result) != 0;
    if (cached == 0) {
        misses.fetch_add(1, std::memory_order_relaxed);
        insert(key, answer);
    } else {
        hits.fetch_add(1, std::memory_order_relaxed);
        if ((cached == 2) != answer) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
            if (logged_mismatches.fetch_add(1) < 20) {
                log("MISMATCH reform=%p attr=%u cached=%d original=%d", reform, attribute,
                    cached == 2 ? 1 : 0, answer ? 1 : 0);
            }
        }
    }
    return result;
}

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        if (mode == Mode::on) {
            log("stats mode=on fast_misses=%llu (hits are not counted in this mode)",
                static_cast<unsigned long long>(fast_misses.load()));
            continue;
        }
        log("stats mode=%s calls=%llu hits=%llu misses=%llu inserts=%llu mismatches=%llu "
            "bypass=%llu full=%llu max_attr=%u",
            mode == Mode::on ? "on" : "shadow",
            static_cast<unsigned long long>(calls.load()),
            static_cast<unsigned long long>(hits.load()),
            static_cast<unsigned long long>(misses.load()),
            static_cast<unsigned long long>(inserts.load()),
            static_cast<unsigned long long>(mismatches.load()),
            static_cast<unsigned long long>(bypass.load()),
            static_cast<unsigned long long>(full.load()), max_attr.load());
    }
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_gov_patch.log";
    const auto ini = directory + L"\\eu4_gov_patch.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }

    wchar_t value[32]{};
    GetPrivateProfileStringW(L"gov", L"attr_cache", L"off", value, 32, ini.c_str());
    mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    if (mode == Mode::off) {
        log("disabled by configuration; no bytes changed");
        installation_status.store(3);
        return 0;
    }

    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!eu4build::code_matches(image, site1_rva, site1_bytes, sizeof(site1_bytes)) ||
        !eu4build::code_matches(image, site2_rva, site2_bytes, sizeof(site2_bytes))) {
        log("REFUSED: bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    original_lookup = reinterpret_cast<LookupFn>(image + eu4build::rva(lookup_rva));
    const void* hook = mode == Mode::on ? reinterpret_cast<const void*>(&fast_hook) :
        reinterpret_cast<const void*>(&lookup_hook);
    char error[160]{};
    speedpatch::CallPatch first;
    speedpatch::CallPatch second;
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(call1_rva)),
            reinterpret_cast<const void*>(original_lookup),
            hook, first, error, sizeof(error)) ||
        !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(call2_rva)),
            reinterpret_cast<const void*>(original_lookup),
            hook, second, error, sizeof(error)) ||
        !speedpatch::commit_call(first, error, sizeof(error)) ||
        !speedpatch::commit_call(second, error, sizeof(error))) {
        speedpatch::rollback_call(first);
        speedpatch::discard_call(first);
        speedpatch::discard_call(second);
        log("REFUSED: %s", error);
        installation_status.store(-5);
        return 5;
    }
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    installation_status.store(mode == Mode::on ? 1 : 2);
    log("initialized version=0.2.0-experimental attr_cache=%s",
        mode == Mode::on ? "on" : "shadow");
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
        installation_status.store(-7);
        return 7;
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4GovPatchStatus() {
    return installation_status.load();
}
