// files.inc — module "files" (included by hoi4_load_patch.cpp).
//
// At start-up the game looks for every content file in each layer of its virtual file system:
// ~45 DLC folders (dlc\..., integrated_dlc\...), the user folder and the game folder. ~96% of
// the ~940k CreateFileW and ~230k GetFileAttributesExW calls fail (error 2 / 3), ~13 s of
// thread time in total, ~2.2 s of it on the main thread.
//
// files=on: an index of everything that exists under the game folder (read-only content,
// 59k files, built once on a background thread while the game creates its window) answers
// lookups of paths that do not exist there with the error Windows would give:
//   ERROR_FILE_NOT_FOUND (2) if the parent folder exists, ERROR_PATH_NOT_FOUND (3) if not.
// Only OPEN_EXISTING opens and attribute queries are answered; any path that exists, any path
// outside the game folder and any path with unusual syntax (., .., trailing dot/space, non-ASCII,
// wildcards, very long) goes to Windows. If the game creates, renames or makes a directory
// under the game folder (CreateFileW/A with a creating disposition, CreateDirectoryW/A,
// SHCreateDirectoryExW, MoveFileExW), the index is switched off for the rest of the session.
// files=shadow: Windows is always asked; the prediction is compared (result and error code).
// files=timing: only counts and times. All calls are made through hoi4.exe import slots.

constexpr std::uintptr_t iat_create_file_w = 0x2785448;
constexpr std::uintptr_t iat_get_attributes_ex_w = 0x27857a0;
constexpr std::uintptr_t iat_create_file_a = 0x2785580;
constexpr std::uintptr_t iat_create_directory_w = 0x27854b8;
constexpr std::uintptr_t iat_create_directory_a = 0x2785678;
constexpr std::uintptr_t iat_move_file_ex_w = 0x2785248;
constexpr std::uintptr_t iat_sh_create_directory_ex_w = 0x2785b30;

