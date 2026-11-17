// Stellaris Intel Patch 0.4.0, Stellaris 4.5.4 x64 — линейный поиск id системы в расчёте
// видимости систем для страны.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   1406eecb0(country C)  строит для C байтовый массив уровней по системам (C+0x58). Для каждой
//       связанной страны и каждой её системы (other+0x2ea8, число +0x2eb4) зовёт
//       1406ef480(ctx, system_id). Запускается в parallel-for по странам (141b45f40).
//   1406ef480  если байт системы ещё 0 и глобальный флаг включён, ищет id системы ([sys+8])
//       ЛИНЕЙНО в списке obj+0x50 (число +0x5c), obj = [C+0x2808] (1406ef51a..1406ef555);
//       нашёл — байт = 1. В 2300 году на TEST5000 этот проход — около 2/3 своего времени
//       1406ef480 (~1,9 % главного потока и ~28 с на всех потоках за 180 с).
//
// Плагин:
//   * ставит переходник на вход 1406eecb0: счётчик вызовов (эпоха) в данных потока;
//   * заменяет проход 1406ef51a..1406ef555 вызовом ssi_contains(obj, id). Внутри одного вызова
//     1406eecb0 список не меняется (его меняет только сама игра вне этого расчёта), поэтому
//     таблица «индекс системы -> полный id», построенная один раз на эпоху и список, отвечает
//     так же, как проход. Если два id в списке делят индекс (младшие 24 бита) — для этого
//     списка используется проход, как в игре. Вне 1406eecb0 — всегда проход.
//
// Режимы ([intel] mode=):
//   off     ничего не меняет;
//   shadow  ответ — проход, как в игре; рядом таблица, расхождения считаются в логе;
//   on      ответ — таблица.
// Глобал ssi_enabled (rva печатается в лог) можно менять на лету: 0 = проход, 1 = таблица.
//
// Бинарный поиск 14071cbb0 ([intel] bsearch=, тот же расчёт): (key, sub) в отсортированном массиве
// obj+0x1998 (число +0x19a4). Запросы идут почти по порядку массива, поэтому плагин ищет
// lower_bound галопом от позиции прошлого ответа по самому массиву игры — ответ тот же при любом
// порядке, таблицы нет. ssb_enabled на лету: 4 = галоп (по умолчанию), 1 = хеш-таблица на расчёт
// (0.2.0), 0 = поиск игры. На BENCH2300: время плагина вдвое меньше, чем с таблицей.
//
// 0.4.0 (МЕНЯЕТ ПОВЕДЕНИЕ): [intel] rr=K — ежедневный пересчёт уровней разведки страны (вызов 140301cc0 из задания
// 140301bf0) выполняется только в дни, когда (id страны + день) % K == 0; в остальные дни массив уровней остаётся
// вчерашним. Прочие вызовы 1406eecb0 (события, создание страны) не трогаются. sir_k (rva в логе) на лету: 0/1 — как игра.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
std::int64_t ssi_enabled = 0;   // 1 — отвечать таблицей (mode=on); 0 — проходом
std::int64_t ssi_shadow = 0;    // 1 — считать оба и сравнивать
std::uint64_t ssi_found = 0;    // 1406ef55a
std::uint64_t ssi_missing = 0;  // 1406ef55f
bool ssi_contains(const std::uint8_t* obj, std::uint32_t id);
void intel_scan_stub();
std::int64_t ssb_enabled = 0;   // поиск 14071cbb0: 4 — галоп от прошлой позиции, 1 — хеш-таблица, 0 — как в игре
std::int64_t ssb_shadow = 0;    // 1 — считать оба и сравнивать
std::uint64_t ssb_true = 0;     // 14071ccc1
std::uint64_t ssb_false = 0;    // 14071ccaf
bool ssb_contains(const std::uint8_t* obj, std::int32_t key, std::uint32_t sub);
void bsearch_stub();
std::int64_t sir_k = 0;
void* sir_outer(void* c, void* b, void* x, void* d);
}

