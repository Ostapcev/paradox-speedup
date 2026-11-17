// Stellaris PathQ Patch 0.4.0, Stellaris 4.5.4 x64 — запрос пути между двумя системами для страны.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140766810(country, out, sysA, sysB) -> out {int64 dist; uint16 hops; ...} (16 байт)
//       если country+0x34b4 == 0 -> «недостижимо» (dist -100000, hops 0xffff);
//       ключ (min id, max id); FNV-хэш в таблице country+0x3530 под rw-спинлоком country+0x3520;
//       в таблице хранятся ТОЛЬКО недостижимые пары; иначе каждый раз Дейкстра 14076b970
//       с правилами прохода страны (140777750) и стоимостью рёбер (14077dd30).
//   В 2300 году на TEST5000 это ~5,6 % главного потока и ещё ~3,6 с на рабочих потоках за 180 с.
//
// Ускорение ([pathq] batch=1): 1407770e0 после найденного пути кладёт в кэш страны ВСЕ пары вершин
//   пути (O(L^2), в 2300 году ~1200 пар на путь, 95 % уже есть), на каждую пару — разделяемая
//   блокировка и поиск. Плагин делает тот же проход в том же порядке под одной эксклюзивной
//   блокировкой на путь (так, как если бы игру никто не прерывал) с предвыборкой корзин.
//   Живой замер: средний «медленный» запрос 176 -> ~147 мкс (-16 %).
//   batch=2 — то же в два прохода: поиск всех пар под разделяемой блокировкой (кусками по 64 пары),
//   затем эксклюзивно только вставка недостающих (3-5 %) в том же порядке с повторной проверкой.
//   С batch=1 читатели других потоков крутились в 1407668a8, пока писатель держит путь целиком
//   (до 8 с CPU в минуту на 14 рабочих потоках).
// prefetch=N — предвыборка соседних систем в Дейкстре: замер без выигрыша, по умолчанию 0.
//
// Режимы ([pathq] mode=):
//   off    ничего не меняет;
//   probe  только измеряет: число запросов, время, повторы того же ключа (страна, пара) и
//          совпадает ли результат повтора с прошлым (по возрасту: <1 с, <10 с, старше).
//          Ничего в игре не меняет, кроме времени.
//   shadow кэш на игровой день: эпоха растёт на входе дневного тика 14025c850; ответ всегда
//          игры, а для повтора ключа в той же эпохе сравнивается с запомненным (все 16 байт).
//          В лог: попадания, их время (сколько сэкономил бы кэш) и расхождения.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include <intrin.h>

extern "C" {
volatile std::int64_t spq_epoch = 1;
std::uint64_t spq_day_tramp = 0;
void spq_day_stub();
std::int64_t spq_pf = 0;            // сколько гиперлиний снятой системы предвыбирать (0 — выкл.)
std::uint64_t spq_gtab = 0;         // адрес глобала таблицы систем по id (14311bed8)
std::uint64_t spq_pf_resume = 0;    // 14076bdf0
void spq_pf_stub();
std::int64_t spq_batch = 0;         // 1 — пары пути в кэш одним проходом под одной блокировкой
std::uint64_t spq_pairs_resume = 0; // 1407776b8 (после цикла пар)
std::uint64_t spq_pairs_orig = 0;   // 14077739b (исходный цикл, после вытесненных инструкций)
void spq_pairs(std::uint8_t* frame);
void spq_pairs_stub();
}

// вход jmp из 140777389 (1407770e0, перед циклом «все пары пути -> кэш»), rbp = кадр функции.
__attribute__((naked)) void spq_pairs_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    cmp qword ptr [rip + spq_batch], 0\n"
        "    je 1f\n"
        "    mov rcx, rbp\n"
        "    sub rsp, 0x20\n"
        "    call spq_pairs\n"
        "    add rsp, 0x20\n"
        "    mov r13, qword ptr [rbp + 0xe0]\n"
        "    xor r10d, r10d\n"
        "    jmp qword ptr [rip + spq_pairs_resume]\n"
        "1:\n"
        "    mov r12d, dword ptr [rbp + 0x14]\n"
        "    dec r12d\n"
        "    cmp r12d, -1\n"
        "    mov r13, qword ptr [rbp + 0xe0]\n"
        "    jmp qword ptr [rip + spq_pairs_orig]\n"
        ".att_syntax\n");
}

