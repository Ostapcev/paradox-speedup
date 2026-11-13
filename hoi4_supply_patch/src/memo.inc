// memo.inc — memo of the per-country supply Dijkstra (included by hoi4_supply_patch.cpp).
//
// The heavy supply phase calls, per country and per hour, from 1412ace70:
//   14129c610 / 14129d990 (rec, system, supply_country, start*, province_count)
// (same signature and same record offsets; the branch is chosen in 1412ace70).
// Call sites: 1412ad1a4 (c610) and 1412ad1b8 (d990).
//
// Reads (static analysis of both variants):
//   rec  +0x00 map province->label (entries 0x18: +0 probe byte, +4 key, +0x10 value),
//            {+8 entries, +0x10 count, +0x14 mask, +0x18 max probe}
//   rec  +0x20 map province->int (entries 0xc), {+0x28 entries, +0x30 count, +0x34 mask, +0x38 probe}
//   rec  +0xb8 nodes (0xe8 each, count +0xc4), +0xd0 bytes (+0xdc), +0x230/+0x248/+0x260 u16
//        (+0x23c/+0x254/+0x26c), +0x278 i32 (+0x284), +0x2c0 / +0x2f0 pairs (+0x2cc / +0x2fc)
//   start entry (0x20 bytes, built from node[rec+0x1a8] in 1412ace70), province_count
//   supply_country +0x90 map (entries 0x18, mask +0x9c, probe +0xa0), +0x160 i32 list (+0x16c)
//   system +0xf0 map province -> edge table (entries 0x30: +4 probe, +8 key, +0x18 edges,
//        +0x24 mask, +0x28 probe); edges 0xa0 each (+4 probe, +0x48 i32 list, count +0x54)
//   NDefines (constant during a session)
// Edges are read only for settled provinces; every settled province is inserted into the
// rec+0x20 map, so the map keys after a run cover the settled set.
//
// mode=shadow: the original always runs. Before it, a key of all inputs is computed (edge
// tables only for the provinces of the previous run's settled set, hashed once per hour per
// province). After it, a strict hash of the outputs. Key equal to the previous hour's key
// but output different => a missed input ("mismatch").

constexpr std::uintptr_t dijkstra_site_rva = 0x12ad19b;
constexpr std::uint8_t dijkstra_bytes[] = {0x89, 0x4c, 0x24, 0x20, 0x48, 0x8b, 0xce, 0x7d, 0x14,
    0xe8, 0x67, 0xf4, 0xfe, 0xff, 0x48, 0x8b, 0x6c, 0x24, 0x68, 0x48, 0x83, 0xc4, 0x70, 0x41,
    0x5f, 0x41, 0x5e, 0x5e, 0xc3, 0xe8, 0xd3, 0x07, 0xff, 0xff};
constexpr std::uintptr_t dijkstra_c610_call_rva = 0x12ad1a4;
constexpr std::uintptr_t dijkstra_d990_call_rva = 0x12ad1b8;
constexpr std::uintptr_t dijkstra_c610_rva = 0x129c610;
constexpr std::uintptr_t dijkstra_d990_rva = 0x129d990;

using DijkstraFn = void (*)(std::uint8_t*, std::uint8_t*, std::uint8_t*, std::uint64_t*, std::uint32_t);
DijkstraFn original_c610 = nullptr;
DijkstraFn original_d990 = nullptr;

inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

