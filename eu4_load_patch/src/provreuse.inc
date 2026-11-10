// ---- provreuse: keep the province modifier cache's buffers across CalculateModifier
// 1409f9500 builds a default (empty) cache, move-assigns it into province+0x988 with 0x9F95D0
// (freeing the old cache's strings and vectors) and then refills the cache in place
// (140960330), allocating the same buffers again. This hook (the call at 0x9F9571 only) takes
// the old buffers out before the move (the game's move then frees nothing, as in provfree),
// destroys the strings inside the vectors of strings, lets the game move the empty cache in,
// and puts the old buffers back as empty containers with their capacity: vectors
// begin=end=old begin, strings size 0 in the old heap buffer. The refill then reuses them
// instead of allocating. Only the containers provfree checks (same layout checks) are kept;
// a container that is not empty after the move keeps the game's value and ours is freed.
constexpr std::uintptr_t provreuse_call_rva = 0x9F9571;   // call 0x9F95D0 in 1409f9500
constexpr std::uintptr_t provreuse_owner_bytes_rva = 0x9F956A;  // lea rcx,[rdi+0x988]; call
Stats provreuse_stats;
std::atomic<long long> provreuse_vectors{0}, provreuse_strings{0}, provreuse_inner{0},
    provreuse_dropped{0};

struct Kept {
    std::uint32_t at;
    std::uint8_t kind;  // 0 string, 1 vector<string>, 2 vector<8>, 3 vector<4>
    std::uint64_t ptr, cap;  // strings: capacity in chars; vectors: end-of-storage pointer
};

struct Keeper {
    unsigned char* base;
    std::vector<Kept>& out;
    void string(std::size_t at) {
        unsigned char* s = base + at;
        const std::uint64_t cap = rd<std::uint64_t>(s, 0x18);
        if (cap <= 15) return;
        out.push_back({static_cast<std::uint32_t>(at), 0, rd<std::uint64_t>(s, 0), cap});
        const std::uint64_t empty[4] = {0, 0, 0, 15};
        std::memcpy(s, empty, sizeof(empty));
    }
    void vector(std::size_t at, std::uint8_t kind) {
        unsigned char* v = base + at;
        const std::uint64_t b = rd<std::uint64_t>(v, 0);
        if (b == 0) return;
        if (kind == 1) {
            // destroy the strings, keep the array
            const std::uint64_t e = rd<std::uint64_t>(v, 8);
            for (std::uint64_t p = b; p < e; p += 32) free_string(reinterpret_cast<void*>(p));
            provreuse_inner.fetch_add(static_cast<long long>((e - b) / 32), std::memory_order_relaxed);
        }
        out.push_back({static_cast<std::uint32_t>(at), kind, b, rd<std::uint64_t>(v, 0x10)});
        std::memset(v, 0, 24);
    }
    void list(std::size_t at) {
        string(at + 0x18);
        vector(at + 0x38, 2);
    }
    void blist(std::size_t at) {
        list(at);
        vector(at + 0xC8, 3);
        vector(at + 0xE0, 1);
        vector(at + 0xF8, 1);
        string(at + 0x110);
        string(at + 0x130);
        string(at + 0x158);
    }
    void all() {
        string(kTopString);
        for (std::size_t a : kAListOffsets) list(a);
        for (std::size_t k = 0; k < kBListCount; ++k) blist(kBListFirst + k * kBListSize);
        for (std::size_t a : kBListExtra) blist(a);
    }
};

void provreuse_drop(const Kept& k) {
    FreeItem item{};
    item.kind = k.kind;
    if (k.kind == 0) {
        item.a = k.ptr;
        item.c = 0;
        item.d = k.cap;
    } else {
        item.a = k.ptr;
        item.b = k.ptr;
        item.c = k.cap;
    }
    free_item(item);
    provreuse_dropped.fetch_add(1, std::memory_order_relaxed);
}

thread_local std::vector<Kept>* provreuse_local = nullptr;
thread_local bool provreuse_bypass = false;  // verify: the second, ordinary computation
bool provreuse_verify = false;
bool provreuse_verify0 = false;  // control: both computations the ordinary way

