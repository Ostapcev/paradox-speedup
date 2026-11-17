// Stellaris Dijk Patch 0.2.0, Stellaris 4.5.4 x64 — точная замена поиска пути 14076b970.
//
// Что в игре (stellaris.exe 4.5.4.1b4f): 140766810 (путевой запрос, вызов 1407669c1) →
// 14076b970(out, start, target, a4, a5, a6, a7) — Дейкстра по системам:
//   * на стеке: A[N] {state 0/1/2, индекс в куче}, B[N] {sys, parent, dist, flag} (32 байта),
//     двоичная куча {int64 dist, int16 idx}; N = [[14311b708]+0x59c]; всё обнуляется на каждый вызов;
//   * извлечение: swap(h[0], h[last]), просеивание вниз (при равенстве детей — правый);
//     если B[u].sys == target → 1407770e0(out, &B[u], —, a4, —, a6, a7) (сборка пути);
//   * рёбра: гиперлинии [sys+0x590] (шаг 32, id +8), затем, если [sys+0x5bc] != 0, доп. связи
//     (140afef50(sys) пишет их в TLS: число +0x384, массив id +0x378; объект связи → id системы +0x78);
//   * фильтр 140777750(&B[u], sys v, ребро, a5): 1 — пропустить, 2 — закрыть v;
//     цена ребра 14077dd30(&tmp, sys u, —, ребро);
//   * при равном расстоянии берётся родитель с меньшим числом переходов (игра считает его обходом
//     цепочки родителей на каждой ничьей).
// Замена делает то же самое в том же порядке, но: глубина узла хранится (у узла в куче ещё нет
// потомков, у закрытого родитель не меняется — значит, это та же величина), куча просеивается
// «дыркой» (те же сравнения), вместо обнуления массивов — штампы поколений, массивы на поток.
// [dijk] mode=off / on / shadow (свой поиск «вхолостую» + игра, сравнение всей цепочки пути).
// abab=1 — режим 0/1 по минутам (замер). dkp_mode (rva в логе): 0 — игра, 1 — замена, 2 — тень,
// 3 — замер: на каждом потоке вызовы по очереди игра / замена.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t dkp_mode = 0;
std::int64_t dkp_resume = 0;  // мкс; 0 — не продолжать обход
void* dkp_search(void* out, void* start, void* target, void* a4, void* a5, void* a6, void* a7);
void* dkp_assemble_hook(void* out, void* entry, std::uint64_t r8, void* a4, std::uint64_t a5, void* a6, void* a7);
}

