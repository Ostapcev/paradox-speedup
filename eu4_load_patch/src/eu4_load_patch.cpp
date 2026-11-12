// EU4 Load Patch: faster start-up of large mods.
// Ideas and game layouts ported from Bellows (Happy Dwarf Team, MIT, see
// LICENSE.bellows.txt): metascript_blocks, metascript_subst, exists_index,
// token_dict_cache, history_add, transfer_skip, verifypath_listing (as a stat-slot swap instead of a code block),
// range_witness, ram_prefetch, open_warmup (src/bellows: copied with a small shim).
//  * blocks: 0xDB1410 resolves [[param] ...] blocks of an expanded scripted
//    effect/trigger from last to first, rebuilding the whole text per block.
//    For sorted, non-overlapping, NUL-free blocks this equals one forward pass.
//  * subst:  0xDB1800 substitutes $param$ the same way (two call sites).
// Modes per module: off | shadow (ours and original, compare every call, the
// original's result stays) | on (ours; every 256th cheap call compared, a
// mismatch disables the module and keeps the original's result).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <climits>
#include <initializer_list>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "call_patch.hpp"
#include "eu4_build.hpp"
#include "bellows/modules.hpp"

namespace {

enum class Mode { off, shadow, on };
std::atomic<long> installation_status{0};
std::wstring log_path;
std::mutex log_mutex;
std::uintptr_t image = 0;
std::int64_t qpc_frequency = 1;

void log(const char* format, ...) noexcept {
    char text[900];
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

const char* mode_name(Mode m) { return m == Mode::on ? "on" : m == Mode::shadow ? "shadow" : "off"; }

Mode read_mode(const std::wstring& ini, const wchar_t* key) {
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"load", key, L"off", value, 32, ini.c_str());
    return _wcsicmp(value, L"on") == 0 ? Mode::on : _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
}

// ---- game layouts
// MSVC std::string: buffer/pointer +0x00, size +0x10, capacity +0x18 (SSO below 16)
struct View {
    const char* data;
    std::size_t size;
};
View string_view(const void* s) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(s);
    std::size_t size = 0, capacity = 0;
    std::memcpy(&size, bytes + 0x10, 8);
    std::memcpy(&capacity, bytes + 0x18, 8);
    const char* data = reinterpret_cast<const char*>(bytes);
    if (capacity > 15) std::memcpy(&data, bytes, 8);
    return {data, size};
}
bool has_nul(View v) noexcept { return v.size != 0 && std::memchr(v.data, 0, v.size) != nullptr; }
bool equal(View a, View b) noexcept {
    return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}
template <class T> T rd(const void* base, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}

// game containers: data pointer +0x08, int count +0x14
constexpr std::size_t vec_data = 0x08;
constexpr std::size_t vec_count = 0x14;
constexpr std::size_t arg_stride = 0x40;   // key string +0x00, value string +0x20
constexpr std::size_t arg_value = 0x20;
constexpr std::size_t block_vec = 0x70;    // in the script object
constexpr std::size_t block_stride = 0x50; // name +0x00, body +0x20, start +0x40, end +0x44, negate +0x48
constexpr std::size_t token_stride = 0x28; // name +0x00, start +0x20, end +0x24

constexpr std::uintptr_t blocks_call_rva = 0xDB08C0;
constexpr std::uintptr_t blocks_rva = 0xDB1410;
constexpr std::uintptr_t subst_call_a_rva = 0xDB090E;
constexpr std::uintptr_t subst_call_b_rva = 0xDB0967;
constexpr std::uintptr_t subst_rva = 0xDB1800;
constexpr std::uintptr_t assign_rva = 0x95110;
constexpr std::uint8_t assign_bytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
    0x56, 0x57, 0x41, 0x57};

using BlocksFn = void (*)(void*, void*, void*);
using SubstFn = void (*)(void*, void*, void*, void*);
using AssignFn = void* (*)(void*, const char*, std::size_t);
BlocksFn original_blocks = nullptr;
SubstFn original_subst = nullptr;
AssignFn assign_fn = nullptr;

struct Stats {
    Mode mode = Mode::off;
    std::atomic<long long> calls{0}, ours{0}, skips{0}, compared{0}, mismatches{0}, units{0};
    std::atomic<long long> ticks{0}, original_ticks{0};
    std::atomic<long long> sequence{0};
    std::atomic<bool> disabled{false};
    std::atomic<bool> sample_pending{false};
};
Stats blocks_stats, subst_stats;

void report(const char* name, Stats& s) {
    log("%s mode=%s calls=%lld ours=%lld skips=%lld compared=%lld mismatches=%lld units=%lld "
        "hook_ms=%.1f original_ms=%.1f disabled=%d",
        name, mode_name(s.mode), s.calls.load(), s.ours.load(), s.skips.load(), s.compared.load(),
        s.mismatches.load(), s.units.load(), ms(s.ticks.load()), ms(s.original_ticks.load()),
        s.disabled.load() ? 1 : 0);
}

struct Timer {
    std::atomic<long long>& sink;
    std::int64_t start = qpc_now();
    explicit Timer(std::atomic<long long>& s) : sink(s) {}
    ~Timer() { sink.fetch_add(qpc_now() - start, std::memory_order_relaxed); }
};

// ---- blocks
bool build_blocks(const void* script, const void* text, const void* arguments, std::string& out) {
    const View source = string_view(text);
    if (source.size > static_cast<std::size_t>(INT_MAX) || has_nul(source)) return false;
    const auto* blocks = rd<const unsigned char*>(script, block_vec + vec_data);
    const std::int32_t block_count = rd<std::int32_t>(script, block_vec + vec_count);
    const auto* args = rd<const unsigned char*>(arguments, vec_data);
    const std::int32_t arg_count = rd<std::int32_t>(arguments, vec_count);
    if (block_count < 0 || arg_count < 0 || (block_count > 0 && !blocks) || (arg_count > 0 && !args)) {
        return false;
    }
    out.clear();
    out.reserve(source.size);
    std::int64_t cursor = 0;
    for (std::int32_t i = 0; i < block_count; ++i) {
        const unsigned char* block = blocks + static_cast<std::size_t>(i) * block_stride;
        const std::int32_t start = rd<std::int32_t>(block, 0x40);
        const std::int32_t end = rd<std::int32_t>(block, 0x44);
        if (start < cursor || end < start || static_cast<std::size_t>(end) > source.size) return false;
        const View name = string_view(block);
        unsigned char present = 0;
        for (std::int32_t a = 0; a < arg_count; ++a) {
            if (equal(string_view(args + static_cast<std::size_t>(a) * arg_stride), name)) {
                present = 1;
                break;
            }
        }
        out.append(source.data + cursor, static_cast<std::size_t>(start - cursor));
        if (rd<unsigned char>(block, 0x48) != present) {
            const View body = string_view(block + 0x20);
            if (has_nul(body)) return false;
            out.append(body.data, body.size);
        }
        cursor = end;
    }
    out.append(source.data + cursor, source.size - static_cast<std::size_t>(cursor));
    return true;
}

void blocks_hook(void* script, void* text, void* arguments) {
    Stats& s = blocks_stats;
    Timer total(s.ticks);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const std::int32_t count = script ? rd<std::int32_t>(script, block_vec + vec_count) : 0;
    std::string result;
    bool ours = false;
    if (count > 0 && !s.disabled.load(std::memory_order_relaxed) && text && arguments) {
        try {
            ours = build_blocks(script, text, arguments, result);
        } catch (...) {
            ours = false;
        }
    }
    if (!ours) {
        if (count > 0) s.skips.fetch_add(1, std::memory_order_relaxed);
        Timer t(s.original_ticks);
        original_blocks(script, text, arguments);
        return;
    }
    s.units.fetch_add(count, std::memory_order_relaxed);
    const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
    if ((sequence & 255) == 0) s.sample_pending.store(true, std::memory_order_relaxed);
    // the original costs ~blocks x length; sample only cheap calls in "on"
    const bool cheap = static_cast<std::uint64_t>(count) * string_view(text).size <= (1ull << 24);
    bool expected = true;
    if (s.mode == Mode::shadow ||
        (cheap && s.sample_pending.compare_exchange_strong(expected, false))) {
        s.compared.fetch_add(1, std::memory_order_relaxed);
        {
            Timer t(s.original_ticks);
            original_blocks(script, text, arguments);
        }
        if (!equal(string_view(text), View{result.data(), result.size()})) {
            if (s.mismatches.fetch_add(1) < 10) {
                log("BLOCKS MISMATCH blocks=%d len=%zu ours=%zu", count, string_view(text).size,
                    result.size());
            }
            if (s.mode == Mode::on) s.disabled.store(true);
        }
        return;  // the original's result stays
    }
    assign_fn(text, result.data(), result.size());
    s.ours.fetch_add(1, std::memory_order_relaxed);
}

// ---- subst
bool build_subst(const void* text, const void* arguments, const void* tokens, std::string& out,
    long long& used) {
    const View source = string_view(text);
    if (has_nul(source)) return false;
    const auto* token_data = rd<const unsigned char*>(tokens, vec_data);
    const std::int32_t token_count = rd<std::int32_t>(tokens, vec_count);
    const auto* args = rd<const unsigned char*>(arguments, vec_data);
    const std::int32_t arg_count = rd<std::int32_t>(arguments, vec_count);
    if (token_count < 0 || arg_count < 0 || (token_count > 0 && !token_data) || (arg_count > 0 && !args)) {
        return false;
    }
    out.clear();
    out.reserve(source.size + 64);
    std::int64_t cursor = 0;
    for (std::int32_t i = 0; i < token_count; ++i) {
        const unsigned char* token = token_data + static_cast<std::size_t>(i) * token_stride;
        const std::int32_t start = rd<std::int32_t>(token, 0x20);
        const std::int32_t end = rd<std::int32_t>(token, 0x24);
        if (start < cursor || end < start || static_cast<std::size_t>(end) > source.size) return false;
        const View name = string_view(token);
        View value{"", 0};
        for (std::int32_t a = 0; a < arg_count; ++a) {
            const unsigned char* arg = args + static_cast<std::size_t>(a) * arg_stride;
            if (equal(string_view(arg), name)) {
                value = string_view(arg + arg_value);
                break;
            }
        }
        if (has_nul(value)) return false;
        out.append(source.data + cursor, static_cast<std::size_t>(start - cursor));
        out.append(value.data, value.size);
        cursor = end;
    }
    out.append(source.data + cursor, source.size - static_cast<std::size_t>(cursor));
    used = token_count;
    return true;
}

