// Stellaris Lazy Patch 0.1.0, Stellaris 4.5.4 x64 — дневные расчёты страны раз в K дней (МЕНЯЕТ ПОВЕДЕНИЕ).
//
// Каждое место — round-robin: работа делается, если (ключ + день) % K == 0, иначе пропускается (остаётся прошлый
// результат). K ≤ 1 — каждый день, как в игре. Все K меняются на лету (rva в логе).
//   vis — видимость страны: дневная задача 140301710 зовёт 14074fa80(страна) [1403017e0] и 14074f8e0(страна)
//         [1403017e8]. Ключ — id страны [c+0x20]. Обе части пропускаются вместе.
//   sec — сектора: 14094b7e0 по всем странам зовёт 14094ba30(id) [14094b84a] — оценки систем секторов. Ключ — id.
//   int — уровень разведки пары стран: задача 14096e860 зовёт 1407b0330(запись, владелец, цель) [14096e952].
//         Ключ — id владельца + 7 * id цели.
//   cb  — поводы к войне: слот +0x48 (1406a7e60) четырёх vtable модулей страны; пропуск только при вызове из
//         дневного обновления страны 1406f1660 (адрес возврата 1406f16c0). Ключ — адрес модуля.
// [lazy] vis=K sec=K int=K cb=K. Лог раз в 60 с (и при смене K): запусков/пропусков и такты по каждому месту.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t lz_vis = 0, lz_sec = 0, lz_int = 0, lz_cb = 0;
std::int64_t lz_vispart = 0;  // 0 — пропускать обе части видимости, 1 — только 14074fa80, 2 — только 14074f8e0
void lz_vis_a(void* c);
void lz_vis_b(void* c);
void lz_sec_h(std::uint32_t id);
void lz_int_h(void* e, void* owner, void* target);
void lz_cb_h(void* m, std::uint64_t a);
}

namespace {

using Fn1 = void (*)(void*);
using FnId = void (*)(std::uint32_t);
using Fn3 = void (*)(void*, void*, void*);
using FnCb = void (*)(void*, std::uint64_t);

Fn1 vis_a = nullptr, vis_b = nullptr;
FnId sec_f = nullptr;
Fn3 int_f = nullptr;
FnCb cb_f = nullptr;
const std::uint8_t* const* g_clock = nullptr;
std::uintptr_t cb_ret = 0;
thread_local bool t_vis_skip = false;

enum { kVis, kSec, kInt, kCb, kN };
const char* const kNames[kN] = {"vis", "sec", "int", "cb"};
std::int64_t* const kVars[kN] = {&lz_vis, &lz_sec, &lz_int, &lz_cb};
std::atomic<long long> c_run[kN], c_skip[kN], c_cyc[kN];
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};

inline std::uint32_t today() {
    const std::uint8_t* c = *g_clock;
    std::uint32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}

inline std::uint32_t id_of(const void* obj) {
    std::uint32_t id = 0;
    std::memcpy(&id, static_cast<const std::uint8_t*>(obj) + 0x20, 4);
    return id & 0xffffff;
}