// вход jmp из 14076bde9 (Дейкстра, снята вершина): rcx = система, на месте — cmp [rcx+0x5bc],0.
// Предвыборка соседей: для первых spq_pf гиперлиний [rcx+0x590] (шаг 0x20, id на +8) — объект
// соседней системы (+0, +0x590, +0x5bc). Регистры и флаги как в игре; порядок работы тот же.
__attribute__((naked)) void spq_pf_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    push rax\n"
        "    push rdx\n"
        "    push r8\n"
        "    push r9\n"
        "    push r10\n"
        "    push r11\n"
        "    mov r9, qword ptr [rip + spq_pf]\n"
        "    test r9, r9\n"
        "    jle 9f\n"
        "    mov r10, qword ptr [rip + spq_gtab]\n"
        "    mov r10, qword ptr [r10]\n"
        "    test r10, r10\n"
        "    jz 9f\n"
        "    movsxd r11, dword ptr [rcx + 0x59c]\n"
        "    cmp r11, r9\n"
        "    cmovg r11, r9\n"
        "    test r11, r11\n"
        "    jle 9f\n"
        "    mov r8, qword ptr [rcx + 0x590]\n"
        "1:\n"
        "    mov eax, dword ptr [r8 + 8]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, dword ptr [r10 + 0x20]\n"
        "    jae 2f\n"
        "    mov rdx, qword ptr [r10 + 0x18]\n"
        "    shl rax, 4\n"
        "    mov rdx, qword ptr [rdx + rax + 8]\n"
        "    test rdx, rdx\n"
        "    jz 2f\n"
        "    prefetcht0 byte ptr [rdx]\n"
        "    prefetcht0 byte ptr [rdx + 0x590]\n"
        "    prefetcht0 byte ptr [rdx + 0x5bc]\n"
        "2:\n"
        "    add r8, 0x20\n"
        "    dec r11\n"
        "    jnz 1b\n"
        "9:\n"
        "    pop r11\n"
        "    pop r10\n"
        "    pop r9\n"
        "    pop r8\n"
        "    pop rdx\n"
        "    pop rax\n"
        "    cmp dword ptr [rcx + 0x5bc], 0\n"
        "    jmp qword ptr [rip + spq_pf_resume]\n"
        ".att_syntax\n");
}

// вход дневного тика 14025c850: эпоха++ и дальше исходный пролог
__attribute__((naked)) void spq_day_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    lock inc qword ptr [rip + spq_epoch]\n"
        "    jmp qword ptr [rip + spq_day_tramp]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kDay = 0x14025c850;
constexpr std::uint64_t kDayBody = 0x14025c861;
// mov [rsp+8],rcx; push rbp/rbx/rsi/rdi/r12/r13/r14/r15
constexpr std::uint8_t kDayPrologue[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57,
                                         0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

constexpr std::uint64_t kQuery = 0x140766810;
constexpr std::uint64_t kQueryBody = 0x14076681f;
constexpr std::uint8_t kPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24,
                                      0x18, 0x48, 0x89, 0x7c, 0x24, 0x20};

using QueryFn = void* (*)(void*, void*, void*, void*);
QueryFn original = nullptr;

struct Key {
    std::uint64_t country;
    std::uint64_t pair;
    bool operator==(const Key& o) const { return country == o.country && pair == o.pair; }
};
struct KeyHash {
    std::size_t operator()(const Key& k) const {
        std::uint64_t h = k.country * 0x9E3779B97F4A7C15ull ^ (k.pair + 0x632BE59BD9B4E019ull);
        h ^= h >> 29;
        return static_cast<std::size_t>(h * 0xBF58476D1CE4E5B9ull);
    }
};
struct Seen {
    std::uint64_t dist;
    std::uint16_t hops;
    std::int64_t when;
};