// 4 independent lanes (xxHash64 round): the multiply chains overlap, ~4x the throughput of
// one serial mix64 chain. Used for long runs of words; finished into one 64-bit value.
struct Lanes {
    static constexpr std::uint64_t p1 = 0x9e3779b185ebca87ull, p2 = 0xc2b2ae3d27d4eb4full;
    std::uint64_t v[4] = {0x60ea27eeadc0b5d6ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull,
                          0x61c8864e7a143579ull};
    std::uint64_t n = 0;
    static inline std::uint64_t round(std::uint64_t acc, std::uint64_t w) noexcept {
        acc += w * p2;
        acc = (acc << 31) | (acc >> 33);
        return acc * p1;
    }
    inline void put(std::uint64_t w) noexcept { v[n & 3] = round(v[n & 3], w); ++n; }
    inline void put4(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept {
        v[0] = round(v[0], a); v[1] = round(v[1], b); v[2] = round(v[2], c); v[3] = round(v[3], d);
        n += 4;
    }
    std::uint64_t done() const noexcept {
        std::uint64_t h = mix64(v[0] ^ n);
        h = mix64(h ^ v[1]);
        h = mix64(h ^ v[2]);
        return mix64(h ^ v[3]);
    }
};

inline std::uint64_t value_word(std::uint64_t w) noexcept {
    return looks_like_pointer(w) ? 0x5050505050505050ull : w;  // pointers: values only
}

struct Hasher {
    std::uint64_t h = 0x9e3779b97f4a7c15ull;
    void add(std::uint64_t v) noexcept { h = mix64(h ^ v) + 0x9e3779b97f4a7c15ull; }
    // qwords that look like pointers are skipped (values only)
    void bytes(const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const std::uint8_t*>(p);
        Lanes lanes;
        std::size_t i = 0;
        for (; i + 32 <= n; i += 32) {
            std::uint64_t w[4];
            std::memcpy(w, b + i, 32);
            lanes.put4(value_word(w[0]), value_word(w[1]), value_word(w[2]), value_word(w[3]));
        }
        for (; i + 8 <= n; i += 8) {
            std::uint64_t w;
            std::memcpy(&w, b + i, 8);
            lanes.put(value_word(w));
        }
        std::uint64_t tail = 0;
        for (std::size_t k = 0; i < n; ++i, ++k) tail |= static_cast<std::uint64_t>(b[i]) << (8 * k);
        lanes.put(tail ^ (static_cast<std::uint64_t>(n) << 40));
        add(lanes.done());
    }
};

template <class T> inline T field(const std::uint8_t* base, std::uint32_t offset) noexcept {
    T v;
    std::memcpy(&v, base + offset, sizeof(T));
    return v;
}

// {T* data; int capacity; int size} at offset
void hash_array(Hasher& h, const std::uint8_t* rec, std::uint32_t offset, std::uint32_t stride) noexcept {
    const auto* data = field<const std::uint8_t*>(rec, offset);
    const auto size = field<std::int32_t>(rec, offset + 0xc);
    h.add(static_cast<std::uint32_t>(size) | (static_cast<std::uint64_t>(offset) << 32));
    if (size > 0 && data) h.bytes(data, static_cast<std::size_t>(size) * stride);
}

// Robin-hood map: entries table of (mask + 1 + max_probe) slots; slot empty if probe byte == 0.
// Order-independent content hash (sum of entry hashes) plus count.
std::uint64_t map_hash(const std::uint8_t* entries, std::uint32_t mask, std::uint32_t probe,
                       std::uint32_t stride, std::uint32_t probe_offset) noexcept {
    if (!entries) return 0x1234;
    std::uint64_t sum = 0, count = 0;
    const std::size_t slots = static_cast<std::size_t>(mask) + 1 + probe;
    for (std::size_t i = 0; i < slots; ++i) {
        const auto* e = entries + i * stride;
        if (!e[probe_offset]) continue;
        Hasher eh;
        eh.bytes(e + probe_offset + 1, stride - probe_offset - 1);
        sum += eh.h;
        ++count;
    }
    return mix64(sum ^ (count << 1));
}

// Record maps, exact layout (insert functions 14033e210 / 1401cc180):
//   map +0x00 entries 0x18: +0 hash u32, +4 probe u8, +8 key u32, +0xc padding (the insert
//                           writes stack garbage there, never read), +0x10 value u64
//   map +0x20 entries 0x0c: +0 probe u8, +1 padding, +4 key u32, +8 value u32
// Empty slots (probe 0) are never read. Slot-exact hash: the run inserts into these tables,
// so the resulting layout depends on the slot positions, not just on the content.
struct MapLayout { std::uint32_t stride, probe; std::uint32_t ranges[3][2]; int range_count; };
constexpr MapLayout map00_layout{0x18, 4, {{0, 5}, {8, 0xc}, {0x10, 0x18}}, 3};
constexpr MapLayout map20_layout{0x0c, 0, {{0, 1}, {4, 0xc}, {0, 0}}, 2};

std::uint64_t slot_hash(const std::uint8_t* entries, std::uint32_t mask, std::uint32_t probe,
                        const MapLayout& layout) noexcept {
    if (!entries) return 0x4321;
    Lanes h;
    const std::size_t slots = static_cast<std::size_t>(mask) + 1 + probe;
    h.put(slots);
    for (std::size_t i = 0; i < slots; ++i) {
        const auto* e = entries + i * layout.stride;
        if (!e[layout.probe]) continue;
        h.put(i);
        for (int r = 0; r < layout.range_count; ++r) {
            std::uint64_t w = 0;
            std::memcpy(&w, e + layout.ranges[r][0], layout.ranges[r][1] - layout.ranges[r][0]);
            h.put(w);
        }
    }
    return h.done();
}

// ---- per-province edge-table hash, computed lazily once per game hour
constexpr int max_provinces = 65536;
std::atomic<std::uint64_t> edge_hash[max_provinces];
std::atomic<std::int32_t> edge_hour[max_provinces];

std::int32_t current_hours() noexcept {
    const auto state = *reinterpret_cast<const volatile std::uint64_t*>(image + 0x33dd6b0);
    return state ? *reinterpret_cast<const volatile std::int32_t*>(state + 0x468) : 0;
}

std::uint64_t province_edges(const std::uint8_t* system, std::uint32_t province, std::int32_t hour) noexcept {
    if (province >= max_provinces) return 0xdead;
    if (edge_hour[province].load(std::memory_order_acquire) == hour) {
        return edge_hash[province].load(std::memory_order_relaxed);
    }
    Hasher h;
    h.add(province);
    const auto* table = field<const std::uint8_t*>(system, 0xf0);
    const auto mask = field<std::uint32_t>(system, 0xfc);
    const auto probe = field<std::uint8_t>(system, 0x100);
    std::uint32_t x = (province & 0xffff) * 0x45d9f3b;
    x = (x >> 16 ^ x) * 0x45d9f3b;
    x = x >> 16 ^ x;
    const std::uint8_t* e = table + static_cast<std::size_t>(static_cast<std::int32_t>(x & mask)) * 0x30;
    const std::uint8_t* end = table + (static_cast<std::size_t>(mask) + 1 + probe) * 0x30;
    const std::uint8_t* found = nullptr;
    for (std::uint8_t d = 1; e < end && e[4] && d <= e[4]; ++d, e += 0x30) {
        if (field<std::uint32_t>(e, 8) == (province & 0xffff)) { found = e; break; }
    }
    if (found) {
        const auto* edges = field<const std::uint8_t*>(found, 0x18);
        const auto emask = field<std::int32_t>(found, 0x24);
        const auto eprobe = found[0x28];
        const std::size_t slots = static_cast<std::size_t>(emask) + 1 + eprobe;
        for (std::size_t i = 0; edges && i < slots; ++i) {
            const auto* edge = edges + i * 0xa0;
            if (!edge[4]) continue;
            h.add(i);
            h.bytes(edge, 0xa0);
            const auto* list = field<const std::uint8_t*>(edge, 0x48);
            const auto n = field<std::int32_t>(edge, 0x54);
            if (list && n > 0) h.bytes(list, static_cast<std::size_t>(n) * 4);
        }
    } else {
        h.add(0xfeedull);
    }
    edge_hash[province].store(h.h, std::memory_order_relaxed);
    edge_hour[province].store(hour, std::memory_order_release);
    return h.h;
}

// ---- per-country state (one task per country per hour => no races per index)
struct CountryMemo {
    bool valid = false;
    std::uint64_t key = 0;
    std::uint64_t output[5] = {};
    std::uint32_t branch = 0;
    std::uint16_t* settled = nullptr;   // provinces (keys of rec+0x20 map after last run)
    std::int32_t settled_count = 0;
    std::int32_t settled_capacity = 0;
};
CountryMemo memo[max_countries];

std::atomic<std::uint64_t> memo_calls{0}, memo_hits{0}, memo_mismatch{0}, memo_first{0};
std::atomic<std::uint64_t> memo_key_qpc{0}, memo_run_qpc{0}, memo_out_qpc{0}, memo_hit_run_qpc{0};
std::atomic<std::uint64_t> memo_c610{0}, memo_settled_sum{0};
std::atomic<int> mismatch_logged{0};

std::atomic<std::uint64_t> key_part_qpc[4], key_slots{0}, key_settled{0}, key_array_bytes[8];
constexpr std::uint32_t key_arrays[8][2] = {{0xb8, 0xe8}, {0xd0, 1}, {0x230, 2}, {0x248, 2},
                                            {0x260, 2}, {0x278, 4}, {0x2c0, 8}, {0x2f0, 8}};

std::uint64_t input_key(const std::uint8_t* rec, const std::uint8_t* system, const std::uint8_t* sc,
                        const std::uint64_t* start, std::uint32_t count, std::uint32_t branch,
                        const CountryMemo& m) noexcept {
    Hasher h;
    std::int64_t tk0 = qpc_now();
    h.add(branch);
    h.add(count);
    h.bytes(start, 0x20);
    h.add(slot_hash(field<const std::uint8_t*>(rec, 0x08), field<std::uint32_t>(rec, 0x14),
        field<std::uint8_t>(rec, 0x18), map00_layout));
    h.add(slot_hash(field<const std::uint8_t*>(rec, 0x28), field<std::uint32_t>(rec, 0x34),
        field<std::uint8_t>(rec, 0x38), map20_layout));
    std::int64_t tk1 = qpc_now();
    // table capacity decides the slot layout of the entries the run inserts
    h.add(field<std::uint64_t>(rec, 0x10) ^ (field<std::uint64_t>(rec, 0x18) << 1));
    h.add(field<std::uint64_t>(rec, 0x30) ^ (field<std::uint64_t>(rec, 0x38) << 1));
    hash_array(h, rec, 0xb8, 0xe8);
    hash_array(h, rec, 0xd0, 1);
    hash_array(h, rec, 0x230, 2);
    hash_array(h, rec, 0x248, 2);
    hash_array(h, rec, 0x260, 2);
    hash_array(h, rec, 0x278, 4);
    hash_array(h, rec, 0x2c0, 8);
    hash_array(h, rec, 0x2f0, 8);
    std::int64_t tk2 = qpc_now();
    h.add(field<std::uint64_t>(rec, 0x1a8));
    h.add(map_hash(field<const std::uint8_t*>(sc, 0x90), field<std::uint32_t>(sc, 0x9c),
        field<std::uint8_t>(sc, 0xa0), 0x18, 4));
    hash_array(h, sc, 0x160, 4);
    std::int64_t tk3 = qpc_now();
    const auto hour = current_hours();
    for (int i = 0; i < m.settled_count; ++i) h.add(province_edges(system, m.settled[i], hour));
    h.add(static_cast<std::uint64_t>(m.settled_count));
    const std::int64_t tk4 = qpc_now();
    key_part_qpc[0].fetch_add(static_cast<std::uint64_t>(tk1 - tk0), std::memory_order_relaxed);
    key_part_qpc[1].fetch_add(static_cast<std::uint64_t>(tk2 - tk1), std::memory_order_relaxed);
    key_part_qpc[2].fetch_add(static_cast<std::uint64_t>(tk3 - tk2), std::memory_order_relaxed);
    key_part_qpc[3].fetch_add(static_cast<std::uint64_t>(tk4 - tk3), std::memory_order_relaxed);
    key_slots.fetch_add((static_cast<std::uint64_t>(field<std::uint32_t>(rec, 0x14)) + 1) |
        ((static_cast<std::uint64_t>(field<std::uint32_t>(rec, 0x34)) + 1) << 32), std::memory_order_relaxed);
    for (int a = 0; a < 8; ++a) key_array_bytes[a].fetch_add(static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(field<std::int32_t>(rec, key_arrays[a][0] + 0xc))) * key_arrays[a][1],
        std::memory_order_relaxed);
    key_settled.fetch_add(static_cast<std::uint64_t>(m.settled_count), std::memory_order_relaxed);
    return h.h;
}