namespace {

constexpr std::uint64_t kCall = 0x1407669c1;
constexpr std::uint8_t kCallBytes[5] = {0xe8, 0xaa, 0x4f, 0x00, 0x00};
constexpr std::uint64_t kAsmCall = 0x14076c3ad;  // в 14076b970: call 1407770e0
constexpr std::uint8_t kAsmCallBytes[5] = {0xe8, 0x2e, 0xad, 0x00, 0x00};

struct Entry {
    std::uint8_t* sys;
    Entry* parent;
    std::int64_t dist;
    std::int32_t flag;
    std::int32_t pad;
};
static_assert(sizeof(Entry) == 32, "entry");
struct HeapEl {
    std::int64_t key;
    std::int32_t idx;
    std::int32_t pad;
};
struct Node {
    std::uint32_t stamp;
    std::uint8_t state;
    std::uint8_t popped;
    std::int16_t pos;
    std::int32_t depth;
};

using DijkFn = void* (*)(void*, void*, void*, void*, void*, void*, void*);
using AssembleFn = void* (*)(void*, void*, std::uint64_t, void*, std::uint64_t, void*, void*);
using FilterFn = int (*)(Entry*, std::uint8_t*, std::int64_t, void*);
using CostFn = std::int64_t* (*)(void*, std::uint8_t*, std::uint64_t, std::int64_t);
using ExtraFn = void (*)(std::uint8_t*);
using TlsInitFn = void (*)();

DijkFn game_dijk = nullptr;
AssembleFn assemble = nullptr;
FilterFn filter = nullptr;
CostFn cost = nullptr;
ExtraFn extra = nullptr;
TlsInitFn tls_init = nullptr;
std::uint8_t** g_state = nullptr;     // 14311b708
std::uint8_t** g_systab = nullptr;    // 14311bed8
std::uint8_t** g_sysdef = nullptr;    // 14311b6f0
std::uint8_t** g_extratab = nullptr;  // 14311bdb8
std::uint8_t** g_extradef = nullptr;  // 14311ad70

// Состояние обхода на поток. Обход можно продолжить для другой цели: порядок извлечения от цели не
// зависит (цель проверяется только при извлечении), поэтому новый поиск из того же старта — префикс
// того же обхода. Последний извлечённый узел (на котором остановились) ещё не раскрыт — раскрыть
// его первым, как сделала бы игра.
struct Work {
    Entry* B = nullptr;
    Node* A = nullptr;
    HeapEl* heap = nullptr;
    int cap = 0;
    std::uint32_t stamp = 0;
    // сохранённый обход
    bool live = false;
    int N = 0;
    int size = 0;
    int pending = -1;  // извлечён, не раскрыт
    bool exhausted = false;
    void* start = nullptr;
    std::uint64_t ctx = 0, a6 = 0, a7 = 0;
    unsigned long long tsc = 0;
};
thread_local Work* t_work = nullptr;
thread_local int t_depth = 0;
thread_local unsigned t_alt = 0;
// повторы: тот же старт и контекст, что у прошлого вызова на этом потоке
thread_local void* t_last_start = nullptr;
thread_local std::uint64_t t_last_ctx = 0, t_last_a6 = 0, t_last_a7 = 0;
thread_local unsigned long long t_last_tsc = 0;
std::atomic<long long> rp_same{0}, rp_same_1ms{0}, rp_same_all{0}, rp_same_target{0};
thread_local void* t_last_target = nullptr;
std::atomic<long long> rs_resumed{0}, rs_closed_hit{0}, rs_fresh{0}, rs_pops{0};
std::int64_t resume_limit = 0;  // тактов TSC; 0 — не продолжать

// тень: цепочка пути
struct Step {
    void* sys;
    std::int64_t dist;
    std::int32_t flag;
};
constexpr int kMaxChain = 4096;
struct Chain {
    int n;
    bool found;
    bool overflow;
    Step s[kMaxChain];
};
thread_local Chain* t_mine = nullptr;
thread_local Chain* t_game = nullptr;
thread_local bool t_capture = false;

void record(Chain& c, const Entry* e) {
    c.found = true;
    c.n = 0;
    c.overflow = false;
    for (; e; e = e->parent) {
        if (c.n == kMaxChain) {
            c.overflow = true;
            return;
        }
        c.s[c.n++] = Step{e->sys, e->dist, e->flag};
    }
}

std::atomic<long long> st_calls[3], st_ticks[3], st_fallback{0}, st_shadow{0}, st_mismatch{0}, st_found{0};
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};
long long window = 0;
bool abab = false;
std::int64_t tsc_per_us = 3000;

inline std::uint8_t* tls_block() {
    auto** arr = reinterpret_cast<std::uint8_t**>(__readgsqword(0x58));
    return arr[0];
}

inline std::uint8_t* lookup(std::uint8_t* tab, std::uint32_t id, std::uint8_t* def) {
    if (tab) {
        const std::uint32_t idx = id & 0xffffffu;
        if (idx < *reinterpret_cast<std::uint32_t*>(tab + 0x20)) {
            std::uint8_t* obj =
                *reinterpret_cast<std::uint8_t**>(*reinterpret_cast<std::uint8_t**>(tab + 0x18) + idx * 16ull + 8);
            if (obj && *reinterpret_cast<std::uint32_t*>(obj + 8) == id) return obj;
        }
    }
    return def;
}

Work* get_work(int n) {
    Work* w = t_work;
    if (!w) {
        w = new Work();
        t_work = w;
    }
    if (w->cap < n) {
        delete[] w->B;
        delete[] w->A;
        delete[] w->heap;
        const int cap = n + 64;
        w->B = new Entry[cap];
        w->A = new Node[cap];
        w->heap = new HeapEl[cap];
        std::memset(w->A, 0, sizeof(Node) * cap);
        w->cap = cap;
        w->stamp = 0;
        w->live = false;
    }
    return w;
}