struct Cached {
    std::int64_t epoch;
    std::uint8_t out[16];
};
std::unordered_map<Key, Cached, KeyHash>* day = nullptr;
struct DayStats {
    long long calls = 0, hits = 0, mism = 0, mism_dh = 0, epochs_seen = 0;
    std::int64_t hit_ticks = 0, ticks = 0;
    std::int64_t first_epoch = 0;
} ds;

SRWLOCK lock = SRWLOCK_INIT;
std::unordered_map<Key, Seen, KeyHash>* seen = nullptr;
LARGE_INTEGER freq{};
std::int64_t last_report = 0;
double report_s = 60.0;

struct Stats {
    long long calls = 0, main_calls = 0, slow = 0, unreachable = 0;
    std::int64_t ticks = 0, slow_ticks = 0;
    long long rep[3] = {0, 0, 0}, diff[3] = {0, 0, 0};
} st;

std::uint32_t id_of(void* sys) {
    std::uint32_t v;
    std::memcpy(&v, static_cast<std::uint8_t*>(sys) + 8, 4);
    return v;
}

void report_locked(std::int64_t now) {
    const double f = static_cast<double>(freq.QuadPart);
    splug::log("probe: calls %lld (main %lld), %.1f ms total, slow(>3us) %lld = %.1f ms, unreachable %lld, "
               "keys %zu; repeats <1s %lld (diff %lld), <10s %lld (diff %lld), older %lld (diff %lld)",
               st.calls, st.main_calls, 1000.0 * static_cast<double>(st.ticks) / f, st.slow,
               1000.0 * static_cast<double>(st.slow_ticks) / f, st.unreachable, seen->size(), st.rep[0],
               st.diff[0], st.rep[1], st.diff[1], st.rep[2], st.diff[2]);
    st = Stats{};
    last_report = now;
    if (seen->size() > 4000000) seen->clear();
}

void* hook(void* country, void* out, void* a, void* b) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    void* r = original(country, out, a, b);
    QueryPerformanceCounter(&t1);
    const std::int64_t dt = t1.QuadPart - t0.QuadPart;
    std::uint64_t dist;
    std::uint16_t hops;
    std::memcpy(&dist, out, 8);
    std::memcpy(&hops, static_cast<std::uint8_t*>(out) + 8, 2);
    std::uint32_t ia = id_of(a), ib = id_of(b);
    if (ia > ib) std::swap(ia, ib);
    const Key key{reinterpret_cast<std::uint64_t>(country), (static_cast<std::uint64_t>(ia) << 32) | ib};
    const bool main = GetCurrentThreadId() == splug::state().main_tid;
    AcquireSRWLockExclusive(&lock);
    ++st.calls;
    if (main) ++st.main_calls;
    st.ticks += dt;
    if (dt * 1000000 > 3 * freq.QuadPart) {
        ++st.slow;
        st.slow_ticks += dt;
    }
    if (hops == 0xffff) ++st.unreachable;
    auto [it, inserted] = seen->try_emplace(key, Seen{dist, hops, t1.QuadPart});
    if (!inserted) {
        const double age = static_cast<double>(t1.QuadPart - it->second.when) / static_cast<double>(freq.QuadPart);
        const int bucket = age < 1.0 ? 0 : age < 10.0 ? 1 : 2;
        ++st.rep[bucket];
        if (it->second.dist != dist || it->second.hops != hops) ++st.diff[bucket];
        it->second = Seen{dist, hops, t1.QuadPart};
    }
    if (static_cast<double>(t1.QuadPart - last_report) >= report_s * static_cast<double>(freq.QuadPart))
        report_locked(t1.QuadPart);
    ReleaseSRWLockExclusive(&lock);
    return r;
}

