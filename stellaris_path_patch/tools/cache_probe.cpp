// cache_probe — читает из запущенного stellaris.exe 4.5.4 кэш расстояний между системами
// (две треугольные матрицы, строятся в 140a59660) и сверяет его с честным Дейкстрой
// по графу гиперлиний, прочитанному из той же памяти. Только чтение (ReadProcessMemory).
//
// Раскладка (stellaris.exe 4.5.4.1b4f, VA при базе 0x140000000):
//   DAT_14311bed8  -> база систем: +0x18 таблица {?, obj*} по 16 байт, +0x20 ёмкость, +0x24 число
//   система: +0x08 id (младшие 24 бита — индекс), +0x590 гиперлинии (шаг 0x20:
//            +0x08 id соседа, +0x10 стоимость int64), +0x59c число гиперлиний
//   DAT_142852fa0 int64[]  расстояния, DAT_142852fac размер; DAT_142852fb8 int32[] начала строк
//   DAT_142852fd0 uint16[] прыжки,     DAT_142852fdc размер; DAT_142852fe8 int32[] начала строк
//   индекс пары i<j: off[i] - i - 1 + j;  -100000 = не посчитано/недостижимо, 0xffff = нет прыжков
//
// Использование: cache_probe.exe [--pid N] [--dump файл.bin]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <queue>
#include <vector>

#include "../src/fast.hpp"

namespace {

constexpr std::uint64_t kBase = 0x140000000ull;
constexpr std::int64_t kUnknown = -100000;

HANDLE process = nullptr;
std::uint64_t image = 0;

template <class T>
T read(std::uint64_t address) {
    T value{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), &got) || got != sizeof(T)) {
        std::fprintf(stderr, "read failed at %llx\n", static_cast<unsigned long long>(address));
        std::exit(10);
    }
    return value;
}

void read_block(std::uint64_t address, void* out, std::size_t size) {
    auto* p = static_cast<std::uint8_t*>(out);
    while (size) {
        const std::size_t chunk = std::min<std::size_t>(size, 1 << 24);
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), p, chunk, &got) || got != chunk) {
            std::fprintf(stderr, "block read failed at %llx\n", static_cast<unsigned long long>(address));
            std::exit(11);
        }
        p += chunk;
        address += chunk;
        size -= chunk;
    }
}

std::uint64_t va(std::uint64_t v) { return image + (v - kBase); }

DWORD find_pid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{}; e.dwSize = sizeof(e);
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, L"stellaris.exe") == 0) pid = e.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

