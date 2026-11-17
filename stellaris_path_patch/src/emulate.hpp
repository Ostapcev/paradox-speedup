// emulate.hpp — побитовая эмуляция пересчёта кэша расстояний Stellaris 4.5.4 (140a59660),
// последовательно: источники по возрастанию индекса, цели по возрастанию (в игре цели одной строки
// идут параллельно в джоб-системе). Нужна, чтобы понять, откуда берутся значения прыжков.
//
// Источники в бинарнике:
//   140a5bbb0  задание строки: цель t > s, у t есть гиперлинии, dist[s][t] == -100000 -> A*(s, t)
//   140a5a4a0  A* одной пары: записи {сист, родитель, g}; куча {f, индекс}; состояния 0/1/2
//   140a5b0f0  эвристика h(x, t): x == t -> 0; иначе оценка по прямой (float)hypot * 90000.
//              Кэшируется во ВРЕМЕННОЙ копии матрицы (local_90 в 140a59660, создана из
//              -100000-заполненной матрицы до пересчёта) — на результат не влияет, h = straight().
//   140a5b210  путь t..s: для всех пар (a, b) на пути dist = g(b) - g(a), hops = разница позиций
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace pathemu {

inline long long g_calls = 0, g_pops = 0, g_path_nodes = 0, g_fill_writes = 0;

constexpr std::int64_t kUnknown = -100000;

struct Edge { int to; std::int64_t cost; };

struct Graph {
    int n = 0;                                // индексы 0..n-1 (n = число систем + 1)
    std::vector<std::vector<Edge>> adj;       // гиперлинии в порядке хранения в системе (+0x590)
    std::vector<char> has_lanes;              // +0x59c != 0
    std::vector<std::int64_t> px, py;         // позиция (+0x410, +0x418), фикс. точка 1e-5
};

struct Matrix {
    int n = 0;
    std::vector<std::int32_t> off;            // начала строк (n - 1 строк)
    std::vector<std::int64_t> dist;
    std::vector<std::uint16_t> hops;
    std::size_t index(int a, int b) const {   // a != b
        if (a > b) { const int t = a; a = b; b = t; }
        return static_cast<std::size_t>(off[a] - a - 1 + b);
    }
    void init(int count) {                    // как 140a59ef0 + заполнение -100000 / 0xffff
        n = count;
        const int rows = count - 1;
        off.assign(rows, 0);
        int acc = 0;
        for (int i = 0; i < rows; ++i) { off[i] = acc; acc += rows - i; }
        dist.assign(static_cast<std::size_t>(acc), kUnknown);
        hops.assign(static_cast<std::size_t>(acc), 0xffff);
    }
};

// Оценка по прямой ровно как в 140a5b0f0 (float-деления, hypotf через double, *90000.0f, усечение).
inline std::int64_t straight(const Graph& g, int x, int t) {
    const float k = 100000.0f;
    const float ny = static_cast<float>(g.py[x]) / k;
    const float dy = static_cast<float>(g.py[t]) / k - ny;
    const float nx = static_cast<float>(g.px[x]) / k;
    const float dx = static_cast<float>(g.px[t]) / k - nx;
    const double a = static_cast<double>(dx), b = static_cast<double>(dy);
    const float d = static_cast<float>(std::sqrt(b * b + a * a));
    return static_cast<std::int64_t>(d * 90000.0f);
}

class AStar {
public:
    explicit AStar(int n) : state_(n, 0), pos_(n, -1), parent_(n, -1), g_(n, 0) {}

    // A*(s, t) над матрицей m (пишет её, как игра).
    void run(const Graph& gr, Matrix& m, int s, int t) {
        if (search(gr, s, t)) fill(m, s, t);
        else m.dist[m.index(s, t)] = kUnknown;
    }

    // Только поиск; после true путь t..s доступен через parent (hops_to(t)).
    bool search(const Graph& gr, int s, int t) {
        ++g_calls;
        for (int v : touched_) { state_[v] = 0; pos_[v] = -1; parent_[v] = -1; g_[v] = 0; }
        touched_.clear();
        heap_.clear();
        touch(s);
        g_[s] = 0;
        parent_[s] = -1;
        push({g_[s] + h(gr, s, t), s});
        for (;;) {
            const int u = pop();
            ++g_pops;
            state_[u] = 2;
            if (u == t) return true;
            for (const Edge& e : gr.adj[u]) {
                const int v = e.to;
                if (v < 0 || v >= gr.n || state_[v] == 2) continue;
                const std::int64_t ng = g_[u] + e.cost;
                if (state_[v] == 1) {
                    bool better = ng < g_[v];
                    if (!better && ng == g_[v]) better = chain(u) + 1 < chain(v);
                    if (better) {
                        parent_[v] = u;
                        g_[v] = ng;
                        heap_[pos_[v]].f = g_[v] + h(gr, v, t);
                        sift_up(pos_[v]);
                    }
                } else {
                    touch(v);
                    parent_[v] = u;
                    g_[v] = ng;
                    state_[v] = 1;
                    push({g_[v] + h(gr, v, t), v});
                }
            }
            if (heap_.empty()) return false;
        }
    }

    int hops_to(int t) const { return chain(t) - 1; }
    std::int64_t g(int v) const { return g_[v]; }
    void path(int t, std::vector<int>& out) const {  // out[0] = t ... back() = s
        out.clear();
        for (int v = t; v >= 0; v = parent_[v]) out.push_back(v);
    }

private:
    struct Item { std::int64_t f; int v; };

    void touch(int v) { touched_.push_back(v); }

    std::int64_t h(const Graph& gr, int x, int t) {
        if (x == t) return 0;
        return straight(gr, x, t);
    }

    int chain(int v) const { int c = 0; for (; v >= 0; v = parent_[v]) ++c; return c; }

    void swap_items(int a, int b) {
        const Item t = heap_[a]; heap_[a] = heap_[b]; heap_[b] = t;
        pos_[heap_[a].v] = a;
        pos_[heap_[b].v] = b;
    }

    void sift_up(int i) {
        while (i >= 1) {
            const int p = (i - 1) >> 1;
            if (heap_[p].f <= heap_[i].f) break;
            swap_items(i, p);
            i = p;
        }
    }

    void push(Item it) {
        pos_[it.v] = static_cast<int>(heap_.size());
        heap_.push_back(it);
        sift_up(static_cast<int>(heap_.size()) - 1);
    }

    int pop() {
        const int last = static_cast<int>(heap_.size()) - 1;
        const Item top = heap_[0];
        heap_[0] = heap_[last];
        heap_[last] = top;
        pos_[heap_[0].v] = 0;
        const int lim = last - 1;  // последний допустимый индекс после изъятия
        int i = 0;
        for (;;) {
            const int l = 2 * i + 1, r = 2 * i + 2;
            int child;
            if (r <= lim && heap_[i].f > heap_[r].f) {
                child = heap_[r].f <= heap_[l].f ? r : l;
            } else {
                if (l > lim || heap_[i].f <= heap_[l].f) break;
                child = l;
            }
            if (child == i) break;
            swap_items(i, child);
            i = child;
        }
        heap_.pop_back();
        return top.v;
    }

    void fill(Matrix& m, int s, int t) {
        path_.clear();
        for (int v = t; v >= 0; v = parent_[v]) path_.push_back(v);  // path_[0] = t ... = s
        g_path_nodes += static_cast<long long>(path_.size());
        g_fill_writes += static_cast<long long>(path_.size()) * (path_.size() - 1) / 2;
        for (int i = static_cast<int>(path_.size()) - 1; i >= 0; --i) {
            const int a = path_[i];
            for (int j = i - 1; j >= 0; --j) {
                const int b = path_[j];
                const std::size_t k = m.index(a, b);
                m.dist[k] = g_[b] - g_[a];
                m.hops[k] = static_cast<std::uint16_t>(i - j);
            }
        }
        m.dist[m.index(s, t)] = g_[t];
    }

    std::vector<char> state_;
    std::vector<int> pos_, parent_;
    std::vector<std::int64_t> g_;
    std::vector<int> touched_, path_;
    std::vector<Item> heap_;
};

// Весь пересчёт последовательно.
inline void rebuild_serial(const Graph& gr, Matrix& m) {
    m.init(gr.n);
    AStar astar(gr.n);
    for (int s = 0; s + 1 < gr.n; ++s) {
        if (!gr.has_lanes[s]) continue;
        for (int t = s + 1; t < gr.n; ++t) {
            if (!gr.has_lanes[t]) continue;
            if (m.dist[m.index(s, t)] != kUnknown) continue;
            astar.run(gr, m, s, t);
        }
    }
}

}  // namespace pathemu