// вход jmp из 14071cc27; живые: rdi (obj), edx (ключ), r9d (подключ); rbx/rsi/rdi восстановятся
// из стека в эпилоге; rsp ≡ 0 mod 16
__attribute__((naked)) void bsearch_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    sub rsp, 0x20\n"
        "    mov rcx, rdi\n"
        "    mov r8d, r9d\n"
        "    call ssb_contains\n"
        "    add rsp, 0x20\n"
        "    test al, al\n"
        "    jz 1f\n"
        "    jmp qword ptr [rip + ssb_true]\n"
        "1:\n"
        "    jmp qword ptr [rip + ssb_false]\n"
        ".att_syntax\n");
}

// вход jmp из 1406ef51a; живые: rbx (система), rdi (индекс байта), r10, r11, r15 (ctx); rsp ≡ 0 mod 16
__attribute__((naked)) void intel_scan_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    push r10\n"
        "    push r11\n"
        "    sub rsp, 0x20\n"
        "    mov rcx, qword ptr [r15 + 0x18]\n"
        "    mov rcx, qword ptr [rcx + 0x2808]\n"
        "    mov edx, dword ptr [rbx + 8]\n"
        "    call ssi_contains\n"
        "    add rsp, 0x20\n"
        "    pop r11\n"
        "    pop r10\n"
        "    test al, al\n"
        "    jz 1f\n"
        "    jmp qword ptr [rip + ssi_found]\n"
        "1:\n"
        "    jmp qword ptr [rip + ssi_missing]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kScan = 0x1406ef51a;
constexpr std::uint64_t kFound = 0x1406ef55a;
constexpr std::uint64_t kMissing = 0x1406ef55f;
constexpr std::uint64_t kCountry = 0x1406eecb0;
constexpr std::uint64_t kCountryBody = 0x1406eecbd;

// 1406ef511: cmp byte [rip+..],0; je 55a; mov rax,[r15+0x18]; xor edx,edx; mov r9d,[rbx+8];
// mov rax,[rax+0x2808]; movsxd rcx,[rax+0x5c]; test ecx,ecx; jle 55f; mov rax,[rax+0x50]; ...
constexpr std::uint8_t kScanBytes[] = {0x80, 0x3d, 0x32, 0xc3, 0x12, 0x02, 0x00, 0x74, 0x40, 0x49, 0x8b, 0x47,
    0x18, 0x33, 0xd2, 0x44, 0x8b, 0x4b, 0x08, 0x48, 0x8b, 0x80, 0x08, 0x28, 0x00, 0x00, 0x48, 0x63, 0x48, 0x5c,
    0x85, 0xc9, 0x7e, 0x2c, 0x48, 0x8b, 0x40, 0x50, 0x4c, 0x8b, 0xc1, 0x8b, 0xca, 0x0f, 0x1f, 0x40, 0x00, 0x44,
    0x39, 0x08, 0x74, 0x10, 0xff, 0xc2, 0x48, 0xff, 0xc1, 0x48, 0x83, 0xc0, 0x04, 0x49, 0x3b, 0xc8, 0x7c, 0xed,
    0xeb, 0x0a, 0x83, 0xfa, 0xff, 0x74, 0x05, 0x41, 0xc6, 0x04, 0x3b, 0x01};
// 1406eecb0: push rbp; push rbx; push rsi; push rdi; push r12; push r13; push r14; push r15
constexpr std::uint8_t kCountryPrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54,
                                             0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

constexpr std::uint64_t kBsearch = 0x14071cc27;
constexpr std::uint64_t kBsTrue = 0x14071ccc1;
constexpr std::uint64_t kBsFalse = 0x14071ccaf;
// 14071cc27: movsxd r11, [rdi+0x19a4]; mov rsi, [rdi+0x1998]
constexpr std::uint8_t kBsBytes[] = {0x4c, 0x63, 0x9f, 0xa4, 0x19, 0x00, 0x00,
                                     0x48, 0x8b, 0xb7, 0x98, 0x19, 0x00, 0x00};

struct BsEntry {
    std::uint64_t pad;
    std::int32_t key;
    std::uint32_t sub;
};

struct BsCache {
    const std::uint8_t* obj = nullptr;
    const BsEntry* data = nullptr;
    int count = -1;
    std::uint64_t built_epoch = 0;
    std::uint32_t gen = 0;
    std::uint32_t mask = 0;
    std::vector<std::uint32_t> stamp;
    std::vector<std::uint64_t> keys;
    const BsEntry* fdata = nullptr;  // режим 4: массив и позиция подсказки
    long long fpos = 0;
    long long calls = 0, builds = 0, mismatch = 0, entries = 0, reuses = 0, hits = 0;
};