void fresh(Work* w, std::uint8_t* start, int N) {
    if (++w->stamp == 0) {
        std::memset(w->A, 0, sizeof(Node) * w->cap);
        w->stamp = 1;
    }
    w->live = true;
    w->N = N;
    w->size = 0;
    w->pending = -1;
    w->exhausted = false;
    const std::uint32_t s = *reinterpret_cast<std::uint32_t*>(start + 8) & 0xffffffu;
    if (static_cast<std::int32_t>(s) >= N) {
        w->exhausted = true;
        return;
    }
    const int si = static_cast<std::int16_t>(s);
    Node& n = w->A[si];
    n.stamp = w->stamp;
    n.state = 0;
    n.popped = 0;
    n.pos = 0;
    n.depth = 0;
    w->B[si] = Entry{start, nullptr, 0, 0, 0};
    w->heap[0] = HeapEl{0, si, 0};
    w->size = 1;
}

// Раскрыть узел u (рёбра, как в игре).
void expand(Work* w, int u, void* a5) {
    Entry* const B = w->B;
    Node* const A = w->A;
    HeapEl* const h = w->heap;
    const std::uint32_t st = w->stamp;
    const int N = w->N;
    std::int64_t tmp[4];
    Entry* const eu = &B[u];
    std::uint8_t* const usys = eu->sys;
    int total = *reinterpret_cast<std::int32_t*>(usys + 0x59c);
    std::uint8_t* tls = nullptr;
    if (*reinterpret_cast<std::int32_t*>(usys + 0x5bc) != 0) {
        extra(usys);
        tls = tls_block();
        if (tls[0x10] == 0) tls_init();
        total += *reinterpret_cast<std::int32_t*>(tls + 0x370 + 0x14);
    }
    const int du = A[u].depth;
    for (int e = 0; e < total; ++e) {
        const int lanes = *reinterpret_cast<std::int32_t*>(usys + 0x59c);
        std::uint32_t id;
        if (e < lanes) {
            id = *reinterpret_cast<std::uint32_t*>(*reinterpret_cast<std::uint8_t**>(usys + 0x590) +
                                                   static_cast<std::int64_t>(e) * 32 + 8);
        } else {
            tls = tls_block();
            if (tls[0x10] == 0) tls_init();
            std::uint8_t* obj = *g_extradef;
            std::uint8_t* tab = *g_extratab;
            if (tab) {
                const std::uint32_t xid =
                    reinterpret_cast<std::uint32_t*>(*reinterpret_cast<std::uint8_t**>(tls + 0x378))[e - lanes];
                obj = lookup(tab, xid, *g_extradef);
            }
            id = *reinterpret_cast<std::uint32_t*>(obj + 0x78);
        }
        const int v = static_cast<std::int16_t>(id);
        if (v < 0 || v >= N) continue;
        Node& nv = A[v];
        const int sv = nv.stamp == st ? nv.state : 0;
        if (sv == 2) continue;
        std::uint8_t* const vsys = lookup(*g_systab, id, *g_sysdef);
        const int r = filter(eu, vsys, e, a5);
        if (r == 1) continue;
        if (r == 2) {
            if (nv.stamp != st) {
                nv.stamp = st;
                nv.popped = 0;
                nv.pos = -1;
            }
            nv.state = 2;
            continue;
        }
        Entry* const ev = &B[v];
        int i;
        if (sv == 1) {
            const std::int64_t nd = eu->dist + *cost(tmp, usys, 0, e);
            if (!(nd < ev->dist || (nd == ev->dist && du + 1 < nv.depth))) continue;
            ev->parent = eu;
            ev->dist = nd;
            ev->flag = e >= *reinterpret_cast<std::int32_t*>(usys + 0x59c) ? 1 : 0;
            nv.depth = du + 1;
            i = nv.pos;
        } else {
            ev->sys = vsys;
            ev->parent = eu;
            ev->flag = e >= *reinterpret_cast<std::int32_t*>(usys + 0x59c) ? 1 : 0;
            ev->dist = eu->dist + *cost(tmp, usys, 0, e);
            nv.stamp = st;
            nv.state = 1;
            nv.popped = 0;
            nv.depth = du + 1;
            i = w->size++;
        }
        // просеивание вверх (как в игре: дальше, пока key < key родителя)
        const HeapEl x{ev->dist, v, 0};
        while (i > 0) {
            const int p = (i - 1) >> 1;
            if (!(x.key < h[p].key)) break;
            h[i] = h[p];
            A[h[i].idx].pos = static_cast<std::int16_t>(i);
            i = p;
        }
        h[i] = x;
        nv.pos = static_cast<std::int16_t>(i);
    }
}

