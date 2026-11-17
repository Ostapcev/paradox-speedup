// fast.hpp — быстрый пересчёт кэша расстояний с ТЕМ ЖЕ результатом, что у игры.
//
// Игра (140a59660): для s по возрастанию, t > s по возрастанию — если пара ещё не заполнена,
// A*(s, t) (эвристика — прямая, от матрицы не зависит => путь A*(s,t) — чистая функция пары);
// найденный путь заполняет ВСЕ пары на нём: dist = разница g, hops = разница позиций.
// Итог: dist — кратчайшее расстояние; hops — длина в прыжках того пути, который последним
// накрыл пару.
//
// Наблюдение: если у пары все кратчайшие пути одной длины в прыжках («однозначная» пара), то и
// у каждой подпары на любом её кратчайшем пути тоже (иначе подстановка дала бы пути пары разной
// длины). Значит, вызовы для однозначных пар задевают только однозначные пары, чьи значения от
// порядка не зависят, а неоднозначные пары (множество Q) накрываются только вызовами для пар из Q.
//
// Алгоритм:
//   1) параллельно по источникам: Дейкстра -> dist; min/max прыжков по DAG кратчайших путей;
//      однозначные пары получают hops = min; неоднозначные собираются в Q;
//   2) параллельно: точный A* игры (pathemu::AStar) для каждой пары из Q -> путь;
//   3) последовательно в порядке игры по Q: пара ещё не накрыта -> «вызов»: все пары из Q на её
//      пути накрываются, hops = разница позиций (последний писатель побеждает).
// Предпосылки (проверяются вызывающим кодом): стоимости гиперлиний > 0; A* игры находит
// кратчайший путь (эвристика допустима) — для Q это сверяется здесь же (Stats::astar_not_shortest).
#pragma once

#include <windows.h>

#include "emulate.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <mutex>
#include <thread>
#include <vector>

namespace pathfast {

using pathemu::Graph;
using pathemu::Matrix;
using pathemu::kUnknown;

struct Stats {
    long long ambiguous = 0;           // |Q|
    long long q_calls = 0;             // «вызовов» в шаге 3
    long long astar_not_shortest = 0;  // A* дал не кратчайший путь (нарушена предпосылка)
    unsigned ms_rows = 0, ms_paths = 0, ms_order = 0;
};

inline bool costs_positive(const Graph& g) {
    for (const auto& row : g.adj)
        for (const auto& e : row)
            if (e.cost <= 0) return false;
    return true;
}

template <class F>
inline void parallel_for(int count, int threads, F&& body) {
    std::atomic<int> next{0};
    auto worker = [&](int id) {
        for (int i; (i = next.fetch_add(1)) < count;) body(id, i);
    };
    if (threads < 1) threads = 1;
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker, i);
    worker(0);
    for (auto& t : pool) t.join();
}

class Row {
public:
    explicit Row(int n) : d_(n), minh_(n), maxh_(n) { order_.reserve(n); }

