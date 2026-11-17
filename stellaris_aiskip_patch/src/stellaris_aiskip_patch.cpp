// Stellaris AI Skip Patch 0.1.0, Stellaris 4.5.4 x64 — дневное обновление ИИ страны не каждый день (МЕНЯЕТ ПОВЕДЕНИЕ).
//
// Что в игре (stellaris.exe 4.5.4.1b4f): фаза ИИ 14026429b → 140b8db40 (параллельный for), задача — vcall +0x38 =
// 140b75420(ai): проверки (страна жива, ИИ включён ...) и в 140b75514 `jmp 141b046f0` — само обновление ИИ страны.
// Страна — [ai+0x3a0] & 0xffffff. Если проверки не прошли, задача возвращает 1 — так же возвращает и плагин при пропуске.
//
// [aiskip] rr=K — страна обновляется в дни, где (id + день) % K == 0 (K ≤ 1 — каждый день).
//          heavy=M — страна, чьё прошлое обновление было вчера и стоило > M млн тактов, сегодня пропускается (0 — нет).
// ask_k, ask_heavy (rva в логе) меняются на лету. Лог раз в 60 с: число обновлений, пропусков, такты ИИ, стена фазы.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t ask_k = 0;
std::int64_t ask_heavy = 0;
std::uint64_t ask_task(void* ai);
std::uint64_t ask_phase(void* a, void* b, void* c, void* d);
}

namespace {

constexpr std::uint64_t kJmp = 0x140b75514;
constexpr std::uint8_t kJmpBytes[5] = {0xe9, 0xd7, 0xf1, 0xf8, 0x00};
constexpr std::uint64_t kUpdate = 0x141b046f0;
constexpr std::uint64_t kPhase = 0x14026429b;
constexpr std::uint8_t kPhaseBytes[5] = {0xe8, 0xa0, 0x98, 0x92, 0x00};

using TaskFn = std::uint64_t (*)(void*);
using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);
TaskFn update = nullptr;
Fn4 phase = nullptr;
const std::uint8_t* const* g_clock = nullptr;

struct Slot {
    std::atomic<std::uint32_t> day{0};
    std::atomic<std::uint32_t> mcyc{0};
};
Slot slots[1 << 16];

std::atomic<long long> c_run{0}, c_skip{0}, c_cyc{0}, c_wall{0}, c_phases{0}, c_max{0};
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};

inline std::uint32_t today() {
    const std::uint8_t* c = *g_clock;
    std::uint32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    static std::int64_t win_k = -1, win_h = -1;
    const bool switched = win_k != -1 && (win_k != ask_k || win_h != ask_heavy);
    if ((q.QuadPart - last < 60 * freq.QuadPart && !switched) || !rep_qpc.compare_exchange_strong(last, q.QuadPart))
        return;
    const std::int64_t sk = win_k == -1 ? ask_k : win_k, sh = win_h == -1 ? ask_heavy : win_h;
    win_k = ask_k;
    win_h = ask_heavy;
    const double secs = static_cast<double>(q.QuadPart - last) / static_cast<double>(freq.QuadPart);
    splug::log("rr=%lld heavy=%lld: %.0f s, phases %lld, updates run %lld, skipped %lld, AI %.0f Mcyc, phase wall %.0f Mcyc, max update %.0f Mcyc, day %u",
               static_cast<long long>(sk), static_cast<long long>(sh), secs, c_phases.exchange(0), c_run.exchange(0),
               c_skip.exchange(0), c_cyc.exchange(0) / 1e6, c_wall.exchange(0) / 1e6, c_max.exchange(0) / 1e6, today());
}

bool patch_jmp() {
    splug::CallPatch p;
    if (const char* e = splug::prepare_call(splug::rva(kJmp), kJmpBytes, reinterpret_cast<const void*>(&ask_task), p)) {
        splug::log("REFUSED jmp: %s", e);
        return false;
    }
    p.replacement[0] = 0xe9;  // хвостовой переход, как в игре
    if (!splug::commit_call(p)) {
        splug::log("REFUSED jmp: commit");
        return false;
    }
    return true;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_aiskip_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"aiskip", L"mode", L"off");
    ask_k = splug::ini_int(L"aiskip", L"rr", 0);
    ask_heavy = splug::ini_int(L"aiskip", L"heavy", 0);
    const auto rva_of = [&](const void* p) {
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(module));
    };
    splug::log("stellaris_aiskip_patch 0.1.0: build %s, mode=%s, rr=%lld, heavy=%lld, ask_k at rva %llx, ask_heavy at rva %llx",
               splug::state().build, splug::mode_name(mode), static_cast<long long>(ask_k),
               static_cast<long long>(ask_heavy), rva_of(&ask_k), rva_of(&ask_heavy));
    if (mode == splug::Mode::off) return;
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    update = reinterpret_cast<TaskFn>(splug::rva(kUpdate));
    std::uint8_t* ps = splug::rva(kPhase);
    // stellaris_ai_patch (грузится раньше) может уже подменить этот вызов — тогда цепляемся за его обёртку.
    if (ps[0] != 0xe8) {
        splug::log("REFUSED: phase site is not a call");
        return;
    }
    const bool chained = std::memcmp(ps, kPhaseBytes, 5) != 0;
    std::uint8_t cur[5];
    std::memcpy(cur, ps, 5);
    phase = reinterpret_cast<Fn4>(splug::call_target(ps));
    splug::log("phase call -> %s", chained ? "another plugin's wrapper (chained)" : "140b8db40");
    splug::CallPatch pp;
    if (const char* e = splug::prepare_call(ps, cur, reinterpret_cast<const void*>(&ask_phase), pp)) {
        splug::log("REFUSED phase: %s", e);
        return;
    }
    if (!splug::commit_call(pp)) return;
    if (!patch_jmp()) return;
    splug::log("installed: jmp 140b75514 -> ask_task, call 14026429b -> ask_phase");
}

}  // namespace

extern "C" std::uint64_t ask_task(void* ai) {
    std::uint32_t id = 0;
    std::memcpy(&id, static_cast<const std::uint8_t*>(ai) + 0x3a0, 4);
    id &= 0xffffff;
    const std::uint32_t day = today();
    Slot& s = slots[id & 0xffff];
    const std::int64_t k = ask_k;
    if (k > 1 && (id + day) % static_cast<std::uint64_t>(k) != 0) {
        c_skip.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    const std::int64_t h = ask_heavy;
    if (h > 0 && s.day.load(std::memory_order_relaxed) + 1 == day &&
        s.mcyc.load(std::memory_order_relaxed) > static_cast<std::uint64_t>(h)) {
        s.day.store(0, std::memory_order_relaxed);
        c_skip.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = update(ai);
    const long long dt = static_cast<long long>(__rdtsc() - t0);
    s.day.store(day, std::memory_order_relaxed);
    s.mcyc.store(static_cast<std::uint32_t>(dt / 1000000), std::memory_order_relaxed);
    c_run.fetch_add(1, std::memory_order_relaxed);
    c_cyc.fetch_add(dt, std::memory_order_relaxed);
    long long m = c_max.load(std::memory_order_relaxed);
    while (dt > m && !c_max.compare_exchange_weak(m, dt)) {
    }
    return r;
}

extern "C" std::uint64_t ask_phase(void* a, void* b, void* c, void* d) {
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = phase(a, b, c, d);
    c_wall.fetch_add(static_cast<long long>(__rdtsc() - t0), std::memory_order_relaxed);
    c_phases.fetch_add(1, std::memory_order_relaxed);
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