using CreateFileWFn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE (WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetAttributesExWFn = BOOL (WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using CreateDirectoryWFn = BOOL (WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using CreateDirectoryAFn = BOOL (WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using MoveFileExWFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using SHCreateDirectoryExWFn = int (WINAPI*)(HWND, LPCWSTR, const SECURITY_ATTRIBUTES*);
CreateFileWFn real_create_file = nullptr;
CreateFileAFn real_create_file_a = nullptr;
GetAttributesExWFn real_get_attributes = nullptr;
CreateDirectoryWFn real_create_directory = nullptr;
CreateDirectoryAFn real_create_directory_a = nullptr;
MoveFileExWFn real_move_file = nullptr;
SHCreateDirectoryExWFn real_sh_create_directory = nullptr;

enum class FilesMode { off, timing, shadow, on };
FilesMode files_mode = FilesMode::off;
DWORD main_thread_id = 0;

// ---- statistics
struct FileStat { std::atomic<std::uint64_t> count{0}, qpc{0}, failed{0}, answered{0}; };
FileStat file_stats[2][2];  // [open, attr][other, main]
std::atomic<std::uint64_t> shadow_checked{0}, shadow_bad{0}, predicted_exists{0}, passed_through{0};
std::atomic<int> shadow_logged{0};
std::atomic<std::uint64_t> fail_game{0}, fail_user{0}, fail_other{0};

// ---- index of the game folder: set of FNV-1a hashes of "rel\path" (lower-case ASCII)
constexpr std::uint8_t kind_file = 1, kind_dir = 2;
std::uint64_t* index_keys = nullptr;
std::uint8_t* index_kinds = nullptr;
std::size_t index_mask = 0, index_count = 0;
std::atomic<bool> index_ready{false};
std::atomic<bool> index_off{false};
std::atomic<int> index_off_logged{0};
wchar_t root[MAX_PATH]{};   // lower-case, '\' separators, no trailing separator
std::size_t root_length = 0;
wchar_t user_root[MAX_PATH]{};
std::size_t user_root_length = 0;

constexpr std::uint64_t fnv_basis = 1469598103934665603ull, fnv_prime = 1099511628211ull;
inline std::uint64_t fnv_add(std::uint64_t h, unsigned c) noexcept { return (h ^ c) * fnv_prime; }
inline unsigned lower_ascii(unsigned c) noexcept { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

bool index_insert(std::uint64_t h, std::uint8_t kind) noexcept {
    if (!h) h = 1;
    if (index_count * 2 + 2 > index_mask + 1) {
        const std::size_t old_mask = index_mask;
        std::uint64_t* old_keys = index_keys;
        std::uint8_t* old_kinds = index_kinds;
        const std::size_t size = old_keys ? (old_mask + 1) * 2 : 1u << 17;
        auto* k = static_cast<std::uint64_t*>(std::calloc(size, sizeof(std::uint64_t)));
        auto* d = static_cast<std::uint8_t*>(std::calloc(size, 1));
        if (!k || !d) { std::free(k); std::free(d); return false; }
        index_keys = k; index_kinds = d; index_mask = size - 1; index_count = 0;
        if (old_keys) {
            for (std::size_t i = 0; i <= old_mask; ++i) if (old_keys[i]) index_insert(old_keys[i], old_kinds[i]);
            std::free(old_keys); std::free(old_kinds);
        }
    }
    std::size_t i = static_cast<std::size_t>(h ^ (h >> 29)) & index_mask;
    while (index_keys[i] && index_keys[i] != h) i = (i + 1) & index_mask;
    if (!index_keys[i]) { index_keys[i] = h; ++index_count; }
    index_kinds[i] |= kind;
    return true;
}

std::uint8_t index_kind(std::uint64_t h) noexcept {
    if (!h) h = 1;
    std::size_t i = static_cast<std::size_t>(h ^ (h >> 29)) & index_mask;
    while (index_keys[i]) {
        if (index_keys[i] == h) return index_kinds[i];
        i = (i + 1) & index_mask;
    }
    return 0;
}

void switch_index_off(const char* why, LPCWSTR path) noexcept {
    if (index_off.exchange(true)) return;
    char narrow[300]{};
    if (path) WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    log("files: index switched OFF for this session (%s: %s)", why, narrow);
}

// recursive listing; returns false on anything unusual (reparse point, error)
bool build_dir(std::wstring& path, std::uint64_t rel_hash, bool rel_empty, int depth) noexcept {
    if (depth > 64) return false;
    const auto base = path.size();
    path += L"\\*";
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(path.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    path.resize(base);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        const wchar_t* name = data.cFileName;
        if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = false; break; }
        std::uint64_t h = rel_hash;
        if (!rel_empty) h = fnv_add(h, '\\');
        for (const wchar_t* c = name; *c; ++c) h = fnv_add(h, lower_ascii(*c));
        const bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // non-ASCII names are indexed too; non-ASCII queries are never answered from the index
        if (!index_insert(h, dir ? kind_dir : kind_file)) { ok = false; break; }
        if (dir) {
            path += L'\\';
            path += name;
            ok = build_dir(path, h, false, depth + 1);
            path.resize(base);
            if (!ok) break;
        }
    } while (FindNextFileW(find, &data));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = false;
    FindClose(find);
    return ok;
}

DWORD WINAPI build_index(void*) {
    const auto t0 = qpc_now();
    std::wstring path(root, root_length);
    const bool ok = build_dir(path, fnv_basis, true, 0);
    if (ok) {
        index_ready.store(true, std::memory_order_release);
        log("files: index ready, %zu entries in %.0f ms", index_count, ms(static_cast<std::uint64_t>(qpc_now() - t0)));
    } else {
        log("files: index NOT built (reparse point or listing error) after %.0f ms; nothing answered",
            ms(static_cast<std::uint64_t>(qpc_now() - t0)));
    }
    return 0;
}

inline bool separator(wchar_t c) noexcept { return c == L'\\' || c == L'/'; }

// does path start with prefix (lower-case, '\') followed by a separator or the end?
// returns the position after the prefix, or 0
std::size_t under(LPCWSTR path, const wchar_t* prefix, std::size_t length) noexcept {
    if (!path || !length) return 0;
    for (std::size_t i = 0; i < length; ++i) {
        const wchar_t c = path[i];
        if (!c) return 0;
        const wchar_t p = prefix[i];
        if (p == L'\\' ? !separator(c) : lower_ascii(c) != static_cast<unsigned>(p)) return 0;
    }
    return separator(path[length]) || !path[length] ? length : 0;
}

// 0 = exists or not decidable (ask Windows), else the error Windows gives for a missing path
DWORD predict_missing(LPCWSTR path) noexcept {
    if (!index_ready.load(std::memory_order_acquire) || index_off.load(std::memory_order_relaxed)) return 0;
    std::size_t i = under(path, root, root_length);
    if (!i) return 0;
    std::uint64_t h = fnv_basis, parent = fnv_basis;
    int components = 0;
    std::size_t total = 0;
    while (path[i]) {
        while (separator(path[i])) ++i;
        if (!path[i]) return 0;  // trailing separator
        const std::size_t start = i;
        std::uint64_t next = components ? fnv_add(h, '\\') : h;
        while (path[i] && !separator(path[i])) {
            const unsigned c = path[i];
            if (c < 0x20 || c > 0x7e || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
                c == '?' || c == '*' || c == '~') return 0;  // '~': possible 8.3 short name
            next = fnv_add(next, lower_ascii(c));
            ++i;
        }
        const std::size_t length = i - start;
        if (path[start] == L'.' && (length == 1 || (length == 2 && path[start + 1] == L'.'))) return 0;
        if (path[i - 1] == L'.' || path[i - 1] == L' ') return 0;
        total += length + 1;
        parent = h;
        h = next;
        ++components;
    }
    if (!components || root_length + total > 240) return 0;
    if (index_kind(h)) return 0;  // exists (or a hash collision): ask Windows
    if (components == 1) return ERROR_FILE_NOT_FOUND;
    const std::uint8_t parent_kind = index_kind(parent);
    if (parent_kind == kind_dir) return ERROR_FILE_NOT_FOUND;
    if (parent_kind) return 0;  // parent is a file (or ambiguous): ask Windows
    return ERROR_PATH_NOT_FOUND;
}

void count_failure_root(LPCWSTR path) noexcept {
    if (under(path, root, root_length)) fail_game.fetch_add(1, std::memory_order_relaxed);
    else if (under(path, user_root, user_root_length)) fail_user.fetch_add(1, std::memory_order_relaxed);
    else fail_other.fetch_add(1, std::memory_order_relaxed);
}

void shadow_compare(int kind, LPCWSTR path, DWORD predicted, bool failed, DWORD error) noexcept {
    shadow_checked.fetch_add(1, std::memory_order_relaxed);
    if (failed && error == predicted) return;
    shadow_bad.fetch_add(1, std::memory_order_relaxed);
    if (shadow_logged.fetch_add(1) < 30) {
        char narrow[400]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        log("files SHADOW MISMATCH %s predicted err=%lu real %s err=%lu: %s", kind ? "attr" : "open",
            predicted, failed ? "failed" : "succeeded", failed ? error : 0, narrow);
    }
}

void account(int kind, LPCWSTR path, std::int64_t t0, bool failed, bool answered) noexcept {
    auto& s = file_stats[kind][GetCurrentThreadId() == main_thread_id ? 1 : 0];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t0), std::memory_order_relaxed);
    if (failed) { s.failed.fetch_add(1, std::memory_order_relaxed); count_failure_root(path); }
    if (answered) s.answered.fetch_add(1, std::memory_order_relaxed);
}

HANDLE WINAPI create_file_hook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    const auto t0 = qpc_now();
    if (disposition != OPEN_EXISTING) {
        if (under(path, root, root_length)) switch_index_off("CreateFileW creating disposition", path);
        HANDLE h = real_create_file(path, access, share, security, disposition, flags, templ);
        const DWORD error = GetLastError();
        account(0, path, t0, h == INVALID_HANDLE_VALUE, false);
        SetLastError(error);
        return h;
    }
    const DWORD predicted = files_mode == FilesMode::timing ? 0 : predict_missing(path);
    if (predicted && files_mode == FilesMode::on) {
        account(0, path, t0, true, true);
        SetLastError(predicted);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = real_create_file(path, access, share, security, disposition, flags, templ);
    const DWORD error = GetLastError();
    if (predicted) shadow_compare(0, path, predicted, h == INVALID_HANDLE_VALUE, error);
    account(0, path, t0, h == INVALID_HANDLE_VALUE, false);
    SetLastError(error);
    return h;
}

BOOL WINAPI get_attributes_hook(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    const auto t0 = qpc_now();
    const DWORD predicted = files_mode == FilesMode::timing ? 0 : predict_missing(path);
    if (predicted && files_mode == FilesMode::on) {
        account(1, path, t0, true, true);
        SetLastError(predicted);
        return FALSE;
    }
    BOOL ok = real_get_attributes(path, level, info);
    const DWORD error = GetLastError();
    if (predicted) shadow_compare(1, path, predicted, !ok, error);
    account(1, path, t0, !ok, false);
    SetLastError(error);
    return ok;
}

// ---- guards: anything that can make a path appear under the game folder
bool under_ansi(LPCSTR path) noexcept {
    if (!path) return false;
    wchar_t wide[MAX_PATH * 2]{};
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 2 - 1)) return true;  // be safe
    return under(wide, root, root_length) != 0;
}