// Output components (strict, table order): 0 record qwords written by the run (learned),
// 1 map +0x00, 2 map +0x20, 3 nodes, 4 visited list.
constexpr int out_parts = 5;
const char* const out_part_names[out_parts] = {"record", "map00", "map20", "nodes", "visited"};
std::atomic<std::uint8_t> record_written[100];  // qword index -> written by some run
std::atomic<std::uint32_t> record_written_count[100];
std::atomic<std::uint64_t> part_mismatch[out_parts];

void output_hash(const std::uint8_t* rec, std::uint64_t out[out_parts]) noexcept {
    Hasher r;
    for (int i = 0; i < 100; ++i) {
        if (record_written[i].load(std::memory_order_relaxed)) r.add(field<std::uint64_t>(rec, i * 8) ^ i);
    }
    out[0] = r.h;
    auto table = [&](std::uint32_t ptr, std::uint32_t mask, std::uint32_t probe, std::uint32_t stride) {
        Hasher h;
        const auto* e = field<const std::uint8_t*>(rec, ptr);
        const std::size_t slots = static_cast<std::size_t>(field<std::uint32_t>(rec, mask)) + 1 +
            field<std::uint8_t>(rec, probe);
        h.add(slots);
        if (e) h.bytes(e, slots * stride);
        return h.h;
    };
    out[1] = table(0x08, 0x14, 0x18, 0x18);
    out[2] = table(0x28, 0x34, 0x38, 0xc);
    Hasher n; hash_array(n, rec, 0xb8, 0xe8); out[3] = n.h;
    Hasher v; hash_array(v, rec, 0x148, 4); out[4] = v.h;
}

