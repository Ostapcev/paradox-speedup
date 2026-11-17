// Stellaris Tail Patch 0.1.0 (пробник), Stellaris 4.5.4 x64 — хвост параллельного цикла 1402c5a40.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   1402ac910 → вызов 1402ac99a → 1402c5a40: параллельный цикл по объектам ([r12]+8, индекс), режим 3
//   (141b569e0): задач не больше числа потоков группы + 1, элементы раздаются по одному (lock xadd
//   в теле задачи 140301710). Каждый элемент — вызовы 1403017e0 (14074fa80) и 1403017e8 (14074f8e0).
//   Главный поток делает свою долю (141b45f40), затем ждёт остальные задачи (1401597b0, спин +
//   SwitchToThread) — ~16 % времени 1402c5a40 на BENCH2446.
//
// Пробник: время всего цикла, начало/конец каждого элемента. В лог раз в минуту:
//   стена, сумма элементов, самый долгий элемент, нижняя граница (max(самый долгий, сумма/потоки)),
//   когда начался самый долгий элемент (доля от стены), тот же ли объект был самым долгим в прошлый раз.
// [tail] mode=off / shadow (пробник).

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <immintrin.h>
#include <cstdint>
#include <cstring>

extern "C" {
std::int64_t tlp_mode = 0;
std::uint64_t tail_run(void* a, void* b, void* c, void* d);
std::uint64_t tail_item_a(void* obj);
std::uint64_t tail_item_b(void* obj);
std::uint64_t graph_run(void* a, void* b);
std::int64_t tlp_wait = 0;
void wait_yield();
}

namespace {

constexpr std::uint64_t kRun = 0x1402ac99a;
constexpr std::uint8_t kRunBytes[5] = {0xe8, 0xa1, 0x90, 0x01, 0x00};
constexpr std::uint64_t kItemA = 0x1403017e0;
constexpr std::uint8_t kItemABytes[5] = {0xe8, 0x9b, 0xe2, 0x44, 0x00};
constexpr std::uint64_t kItemB = 0x1403017e8;
constexpr std::uint8_t kItemBBytes[5] = {0xe8, 0xf3, 0xe0, 0x44, 0x00};

constexpr std::uint64_t kGraph = 0x14029e8fe;
constexpr std::uint8_t kGraphBytes[5] = {0xe8, 0xfd, 0x33, 0x01, 0x00};
constexpr std::uint64_t kYield = 0x14015982a;
constexpr std::uint8_t kYieldBytes[5] = {0xe8, 0x9d, 0xc0, 0xec, 0x01};
std::atomic<DWORD> main_tid{0};
std::atomic<long long> y_main{0}, y_other{0}, y_ticks{0}, y_max{0}, y_spin{0};
const std::uint8_t* const* g_clock = nullptr;
std::int32_t last_day = 0;
long long window = 0;
inline std::int32_t day_now() {
    const std::uint8_t* c = *g_clock;
    std::int32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}
using Fn2 = std::uint64_t (*)(void*, void*);
Fn2 graph_original = nullptr;
std::atomic<long long> g_calls{0}, g_ticks{0}, g_max{0}, g_short{0};
using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);
using Fn1 = std::uint64_t (*)(void*);
Fn4 run_original = nullptr;
Fn1 a_original = nullptr;
Fn1 b_original = nullptr;

struct Item {
    void* obj;
    LONG64 t0, t1;
    DWORD tid;
};
constexpr int kMax = 8192;
Item items[kMax];
std::atomic<int> n_items{0};
std::atomic<int> active{0};
thread_local LONG64 t_start = 0;
thread_local ULONG64 cyc_start = 0, tsc_start = 0;
// по элементам: такты TSC (время по часам) и такты процессора потока (QueryThreadCycleTime)
std::atomic<unsigned long long> w_tsc{0}, w_cyc{0}, m_tsc{0}, m_cyc{0};

LARGE_INTEGER freq{};
bool splug_abab = false;
LONG64 last_report = 0;
void* last_max_obj = nullptr;
// накопители за минуту (в тиках QPC)
long long runs = 0, total_items = 0, overflow = 0, same_max = 0;
double s_wall = 0, s_sum = 0, s_max = 0, s_bound = 0, s_main = 0, s_start_frac = 0, s_threads = 0;