inline bool skip(int site, std::uint64_t key) {
    const std::int64_t k = *kVars[site];
    if (k > 1 && (key + today()) % static_cast<std::uint64_t>(k) != 0) {
        c_skip[site].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    static std::int64_t win[kN] = {-1, -1, -1, -1};
    bool switched = false;
    for (int i = 0; i < kN; ++i) switched |= win[0] != -1 && win[i] != *kVars[i];
    if ((q.QuadPart - last < 60 * freq.QuadPart && !switched) || !rep_qpc.compare_exchange_strong(last, q.QuadPart))
        return;
    std::int64_t shown[kN];
    for (int i = 0; i < kN; ++i) {
        shown[i] = win[0] == -1 ? *kVars[i] : win[i];
        win[i] = *kVars[i];
    }
    const double secs = static_cast<double>(q.QuadPart - last) / static_cast<double>(freq.QuadPart);
    char buf[512];
    int n = std::snprintf(buf, sizeof buf, "%.0f s, day %u |", secs, today());
    for (int i = 0; i < kN; ++i)
        n += std::snprintf(buf + n, sizeof buf - n, " %s k=%lld run %lld skip %lld %.0f Mcyc;", kNames[i],
                           static_cast<long long>(shown[i]), c_run[i].exchange(0), c_skip[i].exchange(0),
                           c_cyc[i].exchange(0) / 1e6);
    splug::log("%s", buf);
}

inline void account(int site, unsigned long long t0) {
    c_run[site].fetch_add(1, std::memory_order_relaxed);
    c_cyc[site].fetch_add(static_cast<long long>(__rdtsc() - t0), std::memory_order_relaxed);
}

bool hook_call(std::uint64_t va, std::uint64_t target, const void* hook, void** original) {
    std::uint8_t* site = splug::rva(va);
    if (site[0] != 0xe8) {
        splug::log("REFUSED %llx: not a call", static_cast<unsigned long long>(va));
        return false;
    }
    void* cur = splug::call_target(site);
    if (cur != splug::rva(target))
        splug::log("note %llx: call goes to %p, not %llx (another plugin) — chaining", static_cast<unsigned long long>(va),
                   cur, static_cast<unsigned long long>(target));
    std::uint8_t bytes[5];
    std::memcpy(bytes, site, 5);
    splug::CallPatch p;
    if (const char* e = splug::prepare_call(site, bytes, hook, p)) {
        splug::log("REFUSED %llx: %s", static_cast<unsigned long long>(va), e);
        return false;
    }
    *original = cur;
    if (!splug::commit_call(p)) {
        splug::log("REFUSED %llx: commit", static_cast<unsigned long long>(va));
        return false;
    }
    return true;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_lazy_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"lazy", L"mode", L"on");
    lz_vis = splug::ini_int(L"lazy", L"vis", 0);
    lz_sec = splug::ini_int(L"lazy", L"sec", 0);
    lz_int = splug::ini_int(L"lazy", L"int", 0);
    lz_cb = splug::ini_int(L"lazy", L"cb", 0);
    lz_vispart = splug::ini_int(L"lazy", L"vispart", 0);
    const auto rva_of = [&](const void* p) {
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(module));
    };
    splug::log("stellaris_lazy_patch 0.1.0: build %s, mode=%s, vis=%lld sec=%lld int=%lld cb=%lld; lz_vis at rva %llx, lz_sec at rva %llx, lz_int at rva %llx, lz_cb at rva %llx, vispart=%lld",
               splug::state().build, splug::mode_name(mode), static_cast<long long>(lz_vis), static_cast<long long>(lz_sec),
               static_cast<long long>(lz_int), static_cast<long long>(lz_cb), rva_of(&lz_vis), rva_of(&lz_sec),
               rva_of(&lz_int), rva_of(&lz_cb), static_cast<long long>(lz_vispart));
    if (mode == splug::Mode::off) return;
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);

    void* o = nullptr;
    if (hook_call(0x1403017e0, 0x14074fa80, reinterpret_cast<const void*>(&lz_vis_a), &o)) vis_a = reinterpret_cast<Fn1>(o);
    if (vis_a && hook_call(0x1403017e8, 0x14074f8e0, reinterpret_cast<const void*>(&lz_vis_b), &o)) vis_b = reinterpret_cast<Fn1>(o);
    if (hook_call(0x14094b84a, 0x14094ba30, reinterpret_cast<const void*>(&lz_sec_h), &o)) sec_f = reinterpret_cast<FnId>(o);
    if (hook_call(0x14096e952, 0x1407b0330, reinterpret_cast<const void*>(&lz_int_h), &o)) int_f = reinterpret_cast<Fn3>(o);

    const std::uint64_t vts[4] = {0x142341fd8, 0x142396738, 0x142396828, 0x142396920};
    cb_f = reinterpret_cast<FnCb>(splug::rva(0x1406a7e60));
    cb_ret = reinterpret_cast<std::uintptr_t>(splug::rva(0x1406f16c0));
    const auto orig = reinterpret_cast<std::uint64_t>(cb_f);
    const auto hook = reinterpret_cast<std::uint64_t>(&lz_cb_h);
    int ok = 0;
    for (std::uint64_t vt : vts) {
        auto* slot = splug::rva<std::uint64_t>(vt + 0x48);
        if (*slot != orig) {
            splug::log("cb: vtable %llx slot +0x48 differs, skipped", static_cast<unsigned long long>(vt));
            continue;
        }
        if (splug::write_bytes(slot, &hook, 8)) ++ok;
    }
    splug::log("installed: vis %s, sec %s, int %s, cb %d/4 vtables", vis_b ? "yes" : "no", sec_f ? "yes" : "no",
               int_f ? "yes" : "no", ok);
}

}  // namespace

extern "C" void lz_vis_a(void* c) {
    t_vis_skip = c && skip(kVis, id_of(c));
    if (t_vis_skip && lz_vispart != 2) return;
    const unsigned long long t0 = __rdtsc();
    vis_a(c);
    account(kVis, t0);
    report();
}

extern "C" void lz_vis_b(void* c) {
    if (t_vis_skip) {
        t_vis_skip = false;
        if (lz_vispart != 1) return;
    }
    const unsigned long long t0 = __rdtsc();
    vis_b(c);
    c_cyc[kVis].fetch_add(static_cast<long long>(__rdtsc() - t0), std::memory_order_relaxed);
}

extern "C" void lz_sec_h(std::uint32_t id) {
    if (skip(kSec, id & 0xffffff)) return;
    const unsigned long long t0 = __rdtsc();
    sec_f(id);
    account(kSec, t0);
}

extern "C" void lz_int_h(void* e, void* owner, void* target) {
    if (owner && target && skip(kInt, id_of(owner) + 7ull * id_of(target))) return;
    const unsigned long long t0 = __rdtsc();
    int_f(e, owner, target);
    account(kInt, t0);
}

extern "C" void lz_cb_h(void* m, std::uint64_t a) {
    if (reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) == cb_ret &&
        skip(kCb, reinterpret_cast<std::uintptr_t>(m) >> 6))
        return;
    const unsigned long long t0 = __rdtsc();
    cb_f(m, a);
    account(kCb, t0);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