void subst_hook(void* unused, void* text, void* arguments, void* tokens) {
    Stats& s = subst_stats;
    Timer total(s.ticks);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    std::string result;
    long long used = 0;
    bool ours = false;
    if (!s.disabled.load(std::memory_order_relaxed) && text && arguments && tokens) {
        try {
            ours = build_subst(text, arguments, tokens, result, used);
        } catch (...) {
            ours = false;
        }
    }
    if (!ours) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        Timer t(s.original_ticks);
        original_subst(unused, text, arguments, tokens);
        return;
    }
    s.units.fetch_add(used, std::memory_order_relaxed);
    const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
    if (s.mode == Mode::shadow || (sequence & 255) == 0) {
        s.compared.fetch_add(1, std::memory_order_relaxed);
        {
            Timer t(s.original_ticks);
            original_subst(unused, text, arguments, tokens);
        }
        if (!equal(string_view(text), View{result.data(), result.size()})) {
            if (s.mismatches.fetch_add(1) < 10) {
                log("SUBST MISMATCH tokens=%lld len=%zu ours=%zu", used, string_view(text).size,
                    result.size());
            }
            if (s.mode == Mode::on) s.disabled.store(true);
        }
        return;
    }
    assign_fn(text, result.data(), result.size());
    s.ours.fetch_add(1, std::memory_order_relaxed);
}

// ---- exists: "does this virtual file exist" (Bellows exists_index)
// The file-system vtable slot 0x1D9C998 (-> 0x15CCE80, a wrapper around PHYSFS_exists) is
// swapped. A query is answered "exists" only when every path part is found in once-read
// listings of a directory root of the PhysFS search path mounted at "/" (write directory
// excluded). Everything else goes to the original, so "not found" is never invented.
// Listings older than 30 s are read again.
constexpr std::uintptr_t exists_slot_rva = 0x1D9C998;
constexpr std::uintptr_t exists_rva = 0x15CCE80;
constexpr std::uint8_t exists_bytes[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0x7A, 0x18, 0x10, 0x72, 0x03,
    0x48, 0x8B, 0x12, 0x48, 0x85, 0xD2, 0x74, 0x18, 0x80, 0x3A, 0x00, 0x74, 0x13};
using ExistsFn = bool (*)(void*, const void*);
ExistsFn original_exists = nullptr;
Stats exists_stats;
std::atomic<long long> exists_listings{0}, exists_misses{0};

enum class Kind : unsigned char { file, directory, unknown };
struct Listing {
    bool usable = false;
    DWORD tick = 0;
    std::unordered_map<std::string, Kind> names;  // upper-cased ASCII name -> kind
};

struct ExistsState {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::wstring> roots;
    std::unordered_map<std::string, std::shared_ptr<const Listing>> listings;
    bool symlinks = false;
};
ExistsState* exists_state = nullptr;
INIT_ONCE exists_once = INIT_ONCE_STATIC_INIT;

bool bad_path_char(unsigned char c) {
    return c < 0x20 || c > 0x7e || c == 0x5c || c == 0x3a || c == 0x2a || c == 0x3f || c == 0x22 ||
        c == 0x3c || c == 0x3e || c == 0x7c || c == 0x7e;  // \ : * ? " < > | ~
}

bool split_virtual_path(const char* text, std::size_t length, std::vector<std::string>& parts) {
    if (length == 0 || text[0] == '/' || text[length - 1] == '/') return false;
    std::string current;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (bad_path_char(c)) return false;
        if (c == '/') {
            if (current.empty() || current == "." || current == "..") return false;
            parts.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c));
    }
    if (current.empty() || current == "." || current == ".." || current.back() == '.' || current.back() == ' ') {
        return false;
    }
    parts.push_back(std::move(current));
    return true;
}

std::shared_ptr<const Listing> read_listing(const std::wstring& directory, bool symlinks) {
    auto listing = std::make_shared<Listing>();
    listing->tick = GetTickCount();
    const std::wstring pattern = directory + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        listing->usable = error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND;
        return listing;
    }
    bool complete = false;
    for (;;) {
        const std::size_t length = wcsnlen(data.cFileName, MAX_PATH);
        const bool dots = (length == 1 && data.cFileName[0] == L'.') ||
            (length == 2 && data.cFileName[0] == L'.' && data.cFileName[1] == L'.');
        std::string key;
        bool ascii = true;
        for (std::size_t i = 0; i < length && ascii; ++i) {
            const wchar_t c = data.cFileName[i];
            ascii = c > 0 && c < 0x7f;
            key.push_back(static_cast<char>(c >= L'a' && c <= L'z' ? c - (L'a' - L'A') : c));
        }
        if (!dots && ascii) {
            Kind kind = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::directory : Kind::file;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                const bool junction = data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
                const bool link = data.dwReserved0 == IO_REPARSE_TAG_SYMLINK && symlinks;
                if (!junction && !link) kind = Kind::unknown;
            }
            listing->names.emplace(std::move(key), kind);
        }
        if (!FindNextFileW(find, &data)) {
            complete = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(find);
    listing->usable = complete;
    return listing;
}

std::shared_ptr<const Listing> get_listing(ExistsState& state, std::size_t root,
    const std::vector<std::string>& parts, std::size_t depth) {
    std::string key = std::to_string(root) + "|";
    std::wstring directory = state.roots[root];
    for (std::size_t i = 0; i < depth; ++i) {
        key += parts[i];
        key += '/';
        directory += L'\\';
        directory.append(parts[i].begin(), parts[i].end());
    }
    AcquireSRWLockShared(&state.lock);
    auto found = state.listings.find(key);
    std::shared_ptr<const Listing> existing = found == state.listings.end() ? nullptr : found->second;
    ReleaseSRWLockShared(&state.lock);
    if (existing && GetTickCount() - existing->tick < 30000) return existing;
    std::shared_ptr<const Listing> fresh = read_listing(directory, state.symlinks);
    exists_listings.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&state.lock);
    try {
        state.listings[key] = fresh;
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&state.lock);
    return fresh;
}

bool utf8_to_wide(const char* text, std::wstring& out) {
    const int length = static_cast<int>(std::strlen(text));
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, nullptr, 0);
    if (required <= 0) return false;
    out.assign(static_cast<std::size_t>(required), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, out.data(), required) == required;
}

BOOL CALLBACK init_exists(PINIT_ONCE, PVOID, PVOID*) noexcept {
    try {
        HMODULE exe = reinterpret_cast<HMODULE>(image);
        auto get_search_path = reinterpret_cast<char** (*)()>(GetProcAddress(exe, "PHYSFS_getSearchPath"));
        auto free_list = reinterpret_cast<void (*)(void*)>(GetProcAddress(exe, "PHYSFS_freeList"));
        auto mount_point = reinterpret_cast<const char* (*)(const char*)>(GetProcAddress(exe, "PHYSFS_getMountPoint"));
        auto symlinks = reinterpret_cast<int (*)()>(GetProcAddress(exe, "PHYSFS_symbolicLinksPermitted"));
        auto write_dir = reinterpret_cast<const char* (*)()>(GetProcAddress(exe, "PHYSFS_getWriteDir"));
        if (!get_search_path || !free_list || !mount_point || !symlinks || !write_dir) {
            log("exists: PhysFS exports missing");
            return TRUE;
        }
        auto* state = new ExistsState();
        state->symlinks = symlinks() != 0;
        std::wstring write;
        if (const char* w = write_dir()) utf8_to_wide(w, write);
        while (!write.empty() && (write.back() == L'\\' || write.back() == L'/')) write.pop_back();
        char** list = get_search_path();
        if (list) {
            for (char** entry = list; *entry; ++entry) {
                const char* mp = mount_point(*entry);
                if (!mp || std::strcmp(mp, "/") != 0) continue;
                std::wstring path;
                if (!utf8_to_wide(*entry, path)) continue;
                while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
                if (!write.empty() && path.size() >= write.size() &&
                    _wcsnicmp(path.c_str(), write.c_str(), write.size()) == 0 &&
                    (path.size() == write.size() || path[write.size()] == L'\\' || path[write.size()] == L'/')) {
                    continue;
                }
                const DWORD attributes = GetFileAttributesW(path.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    state->roots.push_back(path);
                }
            }
            free_list(list);
        }
        log("exists: %zu directory roots", state->roots.size());
        exists_state = state;
    } catch (...) {
        exists_state = nullptr;
    }
    return TRUE;
}

bool exists_hook(void* self, const void* name) {
    Stats& s = exists_stats;
    Timer total(s.ticks);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const DWORD prior_error = GetLastError();
    auto original = [&]() {
        SetLastError(prior_error);
        Timer t(s.original_ticks);
        return original_exists(self, name);
    };
    try {
        if (s.disabled.load(std::memory_order_relaxed) || !name) {
            s.skips.fetch_add(1, std::memory_order_relaxed);
            return original();
        }
        const View path = string_view(name);
        std::vector<std::string> parts;
        if (path.size > 4096 || !split_virtual_path(path.data, path.size, parts)) {
            s.skips.fetch_add(1, std::memory_order_relaxed);
            return original();
        }
        InitOnceExecuteOnce(&exists_once, init_exists, nullptr, nullptr);
        ExistsState* state = exists_state;
        if (!state || state->roots.empty()) {
            s.skips.fetch_add(1, std::memory_order_relaxed);
            return original();
        }
        bool found = false;
        for (std::size_t root = 0; root < state->roots.size() && !found; ++root) {
            bool ok = true;
            for (std::size_t depth = 0; depth < parts.size() && ok; ++depth) {
                const auto listing = get_listing(*state, root, parts, depth);
                if (!listing || !listing->usable) {
                    ok = false;
                    break;
                }
                const auto entry = listing->names.find(parts[depth]);
                if (entry == listing->names.end() || entry->second == Kind::unknown) {
                    ok = false;
                } else if (depth + 1 < parts.size() && entry->second != Kind::directory) {
                    ok = false;
                }
            }
            found = ok;
        }
        if (!found) {
            exists_misses.fetch_add(1, std::memory_order_relaxed);
            return original();
        }
        const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
        if (s.mode == Mode::shadow || (sequence & 1023) == 0) {
            s.compared.fetch_add(1, std::memory_order_relaxed);
            if (!original()) {
                if (s.mismatches.fetch_add(1) < 10) {
                    log("EXISTS MISMATCH %.*s", static_cast<int>(path.size), path.data);
                }
                if (s.mode == Mode::on) s.disabled.store(true);
                SetLastError(prior_error);
                return false;
            }
        }
        s.ours.fetch_add(1, std::memory_order_relaxed);
        SetLastError(prior_error);
        return true;
    } catch (...) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return original();
    }
}