void* shadow_hook(void* country, void* out, void* a, void* b) {
    const std::int64_t e0 = spq_epoch;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    void* r = original(country, out, a, b);
    QueryPerformanceCounter(&t1);
    const std::int64_t dt = t1.QuadPart - t0.QuadPart;
    std::uint32_t ia = id_of(a), ib = id_of(b);
    if (ia > ib) std::swap(ia, ib);
    const Key key{reinterpret_cast<std::uint64_t>(country), (static_cast<std::uint64_t>(ia) << 32) | ib};
    const std::int64_t e1 = spq_epoch;
    AcquireSRWLockExclusive(&lock);
    ++ds.calls;
    ds.ticks += dt;
    if (e0 == e1) {
        auto [it, inserted] = day->try_emplace(key);
        if (!inserted && it->second.epoch == e0) {
            ++ds.hits;
            ds.hit_ticks += dt;
            if (std::memcmp(it->second.out, out, 16) != 0) {
                ++ds.mism;
                if (std::memcmp(it->second.out, out, 10) != 0) ++ds.mism_dh;
            }
        } else {
            it->second.epoch = e0;
            std::memcpy(it->second.out, out, 16);
        }
    }
    if (static_cast<double>(t1.QuadPart - last_report) >= report_s * static_cast<double>(freq.QuadPart)) {
        const double f = static_cast<double>(freq.QuadPart);
        splug::log("day-cache shadow: calls %lld (%.1f ms), same-day repeats %lld (%.1f ms would be saved), "
                   "mismatch %lld (dist/hops %lld), days %lld, keys %zu",
                   ds.calls, 1000.0 * static_cast<double>(ds.ticks) / f, ds.hits,
                   1000.0 * static_cast<double>(ds.hit_ticks) / f, ds.mism, ds.mism_dh, e1 - ds.first_epoch,
                   day->size());
        ds = DayStats{};
        ds.first_epoch = e1;
        last_report = t1.QuadPart;
        if (day->size() > 4000000) day->clear();
    }
    ReleaseSRWLockExclusive(&lock);
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

void install_prefetch(HMODULE module, int lanes) {
    constexpr std::uint64_t kSite = 0x14076bde9;
    constexpr std::uint64_t kResume = 0x14076bdf0;
    constexpr std::uint64_t kGlobalTable = 0x14311bed8;
    // 14076bdcc: mov rcx,[r13]; cmp rcx,[rbp+0xd0]; je ..; mov edi,[rcx+0x59c]; mov [rbp+0xc8],edi;
    // cmp dword [rcx+0x5bc],0; je ..
    constexpr std::uint8_t kBytes[] = {0x49, 0x8b, 0x4d, 0x00, 0x48, 0x3b, 0x8d, 0xd0, 0x00, 0x00, 0x00, 0x0f,
                                       0x84, 0xa4, 0x05, 0x00, 0x00, 0x8b, 0xb9, 0x9c, 0x05, 0x00, 0x00, 0x89,
                                       0xbd, 0xc8, 0x00, 0x00, 0x00, 0x83, 0xb9, 0xbc, 0x05, 0x00, 0x00, 0x00,
                                       0x74, 0x30};
    splug::log("prefetch lanes=%d, spq_pf at rva %llx", lanes,
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&spq_pf) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (lanes <= 0) return;
    if (!splug::bytes_match(0x14076bdcc, kBytes)) {
        splug::log("prefetch REFUSED: bytes differ; not changed");
        return;
    }
    spq_pf = lanes;
    spq_gtab = reinterpret_cast<std::uint64_t>(splug::rva(kGlobalTable));
    spq_pf_resume = reinterpret_cast<std::uint64_t>(splug::rva(kResume));
    std::uint8_t* site = splug::rva(kSite);
    std::uint8_t* tramp = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!tramp) {
        splug::log("prefetch REFUSED: no memory near site");
        return;
    }
    std::uint8_t code[14] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&spq_pf_stub);
    std::memcpy(code + 6, &target, 8);
    std::memcpy(tramp, code, sizeof(code));
    DWORD old = 0;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, sizeof(code));
    const auto disp = reinterpret_cast<std::intptr_t>(tramp) - reinterpret_cast<std::intptr_t>(site + 5);
    if (disp < INT32_MIN || disp > INT32_MAX) {
        splug::log("prefetch REFUSED: trampoline out of rel32 range");
        return;
    }
    const auto rel = static_cast<std::int32_t>(disp);
    std::uint8_t patch[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    std::memcpy(patch + 1, &rel, 4);
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("prefetch REFUSED: cannot write jump");
        return;
    }
    splug::log("prefetch installed at %llx", static_cast<unsigned long long>(kSite));
}

