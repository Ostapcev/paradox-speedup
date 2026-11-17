// Stellaris Fleetscan Patch 0.3.0, Stellaris 4.5.4 x64 — подкачка в обходе всех флотов 140714e50.
//
// Что в игре (stellaris.exe 4.5.4.1b4f): 140714e50(country) зовётся из parallel-for 1402c5870 → 1403019a0
// (вызов 140301a78) по странам из DailyUpdate. Третий цикл (голова 140715140, указатель r14, конец r15)
// идёт по всем флотам игры [[14311b708]+0x500] и для каждого: слот таблицы флотов [1431199e8] (id [флот+0x30]),
// vcall [флот+0x18]+0x20, байт «уже был», затем 140715e50(country, флот): поля +0x32c/+0x1340, +0x1268,
// владелец +0x448, хеш-множество по +0x5a8 и 140716020 → система флота (140e69f60: при [+0x690] != 4 это
// [+0x5a8]) из таблицы [14311bed8], сверка [sys+8]. Главный промах — строка системы (14071606f, ~19 %
// поддерева) и строки флота.
//
// Плагин: на 140715149 (mov rdx,[1431199e8], 7 байт) переход в заглушку: подкачка слота флота на 16
// вперёд, строк флота на 8 вперёд, слота системы на 4 вперёд и строки системы на 2 вперёд; затем то же
// чтение rdx, возврат на 140715150. Только prefetch — состояние не меняется. rax, rcx, rdx, r8–r11 здесь мертвы.
// [fleetscan] mode=off / on. fsp_on (rva в логе): 0 — игра, 1 — подкачка, 3 — замер: вызовы 140714e50
// (вызов 140301a78) по очереди на каждом потоке без/с подкачкой, время по TSC. Подкачка дала −1…−2 % — по умолчанию выкл.
//
// 0.2.0 — кэш цикла 1. В начале 140714e50 для каждой системы [[14311b708]+0x590] зовётся триггер
// 1405d1f90([14315cbc0], sys) (вызов 140714f1a); подходящие системы попадают в множество страны. Аргументы от
// страны не зависят, поэтому в пределах одного запуска задания (вызов 14025e462 → 1402ac600 из DailyUpdate) ответ
// для системы считается один раз и дальше берётся из таблицы. Вне запуска — всегда игра.
// [fleetscan] cache=off / on / shadow. fsc_mode (rva в логе): 0 — игра, 1 — кэш, 2 — тень (всегда игра + сверка
// с первым ответом в запуске), 3 — замер через вызов 140714e50 (по очереди игра/кэш на каждом потоке).

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t fsp_on = 0;
std::uint64_t fsp_db = 0;       // &[1431199e8]
std::uint64_t fsp_sys = 0;      // &[14311bed8]
std::uint64_t fsp_resume = 0;   // 140715150
std::uint64_t fsp_tls_off = 0;  // gs-смещение слота TLS (флаг потока в режиме 3)
void fsp_stub();
std::uint64_t fsp_outer(void* c, void* a, void* b, void* d);
std::int64_t fsc_mode = 0;
// 0.3.0 (МЕНЯЕТ ПОВЕДЕНИЕ): [fleetscan] rr=K — 140714e50 из ежедневного задания (вызов 140301a78) выполняется для страны
// только в дни, когда (id страны + день) % K == 0; в остальные дни список известных стране флотов остаётся вчерашним.
// fsr_k (rva в логе) на лету: 0/1 — каждый день, как в игре.
std::int64_t fsr_k = 0;
std::atomic<long long> fsr_run{0}, fsr_skip{0}, fsr_wall{0};
std::uint64_t fsc_trig(void* holder, void* sys, void* r8, void* r9);
std::uint64_t fsc_epoch(void* a, void* b, void* c, void* d);
}