bool install_exists() {
    auto** slot = reinterpret_cast<void**>(image + eu4build::rva(exists_slot_rva));
    if (*slot != reinterpret_cast<void*>(image + eu4build::rva(exists_rva)) ||
        !eu4build::code_matches(image, exists_rva, exists_bytes, sizeof(exists_bytes))) {
        log("exists REFUSED: slot or bytes differ");
        return false;
    }
    original_exists = reinterpret_cast<ExistsFn>(image + eu4build::rva(exists_rva));
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log("exists REFUSED: VirtualProtect %lu", GetLastError());
        return false;
    }
    *slot = reinterpret_cast<void*>(&exists_hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}


// ---- verifypath: answers PhysFS verifyPath's stat calls from directory listings
// (Bellows verifypath_listing). verifyPath stats every prefix of a path in every directory
// archive only to learn: exists (and not a symlink) or PHYSFS_ERR_NOT_FOUND. Instead of
// patching the call site, the DIR archiver's stat slot (table 0x1C1D7A0 + 0x68 = 0x1C1D808,
// -> 0x1A07080) is swapped with one aligned pointer write; the hook answers only when it was
// called from verifyPath (return address 0x19FB7F8) and passes every other stat through.
// Symlinks, other reparse points, unusual spellings, the write directory and unreadable
// listings go to the original. Listings older than 30 s are read again.
constexpr std::uintptr_t dir_stat_slot_rva = 0x1C1D808;
constexpr std::uintptr_t dir_stat_rva = 0x1A07080;
constexpr std::uintptr_t verifypath_return_rva = 0x19FB7F8;
constexpr std::uint8_t verifypath_call_bytes[] = {0x41, 0xFF, 0x51, 0x68, 0x8B, 0xE8};
constexpr int physfs_not_found = 0xB;
using StatFn = int (*)(void*, const char*, void*);
StatFn original_stat = nullptr;
void (*physfs_set_error)(int) = nullptr;
int (*physfs_get_error)() = nullptr;
const char* (*physfs_write_dir)() = nullptr;
Stats verify_stats;
std::atomic<long long> verify_not_found{0}, verify_listings{0};

struct PhysfsStat {
    std::int64_t filesize, modtime, createtime, accesstime;
    std::int32_t filetype, readonly;
};
static_assert(sizeof(PhysfsStat) == 0x28, "PHYSFS_Stat layout");

struct VpListing {
    bool usable = false;
    bool missing = false;
    DWORD tick = 0;
    std::unordered_map<std::string, Kind> names;
};
SRWLOCK vp_lock = SRWLOCK_INIT;
std::unordered_map<std::wstring, std::shared_ptr<const VpListing>> vp_listings;

std::shared_ptr<const VpListing> vp_read(const std::wstring& directory) {
    auto listing = std::make_shared<VpListing>();
    listing->tick = GetTickCount();
    const std::wstring pattern = directory + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_PATH_NOT_FOUND) {
            listing->usable = true;
            listing->missing = true;
        }
        return listing;
    }
    bool complete = false;
    for (;;) {
        const std::size_t length = wcsnlen(data.cFileName, MAX_PATH);
        const bool dots = (length == 1 && data.cFileName[0] == L'.') ||
            (length == 2 && data.cFileName[0] == L'.' && data.cFileName[1] == L'.');
        std::string key;
        bool ascii = true;
        for (std::size_t i = 0; i < length && ascii; ++i) {
            const wchar_t c = data.cFileName[i];
            ascii = c > 0 && c < 0x7f;
            key.push_back(static_cast<char>(c >= L'a' && c <= L'z' ? c - (L'a' - L'A') : c));
        }
        if (!dots && ascii) {
            Kind kind = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::directory : Kind::file;
            // the platform stat reports a symlink for IO_REPARSE_TAG_SYMLINK; a junction is a directory
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                data.dwReserved0 != IO_REPARSE_TAG_MOUNT_POINT) {
                kind = Kind::unknown;
            }
            listing->names.emplace(std::move(key), kind);
        }
        if (!FindNextFileW(find, &data)) {
            complete = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(find);
    listing->usable = complete;
    return listing;
}

std::shared_ptr<const VpListing> vp_get(const std::wstring& directory) {
    AcquireSRWLockShared(&vp_lock);
    std::shared_ptr<const VpListing> existing;
    auto found = vp_listings.find(directory);
    if (found != vp_listings.end()) existing = found->second;
    ReleaseSRWLockShared(&vp_lock);
    if (existing && GetTickCount() - existing->tick < 30000) return existing;
    std::shared_ptr<const VpListing> fresh = vp_read(directory);
    verify_listings.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&vp_lock);
    try {
        vp_listings[directory] = fresh;
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&vp_lock);
    return fresh;
}

bool is_write_root(const char* root) {
    const char* write = physfs_write_dir ? physfs_write_dir() : nullptr;
    if (!write || !*write) return false;
    std::size_t wl = std::strlen(write);
    while (wl > 0 && (write[wl - 1] == '\\' || write[wl - 1] == '/')) --wl;
    std::size_t rl = std::strlen(root);
    while (rl > 0 && (root[rl - 1] == '\\' || root[rl - 1] == '/')) --rl;
    if (rl < wl || _strnicmp(root, write, wl) != 0) return false;
    return rl == wl || root[wl] == '\\' || root[wl] == '/';
}

enum class Answer { original, exists, not_found };

// native path = root (with its trailing separator) + name, '/' -> '\'
Answer vp_decide(const char* root, const char* name, Kind& kind) {
    if (!root || !name || !*name || is_write_root(root)) return Answer::original;
    const int root_length = static_cast<int>(std::strlen(root));
    if (root_length < 4) return Answer::original;
    const int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, root_length, nullptr, 0);
    if (wide <= 0) return Answer::original;
    std::wstring directory(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, root_length, directory.data(), wide);
    if (directory.back() != L'\\') return Answer::original;
    directory.pop_back();
    std::string component;
    for (const char* cursor = name;; ++cursor) {
        const unsigned char c = static_cast<unsigned char>(*cursor);
        if (c != '/' && c != '\0') {
            if (bad_path_char(c)) return Answer::original;
            component.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c));
            continue;
        }
        if (component.empty() || component == "." || component == ".." || component.back() == '.' ||
            component.back() == ' ') {
            return Answer::original;
        }
        const auto listing = vp_get(directory);
        if (!listing || !listing->usable) return Answer::original;
        if (listing->missing) return Answer::not_found;
        const auto entry = listing->names.find(component);
        if (entry == listing->names.end()) return Answer::not_found;
        if (entry->second == Kind::unknown) return Answer::original;
        if (c == '\0') {
            kind = entry->second;
            return Answer::exists;
        }
        if (entry->second != Kind::directory) return Answer::original;
        directory.push_back(L'\\');
        directory.append(component.begin(), component.end());
        component.clear();
    }
}

__attribute__((noinline)) int stat_hook(void* opaque, const char* name, void* stat_out) {
    if (reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) != image + eu4build::rva(verifypath_return_rva)) {
        return original_stat(opaque, name, stat_out);
    }
    Stats& s = verify_stats;
    Timer total(s.ticks);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    try {
        Kind kind = Kind::unknown;
        const Answer answer = s.disabled.load(std::memory_order_relaxed)
            ? Answer::original : vp_decide(static_cast<const char*>(opaque), name, kind);
        if (answer == Answer::original) {
            s.skips.fetch_add(1, std::memory_order_relaxed);
            Timer t(s.original_ticks);
            return original_stat(opaque, name, stat_out);
        }
        const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
        if (s.mode == Mode::shadow || (sequence & 1023) == 0) {
            s.compared.fetch_add(1, std::memory_order_relaxed);
            PhysfsStat actual{};
            int rc;
            {
                Timer t(s.original_ticks);
                rc = original_stat(opaque, name, &actual);
            }
            const int error = rc ? 0 : physfs_get_error();  // clears the code; set again below
            const bool same = answer == Answer::exists ? (rc == 1 && actual.filetype != 2)
                                                       : (rc == 0 && error == physfs_not_found);
            if (!same) {
                if (s.mismatches.fetch_add(1) < 10) {
                    log("VERIFYPATH MISMATCH root=%s name=%s ours=%s rc=%d type=%d error=%d",
                        static_cast<const char*>(opaque), name,
                        answer == Answer::exists ? (kind == Kind::directory ? "dir" : "file") : "not_found", rc,
                        actual.filetype, error);
                }
                if (s.mode == Mode::on) s.disabled.store(true);
            }
            if (!same || s.mode == Mode::shadow) {
                std::memcpy(stat_out, &actual, sizeof(actual));
                if (!rc) physfs_set_error(error);
                return rc;
            }
        }
        if (answer == Answer::not_found) {
            verify_not_found.fetch_add(1, std::memory_order_relaxed);
            physfs_set_error(physfs_not_found);
            return 0;
        }
        PhysfsStat result{};
        result.filesize = kind == Kind::directory ? 0 : -1;  // not read by verifyPath
        result.modtime = result.createtime = result.accesstime = -1;
        result.filetype = kind == Kind::directory ? 1 : 0;
        std::memcpy(stat_out, &result, sizeof(result));
        s.ours.fetch_add(1, std::memory_order_relaxed);
        return 1;
    } catch (...) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return original_stat(opaque, name, stat_out);
    }
}

