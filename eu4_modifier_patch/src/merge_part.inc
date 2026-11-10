// ---- merge: modifier list merge with an index (port of Bellows modifier_merge, MIT, Happy Dwarf Team)
// Included into eu4_modifier_patch.cpp inside its anonymous namespace. The entry hook of
// 0x8EA420 is this plugin's add_hook; original_add (its trampoline) is the game's function.
enum class MergeMode { off, shadow, on };
MergeMode merge_mode = MergeMode::off;
struct MergeStats {
    std::atomic<long long> calls{0}, ours{0}, skips{0}, compared{0}, mismatches{0}, sequence{0};
    std::atomic<long long> original_ticks{0};
    std::atomic<bool> disabled{false};
};
template <class T> T rd(const void* base, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}
inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
struct MergeTimer {
    std::atomic<long long>& sink;
    std::int64_t start = qpc_now();
    explicit MergeTimer(std::atomic<long long>& s) : sink(s) {}
    ~MergeTimer() { sink.fetch_add(qpc_now() - start, std::memory_order_relaxed); }
};
constexpr std::uintptr_t merge_rva = 0x8EA420;
constexpr std::uintptr_t merge_new_rva = 0x1A332D4;
constexpr std::uintptr_t merge_emplace_rva = 0x8F0E0;
constexpr std::uint8_t merge_bytes[] = {0x40, 0x55, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x48, 0x4C, 0x63, 0x4A, 0x64,
    0x45, 0x33, 0xC0, 0x4C, 0x89, 0x7C, 0x24, 0x20};
constexpr std::uint8_t merge_new_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t merge_emplace_bytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x20};
constexpr std::int32_t mod_static_end = 0x286;
constexpr std::int32_t mod_max_id = 1 << 16;
void* (*merge_new)(std::size_t) = nullptr;
void* (*merge_emplace)(void*, void*, void**) = nullptr;
MergeStats merge_stats;
std::atomic<long long> merge_fast_ticks{0}, merge_refused{0};

struct ModSource {
    bool dense = false;
    const std::int32_t* entries = nullptr;
    std::int32_t count = 0;
};
struct ModIndex {
    std::vector<std::uint32_t> stamp = std::vector<std::uint32_t>(mod_max_id, 0);
    std::vector<std::int32_t> slot = std::vector<std::int32_t>(mod_max_id, 0);
    std::uint32_t generation = 0;
    void next() {
        if (++generation == 0) {
            std::fill(stamp.begin(), stamp.end(), 0u);
            generation = 1;
        }
    }
};
thread_local ModIndex* merge_index = nullptr;

inline std::int32_t add32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
inline std::int32_t source_value(const ModSource& src, std::int32_t i, std::int32_t id) {
    if (src.dense && id <= mod_static_end) return src.entries[2 * static_cast<std::int64_t>(id) + 1];
    return src.entries[2 * static_cast<std::int64_t>(i) + 1];
}

struct GameModList {  // dense +0x10, entries +0x58, capacity +0x60, count +0x64, release object +0x50
    unsigned char* base;
    bool dense() const { return rd<std::uint8_t>(base, 0x10) != 0; }
    std::int32_t* entries() const { return rd<std::int32_t*>(base, 0x58); }
    std::int32_t count() const { return rd<std::int32_t>(base, 0x64); }
    std::int32_t capacity() const { return rd<std::int32_t>(base, 0x60); }
    void set_count(std::int32_t c) const { std::memcpy(base + 0x64, &c, 4); }
    void grow(std::int32_t cap) const {
        auto* fresh = static_cast<std::int32_t*>(merge_new(static_cast<std::uint64_t>(static_cast<std::uint32_t>(cap)) * 8));
        const std::int32_t n = count();
        if (n > 0) std::memcpy(fresh, entries(), static_cast<std::size_t>(n) * 8);
        unsigned char* holder = base + 0x50;
        const auto* vtable = rd<void* const*>(holder, 0);
        reinterpret_cast<void (*)(void*)>(vtable[2])(holder);
        std::memcpy(base + 0x58, &fresh, 8);
        std::memcpy(base + 0x60, &cap, 4);
    }
};
struct CopyModList {
    bool is_dense = false;
    std::vector<std::int32_t> data;
    std::int32_t n = 0;
    bool dense() const { return is_dense; }
    std::int32_t* entries() { return data.data(); }
    const std::int32_t* entries() const { return data.data(); }
    std::int32_t count() const { return n; }
    std::int32_t capacity() const { return static_cast<std::int32_t>(data.size() / 2); }
    void set_count(std::int32_t c) { n = c; }
    void grow(std::int32_t cap) { data.resize(2 * static_cast<std::size_t>(cap), 0); }
};

