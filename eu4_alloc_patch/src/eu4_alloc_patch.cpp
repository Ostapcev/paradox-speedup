// EU4 Alloc Patch 0.3.0 experimental, for EU4 1.37.5.0 Windows x64.
//
// eu4.exe links the CRT statically; its _malloc_base/_free_base/_calloc_base/
// _realloc_base/_msize_base call HeapAlloc/HeapFree/... on the process heap
// (~6% of the main thread in ntdll). This plugin replaces those five functions
// with a size-class allocator with per-thread caches for blocks up to 2048
// bytes; bigger blocks still go to the process heap. Pointers that are not
// ours (allocated before the patch or by the heap fallback) are recognised by
// address and passed to the process heap, so mixing is safe inside eu4.exe.
// The game logic does not depend on allocation addresses.
//
// Installed from DllMain (before the game's CRT starts) by a 12-byte
// "mov rax, hook; jmp rax" at each function entry.
//
// 0.3.0: size classes up to 32 KB (max_small=, default 32768; 2048 = 0.2.0), the
// per-thread cache keeps head/full/count of a class together, and direct=1 also
// sends the game's own new/delete thunks straight to the allocator:
//   1400950b0 std::_Allocate (size<0x1000 -> operator new, else 32-aligned +0x27 trick)
//   141a332d4 operator new (new_handler loop around malloc)
//   1417394f0 / 14175da20 operator delete thunks (jmp -> jmp -> free)
// which removes 2-4 jumps per allocation and per free.
//
// Mode (eu4_alloc_patch.ini, [alloc] mode=): off (default) | on
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "eu4_build.hpp"

namespace {

constexpr std::uintptr_t free_rva = 0x1a6833c;
constexpr std::uintptr_t malloc_rva = 0x1a6837c;
constexpr std::uintptr_t calloc_rva = 0x1a6989c;
constexpr std::uintptr_t realloc_rva = 0x1a6dd98;
constexpr std::uintptr_t msize_rva = 0x1a706f4;
constexpr std::uintptr_t errno_rva = 0x1a583d8;  // int* _errno()

constexpr std::uint8_t free_bytes[] = {0x48, 0x85, 0xc9, 0x74, 0x37, 0x53, 0x48, 0x83, 0xec,
    0x20, 0x4c, 0x8b};
constexpr std::uint8_t malloc_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9,
    0x48, 0x83, 0xf9};
constexpr std::uint8_t calloc_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0xc2,
    0x48, 0x8b, 0xd9};
constexpr std::uint8_t realloc_bytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec,
    0x20, 0x48, 0x8b};
constexpr std::uint8_t msize_bytes[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xc9, 0x75, 0x19,
    0xe8, 0xd6, 0x7c};

// ---- size classes: 16..256 step 16, then 8 classes per power of two
// (..512 step 32, ..1024 step 64, ..2048 step 128, ..4096 step 256, ... ..32768 step 4096)
constexpr int class_count = 72;
constexpr std::size_t class_limit = 32768;
std::size_t max_small = class_limit;  // ini max_small (2048 = 0.2.0 behaviour)
constexpr std::size_t region_shift = 30;  // 1 GiB of address space per class
constexpr std::size_t commit_step = 1u << 20;
constexpr int max_batch = 32;

inline int class_of(std::size_t n) noexcept {
    if (n <= 256) return static_cast<int>((n + 15) / 16) - 1;
    // n in (2^k, 2^(k+1)], k >= 8: 8 classes of 2^(k-3)
    const int k = 63 - __builtin_clzll(n - 1);
    return 16 + (k - 8) * 8 +
        static_cast<int>((n - 1 - (std::size_t{1} << k)) >> (k - 3));
}

inline std::size_t class_size(int c) noexcept {
    if (c < 16) return static_cast<std::size_t>(c + 1) * 16;
    const int k = 8 + (c - 16) / 8;
    return (std::size_t{1} << k) +
        static_cast<std::size_t>((c - 16) % 8 + 1) * (std::size_t{1} << (k - 3));
}

