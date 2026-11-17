// Stellaris Vis Patch 0.2.0, Stellaris 4.5.4 x64 — поиск системы в списке страны без перебора.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   14074fa80(country) — элемент параллельного цикла 1402c5a40 (200 стран, ~77 раз в минуту на
//   BENCH2446, ~690 мс процессора на вызов по всем потокам). Для страны обходит все системы
//   галактики ([g+0x590], счётчик [g+0x59c], индекс в [rsp+0x78]) и для каждой линейно ищет id
//   системы (r9d = [rdi+8]) в списке страны [country+0xe0] (int32, длина [country+0xec]):
//     14074fe0b..14074fe3e: найдено -> or si, 8.
//   Это ~1,4 с из ~2,5 с горячих адресов задания (leaf, v7_2446).
//
// Плагин: на 14074fe0b — jmp в заглушку; она зовёт vis_contains(country, id, index) и ставит бит 8
// в si так же, как игра, затем идёт на 14074fe3e (дальше rax/rcx/rdx/r8/r9 перезаписываются до
// чтения). vis_contains держит в потоке хеш-множество списка страны; перестраивает его, когда
// страна, указатель или длина списка другие либо индекс системы не вырос (новый обход страны).
// 0.2.0 — поиск А: внутри 14071da50 (вызов 14074feab из того же цикла) для пары страна × система
//   16-битное значение ищется перебором в списке страны [country+0x1a48] (140312490, вызов 14071de8e;
//   ~1,1 с горячих адресов). Плагин оборачивает вызов 14074feab (флаг «внутри обхода» в потоке) и
//   на 14071de8e отвечает по битовой карте 64К бит, построенной один раз на обход страны.
//   Вне обхода (другие вызыватели 14071da50) — всегда перебор игры. vsp_mode16 — как vsp_mode.
// [vis] mode=off / shadow (ответ игры + сверка, mismatch в логе) / on. vsp_mode (rva в логе) на лету:
// 0 — перебор игры, 1 — множество, 2 — тень.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
std::int64_t vsp_mode = 0;
std::int64_t vsp_mode16 = 0;
bool vis_has16(const std::uint8_t* arr, std::uint32_t key);
std::uint64_t vis_1da50(void* a, std::uint64_t b, void* c, void* d);
std::uint64_t vsp_resume = 0;  // 14074fe3e
bool vis_contains(const std::uint8_t* country, std::uint32_t id, std::int64_t index);
void vsp_stub();
}

// вход jmp с 14074fe0b: rsp как в теле 14074fa80 (≡ 0 mod 16), [rsp+0x78] — индекс системы
__attribute__((naked)) void vsp_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov rcx, rbp\n"
        "    mov edx, r9d\n"
        "    mov r8, qword ptr [rsp + 0x78]\n"
        "    sub rsp, 0x20\n"
        "    call vis_contains\n"
        "    add rsp, 0x20\n"
        "    test al, al\n"
        "    je 1f\n"
        "    or si, 8\n"
        "1:\n"
        "    jmp qword ptr [rip + vsp_resume]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSite = 0x14074fe0b;
constexpr std::uint64_t kResume = 0x14074fe3e;
constexpr std::uint8_t kSiteBytes[] = {0x48, 0x63, 0x85, 0xec, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x7e, 0x28,
                                       0x4c, 0x8b, 0xc0, 0x48, 0x8b, 0x85, 0xe0, 0x00, 0x00, 0x00, 0x44,
                                       0x39, 0x08, 0x74, 0x10, 0xff, 0xc2, 0x48, 0xff, 0xc1, 0x48, 0x83,
                                       0xc0, 0x04, 0x49, 0x3b, 0xc8, 0x7c, 0xed, 0xeb, 0x09, 0x83, 0xfa,
                                       0xff, 0x74, 0x04, 0x66, 0x83, 0xce, 0x08};

struct Set {
    const std::uint8_t* country = nullptr;
    const std::int32_t* data = nullptr;
    std::int32_t count = -1;
    std::int64_t last_index = -1;
    std::vector<std::uint32_t> slots;  // 0 = пусто; хранится id + 1
    std::uint32_t mask = 0;
};
thread_local Set tset;
// обход страны: новый, когда страна другая или индекс системы не вырос
thread_local const std::uint8_t* tr_country = nullptr;
thread_local std::int64_t tr_index = -1;
thread_local std::uint64_t tr_gen = 0;
thread_local int in_item = 0;
struct Bits {
    std::uint64_t gen = ~0ull;
    const std::uint8_t* arr = nullptr;
    const std::uint16_t* data = nullptr;
    std::int32_t count = -1;
    std::uint64_t words[1024];  // 65536 бит
};
thread_local Bits* tbits = nullptr;  // 4 карты, по кругу
thread_local unsigned tbits_next = 0;
using Fn16 = bool (*)(const std::uint8_t*, std::uint32_t);
using Fn1da50 = std::uint64_t (*)(void*, std::uint64_t, void*, void*);
Fn16 has16_original = nullptr;
Fn1da50 f1da50_original = nullptr;
std::atomic<long long> c16{0}, m16{0}, r16{0}, out16{0};

