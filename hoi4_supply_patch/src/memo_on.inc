// memo_on.inc — mode=on / mode=verify of the supply Dijkstra memo (included after memo.inc).
//
// On a key hit the run is skipped and the saved post-run state is restored.
// What a run changes (both variants, static analysis + shadow): map +0x00 and map +0x20
// tables (+ count dwords +0x10 / +0x30; tables grow only rarely), node fields
// +0x18 +0x20 +0x38 +0x40 +0x48 +0x78 +0x88 +0x90 +0x94 +0x98, counter +0x144, visited list
// +0x148 (data, cap +0x150, size +0x154). Nothing outside the record (1412b62e0 is a lookup,
// the heap and scratch buffers are local).
// Restore preconditions (else the original runs): same table capacity (mask, probe qword) as
// the snapshot, same node count, visited capacity >= snapshot size. So nothing is ever
// allocated here; a run that would grow a container runs for real.
// mode=verify: on a key hit, restore, capture the state, roll back, run the original and
// compare raw bytes (record 800 B, both tables, nodes, visited).

struct Buffer {
    std::uint8_t* data = nullptr;
    std::size_t size = 0, capacity = 0;
    void assign(const void* source, std::size_t n) noexcept {
        if (n > capacity) {
            std::free(data);
            capacity = n + n / 4 + 64;
            data = static_cast<std::uint8_t*>(std::malloc(capacity));
            if (!data) { capacity = 0; size = 0; return; }
        }
        size = n;
        if (n && source) std::memcpy(data, source, n);
    }
    bool ok() const noexcept { return size == 0 || data; }
};

struct Snapshot {
    bool valid = false;
    Buffer map00, map20, nodes, visited;
    std::uint64_t q10 = 0, q18 = 0, q30 = 0, q38 = 0;
    std::int32_t c144 = 0, visited_size = 0, node_count = 0;
};
Snapshot snapshots[max_countries];

inline std::size_t table_bytes(const std::uint8_t* rec, std::uint32_t mask, std::uint32_t probe,
                               std::size_t stride) noexcept {
    return (static_cast<std::size_t>(field<std::uint32_t>(rec, mask)) + 1 +
            field<std::uint8_t>(rec, probe)) * stride;
}

template <class T> inline void put(std::uint8_t* base, std::uint32_t offset, T v) noexcept {
    std::memcpy(base + offset, &v, sizeof(T));
}

void save_snapshot(const std::uint8_t* rec, Snapshot& s) noexcept {
    s.valid = false;
    const auto* t00 = field<const std::uint8_t*>(rec, 0x08);
    const auto* t20 = field<const std::uint8_t*>(rec, 0x28);
    const auto* nodes = field<const std::uint8_t*>(rec, 0xb8);
    const auto* visited = field<const std::uint8_t*>(rec, 0x148);
    const auto node_count = field<std::int32_t>(rec, 0xc4);
    const auto visited_size = field<std::int32_t>(rec, 0x154);
    if (!t00 || !t20 || node_count < 0 || visited_size < 0 || (node_count && !nodes) ||
        (visited_size && !visited)) return;
    s.map00.assign(t00, table_bytes(rec, 0x14, 0x18, 0x18));
    s.map20.assign(t20, table_bytes(rec, 0x34, 0x38, 0xc));
    s.nodes.assign(nodes, static_cast<std::size_t>(node_count) * 0xe8);
    s.visited.assign(visited, static_cast<std::size_t>(visited_size) * 4);
    if (!s.map00.ok() || !s.map20.ok() || !s.nodes.ok() || !s.visited.ok()) return;
    s.q10 = field<std::uint64_t>(rec, 0x10);
    s.q18 = field<std::uint64_t>(rec, 0x18);
    s.q30 = field<std::uint64_t>(rec, 0x30);
    s.q38 = field<std::uint64_t>(rec, 0x38);
    s.c144 = field<std::int32_t>(rec, 0x144);
    s.visited_size = visited_size;
    s.node_count = node_count;
    s.valid = true;
}

bool can_restore(const std::uint8_t* rec, const Snapshot& s) noexcept {
    return s.valid && field<const std::uint8_t*>(rec, 0x08) && field<const std::uint8_t*>(rec, 0x28) &&
        field<std::uint32_t>(rec, 0x14) == static_cast<std::uint32_t>(s.q10 >> 32) &&
        field<std::uint64_t>(rec, 0x18) == s.q18 &&
        field<std::uint32_t>(rec, 0x34) == static_cast<std::uint32_t>(s.q30 >> 32) &&
        field<std::uint64_t>(rec, 0x38) == s.q38 &&
        field<std::int32_t>(rec, 0xc4) == s.node_count &&
        (s.node_count == 0 || field<const std::uint8_t*>(rec, 0xb8)) &&
        field<std::int32_t>(rec, 0x150) >= s.visited_size &&
        (s.visited_size == 0 || field<const std::uint8_t*>(rec, 0x148));
}