// Извлечь минимум (как в игре: swap с последним, просеивание вниз в [0, last-1]).
int pop(Work* w) {
    HeapEl* const h = w->heap;
    Node* const A = w->A;
    const int u = h[0].idx;
    const int last = w->size - 1;
    w->size = last;
    if (last > 0) {
        const HeapEl x = h[last];
        int i = 0;
        const int lim = last - 1;
        for (;;) {
            const int L = 2 * i + 1, R = L + 1;
            int c;
            if (R <= lim)
                c = h[R].key <= h[L].key ? R : L;
            else if (L <= lim)
                c = L;
            else
                break;
            if (!(h[c].key < x.key)) break;
            h[i] = h[c];
            A[h[i].idx].pos = static_cast<std::int16_t>(i);
            i = c;
        }
        h[i] = x;
        A[x.idx].pos = static_cast<std::int16_t>(i);
    }
    A[u].state = 2;
    A[u].popped = 1;
    return u;
}

void unreachable_out(void* out) {
    auto* o = static_cast<std::uint8_t*>(out);
    std::memset(o, 0, 16);
    const std::int64_t unreachable = -100000;
    std::memcpy(o, &unreachable, 8);
    const std::uint16_t ff = 0xffff;
    std::memcpy(o + 8, &ff, 2);
}

// Ответ найден в узле u.
void* finish(void* out, Entry* eu, void* a4, void* a6, void* a7, bool dry) {
    if (dry) {
        record(*t_mine, eu);
        return out;
    }
    st_found.fetch_add(1, std::memory_order_relaxed);
    assemble(out, eu, 0, a4, 0, a6, a7);
    return out;
}