__attribute__((naked)) void fsp_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov rax, qword ptr [rip + fsp_on]\n"
        "    cmp rax, 3\n"
        "    jne 1f\n"
        "    mov rax, qword ptr [rip + fsp_tls_off]\n"
        "    mov rax, qword ptr gs:[rax]\n"
        "1:\n"
        "    test rax, rax\n"
        "    je 9f\n"
        "    mov rdx, qword ptr [rip + fsp_db]\n"
        "    mov rdx, qword ptr [rdx]\n"
        "    test rdx, rdx\n"
        "    je 9f\n"
        "    mov r8d, dword ptr [rdx + 0x20]\n"
        "    mov r9, qword ptr [rdx + 0x18]\n"
        "    mov r10, qword ptr [rip + fsp_sys]\n"
        "    mov r10, qword ptr [r10]\n"
        // слот флота, +16
        "    lea rcx, [r14 + 64]\n"
        "    cmp rcx, r15\n"
        "    jae 2f\n"
        "    mov eax, dword ptr [rcx]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r8d\n"
        "    jae 2f\n"
        "    shl rax, 4\n"
        "    prefetcht0 [r9 + rax + 8]\n"
        "2:\n"
        // строки флота, +8
        "    lea rcx, [r14 + 32]\n"
        "    cmp rcx, r15\n"
        "    jae 3f\n"
        "    mov eax, dword ptr [rcx]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r8d\n"
        "    jae 3f\n"
        "    shl rax, 4\n"
        "    mov rax, qword ptr [r9 + rax + 8]\n"
        "    test rax, rax\n"
        "    je 3f\n"
        "    prefetcht0 [rax + 0x18]\n"
        "    prefetcht0 [rax + 0x30]\n"
        "    prefetcht0 [rax + 0x32c]\n"
        "    prefetcht0 [rax + 0x448]\n"
        "    prefetcht0 [rax + 0x5a8]\n"
        "    prefetcht0 [rax + 0x690]\n"
        "    prefetcht0 [rax + 0x1268]\n"
        "    prefetcht0 [rax + 0x1340]\n"
        "3:\n"
        "    test r10, r10\n"
        "    je 9f\n"
        "    mov r11d, dword ptr [r10 + 0x20]\n"
        // слот системы, +4
        "    lea rcx, [r14 + 16]\n"
        "    cmp rcx, r15\n"
        "    jae 4f\n"
        "    mov eax, dword ptr [rcx]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r8d\n"
        "    jae 4f\n"
        "    shl rax, 4\n"
        "    mov rax, qword ptr [r9 + rax + 8]\n"
        "    test rax, rax\n"
        "    je 4f\n"
        "    cmp dword ptr [rax + 0x690], 4\n"
        "    je 4f\n"
        "    mov eax, dword ptr [rax + 0x5a8]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r11d\n"
        "    jae 4f\n"
        "    shl rax, 4\n"
        "    mov rdx, qword ptr [r10 + 0x18]\n"
        "    prefetcht0 [rdx + rax + 8]\n"
        "4:\n"
        // строка системы, +2
        "    lea rcx, [r14 + 8]\n"
        "    cmp rcx, r15\n"
        "    jae 9f\n"
        "    mov eax, dword ptr [rcx]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r8d\n"
        "    jae 9f\n"
        "    shl rax, 4\n"
        "    mov rax, qword ptr [r9 + rax + 8]\n"
        "    test rax, rax\n"
        "    je 9f\n"
        "    cmp dword ptr [rax + 0x690], 4\n"
        "    je 9f\n"
        "    mov eax, dword ptr [rax + 0x5a8]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r11d\n"
        "    jae 9f\n"
        "    shl rax, 4\n"
        "    mov rdx, qword ptr [r10 + 0x18]\n"
        "    mov rax, qword ptr [rdx + rax + 8]\n"
        "    test rax, rax\n"
        "    je 9f\n"
        "    prefetcht0 [rax + 8]\n"
        "9:\n"
        "    mov rdx, qword ptr [rip + fsp_db]\n"
        "    mov rdx, qword ptr [rdx]\n"
        "    jmp qword ptr [rip + fsp_resume]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSite = 0x140715149;
