// Stellaris Cmpc Patch 0.2.0, Stellaris 4.5.4 x64 — кэш условий «посчитать ⋚ число» в пределах микротика
// (МЕНЯЕТ ПОВЕДЕНИЕ, очень мало).
//
// Что в игре (stellaris.exe 4.5.4.1b4f): vtable 142536320, слот +0x18 = 141952080 — условие «посчитать (count_*/значение)
// и сравнить с числом»; подсчёт через общий перебор 140348850/14034a6c0. ~6 % главного потока. Пробник trig_patch:
// 52–58 % вызовов повторяют ключ (узел + первые 0x40 байт контекста) в том же микротике, из них 0,03 % с другим ответом.
//
// Плагин: слот +0x18 → cmp_eval. Ключ — хеш (узел, 0x40 байт контекста); эпоха — день*16 + микротик ([[14311b708]+0xc4],
// +8). В той же эпохе повторный ключ получает запомненный ответ. Ячейка одна 64-битная: старшие 31 бит хеша | эпоха 32 | ответ.
// Имя «cmpc» — чтобы DLL грузилась раньше stellaris_tdisp_patch (tdisp вшивает адрес слота +0x18 в свои переходники).
// 0.2.0: кэш действует только (а) внутри шага тика 140251780 (вызов 140a21f91 и хвостовой jmp 140963acc) — при создании
// галактики время стоит на месте, и вся стартовая генерация шла одной «эпохой» (тень: ~15 тыс. неверных ответов,
// циклы while по 100 раз — население умножалось); (б) пока не выполнялся ни один скриптовый эффект: слот +0x48
// (Execute) всех 751 vtable эффектов (effect_vtables.hpp) ведёт через переходник, увеличивающий cmc_gen, а cmc_gen
// входит в ключ. Так while/if с подсчётом после create_pop и т. п. видят новое значение.
// [cmpc] mode=off / on / shadow. cmc_mode (rva в логе): 0 — игра, 1 — кэш, 2 — тень (всегда игра + сверка).

#include <windows.h>

#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#include "call_patch.hpp"
#include "effect_vtables.hpp"

extern "C" {
std::int64_t cmc_mode = 0;
std::uint64_t cmc_gen = 0;   // растёт при каждом выполнении эффекта (переходники)
std::int64_t cmc_tick = 0;   // > 0 — идёт шаг тика
void cmc_step(void* gs, void* arg);
bool cmp_eval(void* node, const std::uint8_t* ctx);
}

namespace {

constexpr std::uint64_t kSlot = 0x142536320 + 0x18;
constexpr std::uint64_t kEval = 0x141952080;
constexpr std::uint32_t kBits = 20;

using EvalFn = bool (*)(void*, const std::uint8_t*);
EvalFn original = nullptr;
const std::uint8_t* const* g_clock = nullptr;
std::atomic<std::uint64_t>* table = nullptr;
std::atomic<long long> c_calls{0}, c_hit{0}, c_mis{0}, c_t{0};
thread_local long long l_calls = 0, l_hit = 0, l_t = 0, l_off = 0;
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};
using StepFn = void (*)(void*, void*);
StepFn step_orig = nullptr;
constexpr std::uint64_t kStep = 0x140251780;
constexpr std::uint64_t kStepCall = 0x140a21f91;  // call 140251780
constexpr std::uint64_t kStepJmp = 0x140963acc;   // jmp 140251780 (хвостовой)
std::atomic<long long> c_off{0};

