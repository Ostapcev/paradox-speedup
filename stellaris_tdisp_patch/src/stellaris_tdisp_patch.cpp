// Stellaris Tdisp Patch 0.1.0, Stellaris 4.5.4 x64 — короткий вход в вычисление триггера.
//
// Что в игре (stellaris.exe 4.5.4.1b4f): у 1101 класса триггеров слот vtable +0x10 («вычислить») = 140348850:
//   если флаг профилирования триггеров [1431601b2] выключен — r = vt[+0xb8](this, ctx) ? vt[+0x18](this, ctx) : 0.
//   vt[+0xb8] у всех = 140348f90 («подходит ли область»):
//     mask = vt[+0x70](this) — у классов это константа (`mov eax, imm; ret`, `xor eax, eax; ret`, `mov rax, imm64; ret`);
//     mask == 0 → да; (ctx[+8] & mask) && ctx->vt[+0x38](ctx) → да; vt[+0x98](this) → да; иначе — ветка ошибки.
//   Итого на каждый триггер — два лишних кадра и 2–4 косвенных вызова до настоящей проверки vt[+0x18].
//
// Плагин: для каждого такого класса — свой переходник без кадра (слот +0x10 заменён):
//   флаг игры выключен и наш вентиль открыт, и
//     mask == 0                                                   → jmp vt[+0x18]
//     (ctx[+8] & mask) != 0 и ctx->vt[+0x38] == 1401ae130 (`mov al,1`) → jmp vt[+0x18]
//     (ctx[+8] & mask) == 0 и vt[+0x98] == 1401ae130               → jmp vt[+0x18]
//   всё остальное → jmp 140348850 (исходный путь целиком). Ответ и побочные эффекты те же, что у игры.
// [tdisp] mode=off / on / shadow (слоты ведут в проверку: решение переходника сверяется с настоящим 140348f90,
//   затем исходный 140348850).
// tdp_mode (rva в логе): 0 — вентиль закрыт (всегда исходный путь), 1 — открыт, 3 — замер: вентиль
//   открывается через фазу ИИ (140b8de80), в лог — стена фазы и сумма заданий по режимам.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t tdp_mode = 0;
std::int64_t tdp_and = 0;     // 0 — цикл AND игры, 1 — свой с подкачкой, 2 — свой без подкачки
std::int64_t tdp_and_pf = 1;  // на сколько детей вперёд подкачивать
std::uint64_t tdp_and_loop(void* self, void* ctx);
volatile std::uint8_t tdp_gate = 0;
std::uint64_t tdp_phase(void* desc, void** objs_ref);
std::uint64_t tdp_task(void* self, void* a, void* b, void* c);
std::uint64_t tdp_shadow(void* self, void* ctx);
struct TdpEntry;
std::uint64_t tdp_scope_call(void* self, void* ctx, const void* entry);
}

namespace {

constexpr std::uint64_t kEval = 0x140348850;
constexpr std::uint64_t kScope = 0x140348f90;
constexpr std::uint64_t kTrue = 0x1401ae130;  // mov al,1; ret
constexpr std::uint64_t kFlag = 0x1431601b2;
constexpr std::uint8_t kEvalHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57,
                                      0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b,
                                      0xf2, 0x48, 0x8b, 0xf9, 0x45, 0x33, 0xe4, 0x41, 0x8b, 0xdc, 0x48, 0x89,
                                      0x5c, 0x24, 0x50, 0x41, 0x8b, 0xec, 0x40, 0x38, 0x2d, 0x31, 0x79, 0xe1, 0x02};
constexpr std::uint8_t kEvalCalls[] = {0x48, 0x8b, 0x07, 0x49, 0x8b, 0xd6, 0x48, 0x8b, 0xcf, 0xff, 0x90, 0xb8,
                                       0x00, 0x00, 0x00, 0x84, 0xc0, 0x75, 0x05, 0x40, 0x32, 0xf6, 0xeb, 0x0f,
                                       0x48, 0x8b, 0x07, 0x49, 0x8b, 0xd6, 0x48, 0x8b, 0xcf, 0xff, 0x50, 0x18};