// blocks per chain: 32 up to 2 KB, then about 64 KB per chain (at least 4)
inline int batch_of(int c) noexcept {
    const std::size_t size = class_size(c);
    if (size <= 2048) return max_batch;
    const std::size_t b = 65536 / size;
    return b < 4 ? 4 : static_cast<int>(b);
}
int batch_table[class_count];

struct alignas(64) ClassState {
    SRWLOCK lock = SRWLOCK_INIT;
    std::uint8_t* cursor = nullptr;     // next never-used block
    std::uint8_t* committed = nullptr;  // end of committed memory
    std::uint8_t* limit = nullptr;      // end of the class region
    void* chains = nullptr;  // stack of 32-block chains; block[1] links chains
};

// Per class: `head` is a partial list (count < batch); `full` is one complete
// chain of exactly `batch` blocks kept back so alloc/free never walk a list.
struct ClassCache {
    void* head;
    void* full;
    std::int64_t count;
};
struct ThreadCache {
    ClassCache c[class_count];
};

std::uint8_t* arena = nullptr;
std::uint8_t* arena_end = nullptr;
ClassState classes[class_count];
HANDLE heap = nullptr;
DWORD tls_index = TLS_OUT_OF_INDEXES;
int* (*crt_errno)() = nullptr;

std::atomic<std::uint64_t> large_allocs{0}, refills{0}, flushes{0}, foreign_frees{0},
    committed_bytes{0}, threads{0}, failures{0};

inline bool ours(const void* p) noexcept {
    return p >= arena && p < arena_end;
}

inline int class_of_pointer(const void* p) noexcept {
    return static_cast<int>(
        (static_cast<const std::uint8_t*>(p) - arena) >> region_shift);
}

inline ThreadCache* cache() noexcept {
    void* value;
    if (tls_index < 64) {
        value = reinterpret_cast<void*>(__readgsqword(0x1480 + tls_index * 8));
    } else {
        value = TlsGetValue(tls_index);
    }
    if (value) return static_cast<ThreadCache*>(value);
    auto* created = static_cast<ThreadCache*>(HeapAlloc(heap, HEAP_ZERO_MEMORY,
        sizeof(ThreadCache)));
    if (!created) return nullptr;
    TlsSetValue(tls_index, created);
    threads.fetch_add(1, std::memory_order_relaxed);
    return created;
}

// Takes a chain of `batch` blocks for class c (global lock held inside).
void* take_chain(int c) noexcept {
    ClassState& s = classes[c];
    AcquireSRWLockExclusive(&s.lock);
    void* chain = s.chains;
    if (chain) {
        s.chains = static_cast<void**>(chain)[1];
        ReleaseSRWLockExclusive(&s.lock);
        return chain;
    }
    const std::size_t size = class_size(c);
    const int batch = batch_table[c];
    const std::size_t bytes = size * batch;
    while (s.cursor + bytes > s.committed) {
        if (s.committed + commit_step > s.limit ||
            !VirtualAlloc(s.committed, commit_step, MEM_COMMIT, PAGE_READWRITE)) {
            ReleaseSRWLockExclusive(&s.lock);
            return nullptr;
        }
        s.committed += commit_step;
        committed_bytes.fetch_add(commit_step, std::memory_order_relaxed);
    }
    std::uint8_t* first = s.cursor;
    s.cursor += bytes;
    ReleaseSRWLockExclusive(&s.lock);
    for (int i = 0; i < batch - 1; ++i) {
        *reinterpret_cast<void**>(first + i * size) = first + (i + 1) * size;
    }
    *reinterpret_cast<void**>(first + (batch - 1) * size) = nullptr;
    return first;
}

void give_chain(int c, void* chain) noexcept {
    ClassState& s = classes[c];
    AcquireSRWLockExclusive(&s.lock);
    static_cast<void**>(chain)[1] = s.chains;
    s.chains = chain;
    ReleaseSRWLockExclusive(&s.lock);
}