struct Cache {
    std::uint64_t epoch = 1;
    int depth = 0;
    BsCache bs;
    const std::uint8_t* obj = nullptr;
    const std::uint32_t* data = nullptr;
    int count = -1;
    std::uint64_t built_epoch = 0;
    bool slow = false;
    std::uint32_t gen = 0;
    std::vector<std::uint32_t> stamp, full;
    long long calls = 0, builds = 0, slow_lists = 0, mismatch = 0;
};

DWORD tls = TLS_OUT_OF_INDEXES;
SRWLOCK all_lock = SRWLOCK_INIT;
std::vector<Cache*>* all_caches = nullptr;
volatile LONG64 last_report = 0;
LARGE_INTEGER freq{};
double report_s = 60.0;

thread_local Cache* tcache = nullptr;

Cache* cache() {
    Cache* c = tcache;
    if (!c) {
        c = new Cache();
        tcache = c;
        AcquireSRWLockExclusive(&all_lock);
        all_caches->push_back(c);
        ReleaseSRWLockExclusive(&all_lock);
    }
    return c;
}

bool linear(const std::uint32_t* data, int count, std::uint32_t id) {
    for (int i = 0; i < count; ++i)
        if (data[i] == id) return true;
    return false;
}

void build(Cache& c) {
    ++c.builds;
    if (++c.gen == 0) {  // переполнение поколения — очистить отметки
        std::fill(c.stamp.begin(), c.stamp.end(), 0u);
        c.gen = 1;
    }
    c.slow = false;
    for (int i = 0; i < c.count; ++i) {
        const std::uint32_t id = c.data[i];
        const std::uint32_t idx = id & 0xffffffu;
        if (idx >= c.stamp.size()) {
            const std::size_t n = (static_cast<std::size_t>(idx) + 1024) & ~std::size_t{1023};
            c.stamp.resize(n, 0u);
            c.full.resize(n, 0u);
        }
        if (c.stamp[idx] == c.gen) {
            if (c.full[idx] != id) c.slow = true;  // два разных id на одном индексе — проходом
        } else {
            c.stamp[idx] = c.gen;
            c.full[idx] = id;
        }
    }
    if (c.slow) ++c.slow_lists;
    c.built_epoch = c.epoch;
}

void maybe_report() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONG64 last = last_report;
    if (static_cast<double>(now.QuadPart - last) < report_s * static_cast<double>(freq.QuadPart)) return;
    if (InterlockedCompareExchange64(&last_report, now.QuadPart, last) != last) return;
    long long calls = 0, builds = 0, slow = 0, mismatch = 0;
    AcquireSRWLockShared(&all_lock);
    for (Cache* c : *all_caches) {  // счётчики чужих потоков читаются без синхронизации — для лога
        calls += c->calls;
        builds += c->builds;
        slow += c->slow_lists;
        mismatch += c->mismatch;
    }
    ReleaseSRWLockShared(&all_lock);
    splug::log("%s: lookups %lld, table builds %lld, lists with shared index %lld, mismatch %lld",
               ssi_shadow ? "shadow" : (ssi_enabled ? "on" : "off(live)"), calls, builds, slow, mismatch);
}

void maybe_report_bs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    static volatile LONG64 last_bs = 0;
    const LONG64 last = last_bs;
    if (last == 0) {
        InterlockedCompareExchange64(&last_bs, now.QuadPart, 0);
        return;
    }
    if (static_cast<double>(now.QuadPart - last) < report_s * static_cast<double>(freq.QuadPart)) return;
    if (InterlockedCompareExchange64(&last_bs, now.QuadPart, last) != last) return;
    long long calls = 0, builds = 0, mismatch = 0, entries = 0, reuses = 0, hits = 0;
    AcquireSRWLockShared(&all_lock);
    for (Cache* c : *all_caches) {
        reuses += c->bs.reuses;
        hits += c->bs.hits;
        calls += c->bs.calls;
        builds += c->bs.builds;
        mismatch += c->bs.mismatch;
        entries += c->bs.entries;
    }
    ReleaseSRWLockShared(&all_lock);
    splug::log("bsearch %s/%lld: lookups %lld, table builds %lld (avg %lld entries), at hint %lld, hits %lld, mismatch %lld",
               ssb_shadow ? "shadow" : (ssb_enabled ? "on" : "off(live)"), static_cast<long long>(ssb_enabled),
               calls, builds, builds ? entries / builds : 0, reuses, hits, mismatch);
}

