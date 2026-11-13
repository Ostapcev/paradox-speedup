// Module "tags": country tag -> link index lookup 1401f0a90(links, const std::string& tag).
// The game scans links+0x358 (std::string[32 B], count +0x364) linearly with memcmp and returns
// the FIRST equal index (not found: debug assert, 0; count 0: 140763e10). During a save load
// it is called for every tag reference (~0.3 s).
// on: a hash table string -> first index, built for (links, data pointer, count). A hit is
// re-checked against the live array (data[i] == tag); anything else (table stale, no entry,
// check failed, count 0) runs the original. Every 256th hit is checked with the full scan; a
// mismatch disables the module. shadow: computes the table answer and compares with the
// original for every call. The function entry is replaced by "mov rax, hook; jmp rax" and the
// original runs from a trampoline (its first 13 bytes + jmp back).

enum class TagsMode { off, shadow, on };

constexpr std::uintptr_t tags_rva = 0x1f0a90;
// 40 56 push rsi | 48 83 ec 50 sub rsp,50 | 48 63 81 64030000 movsxd rax,[rcx+364] = 13 bytes
constexpr std::uint8_t tags_prologue[13] = {0x40, 0x56, 0x48, 0x83, 0xec, 0x50, 0x48, 0x63,
                                            0x81, 0x64, 0x03, 0x00, 0x00};

struct GameString { union { char buf[16]; char* ptr; }; std::uint64_t size; std::uint64_t cap; };
static_assert(sizeof(GameString) == 32, "std::string layout");
inline const char* str_data(const GameString* s) noexcept { return s->cap > 15 ? s->ptr : s->buf; }

using TagFn = std::uint64_t (*)(std::uint8_t*, const GameString*);
TagFn tags_original = nullptr;
TagsMode tags_mode = TagsMode::off;

struct TagTable {
    const std::uint8_t* owner;
    const GameString* data;
    std::int32_t count;
    std::uint32_t mask;
    std::int32_t* slots;  // -1 empty, else index
    std::uint64_t* hashes;
};
std::atomic<TagTable*> tag_table{nullptr};
std::atomic<bool> tag_stale{true};
std::atomic<bool> tags_disabled{false};
SRWLOCK tag_lock = SRWLOCK_INIT;

std::atomic<std::uint64_t> tag_calls{0}, tag_hits{0}, tag_fallbacks{0}, tag_rebuilds{0},
    tag_check_fail{0}, tag_mismatch{0}, tag_samples{0}, tag_qpc{0};
std::atomic<std::int32_t> tag_max_count{0};
std::atomic<int> tag_logged{0};

inline std::uint64_t tag_hash(const char* p, std::uint64_t n) noexcept {
    std::uint64_t h = 1469598103934665603ull ^ n;
    for (std::uint64_t i = 0; i < n; ++i) { h ^= static_cast<std::uint8_t>(p[i]); h *= 1099511628211ull; }
    return h | 1;  // 0 never used
}

inline bool same_string(const GameString* a, const char* p, std::uint64_t n) noexcept {
    return a->size == n && (n == 0 || std::memcmp(str_data(a), p, n) == 0);
}

TagTable* build_table(const std::uint8_t* owner, const GameString* data, std::int32_t count) noexcept {
    std::uint32_t size = 16;
    while (size < static_cast<std::uint32_t>(count) * 2) size *= 2;
    auto* t = static_cast<TagTable*>(std::malloc(sizeof(TagTable)));
    auto* slots = static_cast<std::int32_t*>(std::malloc(size * sizeof(std::int32_t)));
    auto* hashes = static_cast<std::uint64_t*>(std::calloc(size, sizeof(std::uint64_t)));
    if (!t || !slots || !hashes) { std::free(t); std::free(slots); std::free(hashes); return nullptr; }
    std::memset(slots, 0xff, size * sizeof(std::int32_t));
    *t = TagTable{owner, data, count, size - 1, slots, hashes};
    for (std::int32_t i = 0; i < count; ++i) {
        const auto* s = data + i;
        const char* p = str_data(s);
        const auto h = tag_hash(p, s->size);
        std::uint32_t j = static_cast<std::uint32_t>(h) & t->mask;
        bool duplicate = false;
        while (slots[j] >= 0) {
            if (hashes[j] == h && same_string(data + slots[j], p, s->size)) { duplicate = true; break; }
            j = (j + 1) & t->mask;
        }
        if (!duplicate) { slots[j] = i; hashes[j] = h; }  // first index wins, like the scan
    }
    return t;
}

// -1 = no answer (use the original)
std::int32_t table_lookup(std::uint8_t* links, const GameString* tag) noexcept {
    const auto* data = *reinterpret_cast<GameString* const*>(links + 0x358);
    const auto count = *reinterpret_cast<const std::int32_t*>(links + 0x364);
    if (count <= 0) return -1;
    TagTable* t = tag_table.load(std::memory_order_acquire);
    if (!t || t->owner != links || t->data != data || t->count != count || tag_stale.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&tag_lock);
        t = tag_table.load(std::memory_order_relaxed);
        if (!t || t->owner != links || t->data != data || t->count != count || tag_stale.load()) {
            TagTable* fresh = build_table(links, data, count);
            if (fresh) {
                tag_table.store(fresh, std::memory_order_release);  // old tables are leaked on purpose
                tag_stale = false;                                   // (readers may still use them)
                tag_rebuilds.fetch_add(1, std::memory_order_relaxed);
                if (count > tag_max_count.load()) tag_max_count = count;
            }
            t = fresh;
        }
        ReleaseSRWLockExclusive(&tag_lock);
        if (!t) return -1;
    }
    const char* p = str_data(tag);
    const auto n = tag->size;
    const auto h = tag_hash(p, n);
    std::uint32_t j = static_cast<std::uint32_t>(h) & t->mask;
    while (t->slots[j] >= 0) {
        if (t->hashes[j] == h) {
            const auto i = t->slots[j];
            if (i < count && same_string(data + i, p, n)) return i;
            tag_check_fail.fetch_add(1, std::memory_order_relaxed);
            tag_stale = true;  // array changed in place: rebuild next time
            return -1;
        }
        j = (j + 1) & t->mask;
    }
    return -1;
}

