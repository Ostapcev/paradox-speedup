// Stellaris Graph Patch 0.1.0 (пробник), Stellaris 4.5.4 x64 — граф задач 1402b1d00.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   14029e860 → вызов 14029e8fe → 1402b1d00: исполнитель графа задач с зависимостями.
//   141b54ea0 отдаёт в пул готовые корни (счётчик зависимостей node+0x80 == 0) через 141b54160;
//   узел выполняется переходником 141b569b0 (add rcx,8; jmp 141b54360), node = [rcx+0x10];
//   по окончании 141b54360 уменьшает счётчики последователей и готовых отдаёт в пул (141b540e0 →
//   вызов 141b54141 → 141b54160). Главный поток: 141b544a0 (выполняет ещё не взятые задачи сам),
//   затем 141b54410 (ждёт каждую задачу: 1401597b0, спин + SwitchToThread).
//   На BENCH2446 ветка 14029e860 — 5,7 % реального времени главного потока, из них ~75 % ожидание.
//
// Пробник (ничего в игре не меняет): за вызов 1402b1d00 — стена, число узлов, сумма времени узлов,
// узлы главного потока, время «ни один узел не идёт» (простой), задержка «отдан в пул → начат»,
// время главного потока в помощи (141b544a0) и ожидании (141b54410). В лог раз в минуту.
// [graph] mode=off / shadow.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t sgp_mode = 0;
std::uint64_t sgp_graph(void* a, void* b);
std::uint64_t sgp_node(void* task);
std::uint64_t sgp_submit(void* a, void* node);
std::uint64_t sgp_help(void* g);
std::uint64_t sgp_wait(void* g);
}

