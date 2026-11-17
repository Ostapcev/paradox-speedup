// Stellaris Alloc Patch 0.1.0 experimental, for Stellaris 4.5.4 (build 1b4f) Windows x64.
// Port of hoi4_alloc_patch 0.1.0 / eu4_alloc_patch 0.3.0 (C:\project europe).
//
// stellaris.exe links the CRT statically; its _malloc_base/_free_base/_calloc_base/
// _realloc_base/_msize_base call HeapAlloc/HeapFree/... on the process heap
// (CRT heap handle 143118180 = GetProcessHeap(), set in 142065368, used only by these five).
// This plugin replaces them with a size-class allocator with per-thread caches for blocks up
// to max_small (default 32 KB); bigger blocks still go to the process heap. Pointers that are
// not ours (allocated before the patch or by the heap fallback) are recognised by address and
// passed to the process heap. The game logic does not depend on allocation addresses.
//
// Installed from DllMain (the loader VERSION.dll loads plugins before the game's CRT starts)
// by a 12-byte "mov rax, hook; jmp rax" at each function entry.
//
//   14205cf58 malloc   14205ca4c free   14205ce78 calloc   14205f500 realloc   1420532a0 _msize
//   142048b1c _errno
// direct=1 also sends the game's new/delete straight to the allocator:
//   142028b18 operator new (new_handler loop around malloc thunk 14203cfe8; 23k callers)
//             failure: n == -1 -> 14015b690 (bad_array_new_length), else 1420253e4 (bad_alloc)
//   141b6fb70 operator delete thunk (jmp 142037874; 48k callers)
//   142037874 free wrapper (mov [rsp+10],0; mov eax,[rsp+10]; jmp free)
// 142028b54 (jmp 141b6fb70, 32k callers) and 142028b5c (jmp new) are 8-byte slots — left as
// is, they reach the hooked targets with one extra jump.
//
// Mode (stellaris_alloc_patch.ini, [alloc] mode=): off (default) | on
#include <windows.h>

#include "stellaris_plugin.hpp"

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint64_t free_va = 0x14205ca4c;
constexpr std::uint64_t malloc_va = 0x14205cf58;
constexpr std::uint64_t calloc_va = 0x14205ce78;
constexpr std::uint64_t realloc_va = 0x14205f500;
constexpr std::uint64_t msize_va = 0x1420532a0;
constexpr std::uint64_t errno_va = 0x142048b1c;  // int* _errno()
constexpr std::uint64_t crt_heap_va = 0x143118180;

constexpr std::uint8_t free_bytes[12] = {0x48, 0x85, 0xc9, 0x74, 0x37, 0x53, 0x48, 0x83, 0xec,
    0x20, 0x4c, 0x8b};
constexpr std::uint8_t malloc_bytes[12] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9,
    0x48, 0x83, 0xf9};
constexpr std::uint8_t calloc_bytes[12] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0xc2,
    0x48, 0x8b, 0xd9};
constexpr std::uint8_t realloc_bytes[12] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec,
    0x20, 0x48, 0x8b};
constexpr std::uint8_t msize_bytes[12] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xc9, 0x75, 0x19,
    0xe8, 0x6e, 0x58};

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


// ---- direct=1: the game's new/delete
using NoReturnFn = void (*)();
NoReturnFn throw_bad_alloc = nullptr;   // 1420253e4
NoReturnFn throw_bad_length = nullptr;  // 14015b690
std::atomic<std::uint64_t> new_failures{0};

void* hook_operator_new(std::size_t n) {
    if (void* p = hook_malloc(n)) return p;
    new_failures.fetch_add(1, std::memory_order_relaxed);
    if (n == static_cast<std::size_t>(-1)) throw_bad_length();
    throw_bad_alloc();
    __builtin_unreachable();
}

void hook_operator_delete(void* p) { hook_free(p); }

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        splug::log("stats committed_mb=%llu threads=%llu refills=%llu flushes=%llu large=%llu "
                   "foreign_frees=%llu failures=%llu new_failures=%llu",
                   static_cast<unsigned long long>(committed_bytes.load() >> 20),
                   static_cast<unsigned long long>(threads.load()),
                   static_cast<unsigned long long>(refills.load()),
                   static_cast<unsigned long long>(flushes.load()),
                   static_cast<unsigned long long>(large_allocs.load()),
                   static_cast<unsigned long long>(foreign_frees.load()),
                   static_cast<unsigned long long>(failures.load()),
                   static_cast<unsigned long long>(new_failures.load()));
    }
}

struct Site {
    std::uint64_t va;
    const std::uint8_t* bytes;
    const void* hook;
};

