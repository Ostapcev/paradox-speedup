// Stellaris Modflush Patch 0.2.0, Stellaris 4.5.4 x64 — отложенный пересчёт графа модификаторов.
//
// Что в игре (stellaris.exe 4.5.4.1b4f): 14029e860(mgr) — если стоит флаг «грязно» [mgr+0x901], пересчитывает
// граф модификаторов 1402b1d00 (DAG задач, ~0,6–0,9 мс). Зовут его 13 мест: точки тика (14024c03a — шаг тика,
// 14025e2e2/14025e922 — DailyUpdate, 1402613d4 — месячное и др.) и выполнение скриптовых эффектов
// (1403afb1f — после каждого блока эффектов события/on_action, 1416e65b8 и др.).
// 0.1.0: пропуск только мест эффектов ничего не дал — граф пересчитывается в следующей точке тика (~6000 раз/мин,
// ~80 раз за игровой день, ~3,4 с/мин ≈ 6 % стены главного потока).
//
// Плагин (меняет поведение!): каждое из 13 мест обёрнуто; при mfp_mode=1 места, чей бит стоит в mfp_mask,
// пропускаются (флаг «грязно» остаётся, граф пересчитают незамаскированные места). Пока граф не пересчитан,
// игра видит значения модификаторов до изменения.
// [modflush] mode=off / on / shadow (= игра + счётчики); mask=<десятичная маска мест> (для on).
// Живые глобалы (rva в логе): mfp_mode (0 игра, 1 откладывать), mfp_mask.
// Отчёт раз в минуту: дни игры, пересчёты и их время, по каждому месту: вызовы, с флагом «грязно», пропущено,
// сколько пересчётов сделало это место.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

extern "C" {
std::int64_t mfp_mode = 0;
std::int64_t mfp_mask = 0;
}

namespace {

using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);
Fn4 flush_orig = nullptr;  // 14029e860
Fn4 graph_orig = nullptr;  // 1402b1d00

constexpr int kN = 13;
constexpr std::uint64_t kSiteVa[kN] = {0x14024c03a, 0x14025742e, 0x140258e35, 0x14025e2e2, 0x14025e922,
                                       0x1402613d4, 0x1403afb1f, 0x1416e65b8, 0x1416e6e74, 0x1416e823a,
                                       0x141740dc4, 0x14175f114, 0x1417d8aeb};
constexpr std::uint64_t kGraph = 0x14029e8fe;
constexpr std::uint8_t kGraphBytes[5] = {0xe8, 0xfd, 0x33, 0x01, 0x00};

struct Cnt {
    std::atomic<long long> calls{0}, dirty{0}, skip{0}, recomp{0};
};
Cnt cnt[kN + 1];  // kN — неизвестное место
std::atomic<long long> c_graph{0}, c_graph_t{0};
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};
int last_day = -1;
thread_local int t_site = kN;
bool installed[kN] = {};

int game_day() {
    const auto* clk = *splug::rva<const std::uint8_t*>(0x14311b708);
    return clk ? *reinterpret_cast<const int*>(clk + 0xc4) : -1;
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        if (rep_qpc.compare_exchange_strong(last, q.QuadPart)) last_day = game_day();
        return;
    }
    static std::int64_t win_mode = -1;
    const bool switched = win_mode != -1 && win_mode != mfp_mode;
    if ((q.QuadPart - last < 60 * freq.QuadPart && !switched) || !rep_qpc.compare_exchange_strong(last, q.QuadPart))
        return;
    const std::int64_t shown = win_mode == -1 ? mfp_mode : win_mode;
    win_mode = mfp_mode;
    const int day = game_day();
    const double secs = static_cast<double>(q.QuadPart - last) / static_cast<double>(freq.QuadPart);
    const long long g = c_graph.exchange(0), gt = c_graph_t.exchange(0);
    char buf[2048];
    int n = 0;
    for (int k = 0; k <= kN; ++k) {
        const long long c = cnt[k].calls.exchange(0), d = cnt[k].dirty.exchange(0), s = cnt[k].skip.exchange(0),
                        r = cnt[k].recomp.exchange(0);
        if (!c && !r) continue;
        n += std::snprintf(buf + n, sizeof(buf) - n, " | %llx c%lld d%lld s%lld r%lld",
                           static_cast<unsigned long long>(k < kN ? kSiteVa[k] : 0), c, d, s, r);
        if (n > 1900) break;
    }
    splug::log("mode %lld mask %llx: %.0f s, days %d, recomputes %lld, %.0f ms%s", static_cast<long long>(shown),
               static_cast<unsigned long long>(mfp_mask), secs, day - last_day, g,
               1000.0 * static_cast<double>(gt) / static_cast<double>(freq.QuadPart), buf);
    last_day = day;
}