namespace {

constexpr std::uint64_t kGraph = 0x14029e8fe;
constexpr std::uint8_t kGraphBytes[5] = {0xe8, 0xfd, 0x33, 0x01, 0x00};
constexpr std::uint64_t kNode = 0x141b569b4;  // jmp 141b54360
constexpr std::uint8_t kNodeBytes[5] = {0xe9, 0xa7, 0xd9, 0xff, 0xff};
constexpr std::uint64_t kSubSucc = 0x141b54141;  // call 141b54160 (последователь готов)
constexpr std::uint8_t kSubSuccBytes[5] = {0xe8, 0x1a, 0x00, 0x00, 0x00};
constexpr std::uint64_t kSubRoot1 = 0x141b54f31;
constexpr std::uint8_t kSubRoot1Bytes[5] = {0xe8, 0x2a, 0xf2, 0xff, 0xff};
constexpr std::uint64_t kSubRoot2 = 0x141b54fb8;
constexpr std::uint8_t kSubRoot2Bytes[5] = {0xe8, 0xa3, 0xf1, 0xff, 0xff};
constexpr std::uint64_t kHelp = 0x1402b1da3;  // call 141b544a0
constexpr std::uint8_t kHelpBytes[5] = {0xe8, 0xf8, 0x26, 0x8a, 0x01};
constexpr std::uint64_t kWait = 0x1402b1dab;  // call 141b54410
constexpr std::uint8_t kWaitBytes[5] = {0xe8, 0x60, 0x26, 0x8a, 0x01};

using Fn1 = std::uint64_t (*)(void*);
using Fn2 = std::uint64_t (*)(void*, void*);
Fn2 graph_orig = nullptr, submit_orig = nullptr;
Fn1 node_orig = nullptr, help_orig = nullptr, wait_orig = nullptr;

DWORD main_tid = 0;
std::atomic<int> g_active{0};
const std::uint8_t* const* g_clock = nullptr;

// за текущий вызов
std::atomic<long long> c_nodes{0}, c_sum{0}, c_main_sum{0}, c_main_nodes{0}, c_lat{0}, c_lat_n{0}, c_lat_max{0},
    c_roots{0}, c_succ{0}, c_node_max{0};
std::atomic<int> running{0};
std::atomic<long long> busy_since{0}, busy_total{0};

// задержка: узел → время отдачи в пул
struct Sub {
    std::atomic<void*> node;
    std::atomic<long long> t;
};
Sub subs[8192];
inline unsigned slot(void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return static_cast<unsigned>((v >> 4) ^ (v >> 17)) & 8191u;
}

inline void amax(std::atomic<long long>& m, long long v) {
    long long cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// за минуту
long long m_calls = 0, m_wall = 0, m_nodes = 0, m_sum = 0, m_main_sum = 0, m_main_nodes = 0, m_idle = 0, m_lat = 0,
          m_lat_n = 0, m_lat_max = 0, m_help = 0, m_wait = 0, m_iters = 0, m_roots = 0, m_succ = 0, m_node_max = 0,
          m_wall_max = 0, m_small = 0;
long long h_t = 0, w_t = 0, iters = 0;
LONG64 rep_qpc = 0;
unsigned long long rep_tsc = 0;
LARGE_INTEGER freq{};
std::int32_t last_day = 0;

std::int32_t day_now() {
    const std::uint8_t* c = *g_clock;
    std::int32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const unsigned long long tsc = __rdtsc();
    if (rep_qpc == 0) {
        rep_qpc = q.QuadPart;
        rep_tsc = tsc;
        last_day = day_now();
        return;
    }
    if (q.QuadPart - rep_qpc < 60 * freq.QuadPart || m_calls == 0) return;
    const double sec = static_cast<double>(q.QuadPart - rep_qpc) / freq.QuadPart;
    const double tpus = static_cast<double>(tsc - rep_tsc) / sec / 1e6;  // тактов TSC на мкс
    auto us = [&](long long v) { return static_cast<double>(v) / tpus; };
    const double n = static_cast<double>(m_calls);
    const std::int32_t d = day_now();
    splug::log("%.0f s, days %d: graph calls %lld (%.1f/s), per call us: wall %.1f (max %.0f), node sum %.1f, "
               "main nodes %.1f, idle (no node running) %.1f, main help %.1f, main wait %.1f; "
               "nodes/call %.1f (main %.1f, roots %.1f, successors %.1f), loop iters/call %.2f; "
               "node avg %.2f us, max %.0f us; submit->start avg %.1f us (n %lld), max %.0f us; calls with sum<50us %lld",
               sec, d - last_day, m_calls, n / sec, us(m_wall) / n, us(m_wall_max), us(m_sum) / n, us(m_main_sum) / n,
               us(m_idle) / n, us(m_help) / n, us(m_wait) / n, m_nodes / n, m_main_nodes / n, m_roots / n, m_succ / n,
               m_iters / n, m_nodes ? us(m_sum) / m_nodes : 0.0, us(m_node_max), m_lat_n ? us(m_lat) / m_lat_n : 0.0,
               m_lat_n, us(m_lat_max), m_small);
    splug::log("  share of %.0f s: wall %.2f%%, node sum %.2f%%, idle %.2f%%", sec, 100.0 * us(m_wall) / (sec * 1e6),
               100.0 * us(m_sum) / (sec * 1e6), 100.0 * us(m_idle) / (sec * 1e6));
    last_day = d;
    rep_qpc = q.QuadPart;
    rep_tsc = tsc;
    m_calls = m_wall = m_nodes = m_sum = m_main_sum = m_main_nodes = m_idle = m_lat = m_lat_n = m_lat_max = m_help =
        m_wait = m_iters = m_roots = m_succ = m_node_max = m_wall_max = m_small = 0;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_graph_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"graph", L"mode", L"off");
    splug::log("stellaris_graph_patch 0.1.0: build %s, mode=%s, sgp_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&sgp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    main_tid = GetCurrentThreadId();
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    graph_orig = reinterpret_cast<Fn2>(splug::call_target(splug::rva(kGraph)));
    node_orig = reinterpret_cast<Fn1>(splug::call_target(splug::rva(kNode)));
    submit_orig = reinterpret_cast<Fn2>(splug::call_target(splug::rva(kSubSucc)));
    help_orig = reinterpret_cast<Fn1>(splug::call_target(splug::rva(kHelp)));
    wait_orig = reinterpret_cast<Fn1>(splug::call_target(splug::rva(kWait)));
    if (splug::call_target(splug::rva(kSubRoot1)) != reinterpret_cast<void*>(submit_orig) ||
        splug::call_target(splug::rva(kSubRoot2)) != reinterpret_cast<void*>(submit_orig)) {
        splug::log("REFUSED: submit targets differ");
        return;
    }
    splug::CallPatch p[7];
    const char* e = splug::prepare_call(splug::rva(kNode), kNodeBytes, reinterpret_cast<const void*>(&sgp_node), p[0]);
    if (!e) e = splug::prepare_call(splug::rva(kSubSucc), kSubSuccBytes, reinterpret_cast<const void*>(&sgp_submit), p[1]);
    if (!e) e = splug::prepare_call(splug::rva(kSubRoot1), kSubRoot1Bytes, reinterpret_cast<const void*>(&sgp_submit), p[2]);
    if (!e) e = splug::prepare_call(splug::rva(kSubRoot2), kSubRoot2Bytes, reinterpret_cast<const void*>(&sgp_submit), p[3]);
    if (!e) e = splug::prepare_call(splug::rva(kHelp), kHelpBytes, reinterpret_cast<const void*>(&sgp_help), p[4]);
    if (!e) e = splug::prepare_call(splug::rva(kWait), kWaitBytes, reinterpret_cast<const void*>(&sgp_wait), p[5]);
    if (!e) e = splug::prepare_call(splug::rva(kGraph), kGraphBytes, reinterpret_cast<const void*>(&sgp_graph), p[6]);
    if (e) {
        splug::log("REFUSED: %s", e);
        return;
    }
    p[0].replacement[0] = 0xe9;  // переходник: jmp, не call
    sgp_mode = 2;
    for (auto& c : p)
        if (!splug::commit_call(c)) {
            splug::log("FAILED while writing calls");
            return;
        }
    splug::log("installed: graph %llx, node %llx, submit %llx/%llx/%llx, help %llx, wait %llx",
               static_cast<unsigned long long>(kGraph), static_cast<unsigned long long>(kNode),
               static_cast<unsigned long long>(kSubSucc), static_cast<unsigned long long>(kSubRoot1),
               static_cast<unsigned long long>(kSubRoot2), static_cast<unsigned long long>(kHelp),
               static_cast<unsigned long long>(kWait));
}

}  // namespace

extern "C" std::uint64_t sgp_submit(void* a, void* node) {
    if (g_active.load(std::memory_order_relaxed)) {
        Sub& s = subs[slot(node)];
        s.t.store(static_cast<long long>(__rdtsc()), std::memory_order_relaxed);
        s.node.store(node, std::memory_order_release);
        if (__builtin_return_address(0) == reinterpret_cast<void*>(splug::rva(kSubSucc + 5)))
            c_succ.fetch_add(1, std::memory_order_relaxed);
        else
            c_roots.fetch_add(1, std::memory_order_relaxed);
    }
    return submit_orig(a, node);
}

extern "C" std::uint64_t sgp_node(void* task) {
    if (!g_active.load(std::memory_order_relaxed)) return node_orig(task);
    void* node = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(task) + 0x10);
    const long long t0 = static_cast<long long>(__rdtsc());
    Sub& s = subs[slot(node)];
    if (s.node.load(std::memory_order_acquire) == node) {
        const long long lat = t0 - s.t.load(std::memory_order_relaxed);
        if (lat > 0) {
            c_lat.fetch_add(lat, std::memory_order_relaxed);
            c_lat_n.fetch_add(1, std::memory_order_relaxed);
            amax(c_lat_max, lat);
        }
    }
    if (running.fetch_add(1) == 0) busy_since.store(t0, std::memory_order_relaxed);
    const std::uint64_t r = node_orig(task);
    const long long t1 = static_cast<long long>(__rdtsc());
    if (running.fetch_sub(1) == 1) busy_total.fetch_add(t1 - busy_since.load(std::memory_order_relaxed));
    const long long d = t1 - t0;
    c_nodes.fetch_add(1, std::memory_order_relaxed);
    c_sum.fetch_add(d, std::memory_order_relaxed);
    amax(c_node_max, d);
    if (GetCurrentThreadId() == main_tid) {
        c_main_sum.fetch_add(d, std::memory_order_relaxed);
        c_main_nodes.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

extern "C" std::uint64_t sgp_help(void* g) {
    if (!g_active.load(std::memory_order_relaxed)) return help_orig(g);
    const long long t0 = static_cast<long long>(__rdtsc());
    const std::uint64_t r = help_orig(g);
    h_t += static_cast<long long>(__rdtsc()) - t0;
    ++iters;
    return r;
}

extern "C" std::uint64_t sgp_wait(void* g) {
    if (!g_active.load(std::memory_order_relaxed)) return wait_orig(g);
    const long long t0 = static_cast<long long>(__rdtsc());
    const std::uint64_t r = wait_orig(g);
    w_t += static_cast<long long>(__rdtsc()) - t0;
    return r;
}

extern "C" std::uint64_t sgp_graph(void* a, void* b) {
    if (sgp_mode != 2 || GetCurrentThreadId() != main_tid) return graph_orig(a, b);
    c_nodes = c_sum = c_main_sum = c_main_nodes = c_lat = c_lat_n = c_lat_max = c_roots = c_succ = c_node_max = 0;
    busy_total = 0;
    running = 0;
    h_t = w_t = iters = 0;
    g_active.store(1);
    const long long t0 = static_cast<long long>(__rdtsc());
    const std::uint64_t r = graph_orig(a, b);
    const long long wall = static_cast<long long>(__rdtsc()) - t0;
    g_active.store(0);
    ++m_calls;
    m_wall += wall;
    if (wall > m_wall_max) m_wall_max = wall;
    m_nodes += c_nodes;
    m_sum += c_sum;
    m_main_sum += c_main_sum;
    m_main_nodes += c_main_nodes;
    const long long idle = wall - busy_total.load();
    m_idle += idle > 0 ? idle : 0;
    m_lat += c_lat;
    m_lat_n += c_lat_n;
    if (c_lat_max > m_lat_max) m_lat_max = c_lat_max;
    if (c_node_max > m_node_max) m_node_max = c_node_max;
    m_help += h_t;
    m_wait += w_t;
    m_iters += iters;
    m_roots += c_roots;
    m_succ += c_succ;
    if (c_sum * 1000 < 50LL * 3000000) ++m_small;  // грубо: сумма < 50 мкс при ~3 ГГц
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