// Возвращает nullptr, если продолжить нельзя и нужен вызов игры (редкий случай).
void* search(void* out, std::uint8_t* start, std::uint8_t* target, void* a4, void* a5, void* a6, void* a7,
             bool dry, int N) {
    Work* w = get_work(N);
    const std::uint64_t ctx = a5 ? *static_cast<std::uint64_t*>(a5) : 0;
    const unsigned long long now = __rdtsc();
    const bool can_resume = resume_limit > 0 && w->live && w->N == N && w->start == start && w->ctx == ctx &&
                            w->a6 == reinterpret_cast<std::uint64_t>(a6) &&
                            w->a7 == reinterpret_cast<std::uint64_t>(a7) &&
                            static_cast<std::int64_t>(now - w->tsc) < resume_limit;
    if (can_resume) {
        rs_resumed.fetch_add(1, std::memory_order_relaxed);
        // цель уже извлечена?
        const std::uint32_t t = *reinterpret_cast<std::uint32_t*>(target + 8) & 0xffffffu;
        if (static_cast<std::int32_t>(t) < N) {
            const int ti = static_cast<std::int16_t>(t);
            const Node& nt = w->A[ti];
            if (ti >= 0 && nt.stamp == w->stamp && nt.popped && w->B[ti].sys == target) {
                rs_closed_hit.fetch_add(1, std::memory_order_relaxed);
                w->tsc = __rdtsc();
                return finish(out, &w->B[ti], a4, a6, a7, dry);
            }
        }
        if (w->pending >= 0) {
            const int p = w->pending;
            w->pending = -1;
            expand(w, p, a5);
        }
    } else {
        rs_fresh.fetch_add(1, std::memory_order_relaxed);
        fresh(w, start, N);
        w->start = start;
        w->ctx = ctx;
        w->a6 = reinterpret_cast<std::uint64_t>(a6);
        w->a7 = reinterpret_cast<std::uint64_t>(a7);
    }
    long long pops = 0;
    while (!w->exhausted && w->size > 0) {
        const int u = pop(w);
        ++pops;
        if (w->B[u].sys == target) {
            w->pending = u;
            w->tsc = __rdtsc();
            rs_pops.fetch_add(pops, std::memory_order_relaxed);
            return finish(out, &w->B[u], a4, a6, a7, dry);
        }
        expand(w, u, a5);
    }
    w->exhausted = true;
    w->tsc = __rdtsc();
    rs_pops.fetch_add(pops, std::memory_order_relaxed);
    if (dry) {
        t_mine->found = false;
        t_mine->n = 0;
        return out;
    }
    unreachable_out(out);
    return out;
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load(std::memory_order_relaxed);
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    if (q.QuadPart - last < 60 * freq.QuadPart) return;
    if (!rep_qpc.compare_exchange_strong(last, q.QuadPart)) return;
    const double sec = static_cast<double>(q.QuadPart - last) / freq.QuadPart;
    long long c[3], t[3];
    for (int m = 0; m < 3; ++m) {
        c[m] = st_calls[m].exchange(0);
        t[m] = st_ticks[m].exchange(0);
    }
    splug::log("window %lld (%.0f s), mode %lld: calls game %lld (avg %.1f kcyc), fast %lld (avg %.1f kcyc), "
               "shadow %lld; found %lld; fallback %lld; shadow compared %lld, mismatches %lld",
               window, sec, static_cast<long long>(dkp_mode), c[0], c[0] ? t[0] / 1000.0 / c[0] : 0.0, c[1],
               c[1] ? t[1] / 1000.0 / c[1] : 0.0, c[2], st_found.exchange(0), st_fallback.exchange(0),
               st_shadow.exchange(0), st_mismatch.load());
    splug::log("  resume %lld us: fresh %lld, resumed %lld (answered from closed %lld), pops %lld",
               static_cast<long long>(dkp_resume), rs_fresh.exchange(0), rs_resumed.exchange(0),
               rs_closed_hit.exchange(0), rs_pops.exchange(0));
    splug::log("  repeats on thread (same start+ctx as previous call): %lld, within ~1 ms %lld, same a6/a7 %lld, same target %lld",
               rp_same.exchange(0), rp_same_1ms.exchange(0), rp_same_all.exchange(0), rp_same_target.exchange(0));
    ++window;
    if (abab) dkp_mode = window & 1;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_dijk_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"dijk", L"mode", L"off");
    abab = splug::ini_int(L"dijk", L"abab", 0) != 0;
    dkp_resume = splug::ini_int(L"dijk", L"resume_us", 1000);
    {
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        const unsigned long long c0 = __rdtsc();
        Sleep(30);
        QueryPerformanceCounter(&q1);
        const unsigned long long c1 = __rdtsc();
        const double us = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1e6 / freq.QuadPart;
        if (us > 0) tsc_per_us = static_cast<std::int64_t>(static_cast<double>(c1 - c0) / us);
    }
    splug::log("stellaris_dijk_patch 0.2.0: build %s, mode=%s, abab=%d, resume_us=%lld (tsc %lld/us), dkp_mode at rva %llx, dkp_resume at rva %llx",
               splug::state().build, splug::mode_name(mode), abab ? 1 : 0, static_cast<long long>(dkp_resume),
               static_cast<long long>(tsc_per_us),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&dkp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&dkp_resume) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    game_dijk = reinterpret_cast<DijkFn>(splug::call_target(splug::rva(kCall)));
    if (reinterpret_cast<std::uintptr_t>(game_dijk) != reinterpret_cast<std::uintptr_t>(splug::rva(0x14076b970))) {
        splug::log("REFUSED: call target differs");
        return;
    }
    assemble = reinterpret_cast<AssembleFn>(splug::call_target(splug::rva(kAsmCall)));
    filter = reinterpret_cast<FilterFn>(splug::rva(0x140777750));
    cost = reinterpret_cast<CostFn>(splug::rva(0x14077dd30));
    extra = reinterpret_cast<ExtraFn>(splug::rva(0x140afef50));
    tls_init = reinterpret_cast<TlsInitFn>(splug::rva(0x142028b08));
    g_state = splug::rva<std::uint8_t*>(0x14311b708);
    g_systab = splug::rva<std::uint8_t*>(0x14311bed8);
    g_sysdef = splug::rva<std::uint8_t*>(0x14311b6f0);
    g_extratab = splug::rva<std::uint8_t*>(0x14311bdb8);
    g_extradef = splug::rva<std::uint8_t*>(0x14311ad70);
    // байты, на которых построена замена (адреса таблиц, вызовы, константы)
    constexpr std::uint8_t kChk1[] = {0x48, 0x8b, 0x05, 0x63, 0xfd, 0x9a, 0x02, 0x4c, 0x63, 0x80, 0x9c, 0x05};  // 14076b99e
    constexpr std::uint8_t kChk2[] = {0x48, 0x8b, 0x15, 0xd0, 0xff, 0x9a, 0x02};  // 14076bf01 systab
    constexpr std::uint8_t kChk3[] = {0x48, 0x8b, 0x35, 0xb8, 0xf7, 0x9a, 0x02};  // 14076bf31 sysdef
    constexpr std::uint8_t kChk4[] = {0x48, 0x8b, 0x05, 0x10, 0xff, 0x9a, 0x02};  // 14076bea1 extratab
    constexpr std::uint8_t kChk5[] = {0x48, 0x8b, 0x05, 0xa3, 0xee, 0x9a, 0x02, 0x8b, 0x58, 0x78};  // 14076bec6
    constexpr std::uint8_t kChk6[] = {0xe8, 0x03, 0xb8, 0x00, 0x00};  // 14076bf48 call 140777750
    constexpr std::uint8_t kChk7[] = {0xe8, 0xaf, 0x1d, 0x01, 0x00};  // 14076bf7c call 14077dd30
    constexpr std::uint8_t kChk8[] = {0xe8, 0x59, 0x31, 0x39, 0x00};  // 14076bdf2 call 140afef50
    if (!splug::bytes_match(0x14076b99e, kChk1) || !splug::bytes_match(0x14076bf01, kChk2) ||
        !splug::bytes_match(0x14076bf31, kChk3) || !splug::bytes_match(0x14076bea1, kChk4) ||
        !splug::bytes_match(0x14076bec6, kChk5) || !splug::bytes_match(0x14076bf48, kChk6) ||
        !splug::bytes_match(0x14076bf7c, kChk7) || !splug::bytes_match(0x14076bdf2, kChk8)) {
        splug::log("REFUSED: function bytes differ; nothing changed");
        return;
    }
    splug::CallPatch p1, p2;
    const char* e = splug::prepare_call(splug::rva(kCall), kCallBytes, reinterpret_cast<const void*>(&dkp_search), p1);
    if (!e)
        e = splug::prepare_call(splug::rva(kAsmCall), kAsmCallBytes, reinterpret_cast<const void*>(&dkp_assemble_hook),
                                p2);
    if (e) {
        splug::log("REFUSED: %s", e);
        return;
    }
    dkp_mode = mode == splug::Mode::shadow ? 2 : 1;
    if (abab) dkp_mode = 0;
    if (!splug::commit_call(p2) || !splug::commit_call(p1)) {
        splug::log("FAILED while writing calls");
        return;
    }
    splug::log("installed at %llx (assemble hook %llx), mode %lld", static_cast<unsigned long long>(kCall),
               static_cast<unsigned long long>(kAsmCall), static_cast<long long>(dkp_mode));
}