constexpr std::uint32_t node_ranges[][2] = {{0x18, 0x28}, {0x38, 0x50}, {0x78, 0x80}, {0x88, 0xa0}};

void restore(std::uint8_t* rec, const Snapshot& s) noexcept {
    if (s.map00.size) std::memcpy(field<std::uint8_t*>(rec, 0x08), s.map00.data, s.map00.size);
    if (s.map20.size) std::memcpy(field<std::uint8_t*>(rec, 0x28), s.map20.data, s.map20.size);
    put<std::uint32_t>(rec, 0x10, static_cast<std::uint32_t>(s.q10));
    put<std::uint32_t>(rec, 0x30, static_cast<std::uint32_t>(s.q30));
    auto* nodes = field<std::uint8_t*>(rec, 0xb8);
    for (std::int32_t i = 0; i < s.node_count; ++i) {
        auto* d = nodes + static_cast<std::size_t>(i) * 0xe8;
        const auto* from = s.nodes.data + static_cast<std::size_t>(i) * 0xe8;
        for (const auto& r : node_ranges) std::memcpy(d + r[0], from + r[0], r[1] - r[0]);
    }
    if (s.visited.size) std::memcpy(field<std::uint8_t*>(rec, 0x148), s.visited.data, s.visited.size);
    put<std::int32_t>(rec, 0x154, s.visited_size);
    put<std::int32_t>(rec, 0x144, s.c144);
}

// raw copy of everything a run or a restore may touch (no allocation changes in between)
struct RawState {
    std::uint8_t record[800];
    Buffer map00, map20, nodes, visited;
    void capture(const std::uint8_t* rec) noexcept {
        std::memcpy(record, rec, 800);
        map00.assign(field<const std::uint8_t*>(rec, 0x08), table_bytes(rec, 0x14, 0x18, 0x18));
        map20.assign(field<const std::uint8_t*>(rec, 0x28), table_bytes(rec, 0x34, 0x38, 0xc));
        nodes.assign(field<const std::uint8_t*>(rec, 0xb8),
            static_cast<std::size_t>(field<std::int32_t>(rec, 0xc4)) * 0xe8);
        const auto* v = field<const std::uint8_t*>(rec, 0x148);
        visited.assign(v, v ? static_cast<std::size_t>(field<std::int32_t>(rec, 0x150)) * 4 : 0);
    }
    void put_back(std::uint8_t* rec) const noexcept {
        std::memcpy(rec, record, 800);
        if (map00.size) std::memcpy(field<std::uint8_t*>(rec, 0x08), map00.data, map00.size);
        if (map20.size) std::memcpy(field<std::uint8_t*>(rec, 0x28), map20.data, map20.size);
        if (nodes.size) std::memcpy(field<std::uint8_t*>(rec, 0xb8), nodes.data, nodes.size);
        if (visited.size) std::memcpy(field<std::uint8_t*>(rec, 0x148), visited.data, visited.size);
    }
};
thread_local RawState raw_before, raw_restored, raw_real;

std::atomic<std::uint64_t> on_calls{0}, on_restored{0}, on_refused{0}, on_first{0}, on_verify_bad{0};
std::atomic<std::uint64_t> on_key_qpc{0}, on_restore_qpc{0}, on_run_qpc{0}, on_runs{0};
std::atomic<std::uint64_t> verify_part_bad[5];
std::atomic<int> verify_logged{0};