void* provreuse_hook(void* dest, void* src) {
    Stats& s = provreuse_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    if (s.disabled.load(std::memory_order_relaxed) || dest == src || provreuse_bypass) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return provfree_next(dest, src);
    }
    std::vector<FreeItem> unused;
    Taker check{static_cast<unsigned char*>(dest), unused};
    check.all(false);
    if (!check.ok) {
        if (provfree_refused.fetch_add(1) < 5) log("provreuse: a container of %p has an unexpected shape; left to the game", dest);
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return provfree_next(dest, src);
    }
    if (!provreuse_local) provreuse_local = new std::vector<Kept>();
    std::vector<Kept>& kept = *provreuse_local;
    kept.clear();
    Keeper keeper{static_cast<unsigned char*>(dest), kept};
    keeper.all();
    void* result = provfree_next(dest, src);
    auto* base = static_cast<unsigned char*>(dest);
    long long vectors = 0, strings = 0;
    for (const Kept& k : kept) {
        unsigned char* p = base + k.at;
        if (k.kind == 0) {
            if (rd<std::uint64_t>(p, 0x18) != 15 || rd<std::uint64_t>(p, 0x10) != 0) {
                provreuse_drop(k);
                continue;
            }
            const std::uint64_t value[4] = {k.ptr, 0, 0, k.cap};
            std::memcpy(p, value, sizeof(value));
            *reinterpret_cast<char*>(k.ptr) = 0;
            ++strings;
        } else {
            if (rd<std::uint64_t>(p, 0) != 0 || rd<std::uint64_t>(p, 8) != 0 || rd<std::uint64_t>(p, 0x10) != 0) {
                provreuse_drop(k);
                continue;
            }
            const std::uint64_t value[3] = {k.ptr, k.ptr, k.cap};
            std::memcpy(p, value, sizeof(value));
            ++vectors;
        }
    }
    s.ours.fetch_add(1, std::memory_order_relaxed);
    provreuse_vectors.fetch_add(vectors, std::memory_order_relaxed);
    provreuse_strings.fetch_add(strings, std::memory_order_relaxed);
    return result;
}


// ---- verify: CalculateModifier twice in the monthly province loop (call 0x9F38D4), first with
// reuse, then the ordinary way, and compare the caches: every byte outside the containers this
// hook touches or that hold heap pointers, plus the contents of all containers.
constexpr std::uintptr_t provreuse_verify_call_rva = 0x9F38D4;
constexpr std::uintptr_t provreuse_calc_rva = 0x9F9500;
constexpr std::size_t kCacheBytes = 0x2468;
using CalcFn = void (*)(void*);
std::atomic<long long> verify_calls{0}, verify_mismatches{0}, verify_other{0};

struct Snapshot {
    std::vector<std::uint8_t> raw;
    std::vector<std::pair<std::uint32_t, std::string>> parts;
};

std::string string_bytes(const unsigned char* s) {
    const std::uint64_t size = rd<std::uint64_t>(s, 0x10), cap = rd<std::uint64_t>(s, 0x18);
    if (size > cap || size > (1u << 20)) return "<bad string>";
    const char* data = cap > 15 ? rd<const char*>(s, 0) : reinterpret_cast<const char*>(s);
    return data ? std::string(data, size) : std::string("<null>");
}
std::string vector_bytes(const unsigned char* v) {
    const std::uint64_t b = rd<std::uint64_t>(v, 0), e = rd<std::uint64_t>(v, 8);
    if (b == 0) return std::string();
    if (e < b || e - b > (1u << 24)) return "<bad vector>";
    return std::string(reinterpret_cast<const char*>(b), e - b);
}

void snapshot(const unsigned char* c, Snapshot& out) {
    out.raw.assign(c, c + kCacheBytes);
    out.parts.clear();
    auto mask = [&](std::size_t at, std::size_t n) { std::memset(out.raw.data() + at, 0, n); };
    auto str = [&](std::size_t at) { mask(at, 32); out.parts.push_back({static_cast<std::uint32_t>(at), string_bytes(c + at)}); };
    auto vec = [&](std::size_t at) { mask(at, 24); out.parts.push_back({static_cast<std::uint32_t>(at), vector_bytes(c + at)}); };
    auto vecstr = [&](std::size_t at) {
        mask(at, 24);
        std::string all;
        const std::uint64_t b = rd<std::uint64_t>(c + at, 0), e = rd<std::uint64_t>(c + at, 8);
        if (b && e >= b && e - b < (1u << 20)) {
            for (std::uint64_t p = b; p < e; p += 32) {
                all += string_bytes(reinterpret_cast<const unsigned char*>(p));
                all += '\0';
            }
        }
        out.parts.push_back({static_cast<std::uint32_t>(at), all});
    };
    auto list = [&](std::size_t at) {
        str(at + 0x18);
        vec(at + 0x38);
        // small vector: ptr +0x58 (inline storage +0x68..+0xA8), count +0x64, 8-byte entries
        const std::uint32_t count = rd<std::uint32_t>(c + at, 0x64);
        const auto* data = rd<const char*>(c + at, 0x58);
        mask(at + 0x58, 8);
        mask(at + 0x68, 0x40);
        out.parts.push_back({static_cast<std::uint32_t>(at + 0x58),
            data && count < 4096 ? std::string(data, count * 8u) : std::string("<bad small>")});
        vec(at + 0xA8);
    };
    auto blist = [&](std::size_t at) {
        list(at);
        vec(at + 0xC8);
        vecstr(at + 0xE0);
        vecstr(at + 0xF8);
        str(at + 0x110);
        str(at + 0x130);
        str(at + 0x158);
    };
    str(kTopString);
    for (std::size_t a : kAListOffsets) list(a);
    for (std::size_t k = 0; k < kBListCount; ++k) blist(kBListFirst + k * kBListSize);
    for (std::size_t a : kBListExtra) blist(a);
}