void collect_settled(const std::uint8_t* rec, CountryMemo& m) noexcept {
    const auto* entries = field<const std::uint8_t*>(rec, 0x28);
    const std::size_t slots = static_cast<std::size_t>(field<std::uint32_t>(rec, 0x34)) + 1 +
        field<std::uint8_t>(rec, 0x38);
    const auto need = field<std::int32_t>(rec, 0x30);
    if (need > m.settled_capacity) {
        std::free(m.settled);
        m.settled_capacity = need + need / 4 + 16;
        m.settled = static_cast<std::uint16_t*>(std::malloc(sizeof(std::uint16_t) * m.settled_capacity));
    }
    int n = 0;
    for (std::size_t i = 0; entries && i < slots && n < m.settled_capacity; ++i) {
        const auto* e = entries + i * 0xc;
        if (e[0]) m.settled[n++] = static_cast<std::uint16_t>(field<std::int32_t>(e, 4));
    }
    m.settled_count = n;
}

enum class MemoMode { shadow, verify, on };
MemoMode memo_mode = MemoMode::shadow;
void dijkstra_on(DijkstraFn original, std::uint32_t branch, std::uint8_t* rec,
                 std::uint8_t* system, std::uint8_t* sc, std::uint64_t* start,
                 std::uint32_t count, std::uintptr_t index);