bool install_verifypath() {
    HMODULE exe = reinterpret_cast<HMODULE>(image);
    physfs_set_error = reinterpret_cast<void (*)(int)>(GetProcAddress(exe, "PHYSFS_setErrorCode"));
    physfs_get_error = reinterpret_cast<int (*)()>(GetProcAddress(exe, "PHYSFS_getLastErrorCode"));
    physfs_write_dir = reinterpret_cast<const char* (*)()>(GetProcAddress(exe, "PHYSFS_getWriteDir"));
    auto** slot = reinterpret_cast<void**>(image + eu4build::rva(dir_stat_slot_rva));
    if (!physfs_set_error || !physfs_get_error || !physfs_write_dir ||
        *slot != reinterpret_cast<void*>(image + eu4build::rva(dir_stat_rva)) ||
        !eu4build::code_matches(image, verifypath_return_rva - 4, verifypath_call_bytes, sizeof(verifypath_call_bytes))) {
        log("verifypath REFUSED: exports, slot or call bytes differ");
        return false;
    }
    original_stat = reinterpret_cast<StatFn>(image + eu4build::rva(dir_stat_rva));
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log("verifypath REFUSED: VirtualProtect %lu", GetLastError());
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(&stat_hook));
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// ---- token: cache of the script tokenizer's keyword lookups (Bellows token_dict_cache)
// The tokenizer's call at 0x170EA12 looks every word up in a global case-insensitive ternary
// tree (0x2326720, root at +0x08) via 0x6E4880. A per-thread open-addressing cache keyed by
// the ASCII-lowercased word answers instead. The tree only grows: a cached hit stays valid
// while the root is the same and the run-time token vector (0x242B8A0 begin, +8 end) did not
// shrink; a cached miss only for the token count read before its lookup.
constexpr std::uintptr_t token_call_rva = 0x170EA12;
constexpr std::uintptr_t token_lookup_rva = 0x6E4880;
constexpr std::uintptr_t token_dict_rva = 0x2326720;
constexpr std::uintptr_t token_dynamic_rva = 0x242B8A0;
using LookupFn = void* (*)(void*, const char*);
LookupFn original_lookup = nullptr;
Stats token_stats;
std::atomic<long long> token_hits{0}, token_flushes{0};

struct TokenCache {
    struct Slot {
        std::uint64_t hash;  // 0 = empty
        void* value;
        std::uint32_t count;
        std::uint32_t offset;
        std::uint32_t length;
        std::uint32_t pad;
    };
    std::vector<Slot> slots;
    std::vector<char> keys;
    std::size_t mask = 0, used = 0;
    const void* root = nullptr;
    std::uint32_t max_count = 0;
    std::uint64_t requests = 0;
    void reset(std::size_t capacity) {
        slots.assign(capacity, Slot{});
        mask = capacity - 1;
        used = 0;
        keys.clear();
    }
    void grow() {
        std::vector<Slot> old;
        old.swap(slots);
        slots.assign(old.size() * 2, Slot{});
        mask = slots.size() - 1;
        for (const Slot& slot : old) {
            if (slot.hash == 0) continue;
            std::size_t i = slot.hash & mask;
            while (slots[i].hash != 0) i = (i + 1) & mask;
            slots[i] = slot;
        }
    }
};
thread_local TokenCache* token_cache = nullptr;

std::uint32_t token_dynamic_count() {
    const auto begin = *reinterpret_cast<const volatile std::uintptr_t*>(image + eu4build::rva(token_dynamic_rva));
    const auto end = *reinterpret_cast<const volatile std::uintptr_t*>(image + eu4build::rva(token_dynamic_rva) + 8);
    return (begin != 0 && end >= begin) ? static_cast<std::uint32_t>((end - begin) / sizeof(void*)) : 0;
}

void* cached_lookup(TokenCache& c, void* dict, const char* key, bool& from_cache) {
    from_cache = false;
    char lower[255];
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    std::size_t n = 0;
    for (; key[n] != 0; ++n) {
        if (n >= sizeof(lower)) return original_lookup(dict, key);
        unsigned char ch = static_cast<unsigned char>(key[n]);
        if (static_cast<unsigned>(ch - 'A') < 26u) ch |= 0x20;
        lower[n] = static_cast<char>(ch);
        h = (h ^ ch) * 0xff51afd7ed558ccdull;
    }
    h ^= h >> 29;
    h |= 1;
    const void* root = rd<const void*>(dict, 0x08);
    const std::uint32_t count = token_dynamic_count();  // before the lookup that may fill a slot
    if (root != c.root || count < c.max_count) {
        if (c.used != 0) token_flushes.fetch_add(1, std::memory_order_relaxed);
        c.reset(c.slots.size());
        c.root = root;
        c.max_count = count;
    } else if (count > c.max_count) {
        c.max_count = count;
    }
    for (std::size_t i = h & c.mask;; i = (i + 1) & c.mask) {
        TokenCache::Slot& slot = c.slots[i];
        if (slot.hash == 0) {
            void* value = original_lookup(dict, key);
            if ((c.used + 1) * 2 > c.slots.size()) {
                c.grow();
                std::size_t j = h & c.mask;
                while (c.slots[j].hash != 0) j = (j + 1) & c.mask;
                i = j;
            }
            TokenCache::Slot& t = c.slots[i];
            t.offset = static_cast<std::uint32_t>(c.keys.size());
            c.keys.insert(c.keys.end(), lower, lower + n);
            t.hash = h;
            t.value = value;
            t.count = count;
            t.length = static_cast<std::uint32_t>(n);
            ++c.used;
            return value;
        }
        if (slot.hash != h || slot.length != n || std::memcmp(c.keys.data() + slot.offset, lower, n) != 0) continue;
        if (slot.value == nullptr && slot.count != count) {
            slot.value = original_lookup(dict, key);  // a miss from before a word was added
            slot.count = count;
            return slot.value;
        }
        from_cache = true;
        return slot.value;
    }
}

void* token_hook(void* dict, const char* key) {
    Stats& s = token_stats;
    if (s.disabled.load(std::memory_order_relaxed) || !key) return original_lookup(dict, key);
    if (!token_cache) {
        try {
            auto* cache = new TokenCache();
            cache->reset(1024);
            token_cache = cache;
        } catch (...) {
            return original_lookup(dict, key);
        }
    }
    TokenCache& c = *token_cache;
    ++c.requests;
    bool from_cache = false;
    void* answer;
    try {
        answer = cached_lookup(c, dict, key, from_cache);
    } catch (...) {
        token_cache = nullptr;  // leaks the broken cache; next call starts a new one
        return original_lookup(dict, key);
    }
    if (from_cache && (c.requests & 4095) == 0) token_hits.fetch_add(4096, std::memory_order_relaxed);
    if (from_cache && (s.mode == Mode::shadow || (c.requests & 255) == 1)) {
        s.compared.fetch_add(1, std::memory_order_relaxed);
        void* actual = original_lookup(dict, key);
        if (actual != answer) {
            if (s.mismatches.fetch_add(1) < 10) log("TOKEN MISMATCH %s", key);
            if (s.mode == Mode::on) s.disabled.store(true);
            return actual;
        }
    }
    return answer;
}

Stats history_stats;
Stats port_stats;

// ---- reporter: logs counters while the game loads (the game is often killed, not closed)
void (*range_report_fn)() = nullptr;
void (*provview_report_fn)() = nullptr;
void (*provfree_report_fn)() = nullptr;
std::atomic<long long>* range_calls_ptr = nullptr;

DWORD WINAPI reporter(void*) {
    long long last = -1;
    for (;;) {
        Sleep(5000);
        const long long now = blocks_stats.calls.load() + subst_stats.calls.load() + exists_stats.calls.load() +
            token_stats.compared.load() + token_hits.load() +
            verify_stats.calls.load() + history_stats.calls.load() +
            (range_calls_ptr ? range_calls_ptr->load() : 0) + port_stats.calls.load();
        if (now == last) continue;
        last = now;
        if (blocks_stats.mode != Mode::off) report("blocks", blocks_stats);
        if (subst_stats.mode != Mode::off) report("subst", subst_stats);
        if (token_stats.mode != Mode::off) {
            log("token mode=%s compared=%lld mismatches=%lld approx_hits=%lld flushes=%lld disabled=%d",
                mode_name(token_stats.mode), token_stats.compared.load(), token_stats.mismatches.load(),
                token_hits.load(), token_flushes.load(), token_stats.disabled.load() ? 1 : 0);
        }
        if (history_stats.mode != Mode::off) report("history", history_stats);
        if (range_report_fn) range_report_fn();
        if (provview_report_fn) provview_report_fn();
        if (provfree_report_fn) provfree_report_fn();
        if (port_stats.mode != Mode::off) report("port", port_stats);
        if (verify_stats.mode != Mode::off) {
            report("verifypath", verify_stats);
            log("verifypath not_found=%lld listings=%lld", verify_not_found.load(), verify_listings.load());
        }
        if (exists_stats.mode != Mode::off) {
            report("exists", exists_stats);
            log("exists listings=%lld misses=%lld", exists_listings.load(), exists_misses.load());
        }
    }
    return 0;
}


// ---- phase timing: ms since process creation at the ends of App_InitGame and the save read
constexpr std::uintptr_t init_call_rva = 0x202691;
constexpr std::uintptr_t init_rva = 0x2043A0;
constexpr std::uintptr_t save_call_rva = 0x74B93D;
constexpr std::uintptr_t save_rva = 0x5CDC60;
using Fn1 = std::uint64_t (*)(void*);
using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);

double since_start_ms() {
    FILETIME creation{}, exit_time{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = creation.dwLowDateTime;
    a.HighPart = creation.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return static_cast<double>(b.QuadPart - a.QuadPart) / 10000.0;
}

::sb::Mode prefetch_mode = ::sb::Mode::Off, warmup_mode = ::sb::Mode::Off;

DWORD WINAPI finish_bellows(void*) {
    if (prefetch_mode != ::sb::Mode::Off) {
        ::sb::FinishRamPrefetch();
        ::sb::ReportRamPrefetch();
    }
    if (warmup_mode != ::sb::Mode::Off) {
        ::sb::FinishOpenWarmup();
        ::sb::ReportOpenWarmup();
    }
    return 0;
}

std::uint64_t init_hook(void* game) {
    const double start = since_start_ms();
    const std::uint64_t result = reinterpret_cast<Fn1>(image + eu4build::rva(init_rva))(game);
    const double end = since_start_ms();
    log("PHASE init_game start_ms=%.0f end_ms=%.0f took_ms=%.0f", start, end, end - start);
    InterlockedExchange(&::sb::g_gameReady, 1);
    if (prefetch_mode != ::sb::Mode::Off || warmup_mode != ::sb::Mode::Off) {
        HANDLE thread = CreateThread(nullptr, 0, finish_bellows, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return result;
}

std::uint64_t save_hook(void* a, void* b, void* c, void* d) {
    const double start = since_start_ms();
    const std::uint64_t result = reinterpret_cast<Fn4>(image + eu4build::rva(save_rva))(a, b, c, d);
    const double end = since_start_ms();
    log("PHASE save_read start_ms=%.0f end_ms=%.0f took_ms=%.0f", start, end, end - start);
    return result;
}

bool hook_call(std::uintptr_t site_rva, std::uintptr_t target_rva, const void* hook, const char* name) {
    char error[160]{};
    speedpatch::CallPatch patch;
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(site_rva)),
            reinterpret_cast<const void*>(image + eu4build::rva(target_rva)), hook, patch, error, sizeof(error)) ||
        !speedpatch::commit_call(patch, error, sizeof(error))) {
        speedpatch::discard_call(patch);
        log("%s REFUSED: %s", name, error);
        return false;
    }
    return true;
}