// точная копия поиска игры: lower_bound по (key знаково, sub беззнаково), затем проверка равенства
bool bs_original(const BsEntry* data, int count, std::int32_t key, std::uint32_t sub) {
    if (count <= 0) return false;
    const BsEntry* first = data;
    long long n = count;
    while (n > 0) {
        const long long half = n >> 1;
        const BsEntry& e = first[half];
        const bool less = e.key != key ? e.key < key : e.sub < sub;
        if (less) {
            first += half + 1;
            n -= half + 1;
        } else {
            n = half;
        }
    }
    if (first == data + count) return false;
    const bool key_less = first->key != key ? key < first->key : sub < first->sub;
    if (key_less) return false;
    return static_cast<int>(first - data) >= 0;
}

inline std::uint64_t bs_pack(std::int32_t key, std::uint32_t sub) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key)) << 32) | sub;
}

inline std::uint32_t bs_hash(std::uint64_t k, std::uint32_t mask) {
    return static_cast<std::uint32_t>((k * 0x9E3779B97F4A7C15ull) >> 40) & mask;
}

void bs_build(BsCache& b) {
    ++b.builds;
    b.entries += b.count;
    std::uint32_t size = 64;
    while (size < static_cast<std::uint32_t>(b.count) * 2u) size <<= 1;
    if (b.stamp.size() < size) {
        b.stamp.assign(size, 0u);
        b.keys.assign(size, 0ull);
        b.gen = 0;
    }
    if (++b.gen == 0) {
        std::fill(b.stamp.begin(), b.stamp.end(), 0u);
        b.gen = 1;
    }
    b.mask = size - 1;
    for (int i = 0; i < b.count; ++i) {
        const std::uint64_t k = bs_pack(b.data[i].key, b.data[i].sub);
        std::uint32_t h = bs_hash(k, b.mask);
        while (b.stamp[h] == b.gen && b.keys[h] != k) h = (h + 1) & b.mask;
        b.stamp[h] = b.gen;
        b.keys[h] = k;
    }
}

bool bs_lookup(const BsCache& b, std::int32_t key, std::uint32_t sub) {
    const std::uint64_t k = bs_pack(key, sub);
    std::uint32_t h = bs_hash(k, b.mask);
    while (b.stamp[h] == b.gen) {
        if (b.keys[h] == k) return true;
        h = (h + 1) & b.mask;
    }
    return false;
}

// режим 4 (по умолчанию): lower_bound галопом от позиции прошлого ответа по тому же массиву. Таблицы нет,
// читается сам массив игры, поэтому ответ точный при любом порядке запросов; подсказка
// влияет только на число шагов. В reuses считаются ответы прямо на подсказке.
inline bool bs_less(const BsEntry& e, std::int32_t key, std::uint32_t sub) {
    return e.key != key ? e.key < key : e.sub < sub;
}

bool bs_finger(BsCache& b, const BsEntry* data, int count, std::int32_t key, std::uint32_t sub) {
    if (b.fdata != data) {
        b.fdata = data;
        b.fpos = 0;
    }
    long long p = b.fpos < count ? b.fpos : count;
    long long lo, hi;
    if (p < count && bs_less(data[p], key, sub)) {
        lo = p + 1;  // ответ правее p
        long long step = 1;
        while (lo + step - 1 < count && bs_less(data[lo + step - 1], key, sub)) {
            lo += step;
            step <<= 1;
        }
        hi = lo + step - 1 < count ? lo + step - 1 : count;
    } else {
        hi = p;  // ответ не правее p
        long long step = 1;
        while (hi - step >= 0 && !bs_less(data[hi - step], key, sub)) {
            hi -= step;
            step <<= 1;
        }
        lo = hi - step >= 0 ? hi - step + 1 : 0;
    }
    if (lo == p && hi == p) ++b.reuses;
    while (lo < hi) {  // lower_bound в [lo, hi)
        const long long mid = lo + ((hi - lo) >> 1);
        if (bs_less(data[mid], key, sub))
            lo = mid + 1;
        else
            hi = mid;
    }
    const bool found = lo < count && data[lo].key == key && data[lo].sub == sub;
    b.fpos = found ? lo + 1 : lo;
    return found;
}