inline std::uint32_t epoch_now() {
    const std::uint8_t* c = *g_clock;
    std::uint32_t d = 0, m = 0;
    if (c) {
        std::memcpy(&d, c + 0xc4, 4);
        std::memcpy(&m, c + 8, 4);
    }
    return d * 16 + (m & 15) + 1;
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

void report() {
    c_calls.fetch_add(l_calls, std::memory_order_relaxed);
    c_hit.fetch_add(l_hit, std::memory_order_relaxed);
    c_t.fetch_add(l_t, std::memory_order_relaxed);
    c_off.fetch_add(l_off, std::memory_order_relaxed);
    l_calls = l_hit = l_t = l_off = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    static std::int64_t win_mode = -1;
    const bool switched = win_mode != -1 && win_mode != cmc_mode;
    if ((q.QuadPart - last < 60 * freq.QuadPart && !switched) || !rep_qpc.compare_exchange_strong(last, q.QuadPart))
        return;
    const std::int64_t shown = win_mode == -1 ? cmc_mode : win_mode;
    win_mode = cmc_mode;
    const double secs = static_cast<double>(q.QuadPart - last) / static_cast<double>(freq.QuadPart);
    const std::uint8_t* c = *g_clock;
    std::uint32_t day = 0;
    if (c) std::memcpy(&day, c + 0xc4, 4);
    splug::log("mode %lld: %.0f s, %.0f Mcyc in condition | calls %lld, same-microtick repeats %lld, mismatches %lld, outside tick %lld, effect gen %llu, day %u",
               static_cast<long long>(shown), secs, c_t.exchange(0) / 1e6, c_calls.exchange(0), c_hit.exchange(0),
               c_mis.load(), c_off.exchange(0), static_cast<unsigned long long>(cmc_gen), day);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_cmpc_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"cmpc", L"mode", L"off");
    splug::log("stellaris_cmpc_patch 0.2.0: build %s, mode=%s, cmc_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&cmc_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    auto* slot = splug::rva<std::uint64_t>(kSlot);
    if (*slot != reinterpret_cast<std::uint64_t>(splug::rva(kEval))) {
        splug::log("REFUSED: vtable slot differs (another plugin loaded first?)");
        return;
    }
    table = static_cast<std::atomic<std::uint64_t>*>(
        VirtualAlloc(nullptr, sizeof(std::uint64_t) << kBits, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!table) return;
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    original = reinterpret_cast<EvalFn>(splug::rva(kEval));
    // шаг тика: call и хвостовой jmp на 140251780
    {
        std::uint8_t* call_site = splug::rva<std::uint8_t>(kStepCall);
        std::uint8_t* jmp_site = splug::rva<std::uint8_t>(kStepJmp);
        if (call_site[0] != 0xE8 || splug::call_target(call_site) != splug::rva(kStep) || jmp_site[0] != 0xE9 ||
            splug::call_target(jmp_site) != splug::rva(kStep)) {
            splug::log("REFUSED: tick step call/jmp bytes differ; no bytes changed");
            return;
        }
        step_orig = reinterpret_cast<StepFn>(splug::rva(kStep));
        std::uint8_t expect_call[5], expect_jmp[5];
        std::memcpy(expect_call, call_site, 5);
        std::memcpy(expect_jmp, jmp_site, 5);
        splug::CallPatch pc{}, pj{};
        if (const char* e = splug::prepare_call(call_site, expect_call, reinterpret_cast<const void*>(&cmc_step), pc)) {
            splug::log("REFUSED: tick call: %s", e);
            return;
        }
        if (const char* e = splug::prepare_call(jmp_site, expect_jmp, reinterpret_cast<const void*>(&cmc_step), pj)) {
            splug::log("REFUSED: tick jmp: %s", e);
            return;
        }
        pj.replacement[0] = 0xE9;  // остаётся хвостовым переходом
        // переходники эффектов (сначала все проверки)
        std::size_t n = sizeof(kEffectVts) / sizeof(kEffectVts[0]), ok = 0;
        auto* thunks = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, n * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!thunks) {
            splug::log("REFUSED: no memory for effect thunks");
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            auto* slot48 = splug::rva<std::uint64_t>(kEffectVts[i].vtable + 0x48);
            if (*slot48 != reinterpret_cast<std::uint64_t>(splug::rva(kEffectVts[i].execute))) continue;
            std::uint8_t* t = thunks + i * 32;
            const std::uint64_t gen_addr = reinterpret_cast<std::uint64_t>(&cmc_gen);
            const std::uint64_t target = *slot48;
            t[0] = 0x48; t[1] = 0xB8; std::memcpy(t + 2, &gen_addr, 8);   // mov rax, &cmc_gen
            t[10] = 0x48; t[11] = 0xFF; t[12] = 0x00;                       // inc qword [rax]
            t[13] = 0x48; t[14] = 0xB8; std::memcpy(t + 15, &target, 8);  // mov rax, execute
            t[23] = 0xFF; t[24] = 0xE0;                                     // jmp rax
            const auto addr = reinterpret_cast<std::uint64_t>(t);
            if (splug::write_bytes(slot48, &addr, 8)) ++ok;
        }
        FlushInstructionCache(GetCurrentProcess(), thunks, n * 32);
        if (!splug::commit_call(pc) || !splug::commit_call(pj)) {
            splug::log("REFUSED: cannot patch tick step (effect thunks %zu already set)", ok);
            return;
        }
        splug::log("tick step hooked (%llx call, %llx jmp); effect Execute thunks %zu of %zu vtables",
                   static_cast<unsigned long long>(kStepCall), static_cast<unsigned long long>(kStepJmp), ok, n);
    }
    cmc_mode = mode == splug::Mode::on ? 1 : 2;
    const auto hook = reinterpret_cast<std::uint64_t>(&cmp_eval);
    if (!splug::write_bytes(slot, &hook, 8)) {
        splug::log("REFUSED: cannot write vtable slot");
        cmc_mode = 0;
        return;
    }
    splug::log("installed: vtable slot %llx -> cmp_eval, mode %lld", static_cast<unsigned long long>(kSlot),
               static_cast<long long>(cmc_mode));
}

inline bool body(void* node, const std::uint8_t* ctx, std::int64_t mode) {
    if (!ctx || !*g_clock || cmc_tick <= 0) {
        if (mode == 1) ++l_off;
        return original(node, ctx);
    }
    const std::uint64_t gen = cmc_gen;
    std::uint64_t h = reinterpret_cast<std::uint64_t>(node);
    for (int i = 0; i < 8; ++i) {
        std::uint64_t q;
        std::memcpy(&q, ctx + 8 * i, 8);
        h = mix(h, q);
    }
    h = mix(h, 0x51ed27f1u);
    h = mix(h, gen);
    const std::uint64_t ep = epoch_now();
    const std::uint64_t key = ((h >> 33) << 33) | (ep << 1);
    std::atomic<std::uint64_t>& cell = table[h & ((1u << kBits) - 1)];
    const std::uint64_t cur = cell.load(std::memory_order_relaxed);
    const bool have = (cur & ~1ull) == key;
    if (have) {
        ++l_hit;
        if (mode == 1) return (cur & 1) != 0;
    }
    const bool r = original(node, ctx);
    if (have) {
        if (r != ((cur & 1) != 0)) c_mis.fetch_add(1, std::memory_order_relaxed);
    } else if (epoch_now() == ep && cmc_gen == gen) {
        cell.store(key | (r ? 1 : 0), std::memory_order_relaxed);
    }
    return r;
}

}  // namespace

extern "C" void cmc_step(void* gs, void* arg) {
    ++cmc_tick;
    step_orig(gs, arg);
    --cmc_tick;
}

extern "C" bool cmp_eval(void* node, const std::uint8_t* ctx) {
    const std::int64_t mode = cmc_mode;
    const unsigned long long t0 = __rdtsc();
    const bool r = mode == 0 ? original(node, ctx) : body(node, ctx, mode);
    l_t += static_cast<long long>(__rdtsc() - t0);
    if ((++l_calls & 0xffff) == 0) report();
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