template <class L> void mod_append(L& dest, std::int32_t id, std::int32_t value) {
    const std::int32_t count = dest.count();
    if (count + 1 > dest.capacity()) {
        const std::int32_t doubled = dest.capacity() * 2;
        dest.grow(count + 1 > doubled ? count + 1 : doubled);
    }
    std::int32_t* e = dest.entries();
    e[2 * static_cast<std::int64_t>(count)] = id;
    e[2 * static_cast<std::int64_t>(count) + 1] = value;
    dest.set_count(count + 1);
}

template <class L> bool merge_applies(const L& dest, const ModSource& src) {
    if (!src.entries || src.count <= 0) return false;
    for (std::int32_t i = 0; i < src.count; ++i) {
        const std::int32_t id = src.entries[2 * static_cast<std::int64_t>(i)];
        if (id < 0 || id >= mod_max_id) return false;
    }
    if (dest.dense()) {
        const std::int32_t* e = dest.entries();
        const std::int32_t end = dest.count() < mod_static_end ? dest.count() : mod_static_end;
        for (std::int32_t j = 0; j < end; ++j) {
            if (e[2 * static_cast<std::int64_t>(j)] > mod_static_end) return false;
        }
    }
    return true;
}

template <class L> std::int64_t merge_work(const L& dest, const ModSource& src) {
    std::int64_t scanned = 0;
    for (std::int32_t i = 0; i < src.count; ++i) {
        const std::int32_t id = src.entries[2 * static_cast<std::int64_t>(i)];
        if (id != mod_static_end && (!dest.dense() || id >= mod_static_end)) ++scanned;
    }
    return scanned * dest.count() + scanned * (scanned - 1) / 2;
}

template <class L> void merge_fast(L& dest, const ModSource& src, ModIndex& index) {
    index.next();
    const std::uint32_t gen = index.generation;
    {
        const std::int32_t* e = dest.entries();
        const std::int32_t count = dest.count();
        for (std::int32_t j = 0; j < count; ++j) {
            const std::int32_t id = e[2 * static_cast<std::int64_t>(j)];
            if (id < 0 || id >= mod_max_id || index.stamp[id] == gen) continue;  // the first entry counts
            index.stamp[id] = gen;
            index.slot[id] = j;
        }
    }
    for (std::int32_t i = 0; i < src.count; ++i) {
        const std::int32_t id = src.entries[2 * static_cast<std::int64_t>(i)];
        const std::int32_t value = source_value(src, i, id);
        if (id == mod_static_end) continue;
        std::int32_t* e = dest.entries();
        if (dest.dense() && id < mod_static_end) {
            e[2 * static_cast<std::int64_t>(id)] = id;
            e[2 * static_cast<std::int64_t>(id) + 1] = add32(e[2 * static_cast<std::int64_t>(id) + 1], value);
            continue;
        }
        if (index.stamp[id] == gen) {
            const std::int64_t j = index.slot[id];
            e[2 * j + 1] = add32(e[2 * j + 1], value);
            continue;
        }
        const std::int32_t at = dest.count();
        mod_append(dest, id, value);
        index.stamp[id] = gen;
        index.slot[id] = at;
    }
}

void merge_record_source(unsigned char* dest, void* src) {
    auto* end = rd<void**>(dest, 0x40);
    if (end != rd<void**>(dest, 0x48)) {
        *end = src;
        ++end;
        std::memcpy(dest + 0x40, &end, 8);
    } else {
        void* value = src;
        merge_emplace(dest + 0x38, end, &value);
    }
}