void dijkstra_common(DijkstraFn original, std::uint32_t branch, std::uint8_t* rec,
                     std::uint8_t* system, std::uint8_t* sc, std::uint64_t* start,
                     std::uint32_t count) {
    const auto base = field<std::uintptr_t>(system, 0x120);
    const auto index = (reinterpret_cast<std::uintptr_t>(rec) - base) / 800;
    if (index >= static_cast<std::uintptr_t>(max_countries)) {
        original(rec, system, sc, start, count);
        return;
    }
    if (memo_mode != MemoMode::shadow) {
        dijkstra_on(original, branch, rec, system, sc, start, count, index);
        return;
    }
    CountryMemo& m = memo[index];
    const auto t0 = qpc_now();
    const auto key = input_key(rec, system, sc, start, count, branch, m);
    std::uint64_t before[100];
    std::memcpy(before, rec, 800);
    const auto t1 = qpc_now();
    original(rec, system, sc, start, count);
    for (int i = 0; i < 100; ++i) {  // learn which record qwords the run writes
        if (field<std::uint64_t>(rec, i * 8) != before[i]) {
            record_written_count[i].fetch_add(1, std::memory_order_relaxed);
            if (!record_written[i].load(std::memory_order_relaxed)) record_written[i].store(1);
        }
    }
    const auto t2 = qpc_now();
    std::uint64_t out[out_parts];
    output_hash(rec, out);
    collect_settled(rec, m);
    const auto t3 = qpc_now();
    memo_calls.fetch_add(1, std::memory_order_relaxed);
    if (branch == 0) memo_c610.fetch_add(1, std::memory_order_relaxed);
    memo_settled_sum.fetch_add(static_cast<std::uint64_t>(m.settled_count), std::memory_order_relaxed);
    memo_key_qpc.fetch_add(static_cast<std::uint64_t>(t1 - t0), std::memory_order_relaxed);
    memo_run_qpc.fetch_add(static_cast<std::uint64_t>(t2 - t1), std::memory_order_relaxed);
    memo_out_qpc.fetch_add(static_cast<std::uint64_t>(t3 - t2), std::memory_order_relaxed);
    if (!m.valid) {
        memo_first.fetch_add(1, std::memory_order_relaxed);
    } else if (key == m.key) {
        memo_hits.fetch_add(1, std::memory_order_relaxed);
        memo_hit_run_qpc.fetch_add(static_cast<std::uint64_t>(t2 - t1), std::memory_order_relaxed);
        bool bad = false;
        char parts[64] = "";
        for (int k = 0; k < out_parts; ++k) {
            if (out[k] != m.output[k]) {
                bad = true;
                part_mismatch[k].fetch_add(1, std::memory_order_relaxed);
                std::strncat(parts, out_part_names[k], sizeof(parts) - std::strlen(parts) - 2);
                std::strncat(parts, " ", sizeof(parts) - std::strlen(parts) - 1);
            }
        }
        if (bad) {
            memo_mismatch.fetch_add(1, std::memory_order_relaxed);
            if (mismatch_logged.fetch_add(1) < 40) {
                log("MISMATCH country=%llu branch=%u hour=%ld settled=%d parts: %s",
                    static_cast<unsigned long long>(index), branch,
                    static_cast<long>(current_hours()), m.settled_count, parts);
            }
        }
    }
    m.valid = true;
    m.key = key;
    std::memcpy(m.output, out, sizeof(out));
    m.branch = branch;
}

