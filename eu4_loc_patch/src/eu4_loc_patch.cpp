// EU4 Loc Patch: removes two quadratic scans from localisation loading.
//  * 1416faae0 (add duplicate key): the linear search of table+0x38 for (hash, file) is
//    replaced by a per-table index.
//  * 1416fb1c0 (report duplicate keys): one linear scan of table+0x38 per duplicate run is
//    replaced by a single pass.
// Mode off only logs a checksum of every table at finalize; shadow computes both ways and
// compares; on uses the fast path. Checksums must match between off and on.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace {

enum class Mode { off, shadow, on };
Mode mode = Mode::off;
std::atomic<long> installation_status{0};
std::wstring log_path;
std::mutex log_mutex;
std::uintptr_t image = 0;
std::int64_t qpc_frequency = 1;

constexpr std::uintptr_t add_dup_rva = 0x16faae0;
constexpr std::uintptr_t finalize_rva = 0x16fb1c0;
constexpr std::uintptr_t finalize_call_rva = 0x16fd849;
constexpr std::uintptr_t string_buffer_rva = 0x2357b20;
constexpr std::uintptr_t lower_bound_rva = 0x16ffd10;
constexpr std::uintptr_t grow38_rva = 0x1700010;
constexpr std::uintptr_t grow20_rva = 0x12e4650;
constexpr std::uintptr_t resize_rva = 0x0416ef0;
constexpr std::uintptr_t report_rva = 0x14b36c0;
constexpr std::uint8_t add_dup_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x44, 0x89, 0x4c,
    0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55};

using LowerBoundFn = std::uint64_t* (*)(std::uint64_t*, std::uint64_t*, const void*);
using GrowFn = void* (*)(void*, void*, const void*);
using ResizeFn = void (*)(void*, std::int64_t);
using ReportFn = void (*)(const char**, const char**, const char**, int, const char*);
using FinalizeFn = std::uint8_t (*)(std::uint8_t*);
LowerBoundFn lower_bound_fn = nullptr;
GrowFn grow38 = nullptr;
GrowFn grow20 = nullptr;
ResizeFn resize_fn = nullptr;
ReportFn report_fn = nullptr;
FinalizeFn original_finalize = nullptr;