// --- пары пути -> кэш (1407770e0, цикл 140777389..1407776b8) ---
// Игра для каждой пары вершин найденного пути берёт разделяемую блокировку [rbp+0x110], ищет ключ
// (min id, max id) в таблице [rbp+0x108] (FNV-1a по 8 байтам, Robin Hood, шаг 0x20), отпускает;
// не нашла — эксклюзивная блокировка и вставка 14077bd80 со значением {dist B - dist A, hops}.
// Здесь: та же работа в том же порядке, но под одной эксклюзивной блокировкой на весь путь
// (как если бы игру никто не прерывал между парами) и с предвыборкой корзин на 4 пары вперёд.
using InsertFn = void* (*)(void* table, void* out, int hash, const std::uint64_t* key, const void* value);
InsertFn insert_fn = nullptr;

struct PathNode {
    const std::uint8_t* system;
    const PathNode* parent;
    std::int64_t dist;
};

inline std::uint32_t fnv8(std::uint64_t k) {
    std::uint32_t h = 0x811c9dc5u;
    for (int b = 0; b < 8; ++b) {
        h ^= static_cast<std::uint8_t>(k >> (8 * b));
        h *= 0x1000193u;
    }
    return h;
}

inline std::uint64_t pair_key(const PathNode* a, const PathNode* b) {
    std::uint32_t ia, ib;
    std::memcpy(&ia, a->system + 8, 4);
    std::memcpy(&ib, b->system + 8, 4);
    const std::uint32_t lo = ia < ib ? ia : ib;
    const std::uint32_t hi = lo == ia ? ib : ia;
    return (static_cast<std::uint64_t>(lo) << 32) | hi;
}

inline const std::uint8_t* bucket_of(const std::uint8_t* table, std::uint32_t h) {
    std::int32_t mask;
    const std::uint8_t* base;
    std::memcpy(&mask, table + 0x14, 4);
    std::memcpy(&base, table + 8, 8);
    const std::int32_t idx = static_cast<std::int32_t>(h & static_cast<std::uint32_t>(mask));
    return base + static_cast<std::int64_t>(idx) * 32;
}

bool table_has(const std::uint8_t* e, std::uint64_t key) {
    std::uint8_t d = 1;
    if (e[4] < d) return false;
    for (;;) {
        std::uint64_t k;
        std::memcpy(&k, e + 8, 8);
        if (k == key) return true;
        ++d;
        e += 0x20;
        if (!(d <= e[4])) return false;
    }
}

void lock_exclusive(volatile long* lock) {
    while (_interlockedbittestandset(lock, 0)) YieldProcessor();
    while (static_cast<unsigned long>(*lock) > 1u) YieldProcessor();
}

void unlock_exclusive(volatile long* lock) { _InterlockedAnd(lock, ~1L); }

// разделяемая блокировка игры (как в 140766810): +2; занята писателем (бит 0) — откат и ожидание.
void lock_shared(volatile long* lock) {
    for (;;) {
        if ((_InterlockedExchangeAdd(lock, 2) & 1) == 0) return;
        _InterlockedExchangeAdd(lock, -2);
        while (*lock & 1) YieldProcessor();
    }
}

void unlock_shared(volatile long* lock) { _InterlockedExchangeAdd(lock, -2); }

struct Value16 {
    std::int64_t dist;
    std::uint16_t hops;
    std::uint8_t pad[6];
};

void insert_pair(std::uint8_t* table, const PathNode* a, const PathNode* b, std::int32_t hops, std::uint64_t key,
                 std::uint32_t h) {
    Value16 value{b->dist - a->dist, static_cast<std::uint16_t>(hops), {}};
    struct {
        std::uint8_t* entry;
        std::uint8_t existed_flag;  // 1 — вставлено сейчас
        std::uint8_t pad[7];
    } res{};
    insert_fn(table, &res, static_cast<int>(h), &key, &value);
    if (res.existed_flag == 0) std::memcpy(res.entry + 0x10, &value, 16);
}

thread_local std::uint32_t* missing = nullptr;
thread_local std::size_t missing_cap = 0;

}  // namespace

volatile LONG64 bp_paths = 0, bp_pairs = 0, bp_found = 0, bp_last = 0;