// ---- code emission for small hand-written stubs near the image
struct Code {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    void bytes(std::initializer_list<std::uint8_t> list) {
        for (std::uint8_t b : list) base[size++] = b;
    }
    void u64(std::uint64_t v) {
        std::memcpy(base + size, &v, 8);
        size += 8;
    }
    void u32(std::uint32_t v) {
        std::memcpy(base + size, &v, 4);
        size += 4;
    }
    void mov_rax(const void* p) {  // mov rax, imm64
        bytes({0x48, 0xB8});
        u64(reinterpret_cast<std::uint64_t>(p));
    }
    void jmp_abs(std::uintptr_t target) {  // jmp [rip+0]; dq target
        bytes({0xFF, 0x25, 0, 0, 0, 0});
        u64(target);
    }
    std::uint8_t* here() const { return base + size; }
};

std::uint8_t* code_page = nullptr;
std::size_t code_used = 0;

std::uint8_t* allocate_code_near(std::uintptr_t target, std::size_t bytes) {
    if (!code_page) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto step = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
        const auto origin = target & ~(step - 1);
        for (std::uintptr_t delta = step; delta < 0x70000000 && !code_page; delta += step) {
            if (delta < origin) {
                code_page = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(origin - delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            }
            if (!code_page) {
                code_page = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(origin + delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            }
        }
        if (!code_page) return nullptr;
    }
    if (code_used + bytes > 4096) return nullptr;
    std::uint8_t* result = code_page + code_used;
    code_used += (bytes + 15) & ~std::size_t{15};
    return result;
}

bool write_code(std::uintptr_t at, const std::uint8_t* bytes, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return true;
}

// ---- history: skip the "already in the list?" walk for an entry just read (Bellows history_add)
// 0xDA66A0 reads one entry of a dated history list from a save and adds it to its owner's
// list (call [rax+98h] at 0xDA69E5) and to the list of all such entries (call [rax+0A0h] at
// 0xDA6A1D). The add 0x7E7CD0 first walks the whole list looking for the entry: quadratic.
// The entry was allocated while it was read, so it cannot be in a list yet. Both call sites
// note the entry (per thread); the next 0x7E7CD0 call with that entry skips the walk and goes
// on at 0x7E7D0E with rdi = entry, rsi = list. The global add skips only for the entry the
// owner add just took. Every 1024th skip walks anyway and disables the module on a hit.
constexpr std::uintptr_t history_owner_call_rva = 0xDA69E5;
constexpr std::uintptr_t history_global_call_rva = 0xDA6A1D;
constexpr std::uintptr_t history_add_rva = 0x7E7CD0;
constexpr std::uintptr_t history_after_walk_rva = 0x7E7D0E;
constexpr std::uint8_t history_owner_bytes[] = {0xFF, 0x90, 0x98, 0x00, 0x00, 0x00};
constexpr std::uint8_t history_global_bytes[] = {0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00};
constexpr std::uint8_t history_add_bytes[] = {0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0x41, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1};
constexpr std::uint8_t history_after_bytes[] = {0x48, 0x8B, 0x06, 0x48, 0x8B, 0xCE, 0x48, 0x89, 0x5C, 0x24, 0x38};

struct HistoryNoted {
    std::uintptr_t read = 0;
    std::uintptr_t fresh = 0;
};
thread_local HistoryNoted history_noted;

extern "C" void history_note_owner(std::uintptr_t entry) {
    history_noted.read = entry;
    history_noted.fresh = entry;
}
extern "C" void history_note_global(std::uintptr_t entry) {
    history_noted.fresh = entry == history_noted.read ? entry : 0;
    history_noted.read = 0;
}
extern "C" std::uint64_t history_decide(const std::uint8_t* list, std::uintptr_t entry) {
    const std::uintptr_t noted = history_noted.fresh;
    history_noted.fresh = 0;
    if (noted == 0 || noted != entry) return 0;
    Stats& s = history_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    if (s.disabled.load(std::memory_order_relaxed)) return 0;
    const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
    if (s.mode == Mode::shadow || (sequence & 1023) == 0) {
        s.compared.fetch_add(1, std::memory_order_relaxed);
        long long walked = 0;
        bool found = false;
        for (auto* node = rd<const std::uint8_t*>(list, 0x20); node; node = rd<const std::uint8_t*>(node, 0x10)) {
            ++walked;
            if (rd<std::uintptr_t>(node, 0) == entry) {
                found = true;
                break;
            }
        }
        s.units.fetch_add(walked, std::memory_order_relaxed);
        if (found) {
            if (s.mismatches.fetch_add(1) < 10) log("HISTORY MISMATCH entry already in the list");
            if (s.mode == Mode::on) s.disabled.store(true);
            return 0;
        }
        if (s.mode == Mode::shadow) return 0;
    }
    s.ours.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// call [rax+disp] (6 bytes) -> call stub; nop. The stub notes rdx and jumps to the virtual target.
bool history_site(std::uintptr_t site_rva, const std::uint8_t* expected, std::uint32_t disp, void (*note)(std::uintptr_t)) {
    if (!eu4build::code_matches(image, site_rva, expected, 6)) return false;
    Code c{allocate_code_near(image + eu4build::rva(site_rva), 64)};
    if (!c.base) return false;
    c.bytes({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51});  // push rax, rcx, rdx, r8, r9
    c.bytes({0x48, 0x83, 0xEC, 0x20});                    // sub rsp, 20h
    c.bytes({0x48, 0x89, 0xD1});                          // mov rcx, rdx
    c.mov_rax(reinterpret_cast<const void*>(note));
    c.bytes({0xFF, 0xD0});                                // call rax
    c.bytes({0x48, 0x83, 0xC4, 0x20});                    // add rsp, 20h
    c.bytes({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r9, r8, rdx, rcx, rax
    c.bytes({0xFF, 0xA0});                                // jmp [rax+disp32]
    c.u32(disp);
    const std::uintptr_t site = image + eu4build::rva(site_rva);
    const std::int64_t rel = reinterpret_cast<std::int64_t>(c.base) - static_cast<std::int64_t>(site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) return false;
    std::uint8_t patch[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(patch + 1, &rel32, 4);
    return write_code(site, patch, sizeof(patch));
}

bool install_history() {
    if (!eu4build::code_matches(image, history_add_rva, history_add_bytes, sizeof(history_add_bytes)) ||
        !eu4build::code_matches(image, history_after_walk_rva, history_after_bytes, sizeof(history_after_bytes)) ||
        !eu4build::code_matches(image, history_owner_call_rva, history_owner_bytes, 6) ||
        !eu4build::code_matches(image, history_global_call_rva, history_global_bytes, 6)) {
        log("history REFUSED: bytes differ");
        return false;
    }
    // entry stub of 0x7E7CD0
    Code c{allocate_code_near(image + eu4build::rva(history_add_rva), 128)};
    if (!c.base) return false;
    c.bytes({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});        // push rcx, rdx, r8, r9
    c.bytes({0x48, 0x83, 0xEC, 0x28});                    // sub rsp, 28h
    c.mov_rax(reinterpret_cast<const void*>(&history_decide));
    c.bytes({0xFF, 0xD0});                                // call rax (rcx = list, rdx = entry)
    c.bytes({0x48, 0x83, 0xC4, 0x28});                    // add rsp, 28h
    c.bytes({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});        // pop r9, r8, rdx, rcx
    c.bytes({0x48, 0x89, 0x74, 0x24, 0x20});              // mov [rsp+20h], rsi
    c.bytes({0x57});                                      // push rdi
    c.bytes({0x48, 0x83, 0xEC, 0x20});                    // sub rsp, 20h
    c.bytes({0x84, 0xC0});                                // test al, al
    const std::size_t jnz_at = c.size + 1;
    c.bytes({0x75, 0x00});                                // jnz skip
    c.bytes({0x48, 0x8B, 0x41, 0x20});                    // mov rax, [rcx+20h]
    c.jmp_abs(image + eu4build::rva(history_add_rva + 14));              // 14 bytes -> 0x7E7CDE (mov rdi,rdx ...)
    c.base[jnz_at] = static_cast<std::uint8_t>(c.size - (jnz_at + 1));  // skip:
    c.bytes({0x48, 0x89, 0xD7});                          // mov rdi, rdx
    c.bytes({0x48, 0x89, 0xCE});                          // mov rsi, rcx
    c.jmp_abs(image + eu4build::rva(history_after_walk_rva));
    std::uint8_t entry[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(c.base);
    std::memcpy(entry + 6, &target, 8);
    if (!history_site(history_owner_call_rva, history_owner_bytes, 0x98, &history_note_owner) ||
        !history_site(history_global_call_rva, history_global_bytes, 0xA0, &history_note_global)) {
        log("history REFUSED: call site stubs");
        return false;
    }
    if (!write_code(image + eu4build::rva(history_add_rva), entry, sizeof(entry))) {
        log("history REFUSED: entry write");
        return false;
    }
    return true;
}

// ---- transfer: no level-9 copy of a text save for a transfer nobody receives (Bellows transfer_skip)
// CFileTransferManager::AddFile (0xD7AAD0, called at 0xD84826 and 0x11337C1) deflates the
// whole save at level 9 when asked to compress, for SendFile only. In a dummy (single player)
// session that is not a server, call it with compress = false. Session: manager+0x20 handler,
// vtable slot 1; CSession +0x54 is server, +0x55 dummy.
constexpr std::uintptr_t transfer_add_rva = 0xD7AAD0;
constexpr std::uintptr_t transfer_call_a_rva = 0xD84826;
constexpr std::uintptr_t transfer_call_b_rva = 0x11337C1;
constexpr std::uint8_t transfer_add_bytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48,
    0x89, 0x7C, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t session_check_bytes[] = {0x80, 0x78, 0x54, 0x00, 0x75, 0x2B, 0x80, 0x78, 0x55, 0x00, 0x75, 0x25};
constexpr std::uint8_t get_session_bytes[] = {0x49, 0x8B, 0x4D, 0x20, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08, 0x48, 0x8B, 0xD0};
using AddFileFn = int (*)(void*, const char*, void**, bool);
Stats transfer_stats;

int transfer_hook(void* manager, const char* name, void** data, bool compress) {
    auto original = reinterpret_cast<AddFileFn>(image + eu4build::rva(transfer_add_rva));
    if (!compress) return original(manager, name, data, compress);
    Stats& s = transfer_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t start = qpc_now();
    bool skip = false;
    try {
        void* handler = rd<void*>(manager, 0x20);
        if (handler) {
            void** vtable = *static_cast<void***>(handler);
            const auto* session = static_cast<const std::uint8_t*>(reinterpret_cast<void* (*)(void*)>(vtable[1])(handler));
            if (session) skip = session[0x54] == 0 && session[0x55] != 0;
        }
    } catch (...) {
        skip = false;
    }
    const bool apply = skip && s.mode == Mode::on;
    const int result = original(manager, name, data, !apply);
    if (apply) s.ours.fetch_add(1, std::memory_order_relaxed);
    else s.skips.fetch_add(1, std::memory_order_relaxed);
    const auto* blob = data ? static_cast<const std::uint8_t* const*>(*data) : nullptr;
    log("transfer: %s save of %lld bytes, AddFile %.0f ms", apply ? "no copy for" : skip ? "(shadow) would skip copy of" : "kept copy of",
        blob ? static_cast<long long>(blob[1] - blob[0]) : -1LL, ms(qpc_now() - start));
    return result;
}

bool install_transfer() {
    if (!eu4build::code_matches(image, transfer_add_rva, transfer_add_bytes, sizeof(transfer_add_bytes)) ||
        !eu4build::code_matches(image, 0x1133713, session_check_bytes, sizeof(session_check_bytes)) ||
        !eu4build::code_matches(image, 0xD7AE4E, get_session_bytes, sizeof(get_session_bytes))) {
        log("transfer REFUSED: bytes differ");
        return false;
    }
    const bool a = hook_call(transfer_call_a_rva, transfer_add_rva, reinterpret_cast<const void*>(&transfer_hook), "transfer a");
    const bool b = hook_call(transfer_call_b_rva, transfer_add_rva, reinterpret_cast<const void*>(&transfer_hook), "transfer b");
    return a || b;
}

// ---- range: country range check answered from the last in-range province (Bellows range_witness)
// 0x3520D0 asks 0x3526B0 (call at 0x35214C) for the scaled distance from the country's
// provinces (then those of qualifying related countries) to a target province, stopping at
// the first one below the threshold, and only compares the result with the threshold. The
// engine (range_witness_core.hpp) remembers that province per (country, target, threshold);
// while it still qualifies with the game's own distance functions and formula, the answer is
// "in range" without the walk. shadow: always the original, compared. on: every 256th check
// is compared; a disagreement turns the module off.
#include "range_witness_core.hpp"

constexpr std::uintptr_t range_call_rva = 0x35214C;
constexpr std::uintptr_t range_walk_rva = 0x3526B0;
constexpr std::uintptr_t range_distance_rva = 0x2D2520;
constexpr std::uintptr_t range_line_rva = 0x88A390;
constexpr std::uintptr_t range_bonus_rva = 0x3F14A0;
constexpr std::uintptr_t range_related_bonus_rva = 0x7ACEC0;
constexpr std::uintptr_t range_default_a_rva = 0x798DC0;
constexpr std::uintptr_t range_default_b_rva = 0x7A73C0;
constexpr std::uintptr_t range_root_rva = 0x14321FC;   // mov rax,[root]; mov ebx,[rax+1DD0h]
constexpr std::uintptr_t range_scale_rva = 0x35272E;   // movss xmm6,[scale]; movss xmm7,[offset]
constexpr std::uintptr_t range_map_rva = 0x3527D5;     // mov r8,[map]
constexpr std::uintptr_t range_bonusv_rva = 0x352899;  // mov edi,[bonus]

struct RangeBytes {
    std::uintptr_t rva;
    std::initializer_list<std::uint8_t> bytes;
};

using RangeWalkFn = int* (*)(void*, int*, void*, void*, int*);
using RangeDistanceFn = void* (*)(void*, std::uint64_t, void*, void*, void*, std::uint64_t);
using RangeLineFn = int (*)(const int*, const int*, int, int, std::uint64_t);
using RangeBoolFn = bool (*)(void*);
using RangeGetterFn = unsigned char* (*)();
RangeWalkFn range_walk = nullptr;
RangeDistanceFn range_distance = nullptr;
RangeLineFn range_line = nullptr;
RangeBoolFn range_bonus = nullptr, range_related_bonus = nullptr;
RangeGetterFn range_default_a = nullptr, range_default_b = nullptr;
float range_scale = 0, range_offset = 0;
const unsigned char* const* range_root = nullptr;
const unsigned char* const* range_map = nullptr;
const int* range_bonus_value = nullptr;
std::size_t range_date_offset = 0;
Stats range_stats;

constexpr std::size_t kCountryInfo = 0x20, kCountryList = 0x1338, kCountryCount = 0x1344;
constexpr std::size_t kRootProvinces = 0x1CA8, kProvinceStride = 0x2E10, kProvinceId = 0x20;
constexpr std::size_t kProvinceFlags = 0x958, kProvinceListBegin = 0x490, kProvinceListEnd = 0x498;
constexpr std::size_t kMapRecords = 0x6C0, kMapRecordStride = 0x230, kMapCoord = 0x7C;
constexpr std::size_t kMapWidth = 0x28, kMapHeight = 0x2C, kMapIntegerFlag = 0x31;
constexpr std::size_t kRootCountries = 0x1CE8, kCountryRelations = 0x1510, kRelationStride = 0x48;
constexpr std::size_t kRelationList = 0x128, kRelationListCount = 0x134;
constexpr std::size_t kRelationFlagHolder = 0x40, kRelationFlag = 0x11F, kCountryModifiers = 0x1420;

struct RangeGame {
    int* Original(void* c, int* out, void* t, void* a4, int* thr) { return range_walk(c, out, t, a4, thr); }
    const std::int32_t* ListBegin(void* c) { return rd<const std::int32_t*>(c, kCountryList); }
    const std::int32_t* ListEnd(void* c) { return ListBegin(c) + rd<std::int32_t>(c, kCountryCount); }
    bool Eligible(void* country, std::int32_t province) {
        const auto* provinces = rd<const unsigned char*>(*range_root, kRootProvinces);
        const auto* p = provinces + static_cast<std::int64_t>(province) * kProvinceStride;
        if ((rd<std::uint8_t>(p, kProvinceFlags) & 0x10) == 0) return false;
        const auto id = static_cast<std::uint16_t>(rd<std::uint64_t>(country, kCountryInfo) >> 32);
        auto* it = rd<const std::uint64_t*>(p, kProvinceListBegin);
        auto* end = rd<const std::uint64_t*>(p, kProvinceListEnd);
        for (; it != end; ++it) {
            if (static_cast<std::uint16_t>(*it >> 32) == id) return true;
        }
        return false;
    }
    std::int32_t RelatedCount(void* c) {
        const auto begin = rd<std::uintptr_t>(c, kCountryRelations);
        const auto end = rd<std::uintptr_t>(c, kCountryRelations + 8);
        return end > begin ? static_cast<std::int32_t>((end - begin) / kRelationStride) : 0;
    }
    void* Related(void* country, std::int32_t index) {
        const auto* record = rd<const unsigned char*>(country, kCountryRelations) +
            static_cast<std::int64_t>(index) * kRelationStride;
        const auto* object = rd<const unsigned char*>(record, 0);
        if (!object) return nullptr;
        const auto other = static_cast<std::int32_t>(static_cast<std::int16_t>(rd<std::uint64_t>(record, 0x10) >> 32));
        const auto* it = rd<const unsigned char*>(object, kRelationList);
        const auto* end = it + static_cast<std::int64_t>(rd<std::int32_t>(object, kRelationListCount)) * 16;
        for (; it != end; it += 16) {
            if (rd<std::int32_t>(it, 0) != other) continue;
            const auto* entry = rd<const unsigned char*>(it, 8);
            if (!entry) return nullptr;
            if (rd<std::uint8_t>(rd<const unsigned char*>(entry, kRelationFlagHolder), kRelationFlag) == 0) return nullptr;
            return rd<void*>(rd<const unsigned char*>(*range_root, kRootCountries), static_cast<std::size_t>(index) * 8);
        }
        return nullptr;
    }
    static bool RelatedBonus(void* owner) {
        auto* modifiers = rd<unsigned char*>(owner, kCountryModifiers);
        unsigned char* fallback = modifiers ? rd<unsigned char*>(modifiers, 0x10) : range_default_a();
        unsigned char* holder = modifiers ? modifiers + 0x38 : fallback ? fallback + 0x78 : range_default_b() + 0x58;
        return range_related_bonus(holder);
    }
    bool Value(void* country, bool related, std::int32_t province, void* target, std::int64_t* value) {
        auto* province_object = const_cast<unsigned char*>(rd<const unsigned char*>(*range_root, kRootProvinces)) +
            static_cast<std::int64_t>(province) * kProvinceStride;
        alignas(16) unsigned char buffer[64] = {};
        range_distance(buffer, rd<std::uint64_t>(country, kCountryInfo), province_object, target, nullptr, 1);
        const int by_path = rd<int>(buffer, 0);
        const std::int32_t province_id = rd<std::int32_t>(province_object, kProvinceId);
        const std::int32_t target_id = rd<std::int32_t>(target, kProvinceId);
        int line = 0;
        if (province_id != target_id) {
            const auto* map = *range_map;
            const auto* records = rd<const unsigned char*>(map, kMapRecords);
            const int a = rd<int>(records + static_cast<std::int64_t>(static_cast<std::int16_t>(target_id)) * kMapRecordStride, kMapCoord);
            const int b = rd<int>(records + static_cast<std::int64_t>(static_cast<std::int16_t>(province_id)) * kMapRecordStride, kMapCoord);
            const std::uint8_t integer = rd<std::uint8_t>(map, kMapIntegerFlag);
            const int squared = range_line(&a, &b, rd<int>(map, kMapWidth), rd<int>(map, kMapHeight), integer);
            if (integer != 0) {
                line = squared * 1000;
            } else {
                if (squared < 0) return false;
                const float root2 = static_cast<float>(std::sqrt(static_cast<double>(squared)));
                float scaled = root2 * range_scale;
                scaled = scaled + range_offset;
                line = static_cast<int>(scaled);
            }
        }
        const int nearest = by_path < line ? by_path : line;
        const bool bonus = related ? RelatedBonus(country) : range_bonus(country);
        const int divisor = bonus ? *range_bonus_value + 1000 : 1000;
        *value = static_cast<std::int64_t>(nearest) * 1000 / divisor;
        return true;
    }
    std::int32_t TargetId(void* t) { return rd<std::int32_t>(t, kProvinceId); }
    std::int32_t CountryId(void* c) { return static_cast<std::uint16_t>(rd<std::uint64_t>(c, kCountryInfo) >> 32); }
    std::int32_t Day() {
        const auto* root = *range_root;
        return root ? static_cast<std::int32_t>((static_cast<std::int64_t>(rd<std::int32_t>(root, range_date_offset)) - 43'800'000) / 24) : 0;
    }
};

RangeGame range_game;
sb::WitnessEngine<RangeGame>* range_engine = nullptr;
SRWLOCK range_lock = SRWLOCK_INIT;

int* range_hook(void* country, int* out, void* target, void* arg4, int* threshold) {
    Stats& s = range_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    if (s.disabled.load(std::memory_order_relaxed)) return range_walk(country, out, target, arg4, threshold);
    const std::int64_t start = qpc_now();
    const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
    const bool verify = s.mode == Mode::shadow || (sequence & 255) == 0;
    int* result;
    AcquireSRWLockExclusive(&range_lock);
    try {
        const auto disagree = range_engine->stats.verify_disagree;
        result = range_engine->Check(range_game, country, out, target, arg4, threshold, verify);
        if (range_engine->stats.verify_disagree != disagree) {
            if (s.mismatches.fetch_add(1) < 10) log("RANGE MISMATCH country=%d", range_game.CountryId(country));
            if (s.mode == Mode::on) s.disabled.store(true);
        }
    } catch (...) {
        ReleaseSRWLockExclusive(&range_lock);
        return range_walk(country, out, target, arg4, threshold);
    }
    ReleaseSRWLockExclusive(&range_lock);
    s.ticks.fetch_add(qpc_now() - start, std::memory_order_relaxed);
    return result;
}

void range_report() {
    AcquireSRWLockShared(&range_lock);
    const sb::WitnessStats w = range_engine->stats;
    const std::size_t size = range_engine->size();
    ReleaseSRWLockShared(&range_lock);
    log("range mode=%s calls=%lld answered=%lld originals=%lld valid=%lld lost=%lld acquired=%lld "
        "out_of_range=%lld verify_agree=%lld verify_disagree=%lld value_differs=%lld witnesses=%zu hook_ms=%.1f "
        "disabled=%d", mode_name(range_stats.mode), static_cast<long long>(w.calls),
        static_cast<long long>(w.answered), static_cast<long long>(w.originals),
        static_cast<long long>(w.witness_valid), static_cast<long long>(w.witness_lost),
        static_cast<long long>(w.acquired), static_cast<long long>(w.out_of_range),
        static_cast<long long>(w.verify_agree), static_cast<long long>(w.verify_disagree),
        static_cast<long long>(w.verify_value_differs), size, ms(range_stats.ticks.load()),
        range_stats.disabled.load() ? 1 : 0);
}

template <class T> const T* rip_target(std::uintptr_t at, std::size_t disp_at, std::size_t next) {
    std::int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(at + disp_at), 4);
    return reinterpret_cast<const T*>(at + next + disp);
}

bool install_range() {
    const RangeBytes checks[] = {
        {range_call_rva, {0xE8, 0x5F, 0x05, 0x00, 0x00}},
        {range_walk_rva, {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08}},
        {range_distance_rva, {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10}},
        {range_line_rva, {0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x41, 0x8B, 0xD8, 0x4C, 0x8B, 0xDA}},
        {range_bonus_rva, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99, 0x20, 0x14, 0x00, 0x00}},
        {range_related_bonus_rva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xF9}},
        {range_default_a_rva, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40}},
        {range_default_b_rva, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20}},
        {range_root_rva, {0x48, 0x8B, 0x05, 0x75, 0xDC, 0xF0, 0x00, 0x8B, 0x98, 0xD0, 0x1D, 0x00, 0x00}},
        {range_scale_rva, {0xF3, 0x0F, 0x10, 0x35, 0x02, 0xF2, 0xA6, 0x01, 0xF3, 0x0F, 0x10, 0x3D, 0x56, 0x76, 0xA4, 0x01}},
        {range_map_rva, {0x4C, 0x8B, 0x05, 0xFC, 0xD6, 0xFE, 0x01}},
        {range_bonusv_rva, {0x8B, 0x3D, 0x1D, 0xB8, 0xFE, 0x01}},
    };
    for (const auto& c : checks) {
        if (!eu4build::code_matches(image, c.rva, c.bytes.begin(), c.bytes.size())) {
            log("range REFUSED: bytes differ at 0x%llx", static_cast<unsigned long long>(c.rva));
            return false;
        }
    }
    try {
        range_engine = new sb::WitnessEngine<RangeGame>();
    } catch (...) {
        return false;
    }
    range_walk = reinterpret_cast<RangeWalkFn>(image + eu4build::rva(range_walk_rva));
    range_distance = reinterpret_cast<RangeDistanceFn>(image + eu4build::rva(range_distance_rva));
    range_line = reinterpret_cast<RangeLineFn>(image + eu4build::rva(range_line_rva));
    range_bonus = reinterpret_cast<RangeBoolFn>(image + eu4build::rva(range_bonus_rva));
    range_related_bonus = reinterpret_cast<RangeBoolFn>(image + eu4build::rva(range_related_bonus_rva));
    range_default_a = reinterpret_cast<RangeGetterFn>(image + eu4build::rva(range_default_a_rva));
    range_default_b = reinterpret_cast<RangeGetterFn>(image + eu4build::rva(range_default_b_rva));
    range_root = rip_target<const unsigned char*>(image + eu4build::rva(range_root_rva), 3, 7);
    range_date_offset = rd<std::uint32_t>(reinterpret_cast<const void*>(image + eu4build::rva(range_root_rva)), 9);
    range_scale = *rip_target<float>(image + eu4build::rva(range_scale_rva), 4, 8);
    range_offset = *rip_target<float>(image + eu4build::rva(range_scale_rva + 8), 4, 8);
    range_map = rip_target<const unsigned char*>(image + eu4build::rva(range_map_rva), 3, 7);
    range_bonus_value = rip_target<int>(image + eu4build::rva(range_bonusv_rva), 2, 6);
    log("range: scale %.3f offset %.3f date_offset 0x%zx", range_scale, range_offset, range_date_offset);
    return hook_call(range_call_rva, range_walk_rva, reinterpret_cast<const void*>(&range_hook), "range");
}

// ---- entry hooks: jmp rel32 at the function start to a stub; the trampoline runs the copied
// prologue (whole instructions, no RIP-relative ones) and jumps back.
void* hook_entry(std::uintptr_t rva, std::initializer_list<std::uint8_t> prologue, std::size_t copy,
    const void* hook, const char* name) {
    const std::uintptr_t target = image + eu4build::rva(rva);
    if (copy < 5 || copy > prologue.size() || target == image ||
        !eu4build::code_matches(image, rva, prologue.begin(), prologue.size())) {
        log("%s REFUSED: entry bytes differ at 0x%llx", name, static_cast<unsigned long long>(rva));
        return nullptr;
    }
    Code c{allocate_code_near(target, 64)};
    if (!c.base) {
        log("%s REFUSED: no code memory near the image", name);
        return nullptr;
    }
    c.mov_rax(hook);
    c.bytes({0xFF, 0xE0});  // jmp rax
    c.size = 16;
    std::uint8_t* trampoline = c.here();
    std::memcpy(trampoline, reinterpret_cast<const void*>(target), copy);
    c.size += copy;
    c.jmp_abs(target + copy);
    FlushInstructionCache(GetCurrentProcess(), c.base, c.size);
    std::uint8_t patch[16];
    patch[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(c.base) -
        static_cast<std::intptr_t>(target + 5));
    std::memcpy(patch + 1, &rel, 4);
    for (std::size_t i = 5; i < copy; ++i) patch[i] = 0x90;
    if (!write_code(target, patch, copy)) {
        log("%s REFUSED: cannot write the entry", name);
        return nullptr;
    }
    return trampoline;
}

// ---- port: the nearest-port table from one list of ports (Bellows port_table)
// 0x350B80(country) rebuilds the country's table (+0x2320) when its flag +0x2887 is set: for
// every province it walks the country's provinces, its relation row and the provinces of each
// friend. The walks do not depend on the province, so bellows/port_table_core.hpp collects the
// ports once and calls the game's distance (0x2D2520) and range (0x350A20) the same way, in the
// same order, with the same strict comparison. shadow: the game's function writes the table and
// ours fills a copy that must match; on: every 256th rebuild is checked like that and a mismatch
// turns the module off.
#include "bellows/port_table_core.hpp"

constexpr std::uintptr_t port_table_rva = 0x350B80;
constexpr std::uintptr_t port_range_rva = 0x350A20;
using PortTableFn = void (*)(void*);
using PortRangeFn = std::int32_t* (*)(void*, std::int32_t*);
using PortDistanceFn = void* (*)(std::int32_t*, std::uint64_t, const void*, const void*, void*, std::uint32_t);
PortTableFn port_next = nullptr;
PortRangeFn port_range = nullptr;
PortDistanceFn port_distance = nullptr;
const unsigned char* const* port_root = nullptr;
thread_local std::vector<sb::port::Bytes>* port_list = nullptr;
std::atomic<long long> port_ports{0};

void port_rebuild(const unsigned char* root, unsigned char* country, std::int16_t* table) {
    if (!port_list) port_list = new std::vector<sb::port::Bytes>();
    port_list->clear();
    sb::port::CollectPorts(root, country, *port_list);
    port_ports.fetch_add(static_cast<long long>(port_list->size()), std::memory_order_relaxed);
    const auto id = sb::port::At<std::uint64_t>(country, sb::port::kCountryId);
    sb::port::Fill(
        root, country, *port_list, table,
        [id](sb::port::Bytes from, sb::port::Bytes to) {
            std::int32_t distance = 0;
            port_distance(&distance, id, from, to, nullptr, 1);
            return distance;
        },
        [country] {
            std::int32_t range = 0;
            port_range(country, &range);
            return range;
        });
}

void port_hook(void* country_ptr) {
    Stats& s = port_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned char* root = *port_root;
    auto* country = static_cast<unsigned char*>(country_ptr);
    if (s.disabled.load(std::memory_order_relaxed) || !root || !sb::port::Applies(root, country)) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        Timer t(s.original_ticks);
        port_next(country_ptr);
        return;
    }
    auto* table = rd<std::int16_t*>(country, sb::port::kCountryTable);
    const std::size_t cells = static_cast<std::size_t>(rd<std::int16_t*>(country, sb::port::kCountryTableEnd) - table);
    const bool check = s.mode == Mode::shadow || (s.sequence.fetch_add(1, std::memory_order_relaxed) & 255) == 0;
    if (!check) {
        try {
            Timer t(s.ticks);
            port_rebuild(root, country, table);
        } catch (...) {
            s.skips.fetch_add(1, std::memory_order_relaxed);
            port_next(country_ptr);
            return;
        }
        country[sb::port::kCountryDirty] = 0;
        s.ours.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<std::int16_t> copy;
    try {
        copy.assign(table, table + cells);
        Timer t(s.ticks);
        port_rebuild(root, country, copy.data());
    } catch (...) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        port_next(country_ptr);
        return;
    }
    {
        Timer t(s.original_ticks);
        port_next(country_ptr);
    }
    s.compared.fetch_add(1, std::memory_order_relaxed);
    s.units.fetch_add(static_cast<long long>(cells), std::memory_order_relaxed);
    if (std::memcmp(copy.data(), table, cells * sizeof(std::int16_t)) != 0) {
        std::size_t at = 0;
        while (at < cells && copy[at] == table[at]) ++at;
        if (s.mismatches.fetch_add(1) < 10) {
            log("PORT MISMATCH province %zu: ours %d game %d", at, copy[at], table[at]);
        }
        if (s.mode == Mode::on) s.disabled.store(true);
    }
}

