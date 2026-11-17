// Stellaris Path Patch 0.1.0, Stellaris 4.5.4 x64 — быстрый пересчёт кэша расстояний между
// системами («Recalc cached data», 20–25 с на галактике ~3000 систем) с тем же результатом.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140a59660  пересчёт: две треугольные матрицы на все пары систем — расстояние (int64,
//              DAT_142852fa0) и число прыжков (uint16, DAT_142852fd0); вызывается при загрузке,
//              генерации галактики и после spawn_system / move_system / создания страны (1408ffa40).
//   140a598e5  call 140a59f80 — строка источника: parallel-for по целям, A* на каждую пару (140a5a4a0)
//   140a59932  call 140a59a40 — постобработка после заполнения матриц
// Разбор алгоритма и доказательство эквивалентности — в src/fast.hpp и src/emulate.hpp.
//
// Режимы ([path] mode=):
//   off     ничего не меняет;
//   shadow  игра считает сама; после пересчёта плагин считает своим способом и сравнивает обе
//           матрицы побайтно (журнал: число расхождений, время игры и своё). Для проверки;
//   on      на первой строке пересчёта плагин заполняет обе матрицы сам, остальные строки игры
//           пропускаются. Перед этим проверяет предпосылки (стоимости > 0, индексы, раскладку
//           матриц, выборочную сверку с точным A* игры); при любом сомнении — FALLBACK: строки
//           считает игра, как без плагина.

#include <windows.h>

#include "call_patch.hpp"
#include "fast.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint64_t kRowSite = 0x140a598e5;
constexpr std::uint8_t kRowBytes[5] = {0xe8, 0x96, 0x06, 0x00, 0x00};   // call 140a59f80
constexpr std::uint64_t kPostSite = 0x140a59932;
constexpr std::uint8_t kPostBytes[5] = {0xe8, 0x09, 0x01, 0x00, 0x00};  // call 140a59a40
constexpr std::uint64_t kPosAccessor = 0x1402373b0;                     // lea rax,[rcx+0x3e0]; ret

// матрицы (объекты pdx-массивов: +8 данные, +0x14 размер)
constexpr std::uint64_t kDistData = 0x142852fa0, kDistSize = 0x142852fac;
constexpr std::uint64_t kOffData = 0x142852fb8, kOffCount = 0x142852fc4;
constexpr std::uint64_t kHopData = 0x142852fd0, kHopSize = 0x142852fdc;
constexpr std::uint64_t kHoffData = 0x142852fe8, kHoffCount = 0x142852ff4;

using RowFn = void (*)(void*, void*, int);
using PostFn = void (*)(void*, void*, std::uint64_t);

splug::Mode mode = splug::Mode::off;
int threads = 1;
int verify_samples = 256;
RowFn orig_row = nullptr;
PostFn orig_post = nullptr;
splug::CallPatch row_patch, post_patch;

enum class RebuildState { idle, ours, game };
RebuildState state = RebuildState::idle;
std::int64_t t_first_row = 0;
long long rebuilds = 0;

std::int64_t qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double ms_since(std::int64_t t0) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(qpc() - t0) / static_cast<double>(f.QuadPart);
}

template <class T>
T& g(std::uint64_t va) { return *splug::rva<T>(va); }