constexpr std::uint8_t kSiteBytes[7] = {0x48, 0x8b, 0x15, 0x98, 0x48, 0xa0, 0x02};
constexpr std::uint64_t kResume = 0x140715150;
// контекст: голова цикла и хвост (add r14,4; cmp r14,r15; jne 140715140)
constexpr std::uint8_t kHead[9] = {0x41, 0x8b, 0x1e, 0x89, 0x9d, 0x50, 0x10, 0x00, 0x00};
constexpr std::uint8_t kTail[13] = {0x49, 0x83, 0xc6, 0x04, 0x4d, 0x3b, 0xf7, 0x0f, 0x85, 0xb1, 0xfe, 0xff, 0xff};
// 140716020: mov r9,[14311bed8] — та же таблица систем
constexpr std::uint8_t kSysUse[7] = {0x4c, 0x8b, 0x0d, 0x96, 0x5e, 0xa0, 0x02};
// 140e69f60: cmp [rdx+0x690],4 ... mov eax,[rdx+0x5a8]
constexpr std::uint8_t kLoc1[7] = {0x83, 0xba, 0x90, 0x06, 0x00, 0x00, 0x04};
constexpr std::uint8_t kLoc2[6] = {0x8b, 0x82, 0xa8, 0x05, 0x00, 0x00};

constexpr std::uint64_t kOuter = 0x140301a78;
constexpr std::uint8_t kOuterBytes[5] = {0xe8, 0xd3, 0x33, 0x41, 0x00};
using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);
Fn4 outer_orig = nullptr;
DWORD tls_idx = TLS_OUT_OF_INDEXES;
thread_local unsigned t_cnt = 0;
thread_local int t_alt = 0;
thread_local long long t_n[2], t_t[2];
std::atomic<long long> o_n[2], o_t[2];
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    if (q.QuadPart - last < 60 * freq.QuadPart || !rep_qpc.compare_exchange_strong(last, q.QuadPart)) return;
    const long long n0 = o_n[0].exchange(0), t0 = o_t[0].exchange(0), n1 = o_n[1].exchange(0), t1 = o_t[1].exchange(0);
    if (n0 || n1)
        splug::log("140714e50 calls: game %lld avg %.1f kcyc, plugin %lld avg %.1f kcyc (%.1f%%)", n0,
                   n0 ? t0 / 1000.0 / n0 : 0.0, n1, n1 ? t1 / 1000.0 / n1 : 0.0,
                   (n0 && n1) ? 100.0 * ((double)t1 / n1) / ((double)t0 / n0) - 100.0 : 0.0);
}

void install_prefetch() {
    if (!splug::bytes_match(kSite, kSiteBytes) || !splug::bytes_match(0x140715140, kHead) ||
        !splug::bytes_match(0x140715282, kTail) || !splug::bytes_match(0x14071603b, kSysUse) ||
        !splug::bytes_match(0x140e69f69, kLoc1) || !splug::bytes_match(0x140e69f78, kLoc2)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    fsp_tls_off = 0x1480 + tls_idx * 8ull;
    fsp_db = reinterpret_cast<std::uint64_t>(splug::rva(0x1431199e8));
    fsp_sys = reinterpret_cast<std::uint64_t>(splug::rva(0x14311bed8));
    fsp_resume = reinterpret_cast<std::uint64_t>(splug::rva(kResume));

    std::uint8_t* site = splug::rva(kSite);
    auto* page = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!page) {
        splug::log("REFUSED: no memory near game code");
        return;
    }
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&fsp_stub);
    std::memcpy(page, jmp, 6);
    std::memcpy(page + 6, &target, 8);
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 16);
    const auto d = reinterpret_cast<std::intptr_t>(page) - reinterpret_cast<std::intptr_t>(site + 5);
    if (d < INT32_MIN || d > INT32_MAX) {
        splug::log("REFUSED: page out of rel32 range");
        return;
    }
    fsp_on = 1;
    std::uint8_t patch[sizeof(kSiteBytes)];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xe9;
    const std::int32_t rel = static_cast<std::int32_t>(d);
    std::memcpy(patch + 1, &rel, 4);
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        fsp_on = 0;
        splug::log("FAILED: cannot write jump");
        return;
    }
    splug::log("prefetch installed at %llx, tls slot %lu", static_cast<unsigned long long>(kSite), tls_idx);
}

}  // namespace