constexpr std::uint8_t kScopeHead[] = {0x48, 0x8b, 0x01, 0xff, 0x50, 0x70, 0x48, 0x8b, 0xf0, 0x48, 0x85, 0xc0,
                                       0x0f, 0x84, 0xf6, 0x01, 0x00, 0x00, 0x49, 0x85, 0x47, 0x08, 0x74, 0x11,
                                       0x49, 0x8b, 0x17, 0x49, 0x8b, 0xcf, 0xff, 0x52, 0x38, 0x84, 0xc0, 0x0f,
                                       0x85, 0xdf, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x06, 0x49, 0x8b, 0xce, 0xff,
                                       0x90, 0x98, 0x00, 0x00, 0x00, 0x84, 0xc0, 0x0f, 0x85, 0xcb, 0x01, 0x00, 0x00};
constexpr std::uint64_t kPhaseSite = 0x140b8dd5d;
constexpr std::uint8_t kPhaseBytes[5] = {0xe8, 0x1e, 0x01, 0x00, 0x00};

struct Entry {
    void** vt;
    std::uint64_t mask;
    std::uint64_t t18;
    bool a98;
};
constexpr int kMaxEntries = 2048;
Entry entries[kMaxEntries];
int n_entries = 0;
// поиск по vtable для тени: открытая адресация
constexpr int kHash = 4096;
int hash_idx[kHash];

std::uint64_t rt_eval = 0, rt_true = 0;
const volatile std::uint8_t* rt_flag = nullptr;
using Fn2 = std::uint64_t (*)(void*, void*);
using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);
Fn2 eval_orig = nullptr;

std::atomic<long long> c_sh{0}, c_fast{0}, c_mis{0}, c_fallback{0};
}  // namespace
using FnV = void* (*)();
using FnF = void (*)(void*);
std::uint64_t rt_and = 0;
FnV and_sentinel = nullptr;  // 14034cdc0
FnF and_free = nullptr;      // 142028b54
// замер ctx->vt38 (только чтение, на поток)
std::int64_t tdp_probe = 0;
thread_local long long p_n, p_cyc, p_rep, p_type[24];
thread_local const void* p_last_ctx;
thread_local std::uint64_t p_last_t, p_last_p;
thread_local std::uint32_t p_last_id;
std::atomic<long long> g_n{0}, g_cyc{0}, g_rep{0}, g_type[24];
namespace {
std::atomic<long long> r_m0{0}, r_hit_true{0}, r_hit_other{0}, r_miss_a98{0}, r_miss_no{0}, r_nomatch{0}, r_flag{0};
constexpr int kOv = 16;
std::atomic<std::uint64_t> ov_addr[kOv];
std::atomic<long long> ov_cnt[kOv];
void ov_add(std::uint64_t a) {
    for (int k = 0; k < kOv; ++k) {
        std::uint64_t cur = ov_addr[k].load(std::memory_order_relaxed);
        if (cur == a) { ov_cnt[k].fetch_add(1, std::memory_order_relaxed); return; }
        if (cur == 0) {
            std::uint64_t z = 0;
            if (ov_addr[k].compare_exchange_strong(z, a) || z == a) { ov_cnt[k].fetch_add(1, std::memory_order_relaxed); return; }
        }
    }
}
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};

int find_entry(void** vt) {
    unsigned h = static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(vt) >> 3) * 2654435761u) & (kHash - 1);
    for (int k = 0; k < kHash; ++k, h = (h + 1) & (kHash - 1)) {
        const int i = hash_idx[h];
        if (i < 0) return -1;
        if (entries[i].vt == vt) return i;
    }
    return -1;
}

void hash_add(int i) {
    unsigned h = static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(entries[i].vt) >> 3) * 2654435761u) & (kHash - 1);
    while (hash_idx[h] >= 0) h = (h + 1) & (kHash - 1);
    hash_idx[h] = i;
}

// константа, которую возвращает функция vt[+0x70]; false — не опознана
bool const_ret(const std::uint8_t* f, std::uint64_t& v) {
    if (f[0] == 0xb8 && f[5] == 0xc3) {
        std::uint32_t x;
        std::memcpy(&x, f + 1, 4);
        v = x;
        return true;
    }
    if (f[0] == 0x33 && f[1] == 0xc0 && f[2] == 0xc3) {
        v = 0;
        return true;
    }
    if (f[0] == 0x48 && f[1] == 0xb8 && f[10] == 0xc3) {
        std::memcpy(&v, f + 2, 8);
        return true;
    }
    if (f[0] == 0x48 && f[1] == 0xc7 && f[2] == 0xc0 && f[7] == 0xc3) {
        std::int32_t x;
        std::memcpy(&x, f + 3, 4);
        v = static_cast<std::uint64_t>(static_cast<std::int64_t>(x));
        return true;
    }
    return false;
}