// Граф из списка систем пересчёта (объект local_b8 в 140a59660: +8 данные sys*[], +0x14 число;
// индекс в списке = id & 0xffffff). Возвращает nullptr или причину отказа.
const char* build_graph(const void* list, pathemu::Graph& gr) {
    const auto* base = static_cast<const std::uint8_t*>(list);
    auto* const* systems = *reinterpret_cast<std::uint8_t* const* const*>(base + 8);
    const int count = *reinterpret_cast<const std::int32_t*>(base + 0x14);
    if (!systems || count < 2) return "empty system list";
    if (count >= 32767) return "too many systems for 16-bit A* indices";
    if (g<std::int32_t>(kOffCount) + 1 != count || g<std::int32_t>(kHoffCount) + 1 != count)
        return "matrix rows != system count";
    const auto accessor = reinterpret_cast<std::uintptr_t>(splug::rva(kPosAccessor));
    gr.n = count;
    gr.adj.assign(count, {});
    gr.has_lanes.assign(count, 0);
    gr.px.assign(count, 0);
    gr.py.assign(count, 0);
    for (int i = 0; i < count; ++i) {
        const std::uint8_t* sys = systems[i];
        if (!sys) return "null system in list";
        if ((*reinterpret_cast<const std::uint32_t*>(sys + 8) & 0xffffff) != static_cast<std::uint32_t>(i))
            return "system index != list position";
        const auto* vt = *reinterpret_cast<const std::uintptr_t* const*>(sys + 0x28);
        if (vt[1] != accessor) return "unexpected position accessor";
        gr.px[i] = *reinterpret_cast<const std::int64_t*>(sys + 0x410);
        gr.py[i] = *reinterpret_cast<const std::int64_t*>(sys + 0x418);
        const auto* lanes = *reinterpret_cast<const std::uint8_t* const*>(sys + 0x590);
        const int lane_count = *reinterpret_cast<const std::int32_t*>(sys + 0x59c);
        gr.has_lanes[i] = lane_count != 0;
        for (int k = 0; k < lane_count; ++k) {
            const auto id = *reinterpret_cast<const std::uint32_t*>(lanes + k * 0x20 + 8);
            const auto cost = *reinterpret_cast<const std::int64_t*>(lanes + k * 0x20 + 0x10);
            const int to = static_cast<int>(id & 0xffffff);
            if (to >= count || !systems[to] || *reinterpret_cast<const std::uint32_t*>(systems[to] + 8) != id)
                return "lane to an unknown system";
            if (cost <= 0) return "non-positive lane cost";
            gr.adj[i].push_back({to, cost});
        }
    }
    return nullptr;
}

const char* check_layout(const pathemu::Matrix& m) {
    const int rows = m.n - 1;
    if (g<std::int32_t>(kDistSize) != static_cast<std::int32_t>(m.dist.size()) ||
        g<std::int32_t>(kHopSize) != static_cast<std::int32_t>(m.hops.size()))
        return "matrix size differs";
    const auto* off = g<std::int32_t*>(kOffData);
    const auto* hoff = g<std::int32_t*>(kHoffData);
    if (!off || !hoff || !g<std::int64_t*>(kDistData) || !g<std::uint16_t*>(kHopData)) return "matrix not allocated";
    if (std::memcmp(off, m.off.data(), rows * 4) != 0 || std::memcmp(hoff, m.off.data(), rows * 4) != 0)
        return "row offsets differ";
    return nullptr;
}

// Выборочная сверка с точным A* игры: пары, которые наш алгоритм не прогонял через A*.
const char* verify_sample(const pathemu::Graph& gr, const pathemu::Matrix& m) {
    if (verify_samples <= 0) return nullptr;
    pathemu::AStar astar(gr.n);
    std::uint64_t x = 0x9e3779b97f4a7c15ull ^ static_cast<std::uint64_t>(gr.n);
    int done = 0;
    for (int tries = 0; done < verify_samples && tries < verify_samples * 20; ++tries) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int s = static_cast<int>(x % gr.n), t = static_cast<int>((x >> 32) % gr.n);
        if (s == t || !gr.has_lanes[s] || !gr.has_lanes[t]) continue;
        if (s > t) { const int tmp = s; s = t; t = tmp; }
        const std::size_t k = m.index(s, t);
        ++done;
        if (!astar.search(gr, s, t)) {
            if (m.dist[k] != pathemu::kUnknown) return "sample: A* unreachable, ours reachable";
            continue;
        }
        if (astar.g(t) != m.dist[k]) return "sample: distance differs from game A*";
        // для однозначных пар длина пути A* обязана совпасть; для неоднозначных значение задаёт
        // порядок вызовов, его проверяет shadow
        (void)astar;
    }
    return nullptr;
}

// Полный пересчёт своим способом. Возвращает nullptr или причину отказа.
const char* compute(const void* list, pathemu::Matrix& m, pathfast::Stats& st, double& ms) {
    const std::int64_t t0 = qpc();
    pathemu::Graph gr;
    if (const char* e = build_graph(list, gr)) return e;
    pathfast::rebuild(gr, m, threads, st);
    if (st.astar_not_shortest) return "A* returned a non-shortest path";
    if (const char* e = check_layout(m)) return e;
    if (const char* e = verify_sample(gr, m)) return e;
    ms = ms_since(t0);
    return nullptr;
}