void provreuse_verify_hook(void* province) {
    auto calc = reinterpret_cast<CalcFn>(image + provreuse_calc_rva);
    provreuse_bypass = provreuse_verify0;
    calc(province);
    provreuse_bypass = false;
    const long long n = verify_calls.fetch_add(1, std::memory_order_relaxed);
    if (n % 4 != 0) return;  // every 4th province
    const auto* cache = static_cast<const unsigned char*>(province) + 0x988;
    Snapshot a, b;
    snapshot(cache, a);
    provreuse_bypass = true;
    calc(province);
    provreuse_bypass = false;
    snapshot(cache, b);
    int bad_raw = -1;
    for (std::size_t i = 0; i < kCacheBytes; ++i) {
        if (a.raw[i] != b.raw[i]) { bad_raw = static_cast<int>(i); break; }
    }
    int bad_part = -1;
    bool other = false;
    char offs[160]{};
    std::size_t used = 0;
    for (std::size_t i = 0; i < a.parts.size() && i < b.parts.size(); ++i) {
        if (a.parts[i].second != b.parts[i].second) {
            // 0x48 / 0xB8 (vector and shared pointers of the list at +0x10) point to objects
            // created anew by every computation; they differ between two ordinary runs too
            if (a.parts[i].first != 0x48 && a.parts[i].first != 0xB8) other = true;
            if (bad_part < 0) bad_part = static_cast<int>(a.parts[i].first);
            if (used < sizeof(offs) - 24) {
                used += std::snprintf(offs + used, sizeof(offs) - used, " %x(%zu/%zu)", a.parts[i].first,
                    a.parts[i].second.size(), b.parts[i].second.size());
            }
        }
    }
    if (bad_raw >= 0 || a.parts.size() != b.parts.size()) other = true;
    if (other) {
        const long long m = verify_other.fetch_add(1, std::memory_order_relaxed);
        if (m < 20) log("provreuse verify OTHER MISMATCH province=%p raw_offset=0x%x parts:%s", province, bad_raw, offs);
    }
    if (bad_raw >= 0 || bad_part >= 0 || a.parts.size() != b.parts.size()) {
        const long long m = verify_mismatches.fetch_add(1, std::memory_order_relaxed);
        if (m < 20) log("provreuse verify MISMATCH province=%p raw_offset=0x%x parts:%s", province, bad_raw, offs);
    }
}

void provreuse_report() {
    report("provreuse", provreuse_stats);
    log("provreuse vectors=%lld strings=%lld inner_strings_freed=%lld dropped=%lld refused=%lld",
        provreuse_vectors.load(), provreuse_strings.load(), provreuse_inner.load(), provreuse_dropped.load(),
        provfree_refused.load());
    if (provreuse_verify) {
        log("provreuse verify calls=%lld mismatches=%lld other_mismatches=%lld", verify_calls.load(),
            verify_mismatches.load(), verify_other.load());
    }
}

// needs the provfree layout checks and free functions (install_provfree with mode off for the hook)
bool install_provreuse() {
    const std::uint8_t owner[] = {0x48, 0x8D, 0x8F, 0x88, 0x09, 0x00, 0x00, 0xE8};
    if (std::memcmp(reinterpret_cast<const void*>(image + provreuse_owner_bytes_rva), owner, sizeof(owner)) != 0) {
        log("provreuse REFUSED: call site bytes differ");
        return false;
    }
    provfree_next = reinterpret_cast<ProvAssignFn>(image + provfree_rva);
    if (provreuse_verify) {
        const std::uint8_t verify_site[] = {0x49, 0x8B, 0xCD, 0xE8};  // mov rcx,r13; call
        if (std::memcmp(reinterpret_cast<const void*>(image + provreuse_verify_call_rva - 3), verify_site, 4) != 0 ||
            !hook_call(provreuse_verify_call_rva, provreuse_calc_rva,
                reinterpret_cast<const void*>(&provreuse_verify_hook), "provreuse verify")) {
            log("provreuse verify REFUSED");
            return false;
        }
    }
    return hook_call(provreuse_call_rva, provfree_rva, reinterpret_cast<const void*>(&provreuse_hook), "provreuse");
}