// ---- генерация переходника ----
struct Asm {
    std::uint8_t* p;
    void b(std::initializer_list<int> xs) {
        for (int x : xs) *p++ = static_cast<std::uint8_t>(x);
    }
    void q(std::uint64_t v) {
        std::memcpy(p, &v, 8);
        p += 8;
    }
    std::uint8_t* jcc(int op) {  // короткий переход, смещение заполняется позже
        *p++ = static_cast<std::uint8_t>(op);
        *p++ = 0;
        return p - 1;
    }
    void bind(std::uint8_t* at) { *at = static_cast<std::uint8_t>(p - (at + 1)); }
};

std::uint8_t* emit_thunk(std::uint8_t* code, const Entry& e) {
    Asm a{code};
    a.b({0x49, 0xbb});  // mov r11, &tdp_gate
    a.q(reinterpret_cast<std::uint64_t>(&tdp_gate));
    a.b({0x41, 0x80, 0x3b, 0x00});  // cmp byte [r11], 0
    std::uint8_t* s1 = a.jcc(0x74);  // je slow
    a.b({0x49, 0xbb});  // mov r11, flag
    a.q(reinterpret_cast<std::uint64_t>(rt_flag));
    a.b({0x41, 0x80, 0x3b, 0x00});
    std::uint8_t* s2 = a.jcc(0x75);  // jne slow
    std::uint8_t* s3 = nullptr;
    std::uint8_t* s4 = nullptr;
    std::uint8_t* f1 = nullptr;
    if (e.mask != 0) {
        a.b({0x48, 0xb8});  // mov rax, mask
        a.q(e.mask);
        a.b({0x48, 0x85, 0x42, 0x08});  // test [rdx+8], rax
        if (e.a98)
            f1 = a.jcc(0x74);  // jz fast (vt98 = «да»)
        else
            s3 = a.jcc(0x74);  // jz slow
        a.b({0x48, 0x8b, 0x02});  // mov rax, [rdx]
        a.b({0x49, 0xbb});        // mov r11, 1401ae130
        a.q(rt_true);
        a.b({0x4c, 0x39, 0x58, 0x38});  // cmp [rax+0x38], r11
        s4 = a.jcc(0x74);               // je fast
        // другой ctx->vt38: вызов его и решение — в tdp_scope_call (обычная функция DLL с данными раскрутки)
        a.b({0x49, 0xb8});  // mov r8, &entry
        a.q(reinterpret_cast<std::uint64_t>(&e));
        a.b({0x48, 0xb8});  // mov rax, tdp_scope_call; jmp rax
        a.q(reinterpret_cast<std::uint64_t>(&tdp_scope_call));
        a.b({0xff, 0xe0});
    }
    if (f1) a.bind(f1);
    if (s4) a.bind(s4);
    a.b({0x48, 0xb8});  // mov rax, vt18; jmp rax
    a.q(e.t18);
    a.b({0xff, 0xe0});
    a.bind(s1);
    a.bind(s2);
    if (s3) a.bind(s3);
    a.b({0x48, 0xb8});  // mov rax, 140348850; jmp rax
    a.q(rt_eval);
    a.b({0xff, 0xe0});
    return a.p;
}

// ---- замер по фазам ИИ (как в aiq_probe) ----
Fn2 phase_orig = nullptr;
constexpr int kMaxVt = 8;
void* ai_vt[kMaxVt] = {};
Fn4 ai_orig[kMaxVt] = {};
std::atomic<int> ai_vt_n{0};
SRWLOCK ai_lock = SRWLOCK_INIT;
std::atomic<int> in_phase{0};
std::atomic<long long> phase_work{0};
long long phase_count = 0;
double m_n[2], m_wall[2], m_work[2];

Fn4 ai_orig_for(void* vt) {
    const int n = ai_vt_n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (ai_vt[i] == vt) return ai_orig[i];
    return nullptr;
}

