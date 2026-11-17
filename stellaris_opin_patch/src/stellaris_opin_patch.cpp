// Stellaris Opin Patch 0.1.0, Stellaris 4.5.4 x64 — общий на все потоки кэш «отношения страны к стране»
// в пределах одного тика ИИ.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140b8db40 (тик 1402640b0) увеличивает счётчик [14315f0a0], создаёт матрицу (N+1)^2 int,
//   заполненную 0x7fffffff, и кладёт её в глобал [14315e798]. 140972110(cache, A, B) читает
//   ячейку [A*N + B]; если там 0x7fffffff — зовёт 140706720(B, A, 0) (вызов 140972163: перебор
//   модификаторов мнения с триггерами, ~3,8 % CPU всех потоков на BENCH2446) и запоминает.
//   Указатель матрицы у потока — глобальный (если байт TLS+0x10624 != 0) или свой (TLS+0x10628).
//
// Плагин: вызов 140972163 идёт в opin_call. Таблица 1024x1024 атомарных ячеек «номер тика | значение»;
// если пара уже посчитана в этом тике (любым потоком) — берётся готовое значение.
// [opin] mode=off / shadow / on. opn_mode (rva в логе) на лету: 0 — как игра, 1 — общий кэш,
// 2 — тень: считается всегда, значение сверяется с кэшем (mismatch), повторы считаются (dups).

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

extern "C" {
std::int64_t opn_mode = 0;
std::int32_t opin_call(void* b, void* a, std::int32_t zero);
}

namespace {

constexpr std::uint64_t kCall = 0x140972163;
constexpr std::uint8_t kCallBytes[5] = {0xe8, 0xb8, 0x45, 0xd9, 0xff};
constexpr std::uint64_t kEpoch = 0x14315f0a0;
constexpr std::uint32_t kDim = 1024;

using Fn = std::int32_t (*)(void*, void*, std::int32_t);
Fn original = nullptr;
const std::uint32_t* g_epoch = nullptr;
std::atomic<std::uint64_t>* table = nullptr;
std::atomic<long long> calls{0}, dups{0}, hits{0}, mismatch{0}, bypass{0};
std::atomic<std::uint32_t>* days = nullptr;  // день игры, когда ячейка записана
const std::uint8_t* const* g_clock = nullptr;  // [14311b708]: +0xc4 день, +8 микротик (0..9)
inline std::int32_t day_now() {
    const std::uint8_t* c = *g_clock;
    std::int32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}
inline std::int32_t micro_now() {
    const std::uint8_t* c = *g_clock;
    std::int32_t d = 0;
    if (c) std::memcpy(&d, c + 8, 4);
    return d;
}
std::atomic<long long> sd_same{0}, sd_diff{0}, od_same{0}, od_diff{0};
LARGE_INTEGER freq{};
std::atomic<LONG64> last_report{0};

inline std::uint32_t idx_of(void* country) {
    std::uint32_t id;
    std::memcpy(&id, static_cast<std::uint8_t*>(country) + 0x20, 4);
    return id & 0xffffffu;
}

void maybe_report() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONG64 last = last_report.load();
    if (last == 0) {
        last_report.compare_exchange_strong(last, now.QuadPart);
        return;
    }
    if (now.QuadPart - last < 60 * freq.QuadPart) return;
    if (!last_report.compare_exchange_strong(last, now.QuadPart)) return;
    splug::log("mode %lld: calls %lld, repeats in tick %lld, served from cache %lld, mismatch %lld, bypass %lld, tick %u, day %d micro %d; earlier tick same day: equal %lld / changed %lld; other day: equal %lld / changed %lld",
               static_cast<long long>(opn_mode), calls.load(), dups.load(), hits.load(), mismatch.load(),
               bypass.load(), *g_epoch, day_now(), micro_now(), sd_same.load(), sd_diff.load(), od_same.load(), od_diff.load());
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_opin_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"opin", L"mode", L"off");
    splug::log("stellaris_opin_patch 0.1.0: build %s, mode=%s, opn_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&opn_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    table = static_cast<std::atomic<std::uint64_t>*>(
        VirtualAlloc(nullptr, sizeof(std::uint64_t) * kDim * kDim, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!table) {
        splug::log("REFUSED: no memory");
        return;
    }
    g_epoch = splug::rva<const std::uint32_t>(kEpoch);
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    days = static_cast<std::atomic<std::uint32_t>*>(
        VirtualAlloc(nullptr, sizeof(std::uint32_t) * kDim * kDim, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!days) return;
    std::uint8_t* site = splug::rva(kCall);
    original = reinterpret_cast<Fn>(splug::call_target(site));
    splug::CallPatch cp;
    if (const char* e = splug::prepare_call(site, kCallBytes, reinterpret_cast<const void*>(&opin_call), cp)) {
        splug::log("REFUSED: %s", e);
        return;
    }
    opn_mode = mode == splug::Mode::on ? 1 : 2;
    if (!splug::commit_call(cp)) {
        splug::log("REFUSED: cannot write call");
        opn_mode = 0;
        return;
    }
    splug::log("installed at %llx, mode %lld", static_cast<unsigned long long>(kCall), static_cast<long long>(opn_mode));
}

}  // namespace

extern "C" std::int32_t opin_call(void* b, void* a, std::int32_t zero) {
    const std::int64_t mode = opn_mode;
    if (mode == 0) return original(b, a, zero);
    const long long n = calls.fetch_add(1, std::memory_order_relaxed);
    if ((n & 0x3fff) == 0) maybe_report();
    const std::uint32_t ia = idx_of(a), ib = idx_of(b);
    // номер тика 0 не используется как метка: пустая ячейка = 0
    const std::uint32_t tick = *g_epoch;
    if (ia >= kDim || ib >= kDim || tick == 0) {
        bypass.fetch_add(1, std::memory_order_relaxed);
        return original(b, a, zero);
    }
    std::atomic<std::uint64_t>& cell = table[ia * kDim + ib];
    const std::uint64_t cur = cell.load(std::memory_order_relaxed);
    const bool have = static_cast<std::uint32_t>(cur >> 32) == tick;
    if (have && mode == 1) {
        hits.fetch_add(1, std::memory_order_relaxed);
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(cur));
    }
    const std::int32_t v = original(b, a, zero);
    if (!have && (cur >> 32) != 0) {  // значение из более раннего тика: изменилось ли?
        const bool same = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur)) == v;
        const bool same_day = days[ia * kDim + ib].load(std::memory_order_relaxed) == static_cast<std::uint32_t>(day_now());
        (same_day ? (same ? sd_same : sd_diff) : (same ? od_same : od_diff)).fetch_add(1, std::memory_order_relaxed);
    }
    days[ia * kDim + ib].store(static_cast<std::uint32_t>(day_now()), std::memory_order_relaxed);
    if (have) {
        dups.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<std::int32_t>(static_cast<std::uint32_t>(cur)) != v) mismatch.fetch_add(1, std::memory_order_relaxed);
    }
    // тик мог смениться за время расчёта — тогда значение не запоминаем
    if (*g_epoch == tick)
        cell.store((static_cast<std::uint64_t>(tick) << 32) | static_cast<std::uint32_t>(v), std::memory_order_relaxed);
    return v;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
