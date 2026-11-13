// Module "mods": modifier value lookup 1405a5210(const CPdxArray<{u32 id; i64 value}>* list,
// i64* out, u32 id) -> out. ~3.5% of the main thread in 1942 (army, combat, navy code).
// The game: branchless binary search (lower bound, unsigned ids, 16-byte entries); not found
// -> 0; else definition D = DAT_1433dd1d0 + id * 0x78: if D+0x68 == 0 or (callback 1433de580
// && (D+0x68 & callback())) the value is used, else 0; if D+0x60 bit 4, the value is clamped
// to [0, 100000].
// on: the same function, but for lists of up to 64 entries the lower bound is found by a linear
// count of entries with id < key (independent loads instead of a dependent chain).
// measure: calls alternate between the original and the new code, cycles (rdtsc) are compared,
// and the size histogram is logged. shadow: both run on every call and the outputs compared.

enum class ModsMode { off, measure, shadow, on };
ModsMode mods_mode = ModsMode::off;

constexpr std::uintptr_t mods_rva = 0x5a5210;
constexpr std::uintptr_t mods_defs_rva = 0x33dd1d0;
constexpr std::uintptr_t mods_callback_rva = 0x33de580;
// mov [rsp+18],rbx | mov [rsp+20],rsi | push rdi | sub rsp,20  (15 bytes, position independent)
constexpr std::uint8_t mods_prologue[15] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                            0x20, 0x57, 0x48, 0x83, 0xec, 0x20};

struct ModEntry { std::uint32_t id; std::uint32_t pad; std::int64_t value; };
struct ModList { ModEntry* data; std::int32_t capacity; std::int32_t size; };
using ModsFn = std::int64_t* (*)(const ModList*, std::int64_t*, std::uint32_t);
using MaskFn = std::uint32_t (*)();
ModsFn mods_original = nullptr;
int linear_limit = 64;

std::atomic<std::uint64_t> mods_calls{0}, mods_hist[9], mods_orig_cycles{0}, mods_orig_n{0},
    mods_fast_cycles{0}, mods_fast_n{0}, mods_mismatch{0};
std::atomic<int> mods_logged{0};

__attribute__((always_inline)) inline std::int64_t* mods_fast(const ModList* list, std::int64_t* out,
                                                              std::uint32_t id) noexcept {
    const std::int64_t n = list->size;
    const ModEntry* e = list->data;
    const ModEntry* p;
    if (n <= linear_limit) {
        std::int64_t below = 0;
        for (std::int64_t i = 0; i < n; ++i) below += e[i].id < id;
        p = e + below;
    } else {  // the game's binary search
        p = e;
        std::uint64_t count = static_cast<std::uint64_t>(n);
        while (count) {
            const std::uint64_t half = count >> 1;
            const std::uint64_t step = (p[half].id < id) ? half + (count & 1) : 0;
            p += step;
            count = half;
        }
    }
    if (p == e + (n > 0 ? n : 0) || id < p->id) { *out = 0; return out; }
    const auto* defs = *reinterpret_cast<const std::uint8_t* const*>(image + mods_defs_rva);
    const auto* d = defs + static_cast<std::int64_t>(static_cast<std::int32_t>(id)) * 0x78;
    const std::uint32_t mask = *reinterpret_cast<const std::uint32_t*>(d + 0x68);
    std::int64_t result = 0;
    bool use = mask == 0;
    if (!use) {
        const auto callback = *reinterpret_cast<const MaskFn*>(image + mods_callback_rva);
        use = callback && (mask & callback()) != 0;
    }
    if (use) {
        const std::int64_t v = p->value;
        if (((*reinterpret_cast<const std::uint32_t*>(d + 0x60) >> 4) & 1) == 0) result = v;
        else result = v < 0 ? 0 : (v > 100000 ? 100000 : v);
    }
    *out = result;
    return out;
}

std::int64_t* mods_hook(const ModList* list, std::int64_t* out, std::uint32_t id) {
    if (mods_mode == ModsMode::on) return mods_fast(list, out, id);
    const auto n = list->size;
    const int bucket = n <= 0 ? 0 : n <= 4 ? 1 : n <= 8 ? 2 : n <= 16 ? 3 : n <= 32 ? 4
        : n <= 64 ? 5 : n <= 128 ? 6 : n <= 256 ? 7 : 8;
    mods_hist[bucket].fetch_add(1, std::memory_order_relaxed);
    mods_calls.fetch_add(1, std::memory_order_relaxed);
    if (mods_mode == ModsMode::shadow) {
        std::int64_t mine = 0;
        mods_fast(list, &mine, id);
        mods_original(list, out, id);
        if (mine != *out) {
            mods_mismatch.fetch_add(1);
            if (mods_logged.fetch_add(1) < 20)
                log("mods MISMATCH id=%u n=%d fast=%lld game=%lld", id, n,
                    static_cast<long long>(mine), static_cast<long long>(*out));
        }
        return out;
    }
    // measure: alternate implementations per call
    static thread_local std::uint32_t flip = 0;
    const auto t0 = __rdtsc();
    if (++flip & 1) {
        mods_original(list, out, id);
        mods_orig_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        mods_orig_n.fetch_add(1, std::memory_order_relaxed);
    } else {
        mods_fast(list, out, id);
        mods_fast_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        mods_fast_n.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

void mods_report() noexcept {
    const auto c = mods_calls.exchange(0);
    if (!c) return;
    std::uint64_t h[9];
    for (int i = 0; i < 9; ++i) h[i] = mods_hist[i].exchange(0);
    const auto on = mods_orig_n.exchange(0), fn = mods_fast_n.exchange(0);
    const auto oc = mods_orig_cycles.exchange(0), fc = mods_fast_cycles.exchange(0);
    log("mods: calls=%llu (per s %.0f) sizes 0:%llu 1-4:%llu 5-8:%llu 9-16:%llu 17-32:%llu 33-64:%llu "
        "65-128:%llu 129-256:%llu >256:%llu | cycles/call game=%.1f fast=%.1f MISMATCH=%llu",
        static_cast<unsigned long long>(c), static_cast<double>(c) / 30.0,
        (unsigned long long)h[0], (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)h[3],
        (unsigned long long)h[4], (unsigned long long)h[5], (unsigned long long)h[6], (unsigned long long)h[7],
        (unsigned long long)h[8], on ? static_cast<double>(oc) / static_cast<double>(on) : 0.0,
        fn ? static_cast<double>(fc) / static_cast<double>(fn) : 0.0,
        static_cast<unsigned long long>(mods_mismatch.exchange(0)));
}

bool install_mods(ModsMode m) {
    auto* entry = reinterpret_cast<std::uint8_t*>(image + mods_rva);
    if (std::memcmp(entry, mods_prologue, sizeof(mods_prologue)) != 0) {
        log("mods REFUSED (1405a5210 bytes differ)");
        return false;
    }
    auto* tramp = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, mods_prologue, 15);
    const std::uint64_t back = image + mods_rva + 15;
    tramp[15] = 0xff; tramp[16] = 0x25; std::memset(tramp + 17, 0, 4);  // jmp [rip+0]
    std::memcpy(tramp + 21, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    mods_original = reinterpret_cast<ModsFn>(tramp);
    mods_mode = m;
    if (!write_jump(entry, reinterpret_cast<const void*>(&mods_hook))) return false;
    return true;
}