void ai_hook(void* obj) {
    void** vt = *reinterpret_cast<void***>(obj);
    if (ai_orig_for(vt)) return;
    AcquireSRWLockExclusive(&ai_lock);
    if (!ai_orig_for(vt)) {
        const int n = ai_vt_n.load();
        if (n < kMaxVt) {
            ai_orig[n] = reinterpret_cast<Fn4>(vt[0x38 / 8]);
            ai_vt[n] = vt;
            void* h = reinterpret_cast<void*>(&tdp_task);
            if (splug::write_bytes(&vt[0x38 / 8], &h, 8)) ai_vt_n.store(n + 1, std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&ai_lock);
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    if (q.QuadPart - last < 60 * freq.QuadPart || !rep_qpc.compare_exchange_strong(last, q.QuadPart)) return;
    if (m_n[0] || m_n[1])
        splug::log(tdp_mode >= 4 ? "AI phases: game AND %.0f (wall %.0f, task sum %.0f Mcyc avg), own AND %.0f (wall %.0f, task sum %.0f Mcyc avg)" : "AI phases: gate closed %.0f (wall %.0f, task sum %.0f Mcyc avg), gate open %.0f (wall %.0f, task sum %.0f Mcyc avg)",
                   m_n[0], m_n[0] ? m_wall[0] / m_n[0] : 0.0, m_n[0] ? m_work[0] / m_n[0] : 0.0, m_n[1],
                   m_n[1] ? m_wall[1] / m_n[1] : 0.0, m_n[1] ? m_work[1] / m_n[1] : 0.0);
    for (int k = 0; k < 2; ++k) m_n[k] = m_wall[k] = m_work[k] = 0;
    if (g_n.load()) {
        const long long n = g_n.exchange(0);
        char buf[600];
        int L = 0;
        for (int k = 0; k < 24; ++k) {
            const long long v = g_type[k].exchange(0);
            if (v) L += wsprintfA(buf + L, " 2^%d:%d%%", k, static_cast<int>(v * 100 / n));
        }
        splug::log("vt38 probe: calls %lld, avg %.0f cyc, same ctx+fields as previous call on thread %.1f%%, scope type bits:%s", n,
                   static_cast<double>(g_cyc.exchange(0)) / n, 100.0 * g_rep.exchange(0) / n, buf);
    }
    if (tdp_mode != 2) splug::log("helper: ctx->vt38 said 'no' (went to the original path) %lld", c_fallback.exchange(0));
    if (c_sh.load()) {
        splug::log("shadow: evaluations %lld, fast decision %lld, mismatches %lld", c_sh.exchange(0), c_fast.exchange(0),
                   c_mis.load());
        splug::log("  reasons: mask 0 %lld; ctx in scope + vt38=true %lld; ctx in scope + other vt38 %lld; out of scope + vt98 true %lld; out of scope + vt98 false %lld; class not in table %lld; profiling flag %lld",
                   r_m0.exchange(0), r_hit_true.exchange(0), r_hit_other.exchange(0), r_miss_a98.exchange(0),
                   r_miss_no.exchange(0), r_nomatch.exchange(0), r_flag.exchange(0));
        const std::uintptr_t img = reinterpret_cast<std::uintptr_t>(splug::rva(0x140000000));
        for (int k = 0; k < kOv; ++k) {
            const std::uint64_t a = ov_addr[k].load();
            if (!a) break;
            splug::log("  other ctx vt38 %llx (va %llx): %lld", static_cast<unsigned long long>(a),
                       static_cast<unsigned long long>(a - img + 0x140000000), ov_cnt[k].exchange(0));
        }
    }
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_tdisp_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"tdisp", L"mode", L"off");
    tdp_probe = splug::ini_int(L"tdisp", L"probe", 0);
    splug::log("stellaris_tdisp_patch 0.1.0: build %s, mode=%s, probe=%lld, tdp_mode at rva %llx", splug::state().build,
               splug::mode_name(mode), static_cast<long long>(tdp_probe),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&tdp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    if (!splug::bytes_match(kEval, kEvalHead) || !splug::bytes_match(0x1403488da, kEvalCalls) ||
        !splug::bytes_match(0x140348fb6, kScopeHead)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    {
        const std::uint8_t t[3] = {0xb0, 0x01, 0xc3};
        if (!splug::bytes_match(kTrue, t)) {
            splug::log("REFUSED: 1401ae130 differs");
            return;
        }
    }
    rt_eval = reinterpret_cast<std::uint64_t>(splug::rva(kEval));
    rt_true = reinterpret_cast<std::uint64_t>(splug::rva(kTrue));
    rt_flag = splug::rva(kFlag);
    eval_orig = reinterpret_cast<Fn2>(rt_eval);
    const std::uint64_t rt_scope = reinterpret_cast<std::uint64_t>(splug::rva(kScope));
    for (int i = 0; i < kHash; ++i) hash_idx[i] = -1;

    // поиск vtable: слот +0x10 = 140348850 во всех секциях, кроме кода
    auto* base = splug::rva(0x140000000);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    std::uintptr_t text_lo = 0, text_hi = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        if (sec[s].Characteristics & IMAGE_SCN_CNT_CODE) {
            text_lo = reinterpret_cast<std::uintptr_t>(base) + sec[s].VirtualAddress;
            text_hi = text_lo + sec[s].Misc.VirtualSize;
        }
    int seen = 0, skipped_mask = 0, skipped_other = 0, n_and = 0;
    rt_and = reinterpret_cast<std::uint64_t>(splug::rva(0x14034a6c0));
    and_sentinel = reinterpret_cast<FnV>(splug::rva(0x14034cdc0));
    and_free = reinterpret_cast<FnF>(splug::rva(0x142028b54));
    bool and_enabled = false;
    {
        // 14034a6c0 целиком до ret (проверка, что цикл тот же)
        constexpr std::uint8_t kAnd[] = {
            0x4c, 0x63, 0xb9, 0x8c, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xff, 0x0f, 0x8e, 0x88, 0x00, 0x00, 0x00, 0x45, 0x33,
            0xed, 0x41, 0x8b, 0xf5, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x87, 0x80, 0x00, 0x00,
            0x00, 0x48, 0x8b, 0x0c, 0xf0, 0x80, 0x7f, 0x08, 0x00, 0x75, 0x1c, 0x48, 0x83, 0x7f, 0x30, 0x00, 0x75, 0x0f,
            0x48, 0x8b, 0x01, 0x49, 0x8b, 0xd4, 0xff, 0x50, 0x10, 0x44, 0x0f, 0xb6, 0xf0, 0xeb, 0x46, 0x48, 0x8b, 0x47,
            0x30, 0xeb, 0x04, 0x48, 0x8d, 0x47, 0x10, 0x48, 0x8d, 0x59, 0x08, 0x48, 0x89, 0x5c, 0x24, 0x50, 0x48, 0x89,
            0x43, 0x28, 0x48, 0x8b, 0x01, 0x49, 0x8b, 0xd4, 0xff, 0x50, 0x10, 0x44, 0x0f, 0xb6, 0xf0, 0x80, 0x3b, 0x00,
            0x74, 0x19, 0xc6, 0x03, 0x00, 0x48, 0x8b, 0x6b, 0x10, 0xe8, 0x64, 0x26, 0x00, 0x00, 0x48, 0x3b, 0xe8, 0x74,
            0x08, 0x48, 0x8b, 0xcd, 0xe8, 0xeb, 0xe3, 0xcd, 0x01, 0x4c, 0x89, 0x6b, 0x28, 0x45, 0x84, 0xf6, 0x74, 0x27,
            0x48, 0xff, 0xc6, 0x49, 0x3b, 0xf7, 0x7c, 0x86, 0xb0, 0x01};
        const int ini_and = splug::ini_int(L"tdisp", L"and", 0);  // своя копия AND: медленнее и упала (см. лог исследований)
        tdp_and_pf = splug::ini_int(L"tdisp", L"and_prefetch", 1);
        and_enabled = ini_and > 0 && splug::bytes_match(0x14034a6e2, kAnd);
        tdp_and = and_enabled ? ini_and : 0;
        splug::log("AND loop 14034a6c0: %s (tdp_and %lld, prefetch %lld ahead)", and_enabled ? "own copy" : "game",
                   static_cast<long long>(tdp_and), static_cast<long long>(tdp_and_pf));
    }
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (sec[s].Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_WRITE)) continue;
        auto* lo = reinterpret_cast<std::uint64_t*>(base + sec[s].VirtualAddress);
        const std::size_t n = sec[s].Misc.VirtualSize / 8;
        for (std::size_t k = 2; k + 0x20 < n; ++k) {
            if (lo[k] != rt_eval) continue;
            void** vt = reinterpret_cast<void**>(&lo[k - 2]);
            const auto in_text = [&](void* f) {
                const auto v = reinterpret_cast<std::uintptr_t>(f);
                return v >= text_lo && v < text_hi;
            };
            if (!in_text(vt[0]) || !in_text(vt[1]) || !in_text(vt[3])) continue;
            ++seen;
            if (reinterpret_cast<std::uint64_t>(vt[0xb8 / 8]) != rt_scope || !in_text(vt[0x70 / 8])) {
                ++skipped_other;
                continue;
            }
            std::uint64_t m = 0;
            if (!const_ret(static_cast<const std::uint8_t*>(vt[0x70 / 8]), m)) {
                ++skipped_mask;
                continue;
            }
            if (n_entries >= kMaxEntries) break;
            std::uint64_t t18 = reinterpret_cast<std::uint64_t>(vt[0x18 / 8]);
            if (t18 == rt_and && and_enabled) {
                t18 = reinterpret_cast<std::uint64_t>(&tdp_and_loop);
                ++n_and;
            }
            entries[n_entries] = {vt, m, t18, reinterpret_cast<std::uint64_t>(vt[0x98 / 8]) == rt_true};
            hash_add(n_entries);
            ++n_entries;
        }
    }
    splug::log("vtables with eval slot: %d; usable %d (AND %d), mask not constant %d, other %d", seen, n_entries, n_and,
               skipped_mask, skipped_other);
    if (n_entries == 0) return;

    // замер фаз ИИ
    phase_orig = reinterpret_cast<Fn2>(splug::call_target(splug::rva(kPhaseSite)));
    splug::CallPatch pp;
    const bool phase_ok =
        reinterpret_cast<std::uintptr_t>(phase_orig) == reinterpret_cast<std::uintptr_t>(splug::rva(0x140b8de80)) &&
        !splug::prepare_call(splug::rva(kPhaseSite), kPhaseBytes, reinterpret_cast<const void*>(&tdp_phase), pp);

    if (mode == splug::Mode::shadow) {
        tdp_mode = 2;
        void* h = reinterpret_cast<void*>(&tdp_shadow);
        for (int i = 0; i < n_entries; ++i) splug::write_bytes(&entries[i].vt[0x10 / 8], &h, 8);
        if (phase_ok) splug::commit_call(pp);
        splug::log("shadow: %d slots point to the checker", n_entries);
        return;
    }
    auto* code = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, static_cast<std::size_t>(n_entries) * 192 + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code) {
        splug::log("REFUSED: no memory for thunks");
        return;
    }
    std::uint8_t* p = code;
    void** thunks = static_cast<void**>(VirtualAlloc(nullptr, n_entries * 8 + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    for (int i = 0; i < n_entries; ++i) {
        thunks[i] = p;
        p = emit_thunk(p, entries[i]);
        while (reinterpret_cast<std::uintptr_t>(p) & 15) *p++ = 0xcc;
    }
    {
        int shown = 0;
        for (int i = 0; i < n_entries && shown < 3; ++i) {
            const bool want = shown == 0 ? entries[i].mask == 0 : shown == 1 ? (entries[i].mask && entries[i].a98)
                                                                            : (entries[i].mask && !entries[i].a98);
            if (!want) continue;
            char hex[400];
            int L = 0;
            const auto* t = static_cast<const std::uint8_t*>(thunks[i]);
            const int len = static_cast<int>((i + 1 < n_entries ? static_cast<const std::uint8_t*>(thunks[i + 1]) : p) - t);
            for (int k = 0; k < len && L < 390; ++k) L += wsprintfA(hex + L, "%02x", t[k]);
            splug::log("thunk %d at %p (mask %llx a98 %d t18 %llx): %s", i, thunks[i],
                       static_cast<unsigned long long>(entries[i].mask), entries[i].a98 ? 1 : 0,
                       static_cast<unsigned long long>(entries[i].t18), hex);
            ++shown;
        }
    }
    DWORD old = 0;
    VirtualProtect(code, static_cast<std::size_t>(p - code), PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, static_cast<std::size_t>(p - code));
    tdp_mode = 1;
    tdp_gate = 1;
    int written = 0;
    for (int i = 0; i < n_entries; ++i) written += splug::write_bytes(&entries[i].vt[0x10 / 8], &thunks[i], 8) ? 1 : 0;
    if (phase_ok) splug::commit_call(pp);
    splug::log("installed: %d thunks (%lld bytes), %d slots written, phase timing %s", n_entries,
               static_cast<long long>(p - code), written, phase_ok ? "on" : "off");
}

}  // namespace

using Fn1 = std::uint64_t (*)(void*);

// копия 14034a6c0 (AND): число детей читается один раз, массив [this+0x80] — на каждом шаге,
// флаги [this+8] / [this+0x30] — на каждом шаге, как у игры
extern "C" std::uint64_t tdp_and_loop(void* self, void* ctx) {
    const std::int64_t m = tdp_and;
    if (m == 0) return reinterpret_cast<Fn2>(rt_and)(self, ctx);
    auto* t = static_cast<std::uint8_t*>(self);
    const std::int64_t n = *reinterpret_cast<const std::int32_t*>(t + 0x8c);
    const std::int64_t pf = m == 1 ? tdp_and_pf : 0;
    for (std::int64_t i = 0; i < n; ++i) {
        void** arr = *reinterpret_cast<void***>(t + 0x80);
        void* c = arr[i];
        if (pf && i + pf < n) __builtin_prefetch(arr[i + pf]);
        bool r;
        void* tip = nullptr;
        if (*t != 0)
            tip = t + 0x10;
        else if (*reinterpret_cast<void**>(t + 0x30) != nullptr)
            tip = *reinterpret_cast<void**>(t + 0x30);
        void** cvt = *static_cast<void***>(c);
        if (!tip) {
            r = (reinterpret_cast<Fn2>(cvt[2])(c, ctx) & 0xff) != 0;
        } else {
            auto* b = static_cast<std::uint8_t*>(c) + 8;
            *reinterpret_cast<void**>(b + 0x28) = tip;
            r = (reinterpret_cast<Fn2>(cvt[2])(c, ctx) & 0xff) != 0;
            if (*b != 0) {
                *b = 0;
                void* q = *reinterpret_cast<void**>(b + 0x10);
                if (q != and_sentinel()) and_free(q);
            }
            *reinterpret_cast<void**>(b + 0x28) = nullptr;
        }
        if (!r) return 0;
    }
    return 1;
}
extern "C" std::uint64_t tdp_scope_call(void* self, void* ctx, const void* entry) {
    const Entry& e = *static_cast<const Entry*>(entry);
    void** cvt = *static_cast<void***>(ctx);
    if (tdp_probe) {
        const auto* c = static_cast<const std::uint8_t*>(ctx);
        std::uint64_t t, pp;
        std::uint32_t id;
        std::memcpy(&t, c + 8, 8);
        std::memcpy(&id, c + 0x10, 4);
        std::memcpy(&pp, c + 0x1c, 8);
        if (ctx == p_last_ctx && t == p_last_t && id == p_last_id && pp == p_last_p) ++p_rep;
        p_last_ctx = ctx;
        p_last_t = t;
        p_last_id = id;
        p_last_p = pp;
        ++p_type[t ? (63 - __builtin_clzll(t)) % 24 : 0];
        const unsigned long long t0 = __rdtsc();
        const std::uint64_t ok = reinterpret_cast<Fn1>(cvt[0x38 / 8])(ctx) & 0xff;
        p_cyc += static_cast<long long>(__rdtsc() - t0);
        if ((++p_n & 4095) == 0) {
            g_n.fetch_add(p_n);
            g_cyc.fetch_add(p_cyc);
            g_rep.fetch_add(p_rep);
            for (int k = 0; k < 24; ++k) g_type[k].fetch_add(p_type[k]), p_type[k] = 0;
            p_n = p_cyc = p_rep = 0;
        }
        if (ok) return reinterpret_cast<Fn2>(e.t18)(self, ctx);
        if (e.a98) return reinterpret_cast<Fn2>(e.t18)(self, ctx);
        c_fallback.fetch_add(1, std::memory_order_relaxed);
        return eval_orig(self, ctx);
    }
    // как 140348f90: (ctx[+8] & mask) уже проверено переходником; ctx->vt38 вызывается ровно один раз
    if (reinterpret_cast<Fn1>(cvt[0x38 / 8])(ctx) & 0xff) return reinterpret_cast<Fn2>(e.t18)(self, ctx);
    if (e.a98) return reinterpret_cast<Fn2>(e.t18)(self, ctx);
    c_fallback.fetch_add(1, std::memory_order_relaxed);
    return eval_orig(self, ctx);  // редкая ветка (область не подходит): исходный путь целиком
}

extern "C" std::uint64_t tdp_shadow(void* self, void* ctx) {
    void** vt = *static_cast<void***>(self);
    const int i = find_entry(vt);
    if (i < 0) r_nomatch.fetch_add(1, std::memory_order_relaxed);
    else if (*rt_flag) r_flag.fetch_add(1, std::memory_order_relaxed);
    if (i >= 0 && *rt_flag == 0) {
        const Entry& e = entries[i];
        bool fast = false;
        if (e.mask == 0) {
            fast = true;
            r_m0.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::uint64_t cm = *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(ctx) + 8);
            if (cm & e.mask) {
                void** cvt = *static_cast<void***>(ctx);
                fast = reinterpret_cast<std::uint64_t>(cvt[0x38 / 8]) == rt_true;
                if (fast) r_hit_true.fetch_add(1, std::memory_order_relaxed);
                else { r_hit_other.fetch_add(1, std::memory_order_relaxed); ov_add(reinterpret_cast<std::uint64_t>(cvt[0x38 / 8])); }
            } else {
                fast = e.a98;
                (fast ? r_miss_a98 : r_miss_no).fetch_add(1, std::memory_order_relaxed);
            }
        }
        c_sh.fetch_add(1, std::memory_order_relaxed);
        if (fast) {
            c_fast.fetch_add(1, std::memory_order_relaxed);
            // настоящий 140348f90 при «да» без побочных эффектов
            const auto scope = reinterpret_cast<Fn2>(vt[0xb8 / 8]);
            if ((scope(self, ctx) & 0xff) == 0) {
                const long long k = c_mis.fetch_add(1);
                if (k < 10) splug::log("MISMATCH: vt %p mask %llx", static_cast<void*>(vt), static_cast<unsigned long long>(e.mask));
            }
        }
    }
    return eval_orig(self, ctx);
}