void c610_hook(std::uint8_t* rec, std::uint8_t* system, std::uint8_t* sc, std::uint64_t* start,
               std::uint32_t count) {
    dijkstra_common(original_c610, 0, rec, system, sc, start, count);
}

void d990_hook(std::uint8_t* rec, std::uint8_t* system, std::uint8_t* sc, std::uint64_t* start,
               std::uint32_t count) {
    dijkstra_common(original_d990, 1, rec, system, sc, start, count);
}

void memo_report() noexcept {
    const auto calls = memo_calls.exchange(0);
    if (!calls) return;
    const auto hits = memo_hits.exchange(0), mismatch = memo_mismatch.exchange(0);
    const auto first = memo_first.exchange(0), c610 = memo_c610.exchange(0);
    const auto key = memo_key_qpc.exchange(0), run = memo_run_qpc.exchange(0);
    const auto out = memo_out_qpc.exchange(0), hit_run = memo_hit_run_qpc.exchange(0);
    const auto settled = memo_settled_sum.exchange(0);
    log("memo shadow: calls=%llu hits=%llu (%.1f%%) MISMATCH=%llu first=%llu c610=%llu "
        "avg settled=%.0f | per call us: key=%.1f run=%.1f out=%.1f | hit share of run time=%.1f%%",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(hits),
        100.0 * static_cast<double>(hits) / static_cast<double>(calls),
        static_cast<unsigned long long>(mismatch), static_cast<unsigned long long>(first),
        static_cast<unsigned long long>(c610),
        static_cast<double>(settled) / static_cast<double>(calls),
        ms(key) * 1000.0 / static_cast<double>(calls), ms(run) * 1000.0 / static_cast<double>(calls),
        ms(out) * 1000.0 / static_cast<double>(calls),
        run ? 100.0 * static_cast<double>(hit_run) / static_cast<double>(run) : 0.0);
    char line[700];
    int length = std::snprintf(line, sizeof(line), "mismatch by part:");
    for (int k = 0; k < out_parts; ++k) {
        length += std::snprintf(line + length, sizeof(line) - length, " %s=%llu", out_part_names[k],
            static_cast<unsigned long long>(part_mismatch[k].exchange(0)));
    }
    length += std::snprintf(line + length, sizeof(line) - length, " | record qwords written (offset:runs):");
    for (int i = 0; i < 100 && length < 650; ++i) {
        const auto c = record_written_count[i].exchange(0);
        if (c) length += std::snprintf(line + length, sizeof(line) - length, " +0x%x:%u", i * 8, c);
    }
    log("%s", line);
}