inline LONG64 now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_tail_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"tail", L"mode", L"off");
    splug::log("stellaris_tail_patch 0.1.0: build %s, mode=%s, tlp_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&tlp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    splug_abab = splug::ini_int(L"tail", L"abab", 0) != 0;
    tlp_wait = splug::ini_int(L"tail", L"wait", 0);
    std::uint8_t* s_run = splug::rva(kRun);
    std::uint8_t* s_a = splug::rva(kItemA);
    std::uint8_t* s_b = splug::rva(kItemB);
    run_original = reinterpret_cast<Fn4>(splug::call_target(s_run));
    a_original = reinterpret_cast<Fn1>(splug::call_target(s_a));
    b_original = reinterpret_cast<Fn1>(splug::call_target(s_b));
    graph_original = reinterpret_cast<Fn2>(splug::call_target(splug::rva(kGraph)));
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    splug::CallPatch c1, c2, c3, c4, c5;
    const char* e = splug::prepare_call(s_run, kRunBytes, reinterpret_cast<const void*>(&tail_run), c1);
    if (!e) e = splug::prepare_call(s_a, kItemABytes, reinterpret_cast<const void*>(&tail_item_a), c2);
    if (!e) e = splug::prepare_call(s_b, kItemBBytes, reinterpret_cast<const void*>(&tail_item_b), c3);
    if (!e) e = splug::prepare_call(splug::rva(kGraph), kGraphBytes, reinterpret_cast<const void*>(&graph_run), c4);
    if (!e && tlp_wait >= 0) e = splug::prepare_call(splug::rva(kYield), kYieldBytes, reinterpret_cast<const void*>(&wait_yield), c5);
    if (e) {
        splug::log("REFUSED: %s", e);
        return;
    }
    tlp_mode = 2;
    // элементы раньше цикла: tail_item_* без активного цикла ничего не пишут
    if (!splug::commit_call(c2) || !splug::commit_call(c3) || !splug::commit_call(c1) || !splug::commit_call(c4) ||
        (tlp_wait >= 0 && !splug::commit_call(c5))) {
        splug::log("FAILED while writing calls");
        return;
    }
    splug::log("installed: run %llx, items %llx/%llx", static_cast<unsigned long long>(kRun),
               static_cast<unsigned long long>(kItemA), static_cast<unsigned long long>(kItemB));
}

void analyze(LONG64 r0, LONG64 r1, DWORD main_tid) {
    const int n = n_items.load() < kMax ? n_items.load() : kMax;
    if (n_items.load() > kMax) ++overflow;
    if (n == 0) return;
    double sum = 0, mx = 0, main_busy = 0;
    LONG64 mx_t0 = r0;
    void* mx_obj = nullptr;
    DWORD tids[64];
    int nt = 0;
    for (int i = 0; i < n; ++i) {
        const Item& it = items[i];
        const double d = static_cast<double>(it.t1 - it.t0);
        sum += d;
        if (it.tid == main_tid) main_busy += d;
        if (d > mx) {
            mx = d;
            mx_t0 = it.t0;
            mx_obj = it.obj;
        }
        bool seen = false;
        for (int k = 0; k < nt; ++k) seen |= tids[k] == it.tid;
        if (!seen && nt < 64) tids[nt++] = it.tid;
    }
    const double wall = static_cast<double>(r1 - r0);
    const double threads = 15.0;
    const double bound = mx > sum / threads ? mx : sum / threads;
    ++runs;
    total_items += n;
    s_wall += wall;
    s_sum += sum;
    s_max += mx;
    s_bound += bound;
    s_main += main_busy;
    s_threads += nt;
    if (wall > 0) s_start_frac += static_cast<double>(mx_t0 - r0) / wall;
    if (mx_obj == last_max_obj) ++same_max;
    last_max_obj = mx_obj;
}

