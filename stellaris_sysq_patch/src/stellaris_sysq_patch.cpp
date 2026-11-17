// Stellaris Sysq Patch 0.1.0, Stellaris 4.5.4 x64 — проверка системы 140868510 в расчёте видимости.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   1402ac530 → вызов 1402ac5ba → 1402c56a0: parallel-for по странам, тело 140301bf0 → 1406eecb0(C)
//   (видимость систем для страны C). Для каждой системы, которую C видит, 1406ef480 вызывает
//   140868510(sys) (вызов 1406ef4e3): [sys+0x570] → объект → vcall +8, затем [obj+0x890] → vcall +8 и
//   [+0x108] → vcall +8. Результат от страны не зависит, а считается для каждой страны заново
//   (~19 % задачи, промахи кэша на объектах).
//
// Плагин: эпоха = номер запуска задачи (вызов 1402ac5ba обёрнут). Внутри запуска ответ 140868510
// для системы берётся из таблицы (эпоха, результат, полный id системы — одно 64-битное слово,
// общее для потоков); первый раз — вызов игры. Вне запуска — всегда игра.
// [sysq] mode=off / on / shadow (игра всегда; таблица рядом, расхождения в логе).
// sqp_mode (rva в логе): 0 — игра, 1 — таблица, 2 — тень, 3 — запуски по очереди игра/таблица (замер).

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <intrin.h>

extern "C" {
std::int64_t sqp_mode = 0;
std::uint64_t sqp_job(void* a, void* b, void* c, void* d);
std::uint64_t sqp_check(std::uint8_t* sys);
}

namespace {

constexpr std::uint64_t kJob = 0x1402ac5ba;
constexpr std::uint8_t kJobBytes[5] = {0xe8, 0xe1, 0x90, 0x01, 0x00};
constexpr std::uint64_t kCheck = 0x1406ef4e3;
constexpr std::uint8_t kCheckBytes[5] = {0xe8, 0x28, 0x90, 0x17, 0x00};

using Fn4 = std::uint64_t (*)(void*, void*, void*, void*);
using Fn1 = std::uint64_t (*)(std::uint8_t*);
Fn4 job_orig = nullptr;
Fn1 check_orig = nullptr;

constexpr std::uint32_t kSlots = 1u << 16;
std::atomic<std::uint64_t> cache[kSlots];
std::atomic<std::uint32_t> epoch{0};  // нечётная — задача идёт
std::atomic<long long> c_calls{0}, c_hits{0}, c_miss{0}, c_outside{0}, c_big{0}, c_mismatch{0}, c_jobs{0};
std::atomic<long long> rep_qpc{0};
std::atomic<long long> j_n[2], j_t[2];
std::int64_t job_mode = 0;  // режим текущего запуска (для mode 3 — по очереди 0/1)
long long job_count = 0;
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
    {
        const long long n0 = j_n[0].exchange(0), t0 = j_t[0].exchange(0), n1 = j_n[1].exchange(0), t1 = j_t[1].exchange(0);
        splug::log("job wall: game %lld runs avg %.0f kcyc, table %lld runs avg %.0f kcyc", n0, n0 ? t0 / 1000.0 / n0 : 0.0,
                   n1, n1 ? t1 / 1000.0 / n1 : 0.0);
    }
    splug::log("mode %lld: jobs %lld, checks off-table-hit %lld (hits %lld, first %lld), outside job %lld, id too big %lld, mismatches %lld",
               static_cast<long long>(sqp_mode), c_jobs.exchange(0), c_calls.exchange(0), c_hits.exchange(0),
               c_miss.exchange(0), c_outside.exchange(0), c_big.exchange(0), c_mismatch.load());
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_sysq_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"sysq", L"mode", L"off");
    splug::log("stellaris_sysq_patch 0.1.0: build %s, mode=%s, sqp_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&sqp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    job_orig = reinterpret_cast<Fn4>(splug::call_target(splug::rva(kJob)));
    check_orig = reinterpret_cast<Fn1>(splug::call_target(splug::rva(kCheck)));
    if (reinterpret_cast<std::uintptr_t>(check_orig) != reinterpret_cast<std::uintptr_t>(splug::rva(0x140868510))) {
        splug::log("REFUSED: check target differs");
        return;
    }
    splug::CallPatch p1, p2;
    const char* e = splug::prepare_call(splug::rva(kJob), kJobBytes, reinterpret_cast<const void*>(&sqp_job), p1);
    if (!e) e = splug::prepare_call(splug::rva(kCheck), kCheckBytes, reinterpret_cast<const void*>(&sqp_check), p2);
    if (e) {
        splug::log("REFUSED: %s", e);
        return;
    }
    sqp_mode = mode == splug::Mode::shadow ? 2 : 1;
    if (!splug::commit_call(p2) || !splug::commit_call(p1)) {
        splug::log("FAILED while writing calls");
        return;
    }
    splug::log("installed: job %llx, check %llx, mode %lld", static_cast<unsigned long long>(kJob),
               static_cast<unsigned long long>(kCheck), static_cast<long long>(sqp_mode));
}

}  // namespace

extern "C" std::uint64_t sqp_job(void* a, void* b, void* c, void* d) {
    job_mode = sqp_mode == 3 ? (++job_count & 1) : sqp_mode;
    const unsigned long long t0 = __rdtsc();
    epoch.fetch_add(1);  // нечётная
    c_jobs.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t r = job_orig(a, b, c, d);
    epoch.fetch_add(1);  // чётная
    if (job_mode == 0 || job_mode == 1) {
        j_n[job_mode].fetch_add(1);
        j_t[job_mode].fetch_add(static_cast<long long>(__rdtsc() - t0));
    }
    report();
    return r;
}

extern "C" std::uint64_t sqp_check(std::uint8_t* sys) {
    const std::int64_t mode = job_mode;
    const std::uint32_t ep = epoch.load(std::memory_order_relaxed);
    if (mode == 0 || !(ep & 1)) {
        if (mode != 0) c_outside.fetch_add(1, std::memory_order_relaxed);
        return check_orig(sys);
    }
    const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(sys + 8);
    const std::uint32_t idx = id & 0xffffffu;
    if (idx >= kSlots) {
        c_big.fetch_add(1, std::memory_order_relaxed);
        return check_orig(sys);
    }
    // в режиме таблицы попадания не считаются: общий счётчик на десятки миллионов вызовов в минуту
    // из всех потоков сам по себе съедал выигрыш (одна строка кэша на все ядра)
    const std::uint64_t tag = static_cast<std::uint64_t>((ep & 0x7fffffffu) << 1);
    const std::uint64_t v = cache[idx].load(std::memory_order_relaxed);
    const bool have = (v >> 32 & ~1ull) == tag && static_cast<std::uint32_t>(v) == id;
    if (have && mode == 1) return (v >> 32) & 1;
    c_calls.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t r = check_orig(sys);
    const std::uint64_t bit = (r & 0xff) ? 1 : 0;
    if (have) {
        c_hits.fetch_add(1, std::memory_order_relaxed);
        if (((v >> 32) & 1) != bit) {
            const long long k = c_mismatch.fetch_add(1);
            if (k < 10) splug::log("MISMATCH: system id %08x cached %llu game %llu", id, (v >> 32) & 1, bit);
        }
    } else {
        c_miss.fetch_add(1, std::memory_order_relaxed);
        cache[idx].store(((tag | bit) << 32) | id, std::memory_order_relaxed);
    }
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