namespace {
std::atomic<long long> p_calls{0}, p_n{0}, p_slot{0}, p_alive{0}, p_pow{0}, p_flag{0}, p_own{0}, p_t{0};
void probe(const std::uint8_t* country) {
    const auto* gs = *splug::rva<const std::uint8_t*>(0x14311b708);
    const auto* db = *splug::rva<const std::uint8_t*>(0x1431199e8);
    if (!gs || !db) return;
    const unsigned long long t0 = __rdtsc();
    const auto* ids = *reinterpret_cast<const std::uint32_t* const*>(gs + 0x500);
    const int n = *reinterpret_cast<const int*>(gs + 0x50c);
    const std::uint32_t lim = *reinterpret_cast<const std::uint32_t*>(db + 0x20);
    const auto* slots = *reinterpret_cast<const std::uint8_t* const*>(db + 0x18);
    const std::uint32_t cid = *reinterpret_cast<const std::uint32_t*>(country + 0x20);
    long long slot = 0, alive = 0, pow = 0, flag = 0, own = 0;
    for (int i = 0; i < n; ++i) {
        const std::uint32_t id = ids[i], idx = id & 0xffffffu;
        if (idx >= lim) continue;
        auto* f = *reinterpret_cast<std::uint8_t* const*>(slots + idx * 16ull + 8);
        if (!f || *reinterpret_cast<const std::uint32_t*>(f + 0x30) != id) continue;
        ++slot;
        void* sub = f + 0x18;
        using V = bool (*)(void*);
        if (!(*reinterpret_cast<V* const*>(sub))[0x20 / 8](sub)) continue;
        ++alive;
        if (*reinterpret_cast<const int*>(f + 0x1340) >= *reinterpret_cast<const int*>(f + 0x32c)) continue;
        ++pow;
        if ((*reinterpret_cast<const std::uint32_t*>(f + 0x1268) >> 5) & 1) continue;
        ++flag;
        if (*reinterpret_cast<const std::uint32_t*>(f + 0x448) == cid) continue;
        ++own;
    }
    p_t.fetch_add(static_cast<long long>(__rdtsc() - t0));
    p_calls.fetch_add(1); p_n.fetch_add(n); p_slot.fetch_add(slot); p_alive.fetch_add(alive);
    p_pow.fetch_add(pow); p_flag.fetch_add(flag); p_own.fetch_add(own);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) { rep_qpc.compare_exchange_strong(last, q.QuadPart); return; }
    if (q.QuadPart - last < 60 * freq.QuadPart || !rep_qpc.compare_exchange_strong(last, q.QuadPart)) return;
    const double k = static_cast<double>(p_calls.exchange(0));
    if (k > 0)
        splug::log("probe: calls %.0f, fleets %.0f, slot ok %.0f, alive %.0f, 32c>1340 %.0f, flag ok %.0f, not own %.0f, walk %.0f kcyc (per call)",
                   k, p_n.exchange(0) / k, p_slot.exchange(0) / k, p_alive.exchange(0) / k, p_pow.exchange(0) / k,
                   p_flag.exchange(0) / k, p_own.exchange(0) / k, p_t.exchange(0) / 1000.0 / k);
}
}  // namespace

extern "C" std::uint64_t fsp_outer(void* c, void* a, void* b, void* d) {
    if (fsp_on == 4) probe(static_cast<const std::uint8_t*>(c));
    if (fsr_k > 1 && c) {
        const auto* clk = *splug::rva<const std::uint8_t*>(0x14311b708);
        const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(c) + 0x20) & 0xffffffu;
        const std::uint32_t day = clk ? *reinterpret_cast<const std::uint32_t*>(clk + 0xc4) : 0;
        if ((id + day) % static_cast<std::uint64_t>(fsr_k) != 0) {
            fsr_skip.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        fsr_run.fetch_add(1, std::memory_order_relaxed);
    }
    if (fsp_on != 3 && fsc_mode != 3) return outer_orig(c, a, b, d);
    const int alt = static_cast<int>(++t_cnt & 1);
    t_alt = alt;
    TlsSetValue(tls_idx, reinterpret_cast<void*>(static_cast<std::uintptr_t>(alt)));
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = outer_orig(c, a, b, d);
    t_t[alt] += static_cast<long long>(__rdtsc() - t0);
    ++t_n[alt];
    TlsSetValue(tls_idx, nullptr);
    t_alt = 0;
    if ((t_cnt & 15) == 0) {
        for (int k = 0; k < 2; ++k) {
            o_n[k].fetch_add(t_n[k]);
            o_t[k].fetch_add(t_t[k]);
            t_n[k] = t_t[k] = 0;
        }
        report();
    }
    return r;
}