std::int32_t scan_first(std::uint8_t* links, const GameString* tag) noexcept {
    const auto* data = *reinterpret_cast<GameString* const*>(links + 0x358);
    const auto count = *reinterpret_cast<const std::int32_t*>(links + 0x364);
    const char* p = str_data(tag);
    for (std::int32_t i = 0; i < count; ++i) if (same_string(data + i, p, tag->size)) return i;
    return -1;
}

std::uint64_t tags_hook(std::uint8_t* links, const GameString* tag) {
    const bool timed = tags_mode == TagsMode::shadow;  // no clock reads per call in mode on
    const auto t0 = timed ? qpc_now() : 0;
    std::uint64_t result;
    if (tags_disabled.load(std::memory_order_relaxed)) {
        result = tags_original(links, tag);
    } else if (tags_mode == TagsMode::shadow) {
        const std::int32_t mine = table_lookup(links, tag);
        result = tags_original(links, tag);
        if (mine >= 0) {
            tag_hits.fetch_add(1, std::memory_order_relaxed);
            if (static_cast<std::uint32_t>(result) != static_cast<std::uint32_t>(mine)) {
                tag_mismatch.fetch_add(1);
                if (tag_logged.fetch_add(1) < 20)
                    log("tags MISMATCH: table %d game %u", mine, static_cast<unsigned>(result));
            }
        } else {
            tag_fallbacks.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        const std::int32_t mine = table_lookup(links, tag);
        if (mine >= 0) {
            tag_hits.fetch_add(1, std::memory_order_relaxed);
            result = static_cast<std::uint32_t>(mine);
            if ((tag_samples.fetch_add(1, std::memory_order_relaxed) & 255) == 255) {
                const auto check = scan_first(links, tag);
                if (check != mine) {
                    tag_mismatch.fetch_add(1);
                    log("tags MISMATCH in sampled check (table %d scan %d): module disabled", mine, check);
                    tags_disabled = true;
                    result = tags_original(links, tag);
                }
            }
        } else {
            tag_fallbacks.fetch_add(1, std::memory_order_relaxed);
            result = tags_original(links, tag);
        }
    }
    tag_calls.fetch_add(1, std::memory_order_relaxed);
    if (timed) tag_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t0), std::memory_order_relaxed);
    return result;
}

void tags_report() noexcept {
    const auto c = tag_calls.exchange(0);
    if (!c) return;
    log("tags %s: calls=%llu hits=%llu fallbacks=%llu rebuilds=%llu check_fail=%llu MISMATCH=%llu%s "
        "time=%.1f ms max_count=%d",
        tags_mode == TagsMode::on ? "on" : "shadow", static_cast<unsigned long long>(c),
        static_cast<unsigned long long>(tag_hits.exchange(0)),
        static_cast<unsigned long long>(tag_fallbacks.exchange(0)),
        static_cast<unsigned long long>(tag_rebuilds.exchange(0)),
        static_cast<unsigned long long>(tag_check_fail.exchange(0)),
        static_cast<unsigned long long>(tag_mismatch.exchange(0)), tags_disabled ? " DISABLED" : "",
        ms(tag_qpc.exchange(0)), tag_max_count.load());
}

bool install_tags(TagsMode m) {
    auto* entry = reinterpret_cast<std::uint8_t*>(image + tags_rva);
    if (std::memcmp(entry, tags_prologue, sizeof(tags_prologue)) != 0) {
        log("tags REFUSED (1401f0a90 bytes differ)");
        return false;
    }
    auto* tramp = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) { log("tags REFUSED (no trampoline memory)"); return false; }
    std::memcpy(tramp, tags_prologue, 13);  // position-independent instructions
    const std::uint64_t back = image + tags_rva + 13;
    tramp[13] = 0xff; tramp[14] = 0x25; std::memset(tramp + 15, 0, 4);  // jmp [rip+0]
    std::memcpy(tramp + 19, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    tags_original = reinterpret_cast<TagFn>(tramp);
    tags_mode = m;
    std::uint8_t patch[13] = {0x48, 0xb8};
    const auto target = reinterpret_cast<std::uint64_t>(&tags_hook);
    std::memcpy(patch + 2, &target, 8);
    patch[10] = 0xff; patch[11] = 0xe0; patch[12] = 0xcc;  // jmp rax; int3
    DWORD old = 0;
    if (!VirtualProtect(entry, 13, PAGE_EXECUTE_READWRITE, &old)) { log("tags REFUSED (VirtualProtect)"); return false; }
    std::memcpy(entry, patch, 13);
    VirtualProtect(entry, 13, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, 13);
    log("tags=%s installed", m == TagsMode::on ? "on" : "shadow");
    return true;
}