std::atomic<long long> calls{0}, rebuilds{0}, mismatch{0}, found{0}, total_len{0};
LARGE_INTEGER freq{};
std::atomic<LONG64> last_report{0};

inline bool linear(const std::uint8_t* country, std::uint32_t id) {
    std::int32_t n;
    const std::int32_t* p;
    std::memcpy(&n, country + 0xec, 4);
    std::memcpy(&p, country + 0xe0, 8);
    for (std::int32_t i = 0; i < n; ++i)
        if (static_cast<std::uint32_t>(p[i]) == id) return true;
    return false;
}

inline std::uint32_t hash(std::uint32_t v) {
    v *= 0x9e3779b1u;
    return v ^ (v >> 15);
}

void rebuild(Set& s, const std::uint8_t* country, const std::int32_t* data, std::int32_t n) {
    s.last_index = 0;
    s.country = country;
    s.data = data;
    s.count = n;
    std::uint32_t cap = 16;
    while (cap < static_cast<std::uint32_t>(n) * 2u) cap <<= 1;
    s.slots.assign(cap, 0u);
    s.mask = cap - 1;
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) + 1u;
        std::uint32_t h = hash(v) & s.mask;
        while (s.slots[h] != 0 && s.slots[h] != v) h = (h + 1) & s.mask;
        s.slots[h] = v;
    }
    rebuilds.fetch_add(1, std::memory_order_relaxed);
    total_len.fetch_add(n, std::memory_order_relaxed);
}

inline bool fast(const std::uint8_t* country, std::uint32_t id, std::int64_t) {
    std::int32_t n;
    const std::int32_t* p;
    std::memcpy(&n, country + 0xec, 4);
    std::memcpy(&p, country + 0xe0, 8);
    if (n <= 0) return false;
    Set& s = tset;
    if (s.country != country || s.data != p || s.count != n || s.last_index == -2) rebuild(s, country, p, n);
    const std::uint32_t v = id + 1u;
    if (v == 0) return linear(country, id);  // id 0xffffffff не кодируется
    std::uint32_t h = hash(v) & s.mask;
    while (true) {
        const std::uint32_t x = s.slots[h];
        if (x == v) return true;
        if (x == 0) return false;
        h = (h + 1) & s.mask;
    }
}

void maybe_report() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONG64 last = last_report.load();
    if (last == 0) {
        last_report.compare_exchange_strong(last, now.QuadPart);
        return;
    }
    if (now.QuadPart - last < 60 * freq.QuadPart) return;
    if (!last_report.compare_exchange_strong(last, now.QuadPart)) return;
    const long long r = rebuilds.exchange(0), tl = total_len.exchange(0);
    splug::log("mode %lld: calls %lld, found %lld, set rebuilds %lld (avg list %.1f), mismatch %lld",
               static_cast<long long>(vsp_mode), calls.exchange(0), found.exchange(0), r, r ? double(tl) / r : 0.0,
               mismatch.load());
    const long long rr = r16.exchange(0);
    splug::log("mode16 %lld: calls %lld, bitmap rebuilds %lld, outside traversal %lld, mismatch %lld",
               static_cast<long long>(vsp_mode16), c16.exchange(0), rr, out16.exchange(0), m16.load());
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_vis_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"vis", L"mode", L"off");
    splug::log("stellaris_vis_patch 0.2.0: build %s, mode=%s, vsp_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&vsp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    if (!splug::bytes_match(kSite, kSiteBytes)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    std::uint8_t* site = splug::rva(kSite);
    auto* page = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!page) {
        splug::log("REFUSED: no memory near game code");
        return;
    }
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&vsp_stub);
    std::memcpy(page, jmp, 6);
    std::memcpy(page + 6, &target, 8);
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 16);
    const auto d = reinterpret_cast<std::intptr_t>(page) - reinterpret_cast<std::intptr_t>(site + 5);
    if (d < INT32_MIN || d > INT32_MAX) {
        splug::log("REFUSED: page out of rel32 range");
        return;
    }
    vsp_resume = reinterpret_cast<std::uint64_t>(splug::rva(kResume));
    vsp_mode = mode == splug::Mode::on ? 1 : 2;
    std::uint8_t patch[sizeof(kSiteBytes)];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xe9;
    const std::int32_t rel = static_cast<std::int32_t>(d);
    std::memcpy(patch + 1, &rel, 4);
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        vsp_mode = 0;
        splug::log("FAILED: cannot write jump");
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    splug::log("installed at %llx, mode %lld", static_cast<unsigned long long>(kSite), static_cast<long long>(vsp_mode));
    const auto mode16 = splug::ini_mode(L"vis", L"lists16", L"off");
    if (mode16 == splug::Mode::off) return;
    static const std::uint8_t kCall1da50[5] = {0xe8, 0xa0, 0xdb, 0xfc, 0xff};
    static const std::uint8_t kCall16[5] = {0xe8, 0xfd, 0x45, 0xbf, 0xff};
    std::uint8_t* s1 = splug::rva(0x14074feab);
    std::uint8_t* s2 = splug::rva(0x14071de8e);
    f1da50_original = reinterpret_cast<Fn1da50>(splug::call_target(s1));
    has16_original = reinterpret_cast<Fn16>(splug::call_target(s2));
    splug::CallPatch c1, c2;
    const char* e = splug::prepare_call(s1, kCall1da50, reinterpret_cast<const void*>(&vis_1da50), c1);
    if (!e) e = splug::prepare_call(s2, kCall16, reinterpret_cast<const void*>(&vis_has16), c2);
    if (e) {
        splug::log("lists16 REFUSED: %s", e);
        return;
    }
    vsp_mode16 = mode16 == splug::Mode::on ? 1 : 2;
    if (!splug::commit_call(c1) || !splug::commit_call(c2)) {
        vsp_mode16 = 0;
        splug::log("lists16 FAILED while writing calls");
        return;
    }
    splug::log("lists16 installed at 14074feab/14071de8e, mode %lld, vsp_mode16 at rva %llx",
               static_cast<long long>(vsp_mode16),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&vsp_mode16) -
                                               reinterpret_cast<std::uintptr_t>(module)));
}

}  // namespace