// ---------------- кэш цикла 1 ----------------
namespace {
constexpr std::uint64_t kTrig = 0x140714f1a;
constexpr std::uint8_t kTrigBytes[5] = {0xe8, 0x71, 0xd0, 0xeb, 0xff};
constexpr std::uint8_t kTrigCtx[7] = {0x48, 0x8b, 0x0d, 0xa6, 0x7c, 0xa4, 0x02};  // 140714f13 mov rcx,[14315cbc0]
constexpr std::uint64_t kEpoch = 0x14025e462;
constexpr std::uint8_t kEpochBytes[5] = {0xe8, 0x99, 0xe1, 0x04, 0x00};
Fn4 trig_orig = nullptr, epoch_orig = nullptr;
std::atomic<std::uint32_t> g_run{0};
std::atomic<int> g_active{0};
void* g_holder = nullptr;
constexpr unsigned kSlots = 1u << 16;
// значение: биты 63..32 — номер запуска, 24..1 — id системы, 0 — ответ
std::atomic<std::uint64_t> g_tab[kSlots];
std::atomic<long long> c_runs{0}, c_calls{0}, c_hits{0}, c_mis{0}, c_byp{0};
thread_local long long l_calls = 0, l_hits = 0, l_byp = 0;
std::atomic<long long> rep2_qpc{0};

void report2() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep2_qpc.load();
    if (last == 0) {
        rep2_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    static std::int64_t win_k = -1;
    const bool switched = win_k != -1 && win_k != fsr_k;
    if ((q.QuadPart - last < 60 * freq.QuadPart && !switched) || !rep2_qpc.compare_exchange_strong(last, q.QuadPart))
        return;
    win_k = fsr_k;
    splug::log("round-robin k=%lld: country updates run %lld, skipped %lld, daily job wall %.0f Mcyc", static_cast<long long>(fsr_k),
               fsr_run.exchange(0), fsr_skip.exchange(0), fsr_wall.exchange(0) / 1e6);
    splug::log("cache mode %lld: runs %lld, trigger calls %lld, from cache/verified %lld, index collisions %lld, mismatches %lld",
               static_cast<long long>(fsc_mode), c_runs.exchange(0), c_calls.exchange(0), c_hits.exchange(0),
               c_byp.exchange(0), c_mis.load());
}

inline void flush_local() {
    c_calls.fetch_add(l_calls, std::memory_order_relaxed);
    c_hits.fetch_add(l_hits, std::memory_order_relaxed);
    c_byp.fetch_add(l_byp, std::memory_order_relaxed);
    l_calls = l_hits = l_byp = 0;
    report2();
}

void install_cache(splug::Mode cm) {
    if (!splug::bytes_match(0x140714f13, kTrigCtx)) {
        splug::log("cache REFUSED: context bytes differ");
        return;
    }
    trig_orig = reinterpret_cast<Fn4>(splug::call_target(splug::rva(kTrig)));
    epoch_orig = reinterpret_cast<Fn4>(splug::call_target(splug::rva(kEpoch)));
    if (reinterpret_cast<std::uintptr_t>(trig_orig) != reinterpret_cast<std::uintptr_t>(splug::rva(0x1405d1f90)) ||
        reinterpret_cast<std::uintptr_t>(epoch_orig) != reinterpret_cast<std::uintptr_t>(splug::rva(0x1402ac600))) {
        splug::log("cache REFUSED: call targets differ");
        return;
    }
    splug::CallPatch pt, pe;
    if (const char* e = splug::prepare_call(splug::rva(kTrig), kTrigBytes, reinterpret_cast<const void*>(&fsc_trig), pt)) {
        splug::log("cache REFUSED: %s", e);
        return;
    }
    if (const char* e = splug::prepare_call(splug::rva(kEpoch), kEpochBytes, reinterpret_cast<const void*>(&fsc_epoch), pe)) {
        splug::log("cache REFUSED: %s", e);
        return;
    }
    fsc_mode = cm == splug::Mode::on ? 1 : 2;
    if (!splug::commit_call(pt) || !splug::commit_call(pe)) {
        splug::log("cache FAILED while writing calls");
        return;
    }
    splug::log("cache installed (%llx, %llx), mode %lld", static_cast<unsigned long long>(kTrig),
               static_cast<unsigned long long>(kEpoch), static_cast<long long>(fsc_mode));
}
}  // namespace