using CountryFn = void* (*)(void*, void*, void*, void*);
CountryFn country_original = nullptr;

void* country_hook(void* a, void* b, void* c, void* d) {
    Cache& k = *cache();
    ++k.depth;
    ++k.epoch;
    void* r = country_original(a, b, c, d);
    --k.depth;
    ++k.epoch;
    return r;
}

std::uint8_t* make_trampoline(const std::uint8_t* bytes, std::size_t n, std::uint64_t resume) {
    auto* t = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    std::memcpy(t, bytes, n);
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(t + n, jmp, 6);
    std::memcpy(t + n + 6, &resume, 8);
    FlushInstructionCache(GetCurrentProcess(), t, n + 14);
    return t;
}

using RrFn = void* (*)(void*, void*, void*, void*);
RrFn rr_orig = nullptr;
std::atomic<long long> rr_run{0}, rr_skip{0};

void install_rr(HMODULE module) {
    sir_k = splug::ini_int(L"intel", L"rr", 0);
    constexpr std::uint8_t kRrBytes[5] = {0xe8, 0xeb, 0xcf, 0x3e, 0x00};
    rr_orig = reinterpret_cast<RrFn>(splug::call_target(splug::rva(0x140301cc0)));
    splug::CallPatch p;
    if (reinterpret_cast<std::uintptr_t>(rr_orig) != reinterpret_cast<std::uintptr_t>(splug::rva(kCountry)) ||
        splug::prepare_call(splug::rva(0x140301cc0), kRrBytes, reinterpret_cast<const void*>(&sir_outer), p) ||
        !splug::commit_call(p)) {
        splug::log("rr REFUSED");
        return;
    }
    splug::log("rr installed at 140301cc0, k=%lld, sir_k at rva %llx", static_cast<long long>(sir_k),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&sir_k) -
                                               reinterpret_cast<std::uintptr_t>(module)));
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_intel_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"intel", L"mode", L"off");
    report_s = splug::ini_int(L"intel", L"report_s", 60);
    splug::log("stellaris_intel_patch 0.3.0: build %s, mode=%s, ssi_enabled at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&ssi_enabled) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    if (!splug::bytes_match(0x1406ef511, kScanBytes) || !splug::bytes_match(kCountry, kCountryPrologue)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    tls = TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES) {
        splug::log("REFUSED: no TLS slot");
        return;
    }
    all_caches = new std::vector<Cache*>();
    ssi_shadow = mode == splug::Mode::shadow ? 1 : 0;
    ssi_enabled = mode == splug::Mode::on ? 1 : 0;
    ssi_found = reinterpret_cast<std::uint64_t>(splug::rva(kFound));
    ssi_missing = reinterpret_cast<std::uint64_t>(splug::rva(kMissing));
    auto* tramp = make_trampoline(kCountryPrologue, sizeof(kCountryPrologue),
                                  reinterpret_cast<std::uint64_t>(splug::rva(kCountryBody)));
    if (!tramp) {
        splug::log("REFUSED: no memory");
        return;
    }
    country_original = reinterpret_cast<CountryFn>(tramp);
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(const_cast<LONG64*>(&last_report)));
    // сначала вход страны (эпоха), потом проход
    if (!splug::write_jump(splug::rva(kCountry), reinterpret_cast<const void*>(&country_hook)) ||
        !splug::write_jump(splug::rva(kScan), reinterpret_cast<const void*>(&intel_scan_stub))) {
        splug::log("REFUSED: cannot write jump");
        return;
    }
    splug::log("installed: entry %llx, scan %llx", static_cast<unsigned long long>(kCountry),
               static_cast<unsigned long long>(kScan));
    install_rr(module);

    const auto bmode = splug::ini_mode(L"intel", L"bsearch", L"off");
    splug::log("bsearch mode=%s, ssb_enabled at rva %llx", splug::mode_name(bmode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&ssb_enabled) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (bmode == splug::Mode::off) return;
    if (!splug::bytes_match(kBsearch, kBsBytes)) {
        splug::log("bsearch REFUSED: bytes differ; not changed");
        return;
    }
    ssb_shadow = bmode == splug::Mode::shadow ? 1 : 0;
    ssb_enabled = 4;  // shadow сверяет режим 4 с поиском игры
    ssb_true = reinterpret_cast<std::uint64_t>(splug::rva(kBsTrue));
    ssb_false = reinterpret_cast<std::uint64_t>(splug::rva(kBsFalse));
    if (!splug::write_jump(splug::rva(kBsearch), reinterpret_cast<const void*>(&bsearch_stub))) {
        splug::log("bsearch REFUSED: cannot write jump");
        return;
    }
    splug::log("bsearch installed at %llx", static_cast<unsigned long long>(kBsearch));
}

}  // namespace

