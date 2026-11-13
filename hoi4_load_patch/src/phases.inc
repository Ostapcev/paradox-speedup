// Module "phases" (measurement only): times calls made while a save is loaded
// (continue game 140e0bb70 -> 140e0b010 -> CCurrentGameState::Load 1401f8ac0 -> 1401df0e0 ...).
// Every listed call site `call rel32` is redirected to a wrapper that calls the same target with
// the same 8 integer argument registers / stack slots and returns its rax. No game logic changes.

struct PhaseSite { std::uintptr_t call_rva; std::uintptr_t target_rva; const char* name; };
constexpr PhaseSite phase_sites[] = {
    {0xe0be4c, 0xe0b010, "continue->140e0b010"},
    {0xe0b336, 0x1f8ac0, "Load 1401f8ac0 (a)"},
    {0xe0b88b, 0x1f8ac0, "Load 1401f8ac0 (b)"},
    {0x1f8bc4, 0x664dc0, " L.140664dc0"},
    {0x1f8bd1, 0x1f1d10, " L.1401f1d10"},
    {0x1f8bdd, 0x1b3ad0, " L.1401b3ad0"},
    {0x1f8c64, 0x1bce90, " L.1401bce90"},
    {0x1f8cd3, 0x1df0e0, " L.1401df0e0"},
    {0x1df149, 0x2001a0, "  D.1402001a0"},
    {0x1df155, 0x1f73e0, "  D.1401f73e0"},
    {0x1df178, 0xa8a820, "  D.140a8a820"},
    {0x1df180, 0xa8aaf0, "  D.140a8aaf0"},
    {0x1df1a3, 0xd54910, "  D.140d54910"},
    {0x1df1a8, 0xd54ad0, "  D.140d54ad0"},
    {0x1df1b1, 0xd54570, "  D.140d54570"},
    {0x1df1f4, 0x2048b0, "  D.1402048b0"},
    {0x1df218, 0x663b50, "  D.140663b50"},
    {0x1df224, 0x668140, "  D.140668140"},
    {0x1df255, 0x2297e60, "  D.142297e60 (read)"},
    {0x1df27b, 0x1f06b0, "  D.1401f06b0"},
    {0x1f8d42, 0x1f9060, " L.1401f9060"},
    {0x1f8da2, 0x1cc3c0, " L.1401cc3c0"},
    {0x1f8e13, 0x19cba0, " L.14019cba0"},
    {0x255fe18, 0x257a870, "PHYSFS_readBytes@14255fe18"},
};
constexpr int phase_count = static_cast<int>(sizeof(phase_sites) / sizeof(phase_sites[0]));

using Fn8 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                              std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
Fn8 phase_targets[phase_count]{};
std::atomic<std::uint64_t> phase_qpc[phase_count]{};
std::atomic<std::uint32_t> phase_calls[phase_count]{};
std::atomic<bool> phase_dirty{false};
std::int64_t phase_first = 0;
std::atomic<std::uint64_t> phase_bytes{0};
std::atomic<bool> in_load{false};
std::atomic<std::uint64_t> load_reads{0}, load_bytes{0}, load_req{0}, load_qpc{0}, load_small{0}, load_main{0};
DWORD load_tid = 0;

template <int N>
std::uint64_t phase_wrap(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                         std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    const auto t0 = qpc_now();
    if (N == 0) { in_load = true; load_tid = GetCurrentThreadId(); }
    if (!phase_first && N != phase_count - 1) phase_first = t0;
    const auto r = phase_targets[N](a, b, c, d, e, f, g, h);
    const auto t1 = qpc_now();
    phase_qpc[N].fetch_add(static_cast<std::uint64_t>(t1 - t0), std::memory_order_relaxed);
    phase_calls[N].fetch_add(1, std::memory_order_relaxed);
    if (N == phase_count - 1) {
        phase_bytes.fetch_add(r, std::memory_order_relaxed);
        if (in_load.load(std::memory_order_relaxed)) {
            load_reads.fetch_add(1); load_bytes.fetch_add(r); load_req.fetch_add(c);
            load_qpc.fetch_add(static_cast<std::uint64_t>(t1 - t0));
            if (c <= 64) load_small.fetch_add(1);
            if (GetCurrentThreadId() == load_tid) load_main.fetch_add(1);
        }
        return r;
    }
    if (N == 0) {
        in_load = false;
        log("load-window PHYSFS reads=%llu main_thread=%llu small(<=64)=%llu requested=%llu got=%llu time=%.1f ms",
            (unsigned long long)load_reads.load(), (unsigned long long)load_main.load(),
            (unsigned long long)load_small.load(), (unsigned long long)load_req.load(),
            (unsigned long long)load_bytes.load(), ms(load_qpc.load()));
    }
    if (N <= 2) log("phase %s: %.1f ms (start +%.1f ms, tid %lu)", phase_sites[N].name, ms(t1 - t0),
                    ms(t0 - phase_first), GetCurrentThreadId());
    phase_dirty = true;
    return r;
}

template <int... I> struct Seq {};
template <int N, int... I> struct MakeSeq : MakeSeq<N - 1, N - 1, I...> {};
template <int... I> struct MakeSeq<0, I...> { using type = Seq<I...>; };
template <int... I> constexpr std::array<const void*, sizeof...(I)> wrappers_of(Seq<I...>) {
    return {reinterpret_cast<const void*>(&phase_wrap<I>)...};
}

void phases_report() noexcept {
    if (!phase_dirty.exchange(false)) return;
    for (int i = 0; i < phase_count; ++i) {
        const auto c = phase_calls[i].load();
        if (c) log("phase total %-26s calls=%u %.1f ms%s", phase_sites[i].name, c, ms(phase_qpc[i].load()),
                   i == phase_count - 1 ? (std::string(" bytes=") + std::to_string(phase_bytes.load())).c_str() : "");
    }
}

bool install_phases(bool physfs) {
    const auto wrappers = wrappers_of(typename MakeSeq<phase_count>::type{});
    int ok = 0;
    for (int i = 0; i < phase_count; ++i) {
        if (i == phase_count - 1 && !physfs) continue;  // PHYSFS_readBytes: ~19M calls, costly
        const auto& s = phase_sites[i];
        auto* site = reinterpret_cast<std::uint8_t*>(image + s.call_rva);
        std::uint8_t expected[5] = {0xe8};
        const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(s.target_rva) -
                                                   static_cast<std::int64_t>(s.call_rva + 5));
        std::memcpy(expected + 1, &rel, 4);
        if (std::memcmp(site, expected, 5) != 0) { log("phase %s REFUSED (bytes differ)", s.name); continue; }
        phase_targets[i] = reinterpret_cast<Fn8>(image + s.target_rva);
        char error[160]{};
        speedpatch::CallPatch patch;
        if (!speedpatch::prepare_call_bytes(site, expected, 5, wrappers[i], patch, error, sizeof(error)) ||
            !speedpatch::commit_call(patch, error, sizeof(error))) {
            speedpatch::discard_call(patch);
            log("phase %s REFUSED (%s)", s.name, error);
            continue;
        }
        ++ok;
    }
    log("phases installed %d/%d", ok, phase_count);
    return ok > 0;
}