void row_hook(void* ctx, void* sys, int len) {
    if (state == RebuildState::idle) {
        t_first_row = qpc();
        state = RebuildState::game;
        if (mode == splug::Mode::on) {
            const void* list = reinterpret_cast<void* const*>(ctx)[3];
            pathemu::Matrix m;
            pathfast::Stats st;
            double ms = 0;
            if (const char* e = compute(list, m, st, ms)) {
                splug::log("rebuild #%lld: FALLBACK to game (%s)", rebuilds + 1, e);
            } else {
                std::memcpy(g<std::int64_t*>(kDistData), m.dist.data(), m.dist.size() * 8);
                std::memcpy(g<std::uint16_t*>(kHopData), m.hops.data(), m.hops.size() * 2);
                state = RebuildState::ours;
                splug::log("rebuild #%lld: ours %.0f ms (systems %d, pairs %zu, ambiguous %lld, calls %lld; "
                           "rows %u ms, paths %u ms, order %u ms)",
                           rebuilds + 1, ms, m.n, m.dist.size(), st.ambiguous, st.q_calls, st.ms_rows,
                           st.ms_paths, st.ms_order);
            }
        }
    }
    if (state == RebuildState::game) orig_row(ctx, sys, len);
}

void post_hook(void* list, void* matrix, std::uint64_t extra) {
    ++rebuilds;
    if (mode == splug::Mode::shadow) {
        const double game_ms = state == RebuildState::idle ? 0.0 : ms_since(t_first_row);
        pathemu::Matrix m;
        pathfast::Stats st;
        double ms = 0;
        if (const char* e = compute(list, m, st, ms)) {
            splug::log("shadow #%lld: game %.0f ms; ours NOT computed (%s)", rebuilds, game_ms, e);
        } else {
            const auto* dist = g<std::int64_t*>(kDistData);
            const auto* hops = g<std::uint16_t*>(kHopData);
            long long dd = 0, hd = 0;
            std::size_t first = SIZE_MAX;
            for (std::size_t k = 0; k < m.dist.size(); ++k) {
                const bool bad_d = dist[k] != m.dist[k], bad_h = hops[k] != m.hops[k];
                dd += bad_d;
                hd += bad_h;
                if ((bad_d || bad_h) && first == SIZE_MAX) first = k;
            }
            splug::log("shadow #%lld: game %.0f ms, ours %.0f ms (x%.1f); systems %d, pairs %zu, ambiguous %lld; "
                       "dist differ %lld, hops differ %lld%s",
                       rebuilds, game_ms, ms, ms > 0 ? game_ms / ms : 0.0, m.n, m.dist.size(), st.ambiguous, dd, hd,
                       dd || hd ? "  <-- MISMATCH" : "  OK");
            if (first != SIZE_MAX)
                splug::log("  first mismatch at pair index %zu: game %lld/%u, ours %lld/%u", first,
                           static_cast<long long>(dist[first]), hops[first],
                           static_cast<long long>(m.dist[first]), m.hops[first]);
        }
    } else if (mode == splug::Mode::on && state == RebuildState::game) {
        splug::log("rebuild #%lld: game %.0f ms (fallback)", rebuilds, ms_since(t_first_row));
    }
    state = RebuildState::idle;
    orig_post(list, matrix, extra);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_path_patch")) return;
    mode = splug::ini_mode(L"path", L"mode", L"off");
    threads = splug::ini_int(L"path", L"threads", 0);
    if (threads <= 0) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        threads = static_cast<int>(si.dwNumberOfProcessors);
    }
    verify_samples = splug::ini_int(L"path", L"verify_samples", 256);
    splug::log("stellaris_path_patch 0.1.0: build %s, mode=%s, threads=%d, verify_samples=%d",
               splug::state().build, splug::mode_name(mode), threads, verify_samples);
    if (mode == splug::Mode::off) return;

    auto* row_site = splug::rva(kRowSite);
    auto* post_site = splug::rva(kPostSite);
    orig_row = reinterpret_cast<RowFn>(splug::call_target(row_site));
    orig_post = reinterpret_cast<PostFn>(splug::call_target(post_site));
    const char* e = splug::prepare_call(row_site, kRowBytes, reinterpret_cast<const void*>(&row_hook), row_patch);
    if (!e) e = splug::prepare_call(post_site, kPostBytes, reinterpret_cast<const void*>(&post_hook), post_patch);
    if (e) {
        splug::log("REFUSED: %s; no bytes changed", e);
        return;
    }
    // постобработку ставим первой: она сбрасывает состояние пересчёта
    if (!splug::commit_call(post_patch) || !splug::commit_call(row_patch)) {
        splug::log("REFUSED: cannot write call sites");
        return;
    }
    splug::log("hooks installed: %llx (rows), %llx (post)", static_cast<unsigned long long>(kRowSite),
               static_cast<unsigned long long>(kPostSite));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