bool same(const Site& s) { return std::memcmp(splug::rva(s.va), s.bytes, 12) == 0; }

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_alloc_patch")) return;
    const splug::Mode mode = splug::ini_mode(L"alloc", L"mode", L"off");
    if (mode != splug::Mode::on) {
        splug::log("stellaris_alloc_patch 0.1.0: mode=%s; no bytes changed", splug::mode_name(mode));
        return;
    }
    const Site sites[] = {
        {free_va, free_bytes, reinterpret_cast<const void*>(&hook_free)},
        {malloc_va, malloc_bytes, reinterpret_cast<const void*>(&hook_malloc)},
        {calloc_va, calloc_bytes, reinterpret_cast<const void*>(&hook_calloc)},
        {realloc_va, realloc_bytes, reinterpret_cast<const void*>(&hook_realloc)},
        {msize_va, msize_bytes, reinterpret_cast<const void*>(&hook_msize)},
    };
    for (const Site& s : sites) {
        if (!same(s)) {
            splug::log("REFUSED: bytes differ at %llx; no bytes changed",
                       static_cast<unsigned long long>(s.va));
            return;
        }
    }
    max_small = static_cast<std::size_t>(
        splug::ini_int(L"alloc", L"max_small", static_cast<int>(class_limit)));
    if (max_small < 256) max_small = 256;
    if (max_small > class_limit) max_small = class_limit;
    const bool direct = splug::ini_int(L"alloc", L"direct", 1) != 0;
    for (int c = 0; c < class_count; ++c) batch_table[c] = batch_of(c);
    const std::uint64_t crt_heap = *splug::rva<std::uint64_t>(crt_heap_va);
    heap = GetProcessHeap();
    tls_index = TlsAlloc();
    if (tls_index == TLS_OUT_OF_INDEXES) {
        splug::log("REFUSED: no TLS index");
        return;
    }
    const std::size_t region = static_cast<std::size_t>(class_count) << region_shift;
    arena = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, region, MEM_RESERVE, PAGE_NOACCESS));
    if (!arena) {
        splug::log("REFUSED: cannot reserve %llu GB", static_cast<unsigned long long>(region >> 30));
        return;
    }
    arena_end = arena + region;
    for (int c = 0; c < class_count; ++c) {
        classes[c].cursor = arena + (static_cast<std::size_t>(c) << region_shift);
        classes[c].committed = classes[c].cursor;
        classes[c].limit = classes[c].cursor + (std::size_t{1} << region_shift);
    }
    crt_errno = reinterpret_cast<int* (*)()>(splug::rva(errno_va));
    for (const Site& s : sites) {
        if (!splug::write_jump(splug::rva(s.va), s.hook)) {
            splug::log("FAILED: VirtualProtect at %llx (partially installed!)",
                       static_cast<unsigned long long>(s.va));
            return;
        }
    }
    int direct_sites = 0;
    if (direct) {
        static constexpr std::uint8_t new_bytes[12] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48,
            0x8b, 0xd9, 0xeb, 0x0f, 0x48};
        static constexpr std::uint8_t delete_bytes[12] = {0xe9, 0xff, 0x7c, 0x4c, 0x00, 0xcc,
            0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        static constexpr std::uint8_t freewrap_bytes[12] = {0xc7, 0x44, 0x24, 0x10, 0x00, 0x00,
            0x00, 0x00, 0x8b, 0x44, 0x24, 0x10};
        const Site direct_list[] = {
            {0x142028b18, new_bytes, reinterpret_cast<const void*>(&hook_operator_new)},
            {0x141b6fb70, delete_bytes, reinterpret_cast<const void*>(&hook_operator_delete)},
            {0x142037874, freewrap_bytes, reinterpret_cast<const void*>(&hook_operator_delete)},
        };
        throw_bad_alloc = reinterpret_cast<NoReturnFn>(splug::rva(0x1420253e4));
        throw_bad_length = reinterpret_cast<NoReturnFn>(splug::rva(0x14015b690));
        bool all = true;
        for (const Site& s : direct_list) all = all && same(s);
        if (!all) {
            splug::log("direct new/delete REFUSED: bytes differ (malloc/free hooks stay)");
        } else {
            for (const Site& s : direct_list) {
                if (!splug::write_jump(splug::rva(s.va), s.hook)) break;
                ++direct_sites;
            }
        }
    }
    if (HANDLE t = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr)) CloseHandle(t);
    splug::log("stellaris_alloc_patch 0.1.0 installed: build %s, mode=on, crt_heap_at_install=%llx "
               "process_heap=%p arena=%p tls=%lu max_small=%llu direct_sites=%d",
               splug::state().build, static_cast<unsigned long long>(crt_heap), heap, arena,
               tls_index, static_cast<unsigned long long>(max_small), direct_sites);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