void set_enomem() noexcept {
    failures.fetch_add(1, std::memory_order_relaxed);
    if (crt_errno) *crt_errno() = 12;
}

void* small_alloc(std::size_t n) noexcept {
    const int c = class_of(n);
    ThreadCache* t = cache();
    if (!t) return nullptr;
    ClassCache& k = t->c[c];
    void* p = k.head;
    if (!p) {
        p = k.full;
        if (p) {
            k.full = nullptr;
        } else {
            p = take_chain(c);
            if (!p) return nullptr;
            refills.fetch_add(1, std::memory_order_relaxed);
        }
        k.count = batch_table[c];
    }
    k.head = *static_cast<void**>(p);
    --k.count;
    return p;
}

void small_free(void* p) noexcept {
    const int c = class_of_pointer(p);
    ThreadCache* t = cache();
    if (!t) return;  // only if the per-thread cache cannot be created; the block is kept
    ClassCache& k = t->c[c];
    *static_cast<void**>(p) = k.head;
    k.head = p;
    if (++k.count == batch_table[c]) {
        if (k.full) {
            give_chain(c, k.full);
            flushes.fetch_add(1, std::memory_order_relaxed);
        }
        k.full = k.head;
        k.head = nullptr;
        k.count = 0;
    }
}

void* hook_malloc(std::size_t n) {
    if (n == 0) n = 1;
    if (n <= max_small) {
        if (void* p = small_alloc(n)) return p;
    } else if (n > static_cast<std::size_t>(-0x20)) {
        set_enomem();
        return nullptr;
    }
    large_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = HeapAlloc(heap, 0, n);
    if (!p) set_enomem();
    return p;
}

void hook_free(void* p) {
    if (!p) return;
    if (ours(p)) {
        small_free(p);
        return;
    }
    foreign_frees.fetch_add(1, std::memory_order_relaxed);
    HeapFree(heap, 0, p);
}

void* hook_calloc(std::size_t count, std::size_t size) {
    if (count && static_cast<std::size_t>(-0x20) / count < size) {
        set_enomem();
        return nullptr;
    }
    std::size_t n = count * size;
    if (n == 0) n = 1;
    if (n <= max_small) {
        if (void* p = small_alloc(n)) {
            std::memset(p, 0, n);
            return p;
        }
    }
    large_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = HeapAlloc(heap, HEAP_ZERO_MEMORY, n);
    if (!p) set_enomem();
    return p;
}

std::size_t hook_msize(void* p) {
    if (!p) {
        if (crt_errno) *crt_errno() = 22;
        return static_cast<std::size_t>(-1);
    }
    if (ours(p)) return class_size(class_of_pointer(p));
    return HeapSize(heap, 0, p);
}

void* hook_realloc(void* p, std::size_t n) {
    if (!p) return hook_malloc(n);
    if (n == 0) {
        hook_free(p);
        return nullptr;
    }
    if (n > static_cast<std::size_t>(-0x20)) {
        set_enomem();
        return nullptr;
    }
    if (ours(p)) {
        const std::size_t old = class_size(class_of_pointer(p));
        if (n <= old && (old <= 16 || n > old / 2)) return p;
        void* q = hook_malloc(n);
        if (!q) return nullptr;
        std::memcpy(q, p, n < old ? n : old);
        small_free(p);
        return q;
    }
    void* q = HeapReAlloc(heap, 0, p, n);
    if (!q) set_enomem();
    return q;
}

// ---- direct=1: the game's new/delete thunks
using NoReturnFn = void (*)();
NoReturnFn throw_bad_alloc = nullptr;   // 141504030
NoReturnFn throw_bad_length = nullptr;  // 140095050
std::atomic<std::uint64_t> new_failures{0};