void* merge_hook(void* dest_ptr, void* src_ptr) {
    MergeStats& s = merge_stats;
    const long long n = s.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & 0x3FFFF) == 0) merge_report();
    auto* dest = static_cast<unsigned char*>(dest_ptr);
    const auto* src_base = static_cast<const unsigned char*>(src_ptr);
    if (dest_ptr == src_ptr || s.disabled.load(std::memory_order_relaxed)) return original_add(dest_ptr, src_ptr);
    const ModSource src{rd<std::uint8_t>(src_base, 0x10) != 0, rd<const std::int32_t*>(src_base, 0x58),
        rd<std::int32_t>(src_base, 0x64)};
    GameModList list{dest};
    if (src.count <= 0 || !src.entries || merge_work(list, src) < 4096) {
        return original_add(dest_ptr, src_ptr);
    }
    if (!merge_applies(list, src)) {
        merge_refused.fetch_add(1, std::memory_order_relaxed);
        return original_add(dest_ptr, src_ptr);
    }
    ModIndex* index = nullptr;
    CopyModList copy;
    const long long sequence = s.sequence.fetch_add(1, std::memory_order_relaxed);
    const bool check = merge_mode == MergeMode::shadow || (sequence & 1023) == 0;
    try {
        if (!merge_index) merge_index = new ModIndex;
        index = merge_index;
        if (check) {
            copy.is_dense = list.dense();
            copy.n = list.count();
            copy.data.assign(2 * static_cast<std::size_t>(list.capacity()), 0);
            if (list.capacity() > 0) std::memcpy(copy.data.data(), list.entries(), copy.data.size() * 4);
            merge_fast(copy, src, *index);
        }
    } catch (...) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return original_add(dest_ptr, src_ptr);
    }
    if (check) {
        s.compared.fetch_add(1, std::memory_order_relaxed);
        void* result;
        {
            MergeTimer t(s.original_ticks);
            result = original_add(dest_ptr, src_ptr);
        }
        const bool same = list.count() == copy.n && list.capacity() == copy.capacity() &&
            (copy.n == 0 || std::memcmp(list.entries(), copy.data.data(), static_cast<std::size_t>(copy.n) * 8) == 0);
        if (!same) {
            if (s.mismatches.fetch_add(1) < 10) log("MERGE MISMATCH count %d vs %d", list.count(), copy.n);
            if (merge_mode == MergeMode::on) s.disabled.store(true);
        }
        return result;
    }
    const std::int64_t start = qpc_now();
    merge_fast(list, src, *index);
    merge_record_source(dest, src_ptr);
    merge_fast_ticks.fetch_add(qpc_now() - start, std::memory_order_relaxed);
    s.ours.fetch_add(1, std::memory_order_relaxed);
    return dest_ptr;
}

// operator new is only called here; eu4_alloc_patch (direct=1) may have replaced its entry
// with "mov rax, hook; jmp rax", which is still a valid operator new
bool alloc_hooked(std::uintptr_t entry_address) {
    const auto* entry = reinterpret_cast<const std::uint8_t*>(entry_address);
    return entry[0] == 0x48 && entry[1] == 0xB8 && entry[10] == 0xFF && entry[11] == 0xE0;
}

bool check_merge(std::uintptr_t image) {
    if (std::memcmp(reinterpret_cast<void*>(image + merge_rva), merge_bytes, sizeof(merge_bytes)) != 0 ||
        (std::memcmp(reinterpret_cast<void*>(image + merge_new_rva), merge_new_bytes, sizeof(merge_new_bytes)) != 0 &&
            !alloc_hooked(image + merge_new_rva)) ||
        std::memcmp(reinterpret_cast<void*>(image + merge_emplace_rva), merge_emplace_bytes,
            sizeof(merge_emplace_bytes)) != 0) {
        log("merge REFUSED: bytes differ");
        return false;
    }
    merge_new = reinterpret_cast<void* (*)(std::size_t)>(image + merge_new_rva);
    merge_emplace = reinterpret_cast<void* (*)(void*, void*, void**)>(image + merge_emplace_rva);
    return true;
}

void merge_report() noexcept {
    const auto f = static_cast<double>(qpc_frequency);
    log("merge mode=%s calls=%lld ours=%lld refused=%lld skips=%lld compared=%lld mismatches=%lld "
        "disabled=%d fast_ms=%.1f original_checked_ms=%.1f",
        merge_mode == MergeMode::on ? "on" : "shadow", merge_stats.calls.load(), merge_stats.ours.load(),
        merge_refused.load(), merge_stats.skips.load(), merge_stats.compared.load(),
        merge_stats.mismatches.load(), merge_stats.disabled.load() ? 1 : 0,
        merge_fast_ticks.load() * 1000.0 / f, merge_stats.original_ticks.load() * 1000.0 / f);
}