void log(const char* format, ...) noexcept {
    char text[700];
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
inline double ms(std::int64_t ticks) { return 1000.0 * static_cast<double>(ticks) / qpc_frequency; }

// ---- table layout
struct Entry20 {  // sorted lookup: hash, value offset, unused
    std::uint64_t hash;
    std::uint32_t valoff;
    std::uint32_t pad;
};
struct Entry38 {  // insertion order: hash, key offset, value offset, file, extra
    std::uint64_t hash;
    std::uint32_t keyoff;
    std::uint32_t valoff;
    std::uint32_t file;
    std::uint32_t extra;
};
static_assert(sizeof(Entry20) == 16 && sizeof(Entry38) == 24, "layout");

template <class T> inline T*& vec_begin(std::uint8_t* table, int offset) {
    return *reinterpret_cast<T**>(table + offset);
}
template <class T> inline T*& vec_end(std::uint8_t* table, int offset) {
    return *reinterpret_cast<T**>(table + offset + 8);
}
template <class T> inline T*& vec_cap(std::uint8_t* table, int offset) {
    return *reinterpret_cast<T**>(table + offset + 16);
}
inline std::uint8_t* string_buffer() {
    return *reinterpret_cast<std::uint8_t**>(image + eu4build::rva(string_buffer_rva));
}

// ---- per-table index of table+0x38: (hash, file) -> first position
struct TableIndex {
    std::unordered_map<std::uint64_t, std::uint32_t> first;
    std::size_t indexed = 0;
    Entry38 first_entry{};
};
std::mutex index_mutex;
std::unordered_map<std::uint8_t*, TableIndex> indexes;

inline std::uint64_t index_key(std::uint32_t hash, std::uint32_t file) {
    return (static_cast<std::uint64_t>(hash) << 32) | file;
}

std::atomic<long long> add_calls{0}, found_calls{0}, rebuilds{0}, add_mismatch{0};
std::atomic<long long> add_ticks{0}, finalize_ticks{0}, finalize_mismatch{0};

void index_catch_up(std::uint8_t* table, TableIndex& index) {
    Entry38* begin = vec_begin<Entry38>(table, 0x38);
    const std::size_t size = static_cast<std::size_t>(vec_end<Entry38>(table, 0x38) - begin);
    if (size < index.indexed ||
        (index.indexed > 0 && (index.first_entry.hash != begin[0].hash ||
                                  index.first_entry.file != begin[0].file))) {
        index.first.clear();
        index.indexed = 0;
        rebuilds.fetch_add(1, std::memory_order_relaxed);
    }
    if (index.indexed == 0 && size > 0) index.first_entry = begin[0];
    for (std::size_t i = index.indexed; i < size; ++i) {
        if (begin[i].hash >> 32) continue;  // never matches a 32-bit key hash
        index.first.emplace(index_key(static_cast<std::uint32_t>(begin[i].hash), begin[i].file),
            static_cast<std::uint32_t>(i));
    }
    index.indexed = size;
}

int linear_find(std::uint8_t* table, std::uint64_t hash, int file) {
    Entry38* begin = vec_begin<Entry38>(table, 0x38);
    const int count = static_cast<int>(vec_end<Entry38>(table, 0x38) - begin);
    int i = 0;
    for (; i < count; ++i) {
        if (begin[i].hash == hash && static_cast<int>(begin[i].file) == file) break;
    }
    return i;
}

void push38(std::uint8_t* table, const Entry38& value) {
    Entry38* end = vec_end<Entry38>(table, 0x38);
    if (end == vec_cap<Entry38>(table, 0x38)) {
        grow38(table + 0x38, end, &value);
    } else {
        *end = value;
        vec_end<Entry38>(table, 0x38) = end + 1;
    }
}

// exact replacement of 1416faae0
std::uint64_t add_dup_hook(std::uint8_t* table, const char* key, const char* value, int file,
    std::uint32_t extra) {
    const std::int64_t start = qpc_now();
    std::uint8_t* sb = string_buffer();
    const std::uint32_t off = static_cast<std::uint32_t>(
        static_cast<int>(*reinterpret_cast<std::uint64_t*>(sb + 0x28)) -
        static_cast<int>(*reinterpret_cast<std::uint64_t*>(sb + 0x20)));
    const int keylen = static_cast<int>(std::strlen(key)) + 1;
    const int vallen = static_cast<int>(std::strlen(value)) + 1;
    std::uint32_t h = 0x811c9dc5u;
    for (const char* p = key; *p; ++p) {
        h = h * 0x1000193u ^ static_cast<std::uint32_t>(static_cast<int>(*p));
    }
    const std::uint64_t hash = h;
    const std::uint32_t valoff = off + static_cast<std::uint32_t>(keylen);
    Entry20* begin20 = vec_begin<Entry20>(table, 0x20);
    const std::int64_t n20 = vec_end<Entry20>(table, 0x20) - begin20;
    struct { std::uint64_t hash; std::uint32_t valoff; std::uint32_t pad; } probe{hash, 0xffffffffu, 0};
    std::uint64_t* found = nullptr;
    bool hit = false;
    if (n20 != 0) {
        std::uint64_t* last = reinterpret_cast<std::uint64_t*>(begin20 + static_cast<int>(n20));
        found = lower_bound_fn(reinterpret_cast<std::uint64_t*>(begin20), last, &probe);
        hit = found != last && *found == hash;
    }
    std::uint64_t result;
    add_calls.fetch_add(1, std::memory_order_relaxed);
    if (!hit) {
        push38(table, Entry38{hash, off, valoff, static_cast<std::uint32_t>(file), extra});
        probe.valoff = valoff;  // pad keeps the original's uninitialised-looking value
        Entry20* end20 = vec_end<Entry20>(table, 0x20);
        if (end20 == vec_cap<Entry20>(table, 0x20)) {
            grow20(table + 0x20, end20, &probe);
        } else {
            end20->hash = probe.hash;
            end20->valoff = probe.valoff;
            end20->pad = probe.pad;
            vec_end<Entry20>(table, 0x20) = end20 + 1;
        }
        result = 1;
    } else {
        found_calls.fetch_add(1, std::memory_order_relaxed);
        reinterpret_cast<std::uint32_t*>(found)[2] = valoff;
        Entry38* begin38 = vec_begin<Entry38>(table, 0x38);
        const int count = static_cast<int>(vec_end<Entry38>(table, 0x38) - begin38);
        int position = count;
        {
            std::lock_guard<std::mutex> lock(index_mutex);
            TableIndex& index = indexes[table];
            index_catch_up(table, index);
            auto it = index.first.find(index_key(h, static_cast<std::uint32_t>(file)));
            if (it != index.first.end()) {
                position = static_cast<int>(it->second);
                if (position >= count || begin38[position].hash != hash ||
                    static_cast<int>(begin38[position].file) != file) {
                    index.first.clear();
                    index.indexed = 0;
                    rebuilds.fetch_add(1, std::memory_order_relaxed);
                    position = linear_find(table, hash, file);
                }
            }
        }
        if (mode == Mode::shadow) {
            const int slow = linear_find(table, hash, file);
            if (slow != position) {
                if (add_mismatch.fetch_add(1) < 20) {
                    log("ADD MISMATCH table=%p key=%s fast=%d slow=%d count=%d", table, key,
                        position, slow, count);
                }
                position = slow;
            }
        }
        if (position < count) {
            begin38[position].keyoff = off;
            begin38[position].valoff = valoff;
        } else {
            push38(table, Entry38{hash, off, valoff, static_cast<std::uint32_t>(file), extra});
        }
        result = 0;
    }
    resize_fn(sb + 0x20, static_cast<std::int64_t>(static_cast<int>(keylen + vallen + off)));
    std::uint8_t* base = *reinterpret_cast<std::uint8_t**>(sb + 0x20);
    std::memcpy(base + static_cast<int>(off), key, static_cast<std::size_t>(keylen));
    std::memcpy(base + static_cast<std::int64_t>(static_cast<int>(off)) + keylen, value,
        static_cast<std::size_t>(vallen));
    add_ticks.fetch_add(qpc_now() - start, std::memory_order_relaxed);
    return result;
}

// ---- finalize
struct Run {
    std::uint64_t hash;
    int length;
};

std::vector<Run> find_runs(std::uint8_t* table) {
    std::vector<Run> runs;
    Entry20* e = vec_begin<Entry20>(table, 0x20);
    const int n = static_cast<int>(vec_end<Entry20>(table, 0x20) - e);
    int i = 0;
    while (i < n - 1) {
        const std::uint64_t h = e[i].hash;
        if (h == e[i + 1].hash) {
            int j = i;
            while (j < n && e[j].hash == h) ++j;
            runs.push_back(Run{h, j - i});
            i = j;
        } else {
            ++i;
        }
    }
    return runs;
}

// positions in table+0x38 of the first `length` entries of each run's hash, in order
std::vector<std::vector<std::uint32_t>> fast_positions(std::uint8_t* table,
    const std::vector<Run>& runs) {
    std::vector<std::vector<std::uint32_t>> result(runs.size());
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> found;
    Entry38* e = vec_begin<Entry38>(table, 0x38);
    const int n = static_cast<int>(vec_end<Entry38>(table, 0x38) - e);
    std::unordered_map<std::uint64_t, int> limit;
    for (const Run& run : runs) {
        int& value = limit[run.hash];
        if (run.length > value) value = run.length;
    }
    for (int i = 0; i < n; ++i) {
        auto it = limit.find(e[i].hash);
        if (it == limit.end()) continue;
        auto& list = found[e[i].hash];
        if (static_cast<int>(list.size()) < it->second) list.push_back(static_cast<std::uint32_t>(i));
    }
    for (std::size_t r = 0; r < runs.size(); ++r) {
        auto& list = found[runs[r].hash];
        const std::size_t take = std::min(list.size(), static_cast<std::size_t>(runs[r].length));
        result[r].assign(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return result;
}

std::vector<std::uint32_t> slow_positions(std::uint8_t* table, const Run& run) {
    std::vector<std::uint32_t> list;
    Entry38* e = vec_begin<Entry38>(table, 0x38);
    const int n = static_cast<int>(vec_end<Entry38>(table, 0x38) - e);
    for (int i = 0; i < n; ++i) {
        if (e[i].hash == run.hash) {
            list.push_back(static_cast<std::uint32_t>(i));
            if (static_cast<int>(list.size()) == run.length) break;
        }
    }
    return list;
}

const char* std_string_cstr(const std::uint8_t* s) {
    const std::uint64_t cap = *reinterpret_cast<const std::uint64_t*>(s + 0x18);
    return cap > 0xf ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

std::uint64_t checksum(std::uint8_t* table, std::size_t& n20, std::size_t& n38, std::size_t& nsb) {
    std::uint64_t h = 0x1234;
    Entry20* e20 = vec_begin<Entry20>(table, 0x20);
    n20 = static_cast<std::size_t>(vec_end<Entry20>(table, 0x20) - e20);
    for (std::size_t i = 0; i < n20; ++i) h = mix(mix(h, e20[i].hash), e20[i].valoff);
    Entry38* e38 = vec_begin<Entry38>(table, 0x38);
    n38 = static_cast<std::size_t>(vec_end<Entry38>(table, 0x38) - e38);
    for (std::size_t i = 0; i < n38; ++i) {
        h = mix(h, e38[i].hash);
        h = mix(h, (static_cast<std::uint64_t>(e38[i].keyoff) << 32) | e38[i].valoff);
        h = mix(h, (static_cast<std::uint64_t>(e38[i].file) << 32) | e38[i].extra);
    }
    std::uint8_t* sb = string_buffer();
    const std::uint8_t* b = *reinterpret_cast<std::uint8_t**>(sb + 0x20);
    nsb = static_cast<std::size_t>(*reinterpret_cast<std::uint8_t**>(sb + 0x28) - b);
    std::uint64_t s = 0x5678;
    std::size_t i = 0;
    for (; i + 8 <= nsb; i += 8) {
        std::uint64_t v;
        std::memcpy(&v, b + i, 8);
        s = (s ^ v) * 0x100000001b3ull;
    }
    for (; i < nsb; ++i) s = (s ^ b[i]) * 0x100000001b3ull;
    return mix(h, s);
}

std::uint8_t finalize_hook(std::uint8_t* table) {
    std::size_t n20 = 0, n38 = 0, nsb = 0;
    const std::uint64_t sum = checksum(table, n20, n38, nsb);
    const std::int64_t start = qpc_now();
    std::uint8_t result = 0;
    std::size_t run_count = 0;
    if (mode == Mode::off) {
        result = original_finalize(table);
    } else {
        const std::vector<Run> runs = find_runs(table);
        run_count = runs.size();
        const auto positions = fast_positions(table, runs);
        if (mode == Mode::shadow) {
            for (std::size_t r = 0; r < runs.size(); ++r) {
                if (slow_positions(table, runs[r]) != positions[r] &&
                    finalize_mismatch.fetch_add(1) < 20) {
                    log("FINALIZE MISMATCH table=%p run=%zu hash=%llx", table, r,
                        static_cast<unsigned long long>(runs[r].hash));
                }
            }
            result = original_finalize(table);
            if (result != (runs.empty() ? 0 : 1)) log("FINALIZE RESULT MISMATCH %d", result);
        } else {
            const char* name = std_string_cstr(table);
            const std::uint8_t* strings = *reinterpret_cast<std::uint8_t**>(string_buffer() + 0x20);
            const std::uint8_t* files = *reinterpret_cast<std::uint8_t**>(table + 0x50);
            Entry38* e38 = vec_begin<Entry38>(table, 0x38);
            std::vector<const char*> keys, values, file_names;
            for (std::size_t r = 0; r < runs.size(); ++r) {
                const auto& list = positions[r];
                keys.assign(list.size(), nullptr);
                values.assign(list.size(), nullptr);
                file_names.assign(list.size(), nullptr);
                for (std::size_t k = 0; k < list.size(); ++k) {
                    const Entry38& e = e38[list[k]];
                    keys[k] = reinterpret_cast<const char*>(strings + e.keyoff);
                    values[k] = reinterpret_cast<const char*>(strings + e.valoff);
                    file_names[k] = e.file == 0xffffffffu ? nullptr :
                        std_string_cstr(files + static_cast<std::uint64_t>(e.file) * 0x20);
                }
                // the original passes stack arrays with at least one slot
                keys.push_back(nullptr);
                values.push_back(nullptr);
                file_names.push_back(nullptr);
                report_fn(keys.data(), values.data(), file_names.data(),
                    static_cast<int>(list.size()), name);
            }
            result = runs.empty() ? 0 : 1;
        }
    }
    const std::int64_t spent = qpc_now() - start;
    finalize_ticks.fetch_add(spent);
    log("finalize table=%p name=%s n20=%zu n38=%zu strings=%zu runs=%zu result=%d checksum=%016llx "
        "finalize_ms=%.1f add_ms_total=%.1f add_calls=%lld found=%lld rebuilds=%lld add_mismatch=%lld "
        "finalize_mismatch=%lld",
        table, std_string_cstr(table), n20, n38, nsb, run_count, result,
        static_cast<unsigned long long>(sum), ms(spent), ms(add_ticks.load()), add_calls.load(),
        found_calls.load(), rebuilds.load(), add_mismatch.load(), finalize_mismatch.load());
    return result;
}

// 14-byte absolute jump written over a function entry
bool write_jump(std::uintptr_t at, const void* target) {
    std::uint8_t code[14] = {0xff, 0x25, 0, 0, 0, 0};
    const auto address = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(code + 6, &address, 8);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(at), code, sizeof(code));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), sizeof(code), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof(code));
    return true;
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_loc_patch.log";
    const auto ini = directory + L"\\eu4_loc_patch.ini";
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"loc", L"mode", L"off", value, 32, ini.c_str());
    mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    lower_bound_fn = reinterpret_cast<LowerBoundFn>(image + eu4build::rva(lower_bound_rva));
    grow38 = reinterpret_cast<GrowFn>(image + eu4build::rva(grow38_rva));
    grow20 = reinterpret_cast<GrowFn>(image + eu4build::rva(grow20_rva));
    resize_fn = reinterpret_cast<ResizeFn>(image + eu4build::rva(resize_rva));
    report_fn = reinterpret_cast<ReportFn>(image + eu4build::rva(report_rva));
    original_finalize = reinterpret_cast<FinalizeFn>(image + eu4build::rva(finalize_rva));
    if (!eu4build::code_matches(image, add_dup_rva, add_dup_bytes, sizeof(add_dup_bytes))) {
        log("REFUSED: add_dup bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    char error[160]{};
    speedpatch::CallPatch finalize;
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(finalize_call_rva)),
            reinterpret_cast<const void*>(original_finalize),
            reinterpret_cast<const void*>(&finalize_hook), finalize, error, sizeof(error)) ||
        !speedpatch::commit_call(finalize, error, sizeof(error))) {
        speedpatch::discard_call(finalize);
        log("REFUSED: %s", error);
        installation_status.store(-5);
        return 5;
    }
    if (mode != Mode::off &&
        !write_jump(image + eu4build::rva(add_dup_rva), reinterpret_cast<const void*>(&add_dup_hook))) {
        log("add_dup: VirtualProtect failed %lu (checksum only)", GetLastError());
        mode = Mode::off;
    }
    installation_status.store(mode == Mode::on ? 1 : mode == Mode::shadow ? 2 : 3);
    log("initialized version=0.1.0-experimental mode=%s",
        mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "off");
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

extern "C" __declspec(dllexport) LONG EU4LocPatchStatus() {
    return installation_status.load();
}