void* hook_operator_new(std::size_t n) {
    if (void* p = hook_malloc(n)) return p;
    new_failures.fetch_add(1, std::memory_order_relaxed);
    if (n == static_cast<std::size_t>(-1)) throw_bad_length();
    throw_bad_alloc();
    __builtin_unreachable();
}

// 1400950b0: std::_Allocate<16, _Default_allocate_traits>; blocks of 4 KB and more
// are 32-byte aligned with the raw pointer at [-8] (the inlined deallocations undo it).
void* hook_std_allocate(std::size_t n) {
    if (n < 0x1000) return n ? hook_operator_new(n) : nullptr;
    const std::size_t total = n + 0x27;
    if (total <= n) {
        throw_bad_length();
        __builtin_unreachable();
    }
    void* raw = hook_operator_new(total);
    auto* aligned = reinterpret_cast<void**>(
        (reinterpret_cast<std::uintptr_t>(raw) + 0x27) & ~std::uintptr_t{0x1f});
    aligned[-1] = raw;
    return aligned;
}

void hook_operator_delete(void* p) {
    hook_free(p);
}

wchar_t log_path[MAX_PATH];

void log(const char* format, ...) noexcept {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[600];
    const int length = std::snprintf(line, sizeof(line), "[%lu:%lu] %s\r\n",
        GetCurrentProcessId(), GetTickCount(), text);
    DWORD written = 0;
    if (length > 0) WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        log("stats committed_mb=%llu threads=%llu refills=%llu flushes=%llu large=%llu "
            "foreign_frees=%llu failures=%llu",
            static_cast<unsigned long long>(committed_bytes.load() >> 20),
            static_cast<unsigned long long>(threads.load()),
            static_cast<unsigned long long>(refills.load()),
            static_cast<unsigned long long>(flushes.load()),
            static_cast<unsigned long long>(large_allocs.load()),
            static_cast<unsigned long long>(foreign_frees.load()),
            static_cast<unsigned long long>(failures.load()));
    }
}

bool write_jump(std::uint8_t* at, const void* target) noexcept {
    std::uint8_t code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    const auto value = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(code + 2, &value, 8);
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, code, sizeof(code));
    DWORD ignored = 0;
    VirtualProtect(at, sizeof(code), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(code));
    return true;
}