bool install_port() {
    const RangeBytes checks[] = {
        {port_range_rva, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA}},
        {range_distance_rva, {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10}},
        {range_root_rva, {0x48, 0x8B, 0x05, 0x75, 0xDC, 0xF0, 0x00}},
    };
    for (const auto& c : checks) {
        if (!eu4build::code_matches(image, c.rva, c.bytes.begin(), c.bytes.size())) {
            log("port REFUSED: bytes differ at 0x%llx", static_cast<unsigned long long>(c.rva));
            return false;
        }
    }
    port_range = reinterpret_cast<PortRangeFn>(image + eu4build::rva(port_range_rva));
    port_distance = reinterpret_cast<PortDistanceFn>(image + eu4build::rva(range_distance_rva));
    port_root = rip_target<const unsigned char*>(image + eu4build::rva(range_root_rva), 3, 7);
    void* next = hook_entry(port_table_rva,
        {0x4C, 0x8B, 0xDC, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00},
        7, reinterpret_cast<const void*>(&port_hook), "port");
    if (!next) return false;
    port_next = reinterpret_cast<PortTableFn>(next);
    return true;
}

#include "save_compress.inc"
#include "provview.inc"
#include "provfree.inc"
#include "provreuse.inc"

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_load_patch.log";
    const auto ini = directory + L"\\eu4_load_patch.ini";
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    blocks_stats.mode = read_mode(ini, L"blocks");
    subst_stats.mode = read_mode(ini, L"subst");
    exists_stats.mode = read_mode(ini, L"exists");
    token_stats.mode = read_mode(ini, L"token");
    verify_stats.mode = read_mode(ini, L"verifypath");
    history_stats.mode = read_mode(ini, L"history");
    transfer_stats.mode = read_mode(ini, L"transfer");
    range_stats.mode = read_mode(ini, L"range");
    port_stats.mode = read_mode(ini, L"port");
    save_stats.mode = read_mode(ini, L"save");
    provview_stats.mode = read_mode(ini, L"provview");
    provfree_stats.mode = read_mode(ini, L"provfree");
    {
        wchar_t v[32]{};
        GetPrivateProfileStringW(L"load", L"provreuse", L"off", v, 32, ini.c_str());
        provreuse_verify0 = _wcsicmp(v, L"verify0") == 0;
        provreuse_verify = provreuse_verify0 || _wcsicmp(v, L"verify") == 0;
        provreuse_stats.mode = provreuse_verify || _wcsicmp(v, L"on") == 0 ? Mode::on : Mode::off;
    }
    save_parallel = read_mode(ini, L"save_parallel");
    {
        const Mode p = read_mode(ini, L"prefetch");
        prefetch_mode = p == Mode::on ? ::sb::Mode::On : p == Mode::shadow ? ::sb::Mode::Verify : ::sb::Mode::Off;
        warmup_mode = read_mode(ini, L"warmup") != Mode::off ? ::sb::Mode::On : ::sb::Mode::Off;
        const UINT mb = GetPrivateProfileIntW(L"load", L"prefetch_mb", 1024, ini.c_str());
        ::sb::SetRamPrefetchBudget(mb);
    }
    if (!eu4build::code_matches(image, assign_rva, assign_bytes, sizeof(assign_bytes))) {
        log("REFUSED: string assign bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    assign_fn = reinterpret_cast<AssignFn>(image + eu4build::rva(assign_rva));
    original_blocks = reinterpret_cast<BlocksFn>(image + eu4build::rva(blocks_rva));
    original_subst = reinterpret_cast<SubstFn>(image + eu4build::rva(subst_rva));
    if (blocks_stats.mode != Mode::off &&
        !hook_call(blocks_call_rva, blocks_rva, reinterpret_cast<const void*>(&blocks_hook), "blocks")) {
        blocks_stats.mode = Mode::off;
    }
    if (subst_stats.mode != Mode::off) {
        if (!hook_call(subst_call_a_rva, subst_rva, reinterpret_cast<const void*>(&subst_hook), "subst a")) {
            subst_stats.mode = Mode::off;
        } else if (!hook_call(subst_call_b_rva, subst_rva, reinterpret_cast<const void*>(&subst_hook),
                       "subst b")) {
            log("subst: site b refused, site a stays hooked");
        }
    }
    if (exists_stats.mode != Mode::off && !install_exists()) exists_stats.mode = Mode::off;
    if (verify_stats.mode != Mode::off && !install_verifypath()) verify_stats.mode = Mode::off;
    if (history_stats.mode != Mode::off && !install_history()) history_stats.mode = Mode::off;
    if (transfer_stats.mode != Mode::off && !install_transfer()) transfer_stats.mode = Mode::off;
    if (range_stats.mode != Mode::off) {
        if (install_range()) {
            range_calls_ptr = &range_stats.calls;
            range_report_fn = &range_report;
        } else {
            range_stats.mode = Mode::off;
        }
    }
    if (port_stats.mode != Mode::off && !install_port()) port_stats.mode = Mode::off;
    if (provview_stats.mode != Mode::off) {
        if (install_provview()) provview_report_fn = [] { report("provview", provview_stats); };
        else provview_stats.mode = Mode::off;
    }
    if (provreuse_stats.mode != Mode::off && provfree_stats.mode != Mode::off) {
        log("provreuse: provfree turned off (both change the same move)");
        provfree_stats.mode = Mode::off;
    }
    if (provreuse_stats.mode != Mode::off) {
        if (install_provfree(false) && install_provreuse()) {
            provfree_report_fn = &provreuse_report;
            log("provreuse installed (call 0x9F9571)");
        } else {
            provreuse_stats.mode = Mode::off;
        }
    }
    if (provfree_stats.mode != Mode::off) {
        if (install_provfree()) provfree_report_fn = &provfree_report;
        else provfree_stats.mode = Mode::off;
    }
    if (save_stats.mode == Mode::off) save_parallel = Mode::off;
    if (save_stats.mode != Mode::off && !install_save()) {
        save_stats.mode = Mode::off;
        save_parallel = Mode::off;
    }
    if (token_stats.mode != Mode::off) {
        original_lookup = reinterpret_cast<LookupFn>(image + eu4build::rva(token_lookup_rva));
        // the call passes the dictionary as lea rcx,[rip -> 0x2326720]
        const auto* lea = reinterpret_cast<const std::uint8_t*>(image + eu4build::rva(token_call_rva) - 7);
        std::int32_t disp = 0;
        std::memcpy(&disp, lea + 3, 4);
        if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x0D ||
            image + eu4build::rva(token_call_rva) + disp != image + eu4build::rva(token_dict_rva) ||
            !hook_call(token_call_rva, token_lookup_rva, reinterpret_cast<const void*>(&token_hook), "token")) {
            log("token REFUSED: dictionary operand differs");
            token_stats.mode = Mode::off;
        }
    }
    const bool init_hooked = hook_call(init_call_rva, init_rva, reinterpret_cast<const void*>(&init_hook), "init timing");
    if (!init_hooked) {
        prefetch_mode = ::sb::Mode::Off;  // nothing would stop them at the end of start-up
        warmup_mode = ::sb::Mode::Off;
    }
    if (prefetch_mode != ::sb::Mode::Off && !::sb::InstallRamPrefetch(image, directory, prefetch_mode)) {
        prefetch_mode = ::sb::Mode::Off;
    }
    if (warmup_mode != ::sb::Mode::Off && !::sb::InstallOpenWarmup(image, directory, warmup_mode)) {
        warmup_mode = ::sb::Mode::Off;
    }
    hook_call(save_call_rva, save_rva, reinterpret_cast<const void*>(&save_hook), "save timing");
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    installation_status.store(1);
    log("initialized version=0.8.0-experimental blocks=%s subst=%s exists=%s token=%s verifypath=%s history=%s "
        "transfer=%s range=%s port=%s save=%s save_parallel=%s provview=%s provfree=%s prefetch=%d warmup=%d", mode_name(blocks_stats.mode), mode_name(subst_stats.mode), mode_name(exists_stats.mode),
        mode_name(token_stats.mode), mode_name(verify_stats.mode), mode_name(history_stats.mode),
        mode_name(transfer_stats.mode), mode_name(range_stats.mode), mode_name(port_stats.mode),
        mode_name(save_stats.mode), mode_name(save_parallel), mode_name(provview_stats.mode), mode_name(provfree_stats.mode),
        static_cast<int>(prefetch_mode), static_cast<int>(warmup_mode));
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

void eu4load_log_line(const char* line) { log("%s", line); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4LoadPatchStatus() {
    return installation_status.load();
}