    // Заполняет строку s (t > s). Неоднозначные t добавляет в q (по возрастанию t).
    void compute(const Graph& gr, Matrix& m, int s, std::vector<int>& q) {
        const int n = gr.n;
        std::fill(d_.begin(), d_.end(), LLONG_MAX);
        order_.clear();
        heap_.clear();
        d_[s] = 0;
        heap_.push_back({0, s});
        while (!heap_.empty()) {
            std::pop_heap(heap_.begin(), heap_.end(), Greater{});
            const Item it = heap_.back();
            heap_.pop_back();
            if (it.d != d_[it.v]) continue;
            order_.push_back(it.v);
            for (const auto& e : gr.adj[it.v]) {
                const long long nd = it.d + e.cost;
                if (nd < d_[e.to]) {
                    d_[e.to] = nd;
                    heap_.push_back({nd, e.to});
                    std::push_heap(heap_.begin(), heap_.end(), Greater{});
                }
            }
        }
        for (int v : order_) { minh_[v] = INT_MAX; maxh_[v] = -1; }
        minh_[s] = maxh_[s] = 0;
        for (int u : order_)  // стоимости > 0: предшественники в DAG раньше по порядку Дейкстры
            for (const auto& e : gr.adj[u])
                if (d_[u] + e.cost == d_[e.to]) {
                    if (minh_[u] + 1 < minh_[e.to]) minh_[e.to] = minh_[u] + 1;
                    if (maxh_[u] + 1 > maxh_[e.to]) maxh_[e.to] = maxh_[u] + 1;
                }
        for (int t = s + 1; t < n; ++t) {
            if (!gr.has_lanes[t] || d_[t] == LLONG_MAX) continue;  // остаётся -100000 / 0xffff
            const std::size_t k = m.index(s, t);
            m.dist[k] = d_[t];
            if (minh_[t] == maxh_[t]) m.hops[k] = static_cast<std::uint16_t>(minh_[t]);
            else q.push_back(t);
        }
    }

private:
    struct Item { long long d; int v; };
    struct Greater { bool operator()(const Item& a, const Item& b) const { return a.d > b.d; } };
    std::vector<long long> d_;
    std::vector<int> minh_, maxh_, order_;
    std::vector<Item> heap_;
};

inline void rebuild(const Graph& gr, Matrix& m, int threads, Stats& st) {
    const int n = gr.n;
    m.init(n);
    if (threads < 1) threads = 1;

    // 1) строки
    DWORD t0 = GetTickCount();
    std::vector<std::vector<int>> q_by_row(n);
    {
        std::vector<Row*> rows(threads, nullptr);
        parallel_for(n - 1, threads, [&](int id, int s) {
            if (!gr.has_lanes[s]) return;
            if (!rows[id]) rows[id] = new Row(n);
            rows[id]->compute(gr, m, s, q_by_row[s]);
        });
        for (Row* r : rows) delete r;
    }
    struct Pair { int s, t; };
    std::vector<Pair> q;
    for (int s = 0; s < n; ++s)
        for (int t : q_by_row[s]) q.push_back({s, t});
    st.ambiguous = static_cast<long long>(q.size());
    DWORD t1 = GetTickCount();
    st.ms_rows = t1 - t0;
    if (q.empty()) return;

    // 2) пути A* для всех пар из Q (чистые функции пары)
    std::vector<std::vector<int>> paths(q.size());
    std::atomic<long long> not_shortest{0};
    {
        std::vector<pathemu::AStar*> astars(threads, nullptr);
        parallel_for(static_cast<int>(q.size()), threads, [&](int id, int i) {
            if (!astars[id]) astars[id] = new pathemu::AStar(n);
            pathemu::AStar& a = *astars[id];
            const Pair p = q[i];
            if (!a.search(gr, p.s, p.t)) { ++not_shortest; return; }
            a.path(p.t, paths[i]);  // paths[i][0] = t ... back() = s
            if (a.g(p.t) != m.dist[m.index(p.s, p.t)]) ++not_shortest;
        });
        for (auto* a : astars) delete a;
    }
    st.astar_not_shortest = not_shortest.load();
    DWORD t2 = GetTickCount();
    st.ms_paths = t2 - t1;

    // 3) порядок игры по Q: последний писатель
    std::vector<std::int32_t> qid(m.dist.size(), -1);
    for (std::size_t i = 0; i < q.size(); ++i) qid[m.index(q[i].s, q[i].t)] = static_cast<std::int32_t>(i);
    std::vector<char> covered(q.size(), 0);
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (covered[i]) continue;
        ++st.q_calls;
        const std::vector<int>& path = paths[i];
        const int len = static_cast<int>(path.size());
        for (int a = len - 1; a >= 0; --a)
            for (int b = a - 1; b >= 0; --b) {
                const std::size_t k = m.index(path[a], path[b]);
                const std::int32_t id = qid[k];
                if (id < 0) continue;
                covered[id] = 1;
                m.hops[k] = static_cast<std::uint16_t>(a - b);
            }
    }
    st.ms_order = GetTickCount() - t2;
}

}  // namespace pathfast