LONG install(HMODULE module) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return -1;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return -1;
    *slash = L'\0';
    std::swprintf(log_path, MAX_PATH, L"%ls\\eu4_alloc_patch.log", path);
    wchar_t ini[MAX_PATH];
    std::swprintf(ini, MAX_PATH, L"%ls\\eu4_alloc_patch.ini", path);

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const wchar_t* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) return -3;

    wchar_t value[32]{};
    GetPrivateProfileStringW(L"alloc", L"mode", L"off", value, 32, ini);
    if (_wcsicmp(value, L"on") != 0) {
        log("disabled by configuration; no bytes changed");
        return 3;
    }
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    struct Site { std::uintptr_t rva; const std::uint8_t* bytes; const void* hook; };
    const Site sites[] = {
        {free_rva, free_bytes, reinterpret_cast<const void*>(&hook_free)},
        {malloc_rva, malloc_bytes, reinterpret_cast<const void*>(&hook_malloc)},
        {calloc_rva, calloc_bytes, reinterpret_cast<const void*>(&hook_calloc)},
        {realloc_rva, realloc_bytes, reinterpret_cast<const void*>(&hook_realloc)},
        {msize_rva, msize_bytes, reinterpret_cast<const void*>(&hook_msize)},
    };
    for (const Site& site : sites) {
        if (!eu4build::code_matches(image, site.rva, site.bytes, 12)) {
            log("REFUSED: bytes differ at rva %llx; no bytes changed",
                static_cast<unsigned long long>(site.rva));
            return -4;
        }
    }
    max_small = static_cast<std::size_t>(
        GetPrivateProfileIntW(L"alloc", L"max_small", static_cast<int>(class_limit), ini));
    if (max_small < 256) max_small = 256;
    if (max_small > class_limit) max_small = class_limit;
    const bool direct = GetPrivateProfileIntW(L"alloc", L"direct", 1, ini) != 0;
    for (int c = 0; c < class_count; ++c) batch_table[c] = batch_of(c);
    const std::uint64_t crt_heap = *reinterpret_cast<std::uint64_t*>(image + eu4build::rva(0x233d850));
    heap = GetProcessHeap();
    tls_index = TlsAlloc();
    if (tls_index == TLS_OUT_OF_INDEXES) {
        log("REFUSED: no TLS index");
        return -5;
    }
    const std::size_t region = static_cast<std::size_t>(class_count) << region_shift;
    arena = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, region, MEM_RESERVE,
        PAGE_NOACCESS));
    if (!arena) {
        log("REFUSED: cannot reserve %llu GB", static_cast<unsigned long long>(region >> 30));
        return -6;
    }
    arena_end = arena + region;
    for (int c = 0; c < class_count; ++c) {
        classes[c].cursor = arena + (static_cast<std::size_t>(c) << region_shift);
        classes[c].committed = classes[c].cursor;
        classes[c].limit = classes[c].cursor + (std::size_t{1} << region_shift);
    }
    crt_errno = reinterpret_cast<int* (*)()>(image + eu4build::rva(errno_rva));
    for (const Site& site : sites) {
        if (!write_jump(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(site.rva)), site.hook)) {
            log("FAILED: VirtualProtect at rva %llx (partially installed!)",
                static_cast<unsigned long long>(site.rva));
            return -7;
        }
    }
    int direct_sites = 0;
    if (direct) {
        static constexpr std::uint8_t allocate_bytes[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x81,
            0xf9, 0x00, 0x10, 0x00, 0x00, 0x72};
        static constexpr std::uint8_t new_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48,
            0x8b, 0xd9, 0xeb, 0x0f, 0x48};
        // jmp rel32 followed by int3 padding (12 bytes needed for the absolute jump)
        static constexpr std::uint8_t delete1_bytes[] = {0xe9, 0x2b, 0x45, 0x02, 0x00, 0xcc,
            0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        static constexpr std::uint8_t delete2_bytes[] = {0xe9, 0x13, 0x88, 0x2e, 0x00, 0xcc,
            0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        const Site direct_list[] = {
            {0x950b0, allocate_bytes, reinterpret_cast<const void*>(&hook_std_allocate)},
            {0x1a332d4, new_bytes, reinterpret_cast<const void*>(&hook_operator_new)},
            {0x17394f0, delete1_bytes, reinterpret_cast<const void*>(&hook_operator_delete)},
            {0x175da20, delete2_bytes, reinterpret_cast<const void*>(&hook_operator_delete)},
        };
        throw_bad_alloc = reinterpret_cast<NoReturnFn>(image + eu4build::rva(0x1504030));
        throw_bad_length = reinterpret_cast<NoReturnFn>(image + eu4build::rva(0x95050));
        bool same = true;
        for (const Site& site : direct_list) {
            same = same &&
                eu4build::code_matches(image, site.rva, site.bytes, 12);
        }
        if (!same) {
            log("direct new/delete REFUSED: bytes differ (malloc/free hooks stay)");
        } else {
            for (const Site& site : direct_list) {
                if (!write_jump(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(site.rva)), site.hook)) {
                    break;
                }
                ++direct_sites;
            }
        }
    }
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    log("initialized version=0.3.0-experimental crt_heap_at_install=%llx process_heap=%p "
        "arena=%p tls=%lu max_small=%llu direct_sites=%d",
        static_cast<unsigned long long>(crt_heap), heap, arena, tls_index,
        static_cast<unsigned long long>(max_small), direct_sites);
    return 1;
}

std::atomic<LONG> installation_status{0};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        installation_status.store(install(module));
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4AllocPatchStatus() {
    return installation_status.load();
}