extern "C" void spq_pairs(std::uint8_t* frame) {
    const PathNode* const* arr;
    std::int32_t n;
    std::uint8_t* table;
    volatile long* lock;
    std::memcpy(&arr, frame + 8, 8);
    std::memcpy(&n, frame + 0x14, 4);
    std::memcpy(&table, frame + 0x108, 8);
    std::memcpy(&lock, frame + 0x110, 8);
    if (n <= 1) return;
    long long found = 0, pairs = 0;
    if (spq_batch == 2) {
        // фаза 1: поиск под разделяемой блокировкой кусками по 64 пары (читатели не ждут), список
        // недостающих в порядке игры; фаза 2: эксклюзивно только вставка, с повторной проверкой.
        const std::size_t total = static_cast<std::size_t>(n) * static_cast<std::size_t>(n - 1) / 2;
        if (missing_cap < total) {
            std::free(missing);
            missing_cap = total < 4096 ? 4096 : total;
            missing = static_cast<std::uint32_t*>(std::malloc(missing_cap * sizeof(std::uint32_t)));
            if (!missing) {
                missing_cap = 0;
                spq_batch = 1;  // нет памяти — старый проход
            }
        }
        if (missing) {
            std::size_t nm = 0;
            int in_chunk = 0;
            lock_shared(lock);
            for (std::int32_t i = n - 1; i > 0; --i) {
                const PathNode* a = arr[i];
                for (std::int32_t j = i - 1; j >= 0; --j) {
                    if (j >= 4)
                        _mm_prefetch(reinterpret_cast<const char*>(bucket_of(table, fnv8(pair_key(a, arr[j - 4])))),
                                     _MM_HINT_T0);
                    const std::uint64_t key = pair_key(a, arr[j]);
                    ++pairs;
                    if (table_has(bucket_of(table, fnv8(key)), key))
                        ++found;
                    else
                        missing[nm++] = (static_cast<std::uint32_t>(i) << 16) | static_cast<std::uint32_t>(j);
                    if (++in_chunk == 64) {
                        in_chunk = 0;
                        unlock_shared(lock);
                        lock_shared(lock);
                    }
                }
            }
            unlock_shared(lock);
            if (nm) {
                lock_exclusive(lock);
                for (std::size_t k = 0; k < nm; ++k) {
                    const std::int32_t i = static_cast<std::int32_t>(missing[k] >> 16);
                    const std::int32_t j = static_cast<std::int32_t>(missing[k] & 0xffff);
                    const std::uint64_t key = pair_key(arr[i], arr[j]);
                    const std::uint32_t h = fnv8(key);
                    if (table_has(bucket_of(table, h), key)) {
                        ++found;
                        continue;
                    }
                    insert_pair(table, arr[i], arr[j], i - j, key, h);
                }
                unlock_exclusive(lock);
            }
            goto report;
        }
    }
    lock_exclusive(lock);
    for (std::int32_t i = n - 1; i > 0; --i) {
        const PathNode* a = arr[i];
        for (std::int32_t j = i - 1; j >= 0; --j) {
            if (j >= 4) _mm_prefetch(reinterpret_cast<const char*>(bucket_of(table, fnv8(pair_key(a, arr[j - 4])))),
                                     _MM_HINT_T0);
            const PathNode* b = arr[j];
            const std::uint64_t key = pair_key(a, b);
            const std::uint32_t h = fnv8(key);
            ++pairs;
            if (table_has(bucket_of(table, h), key)) {
                ++found;
                continue;
            }
            insert_pair(table, a, b, i - j, key, h);
        }
    }
    unlock_exclusive(lock);
report:
    InterlockedIncrement64(&bp_paths);
    InterlockedAdd64(&bp_pairs, pairs);
    InterlockedAdd64(&bp_found, found);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONG64 last = bp_last;
    if (static_cast<double>(now.QuadPart - last) >= report_s * static_cast<double>(freq.QuadPart) &&
        InterlockedCompareExchange64(&bp_last, now.QuadPart, last) == last) {
        const LONG64 p = InterlockedExchange64(&bp_paths, 0), q = InterlockedExchange64(&bp_pairs, 0),
                     f = InterlockedExchange64(&bp_found, 0);
        splug::log("batch: paths %lld, pairs %lld, already cached %lld (%.1f%%), inserted %lld", p, q, f,
                   q ? 100.0 * f / q : 0.0, q - f);
    }
}

