// Stellaris Opday Patch 0.1.0, Stellaris 4.5.4 x64 — мнение страны о стране, кэш на игровой день (МЕНЯЕТ ПОВЕДЕНИЕ).
//
// Что в игре (stellaris.exe 4.5.4.1b4f): 140706720(B, A, r8) — мнение (перебор модификаторов мнения с триггерами).
// ~66 мест вызова: ИИ (через свой кэш на тик ИИ 140972110), скриптовые триггеры opinion (14196b...), дипломатия,
// интерфейс. 8,7 % CPU всех потоков, 4 % стены главного потока (профиль v12). Между тиками ИИ значение меняется у
// ~3 % пересчётов (пробник opin_patch).
//
// Плагин: перехват входа (пролог 15 байт в трамплин). При r8 == 0 (без разбивки для подсказки) ответ для пары (A, B)
// запоминается на текущий игровой день [[14311b708]+0xc4]; в тот же день повторный вызов получает запомненное
// значение. Иначе — всегда игра.
// [opday] mode=off / on / shadow. ody_mode (rva в логе): 0 — игра, 1 — кэш, 2 — тень (всегда игра; при наличии
// значения за этот день — сверка: число расхождений и сумма |разницы|).

#include <windows.h>

#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t ody_mode = 0;
std::int32_t ody_hook(void* b, void* a, void* r8);
}

namespace {

constexpr std::uint64_t kFn = 0x140706720;
constexpr std::uint8_t kPrologue[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
                                        0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18};
constexpr std::uint32_t kDim = 1024;

using Fn = std::int32_t (*)(void*, void*, void*);
Fn original = nullptr;
std::atomic<std::uint64_t>* table = nullptr;
const std::uint8_t* const* g_clock = nullptr;
std::atomic<long long> c_calls{0}, c_hit{0}, c_byp{0}, c_mis{0}, c_absdiff{0};
thread_local long long l_calls = 0, l_hit = 0, l_byp = 0, l_t = 0;
std::atomic<long long> c_t{0};
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};

inline std::uint32_t day_now() {
    const std::uint8_t* c = *g_clock;
    std::uint32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}

inline std::uint32_t idx_of(const void* country) {
    std::uint32_t id;
    std::memcpy(&id, static_cast<const std::uint8_t*>(country) + 0x20, 4);
    return id & 0xffffffu;
}

void report() {
    c_calls.fetch_add(l_calls, std::memory_order_relaxed);
    c_hit.fetch_add(l_hit, std::memory_order_relaxed);
    c_byp.fetch_add(l_byp, std::memory_order_relaxed);
    c_t.fetch_add(l_t, std::memory_order_relaxed);
    l_calls = l_hit = l_byp = l_t = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    static std::int64_t win_mode = -1;
    const bool switched = win_mode != -1 && win_mode != ody_mode;
    if ((q.QuadPart - last < 60 * freq.QuadPart && !switched) || !rep_qpc.compare_exchange_strong(last, q.QuadPart))
        return;
    const std::int64_t shown = win_mode == -1 ? ody_mode : win_mode;
    win_mode = ody_mode;
    const double secs = static_cast<double>(q.QuadPart - last) / static_cast<double>(freq.QuadPart);
    const long long m = c_mis.exchange(0), ad = c_absdiff.exchange(0);
    splug::log("mode %lld: %.0f s, %.0f Mcyc in opinion | calls %lld, same-day repeats %lld, bypass %lld, mismatches %lld (avg |diff| %.2f), day %u",
               static_cast<long long>(shown), secs, c_t.exchange(0) / 1e6, c_calls.exchange(0), c_hit.exchange(0), c_byp.exchange(0), m,
               m ? static_cast<double>(ad) / m : 0.0, day_now());
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_opday_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"opday", L"mode", L"off");
    splug::log("stellaris_opday_patch 0.1.0: build %s, mode=%s, ody_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&ody_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    auto* site = splug::rva<std::uint8_t>(kFn);
    if (std::memcmp(site, kPrologue, sizeof(kPrologue)) != 0) {
        splug::log("REFUSED: prologue differs");
        return;
    }
    table = static_cast<std::atomic<std::uint64_t>*>(
        VirtualAlloc(nullptr, sizeof(std::uint64_t) * kDim * kDim, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* tramp = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!table || !tramp) {
        splug::log("REFUSED: no memory");
        return;
    }
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    std::memcpy(tramp, kPrologue, sizeof(kPrologue));
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(tramp + 15, jmp, sizeof(jmp));
    const auto back = reinterpret_cast<std::uint64_t>(site + 15);
    std::memcpy(tramp + 21, &back, sizeof(back));
    DWORD old = 0;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 29);
    original = reinterpret_cast<Fn>(tramp);
    ody_mode = mode == splug::Mode::on ? 1 : 2;
    std::uint8_t patch[15];
    std::memset(patch, 0xcc, sizeof(patch));
    const auto hook = reinterpret_cast<std::uint64_t>(&ody_hook);
    patch[0] = 0x48;
    patch[1] = 0xb8;
    std::memcpy(patch + 2, &hook, sizeof(hook));
    patch[10] = 0xff;
    patch[11] = 0xe0;
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("REFUSED: cannot write");
        ody_mode = 0;
        return;
    }
    splug::log("installed at %llx, mode %lld", static_cast<unsigned long long>(kFn), static_cast<long long>(ody_mode));
}

}  // namespace

static std::int32_t ody_body(void* b, void* a, void* r8, std::int64_t mode);

extern "C" std::int32_t ody_hook(void* b, void* a, void* r8) {
    const std::int64_t mode = ody_mode;
    const unsigned long long t0 = __rdtsc();
    const std::int32_t v = mode == 0 ? original(b, a, r8) : ody_body(b, a, r8, mode);
    l_t += static_cast<long long>(__rdtsc() - t0);
    if ((++l_calls & 0xfff) == 0) report();
    return v;
}

static std::int32_t ody_body(void* b, void* a, void* r8, std::int64_t mode) {
    if (r8 || !a || !b) {
        ++l_byp;
        return original(b, a, r8);
    }
    const std::uint32_t ia = idx_of(a), ib = idx_of(b);
    if (ia >= kDim || ib >= kDim) {
        ++l_byp;
        return original(b, a, r8);
    }
    const std::uint32_t day = day_now() + 1;  // 0 — пустая ячейка
    std::atomic<std::uint64_t>& cell = table[ib * kDim + ia];
    const std::uint64_t cur = cell.load(std::memory_order_relaxed);
    const bool have = static_cast<std::uint32_t>(cur >> 32) == day;
    if (have) {
        ++l_hit;
        if (mode == 1) return static_cast<std::int32_t>(static_cast<std::uint32_t>(cur));
    }
    const std::int32_t v = original(b, a, r8);
    if (have) {
        const std::int32_t c = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur));
        if (c != v) {
            c_mis.fetch_add(1, std::memory_order_relaxed);
            c_absdiff.fetch_add(c > v ? c - v : v - c, std::memory_order_relaxed);
        }
    } else if (day_now() + 1 == day) {
        cell.store((static_cast<std::uint64_t>(day) << 32) | static_cast<std::uint32_t>(v), std::memory_order_relaxed);
    }
    return v;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