extern "C" std::uint64_t fsc_epoch(void* a, void* b, void* c, void* d) {
    g_holder = *splug::rva<void*>(0x14315cbc0);
    std::uint32_t run = g_run.load() + 1;
    if (run == 0) run = 1;
    g_run.store(run);
    g_active.store(1);
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = epoch_orig(a, b, c, d);
    fsr_wall.fetch_add(static_cast<long long>(__rdtsc() - t0), std::memory_order_relaxed);
    g_active.store(0);
    c_runs.fetch_add(1, std::memory_order_relaxed);
    return r;
}

extern "C" std::uint64_t fsc_trig(void* holder, void* sys, void* r8, void* r9) {
    std::int64_t mode = fsc_mode;
    if (mode == 3) mode = t_alt;
    if (mode == 0 || !g_active.load(std::memory_order_relaxed) || holder != g_holder || !sys)
        return trig_orig(holder, sys, r8, r9);
    const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(sys) + 8) & 0xffffffu;
    const std::uint64_t run = g_run.load(std::memory_order_relaxed);
    const std::uint64_t key = (run << 32) | (static_cast<std::uint64_t>(id) << 1);
    auto& slot = g_tab[id & (kSlots - 1)];
    const std::uint64_t v = slot.load(std::memory_order_relaxed);
    ++l_calls;
    std::uint64_t r;
    if ((v & ~1ull) == key) {
        ++l_hits;
        if (mode == 1) {
            r = v & 1;
        } else {
            r = trig_orig(holder, sys, r8, r9);
            if (((r & 0xff) != 0) != ((v & 1) != 0)) {
                const long long k = c_mis.fetch_add(1);
                if (k < 10) splug::log("MISMATCH: system %06x cached %d game %d", id, int(v & 1), int((r & 0xff) != 0));
            }
        }
    } else {
        r = trig_orig(holder, sys, r8, r9);
        slot.store(key | ((r & 0xff) ? 1 : 0), std::memory_order_relaxed);
        if ((v >> 32) == run) ++l_byp;  // коллизия индекса в том же запуске
    }
    if ((l_calls & 8191) == 0) flush_local();
    return r;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_fleetscan_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"fleetscan", L"mode", L"off");
    const auto cache = splug::ini_mode(L"fleetscan", L"cache", L"off");
    fsr_k = splug::ini_int(L"fleetscan", L"rr", 0);
    auto rva_of = [&](const void* p) {
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(module));
    };
    splug::log("stellaris_fleetscan_patch 0.3.0: build %s, mode=%s, cache=%s, rr=%lld, fsp_on at rva %llx, fsc_mode at rva %llx, fsr_k at rva %llx",
               splug::state().build, splug::mode_name(mode), splug::mode_name(cache), static_cast<long long>(fsr_k),
               rva_of(&fsp_on), rva_of(&fsc_mode), rva_of(&fsr_k));
    if (mode == splug::Mode::off && cache == splug::Mode::off && fsr_k <= 1) return;
    tls_idx = TlsAlloc();
    if (tls_idx >= 64) {
        splug::log("REFUSED: TLS index %lu not in the inline slots", tls_idx);
        return;
    }
    outer_orig = reinterpret_cast<Fn4>(splug::call_target(splug::rva(kOuter)));
    splug::CallPatch op;
    if (reinterpret_cast<std::uintptr_t>(outer_orig) == reinterpret_cast<std::uintptr_t>(splug::rva(0x140714e50)) &&
        !splug::prepare_call(splug::rva(kOuter), kOuterBytes, reinterpret_cast<const void*>(&fsp_outer), op) &&
        splug::commit_call(op))
        splug::log("timing hook at %llx", static_cast<unsigned long long>(kOuter));
    else
        splug::log("timing hook not installed (mode 3 unavailable)");
    if (mode != splug::Mode::off) install_prefetch();
    if (cache != splug::Mode::off) install_cache(cache);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