extern "C" bool vis_contains(const std::uint8_t* country, std::uint32_t id, std::int64_t index) {
    static thread_local unsigned long long tcalls = 0;
    if ((++tcalls & 0xfffff) == 0) maybe_report();
    if (country != tr_country || index <= tr_index) {
        tr_country = country;
        ++tr_gen;
        tset.last_index = -2;  // множество Б тоже перестроить
    }
    tr_index = index;
    const std::int64_t mode = vsp_mode;
    if (mode == 1) return fast(country, id, index);
    if (mode != 2) return linear(country, id);
    // тень: счётчики только здесь (атомики на каждом вызове мешали бы режиму 1)
    calls.fetch_add(1, std::memory_order_relaxed);
    const bool r = linear(country, id);
    if (fast(country, id, index) != r) mismatch.fetch_add(1, std::memory_order_relaxed);
    if (r) found.fetch_add(1, std::memory_order_relaxed);
    return r;
}

namespace {
inline bool linear16(const std::uint8_t* arr, std::uint32_t key) {
    std::int32_t n;
    const std::uint16_t* p;
    std::memcpy(&n, arr + 0x1c, 4);
    std::memcpy(&p, arr + 0x10, 8);
    for (std::int32_t i = 0; i < n; ++i)
        if (p[i] == static_cast<std::uint16_t>(key)) return true;
    return false;
}
inline bool fast16(const std::uint8_t* arr, std::uint32_t key) {
    std::int32_t n;
    const std::uint16_t* p;
    std::memcpy(&n, arr + 0x1c, 4);
    std::memcpy(&p, arr + 0x10, 8);
    if (n <= 0) return false;
    if (!tbits) tbits = new Bits[4]();
    Bits* hit = nullptr;
    for (int i = 0; i < 4; ++i) {
        Bits& c = tbits[i];
        if (c.gen == tr_gen && c.arr == arr && c.data == p && c.count == n) {
            hit = &c;
            break;
        }
    }
    if (!hit) {
        hit = &tbits[tbits_next++ & 3];
        Bits& b = *hit;
        std::memset(b.words, 0, sizeof(b.words));
        for (std::int32_t i = 0; i < n; ++i) b.words[p[i] >> 6] |= 1ull << (p[i] & 63);
        b.gen = tr_gen;
        b.arr = arr;
        b.data = p;
        b.count = n;
        r16.fetch_add(1, std::memory_order_relaxed);
    }
    const Bits& b = *hit;
    const std::uint16_t k = static_cast<std::uint16_t>(key);
    return (b.words[k >> 6] >> (k & 63)) & 1;
}
}  // namespace

extern "C" std::uint64_t vis_1da50(void* a, std::uint64_t b, void* c, void* d) {
    ++in_item;
    const std::uint64_t r = f1da50_original(a, b, c, d);
    --in_item;
    return r;
}

extern "C" bool vis_has16(const std::uint8_t* arr, std::uint32_t key) {
    const std::int64_t mode = vsp_mode16;
    if (mode == 0) return has16_original(arr, key);
    if (!in_item) {
        if (mode == 2) out16.fetch_add(1, std::memory_order_relaxed);
        return has16_original(arr, key);
    }
    if (mode == 1) return fast16(arr, key);
    c16.fetch_add(1, std::memory_order_relaxed);
    const bool r = linear16(arr, key);
    if (fast16(arr, key) != r) m16.fetch_add(1, std::memory_order_relaxed);
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