extern "C" std::uint64_t tdp_task(void* self, void* a, void* b, void* c) {
    Fn4 o = ai_orig_for(*reinterpret_cast<void**>(self));
    if (!in_phase.load(std::memory_order_relaxed)) return o(self, a, b, c);
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = o(self, a, b, c);
    phase_work.fetch_add(static_cast<long long>(__rdtsc() - t0), std::memory_order_relaxed);
    return r;
}

extern "C" std::uint64_t tdp_phase(void* desc, void** objs_ref) {
    const int n = *reinterpret_cast<int*>(static_cast<std::uint8_t*>(desc) + 0x10);
    void** objs = reinterpret_cast<void**>(*objs_ref);
    for (int i = 0; i < n; ++i)
        if (objs[i]) ai_hook(objs[i]);
    int g = -1;
    if (tdp_mode == 3) {
        g = static_cast<int>(++phase_count & 1);
        tdp_gate = static_cast<std::uint8_t>(g);
    } else if (tdp_mode >= 4) {
        tdp_gate = 1;
        g = static_cast<int>(++phase_count & 1);
        tdp_and = g ? tdp_mode - 3 : 0;  // 4 — свой с подкачкой, 5 — свой без подкачки
    } else if (tdp_mode == 0 || tdp_mode == 1) {
        tdp_gate = static_cast<std::uint8_t>(tdp_mode);
        g = static_cast<int>(tdp_mode);
    }
    phase_work.store(0);
    in_phase.store(1);
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = phase_orig(desc, objs_ref);
    const unsigned long long t1 = __rdtsc();
    in_phase.store(0);
    if (g >= 0) {
        m_n[g] += 1;
        m_wall[g] += static_cast<double>(t1 - t0) / 1e6;
        m_work[g] += static_cast<double>(phase_work.load()) / 1e6;
    }
    report();
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
