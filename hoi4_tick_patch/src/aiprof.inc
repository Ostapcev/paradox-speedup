// Module "aiprof" (measurement only): per-country time of the hourly AI.
// DoAIHourlyUpdates (14020bd80, on a worker) runs two parallel-for phases over the CCountryAI
// array: phase 1 calls vfunc +0x10 (1402c44a0), phase 2 vfunc +0x18 (1402c4510). Both slots of
// the CCountryAI vtable 1427b4b80 are replaced by wrappers that call the same function and time
// it. No game logic changes. Per phase and game hour: wall = last end - first start; per country:
// duration and start/end offsets within the phase.

template <int... I> struct Seq {};
template <int N, int... I> struct MakeSeq : MakeSeq<N - 1, N - 1, I...> {};
template <int... I> struct MakeSeq<0, I...> { using type = Seq<I...>; };

constexpr std::uintptr_t ai_vtable_rva = 0x27b4b80;
constexpr std::uintptr_t ai_slot_rva[2] = {0x2c44a0, 0x2c4510};
constexpr std::uintptr_t ai_gamestate_rva = 0x33dd6b0;
constexpr int ai_slots = 1024;

// All wrappers forward 4 integer argument registers: +0x30 takes (self, slice index).
using AiFn = void (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
AiFn ai_original[2]{};

struct AiSlot {
    std::atomic<const void*> self{nullptr};
    char tag[8]{};
    std::atomic<std::uint64_t> n{0}, sum{0}, max{0}, sum_start{0}, sum_end{0};
};
struct AiPhase {
    std::atomic<std::int32_t> hour{-1};
    std::atomic<std::int64_t> start{0}, end{0};
    std::atomic<std::uint64_t> walls{0}, hours{0}, calls{0}, sum{0};
};
AiSlot ai_table[2][ai_slots];
AiPhase ai_phase[2];
std::atomic<bool> ai_on{false};

inline std::int32_t ai_hour() noexcept {
    const auto state = *reinterpret_cast<const volatile std::uint64_t*>(image + ai_gamestate_rva);
    return state ? *reinterpret_cast<const volatile std::int32_t*>(state + 0x468) : -1;
}

void ai_find_tag(const void* self, char* out) noexcept {
    // country = (*(self+8))->vfunc +0x50; tag = first inline std::string of size 3 in it
    const auto holder = *reinterpret_cast<std::uint8_t* const*>(static_cast<const std::uint8_t*>(self) + 8);
    if (!holder) return;
    using GetFn = const std::uint8_t* (*)(const void*);
    const auto get = *reinterpret_cast<const GetFn*>(*reinterpret_cast<const std::uint8_t* const*>(holder) + 0x50);
    const std::uint8_t* country = get(holder);
    if (!country) return;
    for (int off = 0; off <= 0x600; off += 8) {
        const auto size = *reinterpret_cast<const std::uint64_t*>(country + off + 16);
        const auto cap = *reinterpret_cast<const std::uint64_t*>(country + off + 24);
        if (size != 3 || cap != 15) continue;
        const char* p = reinterpret_cast<const char*>(country + off);
        bool ok = p[3] == 0;
        for (int i = 0; i < 3 && ok; ++i) ok = (p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9');
        if (ok) { std::memcpy(out, p, 3); out[3] = 0; return; }
    }
}

AiSlot* ai_slot_of(int phase, const void* self) noexcept {
    auto h = (reinterpret_cast<std::uintptr_t>(self) >> 4) * 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < ai_slots; ++i) {
        AiSlot& s = ai_table[phase][(h + i) & (ai_slots - 1)];
        const void* cur = s.self.load(std::memory_order_acquire);
        if (cur == self) return &s;
        if (!cur) {
            const void* expected = nullptr;
            if (s.self.compare_exchange_strong(expected, self)) { ai_find_tag(self, s.tag); return &s; }
            if (expected == self) return &s;
        }
    }
    return nullptr;
}

template <int P>
void ai_wrap(void* self, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    const auto t0 = qpc_now();
    AiPhase& ph = ai_phase[P];
    const std::int32_t h = ai_hour();
    std::int32_t cur = ph.hour.load();
    if (cur != h && ph.hour.compare_exchange_strong(cur, h)) {
        const auto s = ph.start.load(), e = ph.end.load();
        if (cur != -1 && e > s) { ph.walls.fetch_add(static_cast<std::uint64_t>(e - s)); ph.hours.fetch_add(1); }
        ph.start.store(t0);
        ph.end.store(t0);
    }
    ai_original[P](self, b, c, d);
    const auto t1 = qpc_now();
    for (auto e = ph.end.load(); t1 > e && !ph.end.compare_exchange_weak(e, t1);) {}
    for (auto s = ph.start.load(); t0 < s && !ph.start.compare_exchange_weak(s, t0);) {}
    const auto dt = static_cast<std::uint64_t>(t1 - t0);
    ph.calls.fetch_add(1, std::memory_order_relaxed);
    ph.sum.fetch_add(dt, std::memory_order_relaxed);
    if (AiSlot* s = ai_slot_of(P, self)) {
        const auto base = ph.start.load();
        s->n.fetch_add(1, std::memory_order_relaxed);
        s->sum.fetch_add(dt, std::memory_order_relaxed);
        for (auto m = s->max.load(); dt > m && !s->max.compare_exchange_weak(m, dt);) {}
        s->sum_start.fetch_add(static_cast<std::uint64_t>(t0 > base ? t0 - base : 0), std::memory_order_relaxed);
        s->sum_end.fetch_add(static_cast<std::uint64_t>(t1 > base ? t1 - base : 0), std::memory_order_relaxed);
    }
}

void ai_steps_report() noexcept;
void ai_report() noexcept {
    for (int p = 0; p < 2; ++p) {
        AiPhase& ph = ai_phase[p];
        const auto hours = ph.hours.exchange(0);
        const auto walls = ph.walls.exchange(0);
        const auto calls = ph.calls.exchange(0);
        const auto sum = ph.sum.exchange(0);
        if (!hours) continue;
        struct Row { const char* tag; double avg, max, start, end, share; std::uint64_t n; };
        Row rows[ai_slots];
        int count = 0;
        double max_avg = 0, max_end = 0;
        for (auto& s : ai_table[p]) {
            if (!s.self.load()) continue;
            const auto n = s.n.exchange(0);
            const auto su = s.sum.exchange(0), mx = s.max.exchange(0);
            const auto ss = s.sum_start.exchange(0), se = s.sum_end.exchange(0);
            if (!n) continue;
            Row r{s.tag[0] ? s.tag : "?", us(static_cast<double>(su) / n), us(static_cast<double>(mx)),
                  us(static_cast<double>(ss) / n), us(static_cast<double>(se) / n),
                  100.0 * static_cast<double>(su) / static_cast<double>(sum ? sum : 1), n};
            if (r.avg > max_avg) max_avg = r.avg;
            if (r.end > max_end) max_end = r.end;
            rows[count++] = r;
        }
        for (int i = 0; i < count; ++i)  // sort by average duration, descending
            for (int j = i + 1; j < count; ++j)
                if (rows[j].avg > rows[i].avg) std::swap(rows[i], rows[j]);
        const double wall = us(static_cast<double>(walls) / hours);
        const double serial = us(static_cast<double>(sum) / hours);
        log("aiprof phase %d (vfunc +0x%x): hours=%llu wall/h=%.0f us  sum/h=%.0f us  items/h=%.1f  "
            "longest country avg=%.0f us  sum/16=%.0f us  latest avg end=%.0f us",
            p + 1, p == 0 ? 0x10 : 0x18, static_cast<unsigned long long>(hours), wall, serial,
            static_cast<double>(calls) / hours, max_avg, serial / 16.0, max_end);
        char line[1024];
        int len = 0;
        for (int i = 0; i < count && i < 12; ++i)
            len += std::snprintf(line + len, sizeof(line) - len, " %s %.0f(max %.0f, start %.0f, end %.0f, %.1f%%)",
                                 rows[i].tag, rows[i].avg, rows[i].max, rows[i].start, rows[i].end, rows[i].share);
        log("aiprof phase %d top:%s", p + 1, line);
    }
    ai_steps_report();
}


// Periodic steps of CAICore.update_parallel 1420d3920 (CCountryAI vtable slots +0x20..+0x48):
// +0x20 hourly, +0x28 every 24-31 h, +0x30 catch-up, +0x38 ~weekly, +0x40 ~monthly, +0x48 ~yearly.
constexpr int ai_steps = 6;
constexpr std::uintptr_t ai_step_rva[ai_steps] = {0x2c1690, 0x2bfee0, 0x2bff30, 0x2c4520, 0x2c26c0, 0x2c46e0};
AiFn ai_step_original[ai_steps]{};
struct AiStep { std::atomic<std::uint64_t> n{0}, sum{0}, max{0}, over2{0}, over5{0}, over10{0}, sum_over5{0}; };
AiStep ai_step[ai_steps];
std::atomic<std::uint64_t> ai_slice_n[26], ai_slice_sum[26], ai_slice_max[26];

template <int K>
void ai_step_wrap(void* self, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    const auto t0 = qpc_now();
    ai_step_original[K](self, b, c, d);
    const auto dt = static_cast<std::uint64_t>(qpc_now() - t0);
    if (K == 2) {  // per slice of the daily work
        const auto i = static_cast<std::uint32_t>(b) < 25 ? static_cast<std::uint32_t>(b) : 25;
        ai_slice_n[i].fetch_add(1, std::memory_order_relaxed);
        ai_slice_sum[i].fetch_add(dt, std::memory_order_relaxed);
        for (auto m = ai_slice_max[i].load(); dt > m && !ai_slice_max[i].compare_exchange_weak(m, dt);) {}
    }
    AiStep& s = ai_step[K];
    s.n.fetch_add(1, std::memory_order_relaxed);
    s.sum.fetch_add(dt, std::memory_order_relaxed);
    for (auto m = s.max.load(); dt > m && !s.max.compare_exchange_weak(m, dt);) {}
    const double u = us(static_cast<double>(dt));
    if (u > 2000) s.over2.fetch_add(1, std::memory_order_relaxed);
    if (u > 5000) { s.over5.fetch_add(1, std::memory_order_relaxed); s.sum_over5.fetch_add(dt, std::memory_order_relaxed); }
    if (u > 10000) s.over10.fetch_add(1, std::memory_order_relaxed);
}

void ai_steps_report() noexcept {
    {
        char line[1400];
        int len = 0;
        for (int i = 0; i < 26; ++i) {
            const auto n = ai_slice_n[i].exchange(0);
            const auto su = ai_slice_sum[i].exchange(0), mx = ai_slice_max[i].exchange(0);
            if (n) len += std::snprintf(line + len, sizeof(line) - len, " %d:%llux%.0f(max %.0f)", i,
                                        static_cast<unsigned long long>(n), us(static_cast<double>(su) / n),
                                        us(static_cast<double>(mx)));
        }
        if (len) log("aiprof +0x30 slices (index:calls x avg us):%s", line);
    }
    for (int k = 0; k < ai_steps; ++k) {
        AiStep& s = ai_step[k];
        const auto n = s.n.exchange(0);
        if (!n) continue;
        const auto sum = s.sum.exchange(0);
        log("aiprof step +0x%x: calls=%llu total=%.0f ms avg=%.0f us max=%.0f us >2ms=%llu >5ms=%llu >10ms=%llu time_in_>5ms=%.0f ms",
            0x20 + 8 * k, static_cast<unsigned long long>(n), us(static_cast<double>(sum)) / 1000.0,
            us(static_cast<double>(sum) / n), us(static_cast<double>(s.max.exchange(0))),
            static_cast<unsigned long long>(s.over2.exchange(0)), static_cast<unsigned long long>(s.over5.exchange(0)),
            static_cast<unsigned long long>(s.over10.exchange(0)), us(static_cast<double>(s.sum_over5.exchange(0))) / 1000.0);
    }
}

template <int... I> void ai_fill_steps(std::uint64_t* vt, Seq<I...>) {
    ((vt[4 + I] = reinterpret_cast<std::uint64_t>(&ai_step_wrap<I>)), ...);
}

bool install_aiprof() {
    auto* vt = reinterpret_cast<std::uint64_t*>(image + ai_vtable_rva);
    for (int p = 0; p < 2; ++p)
        if (vt[2 + p] != image + ai_slot_rva[p]) { log("aiprof REFUSED (CCountryAI vtable differs)"); return false; }
    for (int k = 0; k < ai_steps; ++k) {
        if (vt[4 + k] != image + ai_step_rva[k]) { log("aiprof REFUSED (CCountryAI step slots differ)"); return false; }
        ai_step_original[k] = reinterpret_cast<AiFn>(image + ai_step_rva[k]);
    }
    ai_original[0] = reinterpret_cast<AiFn>(image + ai_slot_rva[0]);
    ai_original[1] = reinterpret_cast<AiFn>(image + ai_slot_rva[1]);
    DWORD old = 0;
    if (!VirtualProtect(vt + 2, 64, PAGE_READWRITE, &old)) { log("aiprof REFUSED (VirtualProtect)"); return false; }
    vt[2] = reinterpret_cast<std::uint64_t>(&ai_wrap<0>);
    vt[3] = reinterpret_cast<std::uint64_t>(&ai_wrap<1>);
    ai_fill_steps(vt, typename MakeSeq<ai_steps>::type{});
    VirtualProtect(vt + 2, 64, old, &old);
    ai_on = true;
    log("aiprof installed");
    return true;
}