template <int K>
std::uint64_t site_hook(void* mgr, void* b, void* c, void* d) {
    Cnt& x = cnt[K];
    x.calls.fetch_add(1, std::memory_order_relaxed);
    const bool dirty = mgr && *(static_cast<const std::uint8_t*>(mgr) + 0x901);
    if (dirty) x.dirty.fetch_add(1, std::memory_order_relaxed);
    if (mfp_mode == 1 && ((mfp_mask >> K) & 1)) {
        x.skip.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const int prev = t_site;
    t_site = K;
    const std::uint64_t r = flush_orig(mgr, b, c, d);
    t_site = prev;
    return r;
}

const Fn4 kHooks[kN] = {&site_hook<0>, &site_hook<1>, &site_hook<2>,  &site_hook<3>,  &site_hook<4>,
                        &site_hook<5>, &site_hook<6>, &site_hook<7>,  &site_hook<8>,  &site_hook<9>,
                        &site_hook<10>, &site_hook<11>, &site_hook<12>};

std::uint64_t graph_hook(void* a, void* b, void* c, void* d) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const std::uint64_t r = graph_orig(a, b, c, d);
    QueryPerformanceCounter(&t1);
    c_graph.fetch_add(1, std::memory_order_relaxed);
    c_graph_t.fetch_add(t1.QuadPart - t0.QuadPart, std::memory_order_relaxed);
    cnt[t_site].recomp.fetch_add(1, std::memory_order_relaxed);
    report();
    return r;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_modflush_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"modflush", L"mode", L"off");
    const long long mask = splug::ini_int(L"modflush", L"mask", 0);
    auto rva_of = [&](const void* p) {
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(module));
    };
    splug::log("stellaris_modflush_patch 0.2.0: build %s, mode=%s, mask=%llx, mfp_mode at rva %llx, mfp_mask at rva %llx",
               splug::state().build, splug::mode_name(mode), static_cast<unsigned long long>(mask), rva_of(&mfp_mode),
               rva_of(&mfp_mask));
    if (mode == splug::Mode::off) return;
    flush_orig = reinterpret_cast<Fn4>(splug::rva(0x14029e860));
    graph_orig = reinterpret_cast<Fn4>(splug::call_target(splug::rva(kGraph)));
    splug::CallPatch pg;
    if (reinterpret_cast<std::uintptr_t>(graph_orig) != reinterpret_cast<std::uintptr_t>(splug::rva(0x1402b1d00)) ||
        splug::prepare_call(splug::rva(kGraph), kGraphBytes, reinterpret_cast<const void*>(&graph_hook), pg) ||
        !splug::commit_call(pg)) {
        splug::log("REFUSED: graph call");
        return;
    }
    mfp_mask = mask;
    mfp_mode = mode == splug::Mode::on ? 1 : 0;
    int ok = 0;
    for (int k = 0; k < kN; ++k) {
        std::uint8_t* s = splug::rva(kSiteVa[k]);
        if (s[0] != 0xe8 || splug::call_target(s) != reinterpret_cast<void*>(flush_orig)) {
            splug::log("site %llx: not a call of 14029e860, skipped", static_cast<unsigned long long>(kSiteVa[k]));
            continue;
        }
        std::uint8_t bytes[5];
        std::memcpy(bytes, s, 5);
        splug::CallPatch p;
        if (const char* e = splug::prepare_call(s, bytes, reinterpret_cast<const void*>(kHooks[k]), p)) {
            splug::log("site %llx: %s", static_cast<unsigned long long>(kSiteVa[k]), e);
            continue;
        }
        if (splug::commit_call(p)) {
            installed[k] = true;
            ++ok;
        }
    }
    splug::log("installed: %d of %d sites, mfp_mode %lld", ok, kN, static_cast<long long>(mfp_mode));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