extern "C" void* sir_outer(void* c, void* b, void* x, void* d) {
    if (sir_k > 1 && c) {
        const auto* clk = *splug::rva<const std::uint8_t*>(0x14311b708);
        const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(c) + 0x20) & 0xffffffu;
        const std::uint32_t day = clk ? *reinterpret_cast<const std::uint32_t*>(clk + 0xc4) : 0;
        if ((id + day) % static_cast<std::uint64_t>(sir_k) != 0) {
            rr_skip.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    const long long n = rr_run.fetch_add(1, std::memory_order_relaxed);
    if ((n & 4095) == 4095)
        splug::log("rr k=%lld: country intel updates run %lld, skipped %lld", static_cast<long long>(sir_k),
                   rr_run.exchange(0), rr_skip.exchange(0));
    return rr_orig(c, b, x, d);
}

extern "C" bool ssi_contains(const std::uint8_t* obj, std::uint32_t id) {
    int count;
    const std::uint32_t* data;
    std::memcpy(&count, obj + 0x5c, 4);
    if (count <= 0) return false;
    std::memcpy(&data, obj + 0x50, 8);
    Cache& c = *cache();
    ++c.calls;
    if ((c.calls & 0xffff) == 0) maybe_report();
    if (!ssi_enabled && !ssi_shadow) return linear(data, count, id);
    if (c.depth == 0) return linear(data, count, id);
    if (c.built_epoch != c.epoch || c.obj != obj || c.data != data || c.count != count) {
        c.obj = obj;
        c.data = data;
        c.count = count;
        build(c);
    }
    bool fast;
    if (c.slow) {
        fast = linear(data, count, id);
    } else {
        const std::uint32_t idx = id & 0xffffffu;
        fast = idx < c.stamp.size() && c.stamp[idx] == c.gen && c.full[idx] == id;
    }
    if (ssi_shadow) {
        const bool slow = linear(data, count, id);
        if (slow != fast) ++c.mismatch;
        return slow;
    }
    return fast;
}

extern "C" bool ssb_contains(const std::uint8_t* obj, std::int32_t key, std::uint32_t sub) {
    int count;
    const BsEntry* data;
    std::memcpy(&count, obj + 0x19a4, 4);
    std::memcpy(&data, obj + 0x1998, 8);
    if (count <= 0) return false;
    if (!ssb_enabled && !ssb_shadow) return bs_original(data, count, key, sub);
    Cache& c = *cache();
    if (c.depth == 0) return bs_original(data, count, key, sub);
    BsCache& b = c.bs;
    ++b.calls;
    if ((b.calls & 0xffff) == 0) maybe_report_bs();
    if (ssb_enabled == 4) {
        const bool fast = bs_finger(b, data, count, key, sub);
        b.hits += fast;
        if (ssb_shadow) {
            const bool slow = bs_original(data, count, key, sub);
            if (slow != fast) ++b.mismatch;
            return slow;
        }
        return fast;
    }
    if (b.built_epoch != c.epoch || b.obj != obj || b.data != data || b.count != count) {
        b.obj = obj;
        b.data = data;
        b.count = count;
        b.built_epoch = c.epoch;
        bs_build(b);
    }
    const bool fast = bs_lookup(b, key, sub);
    b.hits += fast;
    if (ssb_shadow) {
        const bool slow = bs_original(data, count, key, sub);
        if (slow != fast) ++b.mismatch;
        return slow;
    }
    return fast;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