void maybe_report() {
    const LONG64 t = now();
    if (last_report == 0) {
        last_report = t;
        return;
    }
    if (t - last_report < 60 * freq.QuadPart || runs == 0) return;
    last_report = t;
    const double ms = 1000.0 / static_cast<double>(freq.QuadPart);
    splug::log("runs %lld, items/run %.1f, threads/run %.1f; per run ms: wall %.3f, sum %.3f, max item %.3f, "
               "bound %.3f, main busy %.3f, main wait ~%.3f; max item starts at %.0f%% of wall; same max object %lld/%lld; overflow %lld",
               runs, static_cast<double>(total_items) / runs, s_threads / runs, s_wall * ms / runs, s_sum * ms / runs,
               s_max * ms / runs, s_bound * ms / runs, s_main * ms / runs, (s_wall - s_main) * ms / runs,
               100.0 * s_start_frac / runs, same_max, runs, overflow);
    {
        const double wt = static_cast<double>(w_tsc.exchange(0)), wc = static_cast<double>(w_cyc.exchange(0));
        const double mt = static_cast<double>(m_tsc.exchange(0)), mc = static_cast<double>(m_cyc.exchange(0));
        splug::log("items on CPU: workers %.1f%% (cycles %.3g / tsc %.3g), main %.1f%% (cycles %.3g / tsc %.3g)",
                   wt > 0 ? 100.0 * wc / wt : 0.0, wc, wt, mt > 0 ? 100.0 * mc / mt : 0.0, mc, mt);
    }
    const long long gc = g_calls.exchange(0), gt = g_ticks.exchange(0), gm = g_max.exchange(0), gs = g_short.exchange(0);
    splug::log("graph 1402b1d00: calls %lld, total %.1f ms, avg %.1f us, max %.2f ms, under 100 us %lld", gc, gt * ms,
               gc ? gt * ms * 1000.0 / gc : 0.0, gm * ms, gs);
    const long long ym = y_main.exchange(0), yo = y_other.exchange(0), yt = y_ticks.exchange(0), yx = y_max.exchange(0),
                    ys = y_spin.exchange(0);
    const std::int32_t d = day_now();
    splug::log("window %lld wait mode %lld: days %d; main yields %lld (%.1f ms, max %.3f ms), main spins %lld, other-thread yields %lld",
               window, static_cast<long long>(tlp_wait), last_day ? d - last_day : 0, ym, yt * ms, yx * ms, ys, yo);
    last_day = d;
    ++window;
    if (splug_abab) tlp_wait = window & 1;
    runs = total_items = overflow = same_max = 0;
    s_wall = s_sum = s_max = s_bound = s_main = s_start_frac = s_threads = 0;
}

}  // namespace

extern "C" std::uint64_t tail_item_a(void* obj) {
    if (active.load(std::memory_order_relaxed)) {
        t_start = now();
        QueryThreadCycleTime(GetCurrentThread(), &cyc_start);
        tsc_start = __rdtsc();
    }
    return a_original(obj);
}

extern "C" std::uint64_t tail_item_b(void* obj) {
    const std::uint64_t r = b_original(obj);
    if (active.load(std::memory_order_relaxed) && t_start) {
        const int i = n_items.fetch_add(1, std::memory_order_relaxed);
        if (i < kMax) items[i] = Item{obj, t_start, now(), GetCurrentThreadId()};
        ULONG64 cyc = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cyc);
        const ULONG64 tsc = __rdtsc();
        const bool on_main = GetCurrentThreadId() == main_tid.load(std::memory_order_relaxed);
        (on_main ? m_tsc : w_tsc).fetch_add(tsc - tsc_start, std::memory_order_relaxed);
        (on_main ? m_cyc : w_cyc).fetch_add(cyc - cyc_start, std::memory_order_relaxed);
        t_start = 0;
    }
    return r;
}

extern "C" std::uint64_t tail_run(void* a, void* b, void* c, void* d) {
    if (main_tid.load(std::memory_order_relaxed) == 0) main_tid.store(GetCurrentThreadId());
    if (tlp_mode != 2 || active.load()) return run_original(a, b, c, d);
    n_items.store(0);
    active.store(1);
    const LONG64 r0 = now();
    const std::uint64_t r = run_original(a, b, c, d);
    const LONG64 r1 = now();
    active.store(0);
    analyze(r0, r1, GetCurrentThreadId());
    maybe_report();
    return r;
}

extern "C" std::uint64_t graph_run(void* a, void* b) {
    const LONG64 t0 = now();
    const std::uint64_t r = graph_original(a, b);
    const long long d = now() - t0;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_ticks.fetch_add(d, std::memory_order_relaxed);
    if (d * 10000 < freq.QuadPart) g_short.fetch_add(1, std::memory_order_relaxed);
    long long m = g_max.load(std::memory_order_relaxed);
    while (d > m && !g_max.compare_exchange_weak(m, d)) {}
    return r;
}

extern "C" void wait_yield() {
    if (GetCurrentThreadId() != main_tid.load(std::memory_order_relaxed)) {
        y_other.fetch_add(1, std::memory_order_relaxed);
        SwitchToThread();
        return;
    }
    if (tlp_wait == 1) {
        y_spin.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < 64; ++i) _mm_pause();
        return;
    }
    const LONG64 t0 = now();
    SwitchToThread();
    const long long dt = now() - t0;
    y_main.fetch_add(1, std::memory_order_relaxed);
    y_ticks.fetch_add(dt, std::memory_order_relaxed);
    long long m = y_max.load(std::memory_order_relaxed);
    while (dt > m && !y_max.compare_exchange_weak(m, dt)) {}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