// bit mask of differing parts: 1 record, 2 map00, 4 map20, 8 nodes, 16 visited (up to size)
unsigned compare_raw(const RawState& a, const RawState& b, char* detail, std::size_t detail_size) noexcept {
    unsigned bad = 0;
    detail[0] = 0;
    for (int i = 0; i < 800; ++i) {
        if (a.record[i] != b.record[i]) {
            bad |= 1;
            std::snprintf(detail, detail_size, "record+0x%x ", i);
            break;
        }
    }
    auto same = [](const Buffer& x, const Buffer& y) {
        return x.size == y.size && (x.size == 0 || std::memcmp(x.data, y.data, x.size) == 0);
    };
    auto table_detail = [&](const Buffer& x, const Buffer& y, std::size_t stride, const char* name) {
        const auto n = x.size < y.size ? x.size : y.size;
        std::size_t diffs = 0, first = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (x.data[i] != y.data[i]) { if (first == n) first = i; ++diffs; }
        }
        const auto len = std::strlen(detail);
        if (first == n) {
            std::snprintf(detail + len, detail_size - len, "%s size %zu/%zu ", name, x.size, y.size);
            return;
        }
        const auto slot = first / stride, off = first % stride;
        const auto* ex = x.data + slot * stride;
        const auto* ey = y.data + slot * stride;
        std::uint64_t qx[3] = {}, qy[3] = {};
        std::memcpy(qx, ex, stride < 24 ? stride : 24);
        std::memcpy(qy, ey, stride < 24 ? stride : 24);
        std::snprintf(detail + len, detail_size - len,
            "%s slot%zu+0x%zx diffbytes=%zu restored=%016llx.%016llx.%016llx real=%016llx.%016llx.%016llx ",
            name, slot, off, diffs, static_cast<unsigned long long>(qx[0]),
            static_cast<unsigned long long>(qx[1]), static_cast<unsigned long long>(qx[2]),
            static_cast<unsigned long long>(qy[0]), static_cast<unsigned long long>(qy[1]),
            static_cast<unsigned long long>(qy[2]));
    };
    // tables: compared by meaning (probe bytes; hash/key/value of occupied slots)
    auto same_slots = [](const Buffer& x, const Buffer& y, const MapLayout& layout) {
        if (x.size != y.size) return false;
        for (std::size_t i = 0; i + layout.stride <= x.size; i += layout.stride) {
            const auto* ex = x.data + i;
            const auto* ey = y.data + i;
            if (ex[layout.probe] != ey[layout.probe]) return false;
            if (!ex[layout.probe]) continue;
            for (int r = 0; r < layout.range_count; ++r) {
                if (std::memcmp(ex + layout.ranges[r][0], ey + layout.ranges[r][0],
                        layout.ranges[r][1] - layout.ranges[r][0]) != 0) return false;
            }
        }
        return true;
    };
    if (!same_slots(a.map00, b.map00, map00_layout)) { bad |= 2; table_detail(a.map00, b.map00, 0x18, "map00"); }
    if (!same_slots(a.map20, b.map20, map20_layout)) { bad |= 4; table_detail(a.map20, b.map20, 0xc, "map20"); }
    if (!same(a.nodes, b.nodes)) {
        bad |= 8;
        const auto n = a.nodes.size < b.nodes.size ? a.nodes.size : b.nodes.size;
        for (std::size_t i = 0; i < n; ++i) {
            if (a.nodes.data[i] != b.nodes.data[i]) {
                const auto len = std::strlen(detail);
                std::snprintf(detail + len, detail_size - len, "node%zu+0x%zx ", i / 0xe8, i % 0xe8);
                break;
            }
        }
    }
    std::int32_t sa, sb;
    std::memcpy(&sa, a.record + 0x154, 4);
    std::memcpy(&sb, b.record + 0x154, 4);
    const auto bytes = static_cast<std::size_t>(sa < 0 ? 0 : sa) * 4;
    if (sa != sb || sa < 0 || bytes > a.visited.size || bytes > b.visited.size ||
        (bytes && std::memcmp(a.visited.data, b.visited.data, bytes) != 0)) bad |= 16;
    return bad;
}