HANDLE WINAPI create_file_a_hook(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                 DWORD disposition, DWORD flags, HANDLE templ) {
    if (disposition != OPEN_EXISTING && under_ansi(path)) switch_index_off("CreateFileA creating disposition", nullptr);
    return real_create_file_a(path, access, share, security, disposition, flags, templ);
}
BOOL WINAPI create_directory_hook(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    if (under(path, root, root_length)) switch_index_off("CreateDirectoryW", path);
    return real_create_directory(path, security);
}
BOOL WINAPI create_directory_a_hook(LPCSTR path, LPSECURITY_ATTRIBUTES security) {
    if (under_ansi(path)) switch_index_off("CreateDirectoryA", nullptr);
    return real_create_directory_a(path, security);
}
BOOL WINAPI move_file_hook(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (under(to, root, root_length)) switch_index_off("MoveFileExW", to);
    return real_move_file(from, to, flags);
}
int WINAPI sh_create_directory_hook(HWND window, LPCWSTR path, const SECURITY_ATTRIBUTES* security) {
    if (under(path, root, root_length)) switch_index_off("SHCreateDirectoryExW", path);
    return real_sh_create_directory(window, path, security);
}

void files_report() noexcept {
    std::uint64_t total = 0;
    for (auto& k : file_stats) for (auto& s : k) total += s.count.load();
    static std::uint64_t last_total = 0;
    if (total == last_total) return;
    last_total = total;
    static const char* const kinds[2] = {"open", "attr"};
    char line[900];
    int n = std::snprintf(line, sizeof(line), "files (cumulative):");
    for (int k = 0; k < 2; ++k) {
        for (int m = 1; m >= 0; --m) {
            const auto& s = file_stats[k][m];
            n += std::snprintf(line + n, sizeof(line) - n, " %s/%s=%llu (%.0f ms, fail %llu, answered %llu)",
                kinds[k], m ? "main" : "other", static_cast<unsigned long long>(s.count.load()),
                ms(s.qpc.load()), static_cast<unsigned long long>(s.failed.load()),
                static_cast<unsigned long long>(s.answered.load()));
        }
    }
    n += std::snprintf(line + n, sizeof(line) - n,
        " | failures game=%llu user=%llu other=%llu | shadow checked=%llu MISMATCH=%llu%s",
        static_cast<unsigned long long>(fail_game.load()), static_cast<unsigned long long>(fail_user.load()),
        static_cast<unsigned long long>(fail_other.load()),
        static_cast<unsigned long long>(shadow_checked.load()), static_cast<unsigned long long>(shadow_bad.load()),
        index_off.load() ? " INDEX OFF" : "");
    log("%s", line);
}