namespace {

void install_batch(HMODULE module, int on) {
    constexpr std::uint64_t kSite = 0x140777389;
    constexpr std::uint8_t kBytes[] = {0x44, 0x8b, 0x65, 0x14, 0x41, 0xff, 0xcc, 0x41, 0x83, 0xfc, 0xff, 0x4c,
                                       0x8b, 0xad, 0xe0, 0x00, 0x00, 0x00, 0x0f, 0x8e, 0x17, 0x03, 0x00, 0x00};
    splug::log("batch=%d, spq_batch at rva %llx", on,
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&spq_batch) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (on < 0) return;
    if (!splug::bytes_match(kSite, kBytes)) {
        splug::log("batch REFUSED: bytes differ; not changed");
        return;
    }
    insert_fn = reinterpret_cast<InsertFn>(splug::rva(0x14077bd80));
    spq_pairs_resume = reinterpret_cast<std::uint64_t>(splug::rva(0x1407776b8));
    spq_pairs_orig = reinterpret_cast<std::uint64_t>(splug::rva(0x14077739b));
    spq_batch = on;
    if (!splug::write_jump(splug::rva(kSite), reinterpret_cast<const void*>(&spq_pairs_stub))) {
        splug::log("batch REFUSED: cannot write jump");
        return;
    }
    splug::log("batch hook installed at %llx", static_cast<unsigned long long>(kSite));
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_pathq_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_string(L"pathq", L"mode", L"off");
    report_s = splug::ini_int(L"pathq", L"report_s", 60);
    splug::log("stellaris_pathq_patch 0.4.0: build %s, mode=%ls", splug::state().build, mode.c_str());
    install_prefetch(module, splug::ini_int(L"pathq", L"prefetch", 0));
    install_batch(module, splug::ini_int(L"pathq", L"batch", -1));
    if (mode != L"probe" && mode != L"shadow") return;
    const bool shadow = mode == L"shadow";
    if (!splug::bytes_match(kQuery, kPrologue) || (shadow && !splug::bytes_match(kDay, kDayPrologue))) {
        splug::log("REFUSED: prologue bytes differ; no bytes changed");
        return;
    }
    // переходник: исходные 15 байт пролога + jmp [rip+0] на 14076681f
    auto* tramp = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                                          PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        splug::log("REFUSED: no memory");
        return;
    }
    std::memcpy(tramp, kPrologue, sizeof(kPrologue));
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(tramp + 15, jmp, 6);
    const auto body = reinterpret_cast<std::uint64_t>(splug::rva(kQueryBody));
    std::memcpy(tramp + 21, &body, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, 29);
    original = reinterpret_cast<QueryFn>(tramp);
    seen = new std::unordered_map<Key, Seen, KeyHash>();
    seen->reserve(1 << 20);
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&last_report));
    if (shadow) {
        day = new std::unordered_map<Key, Cached, KeyHash>();
        day->reserve(1 << 20);
        auto* dt = make_trampoline(kDayPrologue, sizeof(kDayPrologue),
                                   reinterpret_cast<std::uint64_t>(splug::rva(kDayBody)));
        if (!dt) {
            splug::log("REFUSED: no memory");
            return;
        }
        spq_day_tramp = reinterpret_cast<std::uint64_t>(dt);
        if (!splug::write_jump(splug::rva(kDay), reinterpret_cast<const void*>(&spq_day_stub))) {
            splug::log("REFUSED: cannot write day jump");
            return;
        }
    }
    if (!splug::write_jump(splug::rva(kQuery),
                           shadow ? reinterpret_cast<const void*>(&shadow_hook) : reinterpret_cast<const void*>(&hook))) {
        splug::log("REFUSED: cannot write jump");
        return;
    }
    splug::log("%s installed at %llx", shadow ? "day-cache shadow" : "probe", static_cast<unsigned long long>(kQuery));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