void dijkstra_on(DijkstraFn original, std::uint32_t branch, std::uint8_t* rec,
                 std::uint8_t* system, std::uint8_t* sc, std::uint64_t* start,
                 std::uint32_t count, std::uintptr_t index) {
    CountryMemo& m = memo[index];
    Snapshot& s = snapshots[index];
    const auto t0 = qpc_now();
    const auto key = input_key(rec, system, sc, start, count, branch, m);
    const auto t1 = qpc_now();
    on_calls.fetch_add(1, std::memory_order_relaxed);
    on_key_qpc.fetch_add(static_cast<std::uint64_t>(t1 - t0), std::memory_order_relaxed);
    const bool hit = m.valid && key == m.key;
    if (hit && can_restore(rec, s)) {
        if (memo_mode == MemoMode::on) {
            restore(rec, s);
            on_restore_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t1), std::memory_order_relaxed);
            on_restored.fetch_add(1, std::memory_order_relaxed);
            return;  // key, settled set and snapshot stay valid
        }
        raw_before.capture(rec);
        restore(rec, s);
        raw_restored.capture(rec);
        raw_before.put_back(rec);
        on_restore_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t1), std::memory_order_relaxed);
        on_restored.fetch_add(1, std::memory_order_relaxed);
        const auto r0 = qpc_now();
        original(rec, system, sc, start, count);
        on_run_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - r0), std::memory_order_relaxed);
        on_runs.fetch_add(1, std::memory_order_relaxed);
        raw_real.capture(rec);
        char detail[400];
        const unsigned bad = compare_raw(raw_restored, raw_real, detail, sizeof(detail));
        if (bad) {
            on_verify_bad.fetch_add(1, std::memory_order_relaxed);
            for (int k = 0; k < 5; ++k) {
                if (bad & (1u << k)) verify_part_bad[k].fetch_add(1, std::memory_order_relaxed);
            }
            if (verify_logged.fetch_add(1) < 40) {
                log("VERIFY MISMATCH country=%llu branch=%u hour=%ld parts=0x%x %s",
                    static_cast<unsigned long long>(index), branch,
                    static_cast<long>(current_hours()), bad, detail);
            }
        }
    } else {
        if (hit) on_refused.fetch_add(1, std::memory_order_relaxed);
        if (!m.valid) on_first.fetch_add(1, std::memory_order_relaxed);
        const auto r0 = qpc_now();
        original(rec, system, sc, start, count);
        on_run_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - r0), std::memory_order_relaxed);
        on_runs.fetch_add(1, std::memory_order_relaxed);
    }
    save_snapshot(rec, s);
    collect_settled(rec, m);
    m.valid = s.valid;
    m.key = key;
    m.branch = branch;
}

void on_report() noexcept {
    const auto calls = on_calls.exchange(0);
    if (!calls) return;
    const auto restored = on_restored.exchange(0), refused = on_refused.exchange(0);
    const auto first = on_first.exchange(0), bad = on_verify_bad.exchange(0);
    const auto key = on_key_qpc.exchange(0), rest = on_restore_qpc.exchange(0);
    const auto run = on_run_qpc.exchange(0), runs = on_runs.exchange(0);
    std::uint64_t parts[5];
    for (int k = 0; k < 5; ++k) parts[k] = verify_part_bad[k].exchange(0);
    log("memo %s: calls=%llu restored=%llu (%.1f%%) refused=%llu first=%llu VERIFY_BAD=%llu "
        "(record=%llu map00=%llu map20=%llu nodes=%llu visited=%llu) | us: key/call=%.1f "
        "restore/restored=%.1f run/run=%.1f",
        memo_mode == MemoMode::on ? "on" : "verify",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(restored),
        100.0 * static_cast<double>(restored) / static_cast<double>(calls),
        static_cast<unsigned long long>(refused), static_cast<unsigned long long>(first),
        static_cast<unsigned long long>(bad),
        static_cast<unsigned long long>(parts[0]), static_cast<unsigned long long>(parts[1]),
        static_cast<unsigned long long>(parts[2]), static_cast<unsigned long long>(parts[3]),
        static_cast<unsigned long long>(parts[4]),
        ms(key) * 1000.0 / static_cast<double>(calls),
        restored ? ms(rest) * 1000.0 / static_cast<double>(restored) : 0.0,
        runs ? ms(run) * 1000.0 / static_cast<double>(runs) : 0.0);
    const double c = static_cast<double>(calls);
    const auto slots = key_slots.exchange(0);
    log("memo key parts us/call: maps=%.1f arrays=%.1f sc=%.1f edges=%.1f | avg slots map00=%.0f map20=%.0f settled=%.0f",
        ms(key_part_qpc[0].exchange(0)) * 1000.0 / c, ms(key_part_qpc[1].exchange(0)) * 1000.0 / c,
        ms(key_part_qpc[2].exchange(0)) * 1000.0 / c, ms(key_part_qpc[3].exchange(0)) * 1000.0 / c,
        static_cast<double>(slots & 0xffffffffu) / c, static_cast<double>(slots >> 32) / c,
        static_cast<double>(key_settled.exchange(0)) / c);
    log("memo key array bytes/call: b8=%.0f d0=%.0f 230=%.0f 248=%.0f 260=%.0f 278=%.0f 2c0=%.0f 2f0=%.0f",
        key_array_bytes[0].exchange(0) / c, key_array_bytes[1].exchange(0) / c, key_array_bytes[2].exchange(0) / c,
        key_array_bytes[3].exchange(0) / c, key_array_bytes[4].exchange(0) / c, key_array_bytes[5].exchange(0) / c,
        key_array_bytes[6].exchange(0) / c, key_array_bytes[7].exchange(0) / c);
}