struct Edge { int to; std::int64_t cost; };

}  // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    const wchar_t* dump = nullptr;
    bool emulate = false, fast = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) pid = wcstoul(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--dump") && i + 1 < argc) dump = argv[++i];
        else if (!wcscmp(argv[i], L"--emulate")) emulate = true;
        else if (!wcscmp(argv[i], L"--fast")) fast = true;
    }
    if (!pid) pid = find_pid();
    if (!pid) { std::fprintf(stderr, "stellaris.exe not running\n"); return 1; }
    process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) { std::fprintf(stderr, "OpenProcess failed %lu\n", GetLastError()); return 2; }
    HMODULE mods[1]; DWORD need = 0;
    if (!EnumProcessModules(process, mods, sizeof(mods), &need)) return 3;
    image = reinterpret_cast<std::uint64_t>(mods[0]);
    std::printf("pid %lu image %llx\n", pid, static_cast<unsigned long long>(image));

    const auto db = read<std::uint64_t>(va(0x14311bed8));
    const auto table = read<std::uint64_t>(db + 0x18);
    const auto cap = read<std::uint32_t>(db + 0x20);
    const auto count = read<std::int32_t>(db + 0x24);
    const auto dist_ptr = read<std::uint64_t>(va(0x142852fa0));
    const auto dist_size = read<std::int32_t>(va(0x142852fac));
    const auto off_ptr = read<std::uint64_t>(va(0x142852fb8));
    const auto off_count = read<std::int32_t>(va(0x142852fc4));
    const auto hop_ptr = read<std::uint64_t>(va(0x142852fd0));
    const auto hop_size = read<std::int32_t>(va(0x142852fdc));
    const auto hoff_ptr = read<std::uint64_t>(va(0x142852fe8));
    const auto hoff_count = read<std::int32_t>(va(0x142852ff4));
    std::printf("systems: cap %u count %d; dist size %d rows %d; hops size %d rows %d\n", cap, count,
                dist_size, off_count, hop_size, hoff_count);
    const int n = off_count + 1;  // индексы 0..n-1

    // граф
    std::vector<std::vector<Edge>> adj(n);
    std::vector<char> has_lanes(n, 0), valid(n, 0);
    std::vector<std::int64_t> px(n, 0), py(n, 0);
    long long odd_position = 0;
    int systems = 0;
    long long lanes = 0, bad_lanes = 0;
    for (std::uint32_t i = 0; i < cap && static_cast<int>(i) < n; ++i) {
        const auto obj = read<std::uint64_t>(table + i * 16ull + 8);
        if (!obj) continue;
        const auto id = read<std::uint32_t>(obj + 8);
        if ((id & 0xffffff) != i) continue;
        valid[i] = 1;
        ++systems;
        const auto lp = read<std::uint64_t>(obj + 0x590);
        const auto lc = read<std::int32_t>(obj + 0x59c);
        has_lanes[i] = lc != 0;
        // позиция: (obj+0x28)->vfunc[1] = 1402373b0: lea rax,[rcx+0x3e0] -> x +8, y +0x10
        const auto vt = read<std::uint64_t>(obj + 0x28);
        if (read<std::uint64_t>(vt + 8) != va(0x1402373b0)) ++odd_position;
        px[i] = read<std::int64_t>(obj + 0x410);
        py[i] = read<std::int64_t>(obj + 0x418);
        for (int k = 0; k < lc; ++k) {
            const auto to_id = read<std::uint32_t>(lp + k * 0x20ull + 8);
            const auto cost = read<std::int64_t>(lp + k * 0x20ull + 0x10);
            const int to = static_cast<int>(to_id & 0xffffff);
            if (to >= n) { ++bad_lanes; continue; }
            const auto o2 = read<std::uint64_t>(table + to * 16ull + 8);
            if (!o2 || read<std::uint32_t>(o2 + 8) != to_id) { ++bad_lanes; continue; }
            adj[i].push_back({to, cost});
            ++lanes;
        }
    }
    std::printf("valid systems %d, lanes %lld (bad %lld), position accessor differs: %lld\n", systems, lanes,
                bad_lanes, odd_position);

    std::vector<std::int32_t> off(off_count), hoff(hoff_count);
    read_block(off_ptr, off.data(), off.size() * 4);
    read_block(hoff_ptr, hoff.data(), hoff.size() * 4);
    std::vector<std::int64_t> dist(dist_size);
    std::vector<std::uint16_t> hops(hop_size);
    read_block(dist_ptr, dist.data(), dist.size() * 8);
    read_block(hop_ptr, hops.data(), hops.size() * 2);
    if (dump) {
        FILE* f = _wfopen(dump, L"wb");
        if (f) {
            std::fwrite(&n, 4, 1, f);
            std::fwrite(off.data(), 4, off.size(), f);
            std::fwrite(dist.data(), 8, dist.size(), f);
            std::fwrite(hops.data(), 2, hops.size(), f);
            std::fclose(f);
        }
    }

    if (emulate || fast) {
        pathemu::Graph gr;
        gr.n = n;
        gr.adj.resize(n);
        for (int i = 0; i < n; ++i)
            for (const Edge& e : adj[i]) gr.adj[i].push_back({e.to, e.cost});
        gr.has_lanes = has_lanes;
        gr.px = px;
        gr.py = py;
        double min_ratio = 1e30;
        for (int i = 0; i < n; ++i)
            for (const auto& e : gr.adj[i]) {
                const auto sl = pathemu::straight(gr, i, e.to);
                if (sl > 0) min_ratio = std::min(min_ratio, double(e.cost) / double(sl));
            }
        std::printf("lane cost / straight estimate: min %.6f; costs positive: %d\n", min_ratio,
                    pathfast::costs_positive(gr) ? 1 : 0);
        pathemu::Matrix m;
        pathfast::Stats fst;
        const DWORD t0 = GetTickCount();
        if (fast) {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            pathfast::rebuild(gr, m, static_cast<int>(si.dwNumberOfProcessors), fst);
        } else {
            pathemu::rebuild_serial(gr, m);
        }
        const DWORD t1 = GetTickCount();
        if (fast) std::printf("fast: |Q| %lld, Q calls %lld, A* not shortest %lld; rows %u ms, paths %u ms, order %u ms\n", fst.ambiguous, fst.q_calls, fst.astar_not_shortest, fst.ms_rows, fst.ms_paths, fst.ms_order);
        long long dd = 0, hd = 0;
        for (std::size_t k = 0; k < m.dist.size() && k < dist.size(); ++k) {
            if (m.dist[k] != dist[k]) ++dd;
            if (m.hops[k] != hops[k]) {
                if (hd < 10) {
                    int a = 0;
                    while (a + 1 < n - 1 && static_cast<std::size_t>(off[a + 1]) <= k) ++a;
                    const int b = static_cast<int>(k) - (off[a] - a - 1);
                    std::printf("  hops differ: pair %d-%d game %u ours %u\n", a, b, hops[k], m.hops[k]);
                }
                ++hd;
            }
        }
        std::printf("emulation: %lu ms, size %zu vs game %zu; dist differs %lld, hops differ %lld\n",
                    t1 - t0, m.dist.size(), dist.size(), dd, hd);
        std::printf("  A* calls %lld, pops %lld (%.1f per call), path nodes %.1f per call, fill writes %lld\n",
                    pathemu::g_calls, pathemu::g_pops, pathemu::g_calls ? double(pathemu::g_pops) / pathemu::g_calls : 0.0,
                    pathemu::g_calls ? double(pathemu::g_path_nodes) / pathemu::g_calls : 0.0, pathemu::g_fill_writes);
    }

    // Дейкстра: стоимость, затем минимум прыжков среди путей минимальной стоимости
    long long exact = 0, exact_bad_dist = 0, exact_bad_hops = 0, unreachable_ok = 0, missing = 0,
              estimate = 0, estimate_below = 0, estimate_above = 0, unknown_but_unreach = 0, other = 0;
    long long est_samples = 0; double est_ratio_sum = 0;
    std::vector<std::int64_t> d(n);
    std::vector<int> h(n);
    // ключ (стоимость, прыжки) — строгий лексикографический минимум
    struct Item { std::int64_t d; int h; int v; bool operator>(const Item& o) const {
        return d != o.d ? d > o.d : h != o.h ? h > o.h : v > o.v; } };
    long long hops_game_more = 0, hops_game_less = 0;
    for (int s = 0; s + 1 < n; ++s) {
        if (!valid[s] || !has_lanes[s]) continue;
        std::fill(d.begin(), d.end(), INT64_MAX);
        std::fill(h.begin(), h.end(), INT32_MAX);
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        d[s] = 0; h[s] = 0; pq.push({0, 0, s});
        while (!pq.empty()) {
            const Item it = pq.top(); pq.pop();
            const int u = it.v;
            if (it.d != d[u] || it.h != h[u]) continue;
            for (const Edge& e : adj[u]) {
                const std::int64_t nd = it.d + e.cost;
                const int nh = it.h + 1;
                if (nd < d[e.to] || (nd == d[e.to] && nh < h[e.to])) {
                    d[e.to] = nd; h[e.to] = nh;
                    pq.push({nd, nh, e.to});
                }
            }
        }
        for (int t = s + 1; t < n; ++t) {
            if (!valid[t] || !has_lanes[t]) continue;
            const std::size_t idx = static_cast<std::size_t>(off[s] - s - 1 + t);
            const std::size_t hidx = static_cast<std::size_t>(hoff[s] - s - 1 + t);
            if (idx >= dist.size() || hidx >= hops.size()) { ++other; continue; }
            const std::int64_t v = dist[idx];
            const std::uint16_t hv = hops[hidx];
            const bool reach = d[t] != INT64_MAX;
            if (hv != 0xffff) {
                ++exact;
                if (!reach || v != d[t]) ++exact_bad_dist;
                else if (hv != h[t]) { ++exact_bad_hops; if (hv > h[t]) ++hops_game_more; else ++hops_game_less; }
            } else if (v == kUnknown) {
                if (reach) ++missing; else ++unreachable_ok;
            } else {
                ++estimate;
                if (reach) {
                    if (v < d[t]) ++estimate_below; else if (v > d[t]) ++estimate_above;
                    if (d[t] > 0) { est_ratio_sum += static_cast<double>(v) / d[t]; ++est_samples; }
                } else ++unknown_but_unreach;
            }
        }
    }
    const long long total = exact + unreachable_ok + missing + estimate + other;
    std::printf("pairs checked: %lld\n", total);
    std::printf("  exact (hops set):          %lld  (dist mismatch %lld, hops differ from min-hops %lld)\n",
                exact, exact_bad_dist, exact_bad_hops);
    std::printf("    hops: game > min %lld, game < min %lld\n", hops_game_more, hops_game_less);
    std::printf("  unreachable (-100000):     %lld\n", unreachable_ok);
    std::printf("  reachable but -100000:     %lld\n", missing);
    std::printf("  estimate (hops=0xffff):    %lld  (below true %lld, above %lld, unreachable %lld, mean v/true %.3f)\n",
                estimate, estimate_below, estimate_above, unknown_but_unreach,
                est_samples ? est_ratio_sum / est_samples : 0.0);
    std::printf("  other/out of range:        %lld\n", other);
    CloseHandle(process);
    return 0;
}
