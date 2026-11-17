// Stellaris Trig Patch 0.1.0 (пробник), Stellaris 4.5.4 x64 — повторяются ли условия-сравнения
// в пределах игрового дня и совпадает ли при этом результат.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   vtable 142536320, слот +0x18 = 141952080: условие «посчитать (count_*/значение) и сравнить
//   с числом». Подсчёт идёт через общий перебор 140348850/14034a6c0. На BENCH2446 ~6 % главного
//   потока, ~7 % всех потоков; вызывается из десятков систем (ИИ, археология, on_action, отношения).
//
// Пробник: слот +0x18 заменён на trig_eval. Результат всегда считает игра. Ключ — узел + хеш
// первых 0x40 байт контекста (раскладка контекста не разобрана). Таблица 1M ячеек:
// ключ, день, результат. Раз в минуту в лог: вызовы, повторы ключа в тот же день, из них с другим
// результатом, и время в 141952080 по всем потокам.
// [trig] mode=off / shadow.

#include <windows.h>

#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

extern "C" {
std::int64_t trg_mode = 0;
bool trig_eval(void* node, const std::uint8_t* ctx);
}

namespace {

constexpr std::uint64_t kSlot = 0x142536320 + 0x18;
constexpr std::uint64_t kEval = 0x141952080;
constexpr std::uint32_t kBits = 20;

using EvalFn = bool (*)(void*, const std::uint8_t*);
EvalFn original = nullptr;
const std::uint8_t* const* g_clock = nullptr;

struct Cell {
    std::atomic<std::uint64_t> key;
    std::atomic<std::uint64_t> day_res;  // день << 1 | результат
};
Cell* table = nullptr;
std::atomic<long long> same_micro{0}, same_micro_diff{0};
std::atomic<long long> calls{0}, same_day{0}, same_day_diff{0}, other_day{0}, other_day_diff{0}, ticks{0};
LARGE_INTEGER freq{};
std::atomic<LONG64> last_report{0};

inline std::uint32_t day_now() {
    const std::uint8_t* c = *g_clock;
    std::uint32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}
// день * 16 + микротик (+8, 0..9)
inline std::uint32_t epoch_now() {
    const std::uint8_t* c = *g_clock;
    std::uint32_t d = 0, m = 0;
    if (c) {
        std::memcpy(&d, c + 0xc4, 4);
        std::memcpy(&m, c + 8, 4);
    }
    return d * 16 + (m & 15);
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
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
    const long long sm = same_micro.exchange(0), smd = same_micro_diff.exchange(0);
    const long long c = calls.exchange(0), sd = same_day.exchange(0), sdd = same_day_diff.exchange(0),
                    od = other_day.exchange(0), odd = other_day_diff.exchange(0), t = ticks.exchange(0);
    splug::log("calls %lld (%.1f ms in eval, all threads); same-day repeats %lld (%.1f%%), of them different result %lld; "
               "earlier-day repeats %lld, different %lld; day %u; same micro-tick repeats %lld, different %lld",
               c, 1000.0 * static_cast<double>(t) / static_cast<double>(freq.QuadPart), sd, c ? 100.0 * sd / c : 0.0, sdd,
               od, odd, day_now(), sm, smd);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_trig_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"trig", L"mode", L"off");
    splug::log("stellaris_trig_patch 0.1.0: build %s, mode=%s", splug::state().build, splug::mode_name(mode));
    if (mode == splug::Mode::off) return;
    auto* slot = splug::rva<std::uint64_t>(kSlot);
    if (*slot != reinterpret_cast<std::uint64_t>(splug::rva(kEval))) {
        splug::log("REFUSED: vtable slot differs");
        return;
    }
    table = static_cast<Cell*>(VirtualAlloc(nullptr, sizeof(Cell) << kBits, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!table) return;
    g_clock = splug::rva<const std::uint8_t*>(0x14311b708);
    original = reinterpret_cast<EvalFn>(splug::rva(kEval));
    trg_mode = 2;
    const auto hook = reinterpret_cast<std::uint64_t>(&trig_eval);
    if (!splug::write_bytes(slot, &hook, 8)) {
        splug::log("REFUSED: cannot write vtable slot");
        return;
    }
    splug::log("installed: vtable slot %llx -> trig_eval", static_cast<unsigned long long>(kSlot));
}

}  // namespace

extern "C" bool trig_eval(void* node, const std::uint8_t* ctx) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const bool r = original(node, ctx);
    QueryPerformanceCounter(&b);
    ticks.fetch_add(b.QuadPart - a.QuadPart, std::memory_order_relaxed);
    const long long n = calls.fetch_add(1, std::memory_order_relaxed);
    if ((n & 0xffff) == 0) maybe_report();
    if (!ctx) return r;
    std::uint64_t h = reinterpret_cast<std::uint64_t>(node);
    for (int i = 0; i < 8; ++i) {
        std::uint64_t q;
        std::memcpy(&q, ctx + 8 * i, 8);
        h = mix(h, q);
    }
    h |= 1;  // 0 = пустая ячейка
    Cell& c = table[h & ((1u << kBits) - 1)];
    const std::uint64_t ep = epoch_now();
    const std::uint64_t day = ep >> 4;
    const std::uint64_t v = (ep << 1) | (r ? 1 : 0);
    if (c.key.load(std::memory_order_relaxed) == h) {
        const std::uint64_t old = c.day_res.load(std::memory_order_relaxed);
        if ((old >> 1) == ep) {
            same_micro.fetch_add(1, std::memory_order_relaxed);
            if ((old & 1) != (v & 1)) same_micro_diff.fetch_add(1, std::memory_order_relaxed);
        }
        if ((old >> 5) == day) {
            same_day.fetch_add(1, std::memory_order_relaxed);
            if ((old & 1) != (v & 1)) same_day_diff.fetch_add(1, std::memory_order_relaxed);
        } else {
            other_day.fetch_add(1, std::memory_order_relaxed);
            if ((old & 1) != (v & 1)) other_day_diff.fetch_add(1, std::memory_order_relaxed);
        }
    }
    c.key.store(h, std::memory_order_relaxed);
    c.day_res.store(v, std::memory_order_relaxed);
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