bool same(const Chain& a, const Chain& b) {
    if (a.found != b.found) return false;
    if (!a.found) return true;
    if (a.overflow || b.overflow || a.n != b.n) return false;
    for (int i = 0; i < a.n; ++i)
        if (a.s[i].sys != b.s[i].sys || a.s[i].dist != b.s[i].dist || a.s[i].flag != b.s[i].flag) return false;
    return true;
}

}  // namespace

extern "C" void* dkp_assemble_hook(void* out, void* entry, std::uint64_t r8, void* a4, std::uint64_t a5, void* a6,
                                   void* a7) {
    if (t_capture) record(*t_game, static_cast<Entry*>(entry));
    return assemble(out, entry, r8, a4, a5, a6, a7);
}

extern "C" void* dkp_search(void* out, void* start, void* target, void* a4, void* a5, void* a6, void* a7) {
    resume_limit = dkp_resume * tsc_per_us;
    std::int64_t mode = dkp_mode;
    if (mode == 3) mode = (++t_alt & 1) ? 1 : 0;  // замер: через вызов
    const unsigned long long t0 = __rdtsc();
    void* r;
    int m = static_cast<int>(mode);
    if (mode == 0 || t_depth > 0) {
        if (t_depth > 0) st_fallback.fetch_add(1, std::memory_order_relaxed);
        m = 0;
        r = game_dijk(out, start, target, a4, a5, a6, a7);
    } else {
        const int N = *reinterpret_cast<std::int32_t*>(*g_state + 0x59c);
        if (N <= 0 || N > 32000) {
            st_fallback.fetch_add(1, std::memory_order_relaxed);
            m = 0;
            r = game_dijk(out, start, target, a4, a5, a6, a7);
        } else if (mode == 1) {
            ++t_depth;
            r = search(out, static_cast<std::uint8_t*>(start), static_cast<std::uint8_t*>(target), a4, a5, a6, a7,
                       false, N);
            --t_depth;
        } else {
            if (!t_mine) {
                t_mine = new Chain();
                t_game = new Chain();
            }
            ++t_depth;
            search(out, static_cast<std::uint8_t*>(start), static_cast<std::uint8_t*>(target), a4, a5, a6, a7, true,
                   N);
            --t_depth;
            t_game->found = false;
            t_game->n = 0;
            t_capture = true;
            r = game_dijk(out, start, target, a4, a5, a6, a7);
            t_capture = false;
            st_shadow.fetch_add(1, std::memory_order_relaxed);
            if (!same(*t_mine, *t_game)) {
                const long long k = st_mismatch.fetch_add(1);
                if (k < 10)
                    splug::log("MISMATCH: mine found %d len %d, game found %d len %d (first sys %p/%p dist %lld/%lld)",
                               t_mine->found, t_mine->n, t_game->found, t_game->n,
                               t_mine->n ? t_mine->s[0].sys : nullptr, t_game->n ? t_game->s[0].sys : nullptr,
                               t_mine->n ? static_cast<long long>(t_mine->s[0].dist) : 0LL,
                               t_game->n ? static_cast<long long>(t_game->s[0].dist) : 0LL);
            }
        }
    }
    {
        const std::uint64_t ctx = a5 ? *static_cast<std::uint64_t*>(a5) : 0;
        if (start == t_last_start && ctx == t_last_ctx) {
            rp_same.fetch_add(1, std::memory_order_relaxed);
            if (t0 - t_last_tsc < 4000000ull) rp_same_1ms.fetch_add(1, std::memory_order_relaxed);
            if (reinterpret_cast<std::uint64_t>(a6) == t_last_a6 && reinterpret_cast<std::uint64_t>(a7) == t_last_a7)
                rp_same_all.fetch_add(1, std::memory_order_relaxed);
            if (target == t_last_target) rp_same_target.fetch_add(1, std::memory_order_relaxed);
        }
        t_last_start = start;
        t_last_ctx = ctx;
        t_last_a6 = reinterpret_cast<std::uint64_t>(a6);
        t_last_a7 = reinterpret_cast<std::uint64_t>(a7);
        t_last_target = target;
        t_last_tsc = __rdtsc();
    }
    st_calls[m].fetch_add(1, std::memory_order_relaxed);
    st_ticks[m].fetch_add(static_cast<long long>(__rdtsc() - t0), std::memory_order_relaxed);
    report();
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