bool patch_iat(std::uintptr_t rva, const void* hook_fn, void** original) noexcept {
    auto** slot = reinterpret_cast<void**>(image + rva);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = *slot;
    *slot = const_cast<void*>(hook_fn);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

void set_root(wchar_t* target, std::size_t& length, const wchar_t* path) noexcept {
    length = 0;
    for (const wchar_t* c = path; *c && length < MAX_PATH - 1; ++c) {
        target[length++] = *c == L'/' ? L'\\' : static_cast<wchar_t>(lower_ascii(*c));
    }
    while (length && target[length - 1] == L'\\') --length;
    target[length] = 0;
}

bool install_files(FilesMode requested) noexcept {
    files_mode = requested;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    struct Slot { std::uintptr_t rva; const void* expected; const void* hook_fn; void** original; };
    const Slot slots[] = {
        {iat_create_file_w, reinterpret_cast<void*>(GetProcAddress(k32, "CreateFileW")),
            reinterpret_cast<const void*>(&create_file_hook), reinterpret_cast<void**>(&real_create_file)},
        {iat_get_attributes_ex_w, reinterpret_cast<void*>(GetProcAddress(k32, "GetFileAttributesExW")),
            reinterpret_cast<const void*>(&get_attributes_hook), reinterpret_cast<void**>(&real_get_attributes)},
        {iat_create_file_a, reinterpret_cast<void*>(GetProcAddress(k32, "CreateFileA")),
            reinterpret_cast<const void*>(&create_file_a_hook), reinterpret_cast<void**>(&real_create_file_a)},
        {iat_create_directory_w, reinterpret_cast<void*>(GetProcAddress(k32, "CreateDirectoryW")),
            reinterpret_cast<const void*>(&create_directory_hook), reinterpret_cast<void**>(&real_create_directory)},
        {iat_create_directory_a, reinterpret_cast<void*>(GetProcAddress(k32, "CreateDirectoryA")),
            reinterpret_cast<const void*>(&create_directory_a_hook), reinterpret_cast<void**>(&real_create_directory_a)},
        {iat_move_file_ex_w, reinterpret_cast<void*>(GetProcAddress(k32, "MoveFileExW")),
            reinterpret_cast<const void*>(&move_file_hook), reinterpret_cast<void**>(&real_move_file)},
        {iat_sh_create_directory_ex_w, shell ? reinterpret_cast<void*>(GetProcAddress(shell, "SHCreateDirectoryExW")) : nullptr,
            reinterpret_cast<const void*>(&sh_create_directory_hook), reinterpret_cast<void**>(&real_sh_create_directory)},
    };
    for (const auto& s : slots) {
        if (!s.expected || *reinterpret_cast<void**>(image + s.rva) != s.expected) {
            log("files REFUSED (import slot +0x%llx does not hold the expected function)",
                static_cast<unsigned long long>(s.rva));
            return false;
        }
    }
    main_thread_id = GetCurrentThreadId();
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (auto* slash = wcsrchr(exe, L'\\')) *slash = 0;
    set_root(root, root_length, exe);
    wchar_t documents[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", documents, MAX_PATH)) {
        std::wstring user = std::wstring(documents) + L"\\Documents\\Paradox Interactive\\Hearts of Iron IV";
        set_root(user_root, user_root_length, user.c_str());
    }
    for (const auto& s : slots) {
        if (!patch_iat(s.rva, s.hook_fn, s.original)) {
            log("files: import slot +0x%llx not patched", static_cast<unsigned long long>(s.rva));
            return false;  // already patched slots forward to the real functions: harmless
        }
    }
    if (requested != FilesMode::timing) {
        HANDLE thread = CreateThread(nullptr, 0, build_index, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return true;
}
