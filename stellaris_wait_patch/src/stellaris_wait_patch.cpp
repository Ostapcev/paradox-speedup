// Stellaris Wait Patch 0.3.0, Stellaris 4.5.4 x64 — ожидание главного потока (сохранение, задачи пула).
//
// [spin] (0.3.0): 1401597b0 ждёт задачу пула: спин pause с удвоением, затем цикл вызовов
//   SwitchToThread (вызов 14015982a). На главном потоке (тот, что грузит плагин) плагин вместо
//   этого делает 64 pause и только каждый every-й раз настоящий SwitchToThread.
//   Живое ABAB на BENCH2446: первая серия (спин всегда) — граф 1402b1d00 561 против 621 мкс,
//   повтор (эта версия) — 550/631 против 562/541 мкс: эффект в пределах шума. По умолчанию off.
//   Глобал swp_spin (rva в логе): 0 — как в игре, 1 — спин.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140a8fb30  цикл главного потока (vcall +0x118 из основного цикла 140336b8b): пока задача
//       [[rsi]+0x10] не в состоянии 2/3 (байт +0x4c), по желанию рисует кадр (14024be40) и спит
//       sleep_for(1 мс) через 14030d740 (вызов в 140a8fbc0). Задача (vtable 1423c3aa8, работа
//       140a8fd60) — сжатие сохранения на рабочем потоке: 141d0b060 -> zlib 1.2.8 deflate.
//       На автосохранении BENCH2300 главный поток ждёт так ~24 раза по ~95 мс (≈2,3 с).
//   141d0b0ca  call deflateInit2_(strm, level=5, 8, -15, 9, 0, "1.2.8", 0x58). Уровень 5 —
//       медленный путь deflate_slow; уровни 1–3 — deflate_fast, в 2–3 раза быстрее.
//
// Плагин:
//   [save] level=N (1..9) — подменяет вызов 141d0b0ca переходником, который ставит уровень N.
//       Получается обычный deflate, игра читает его так же; файл немного больше.
//       level=0 или пусто — не трогать (уровень игры 5). Глобал swp_level можно менять на лету.
//   [wait] mode=probe — только измеряет ожидание: по каждому сохранению (серия ожиданий)
//       пишет в лог число ожиданий, их сумму и уровень сжатия.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>
#include <immintrin.h>

extern "C" {
std::int64_t swp_level = 5;           // уровень сжатия для 141d0b0ca
std::uint64_t swp_deflate_init = 0;   // 141d673d0
void swp_hook(const std::int64_t* ms, const std::uint8_t* const* holder);
void swp_stub();
void swp_level_stub();
std::int64_t swp_spin = 0;
void swp_yield();
}

// вход из near-stub вызова 140a8fbc0: rcx = &длительность, rsi вызывающего = держатель задачи
__attribute__((naked)) void swp_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov rdx, rsi\n"
        "    jmp swp_hook\n"
        ".att_syntax\n");
}

// вход из near-stub вызова 141d0b0ca: аргументы deflateInit2_ как есть, меняется только edx
__attribute__((naked)) void swp_level_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov edx, dword ptr [rip + swp_level]\n"
        "    jmp qword ptr [rip + swp_deflate_init]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSleepCall = 0x140a8fbc0;
constexpr std::uint64_t kSleepFn = 0x14030d740;
constexpr std::uint8_t kSleepCallBytes[5] = {0xe8, 0x7b, 0xdb, 0x87, 0xff};
constexpr std::uint64_t kDeflateCall = 0x141d0b0ca;
constexpr std::uint64_t kDeflateInit = 0x141d673d0;
constexpr std::uint8_t kDeflateCallBytes[5] = {0xe8, 0x01, 0xc3, 0x05, 0x00};
// 141d0b0b3: lea edx,[rbx+5]; mov [rsp+0x28],ebx; lea r9d,[rbx-0xf]; lea r8d,[rbx+8]; mov dword [rsp+0x20],9
constexpr std::uint8_t kDeflateArgs[] = {0x8d, 0x53, 0x05, 0x89, 0x5c, 0x24, 0x28, 0x44, 0x8d, 0x4b, 0xf1,
                                         0x44, 0x8d, 0x43, 0x08, 0xc7, 0x44, 0x24, 0x20, 0x09, 0x00, 0x00, 0x00};

using SleepFn = void (*)(const std::int64_t*);
SleepFn sleep_original = nullptr;

LARGE_INTEGER freq{};
// время задачи сохранения (рабочий поток) и её сжатия 141d0a680
volatile LONG64 task_n = 0, task_ticks = 0, comp_n = 0, comp_ticks = 0;

// всё только из главного потока
long long burst_waits = 0, burst_sleeps = 0;
double burst_ms = 0, burst_max = 0;
LONG64 wait_start = 0, last_end = 0, burst_start = 0;
const void* last_task = nullptr;
std::int64_t burst_level = 0;

double ms(LONG64 d) { return 1000.0 * static_cast<double>(d) / static_cast<double>(freq.QuadPart); }

void close_wait() {
    if (!wait_start) return;
    const double w = ms(last_end - wait_start);
    burst_ms += w;
    if (w > burst_max) burst_max = w;
    wait_start = 0;
}

void close_burst() {
    close_wait();
    if (burst_waits) {
        const LONG64 tn = InterlockedExchange64(&task_n, 0), tt = InterlockedExchange64(&task_ticks, 0);
        const LONG64 cn = InterlockedExchange64(&comp_n, 0), ct = InterlockedExchange64(&comp_ticks, 0);
        splug::log("save wait: %lld waits (%lld sleeps), total %.0f ms, max %.1f ms, span %.1f s, level %lld; "
                   "tasks %lld = %.0f ms, compress %lld = %.0f ms",
                   burst_waits, burst_sleeps, burst_ms, burst_max, ms(last_end - burst_start) / 1000.0, burst_level,
                   tn, ms(tt), cn, ms(ct));
    }
    burst_waits = burst_sleeps = 0;
    burst_ms = burst_max = 0;
    burst_start = 0;
}

splug::CallPatch sleep_patch, level_patch, compress_patch;

using TaskFn = std::uint64_t (*)(void*);
TaskFn task_original = nullptr;
using CompFn = std::uint64_t (*)(void*, void*, void*);
CompFn comp_original = nullptr;

std::uint64_t task_wrap(void* self) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const std::uint64_t r = task_original(self);
    QueryPerformanceCounter(&b);
    InterlockedIncrement64(&task_n);
    InterlockedAdd64(&task_ticks, b.QuadPart - a.QuadPart);
    return r;
}

std::uint64_t comp_wrap(void* a1, void* a2, void* a3) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const std::uint64_t r = comp_original(a1, a2, a3);
    QueryPerformanceCounter(&b);
    InterlockedIncrement64(&comp_n);
    InterlockedAdd64(&comp_ticks, b.QuadPart - a.QuadPart);
    return r;
}

void install_timers() {
    constexpr std::uint64_t kTaskSlot = 0x1423c3ab8;  // vtable 1423c3aa8 + 0x10 -> 140a912b0
    constexpr std::uint64_t kTaskFn = 0x140a912b0;
    constexpr std::uint64_t kCompCall = 0x140a9028d;
    constexpr std::uint64_t kCompFn = 0x141d0a680;
    constexpr std::uint8_t kCompCallBytes[5] = {0xe8, 0xee, 0xa3, 0x27, 0x01};
    auto* slot = reinterpret_cast<std::uint64_t*>(splug::rva(kTaskSlot));
    if (*slot != reinterpret_cast<std::uint64_t>(splug::rva(kTaskFn))) {
        splug::log("task timer REFUSED: vtable slot differs");
        return;
    }
    task_original = reinterpret_cast<TaskFn>(splug::rva(kTaskFn));
    comp_original = reinterpret_cast<CompFn>(splug::rva(kCompFn));
    if (const char* err = splug::prepare_call(splug::rva(kCompCall), kCompCallBytes,
                                              reinterpret_cast<const void*>(&comp_wrap), compress_patch)) {
        splug::log("compress timer REFUSED: %s", err);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        splug::log("task timer REFUSED: cannot unprotect vtable");
        return;
    }
    *slot = reinterpret_cast<std::uint64_t>(&task_wrap);
    VirtualProtect(slot, 8, old, &old);
    if (!splug::commit_call(compress_patch)) splug::log("compress timer REFUSED: cannot write call");
    splug::log("timers installed: task slot %llx, compress call %llx", static_cast<unsigned long long>(kTaskSlot),
               static_cast<unsigned long long>(kCompCall));
}

constexpr std::uint64_t kYieldCall = 0x14015982a;
constexpr std::uint8_t kYieldCallBytes[5] = {0xe8, 0x9d, 0xc0, 0xec, 0x01};
DWORD spin_tid = 0;
unsigned spin_mask = 63;

void install_spin() {
    spin_tid = GetCurrentThreadId();
    const int every = splug::ini_int(L"spin", L"every", 64);
    unsigned m = 1;
    while (m < static_cast<unsigned>(every > 1 ? every : 1)) m <<= 1;
    spin_mask = m - 1;
    splug::CallPatch cp;
    if (const char* err = splug::prepare_call(splug::rva(kYieldCall), kYieldCallBytes,
                                              reinterpret_cast<const void*>(&swp_yield), cp)) {
        splug::log("spin REFUSED: %s", err);
        return;
    }
    swp_spin = 1;
    if (!splug::commit_call(cp)) {
        swp_spin = 0;
        splug::log("spin REFUSED: cannot write call");
        return;
    }
    splug::log("spin installed at %llx: main thread %lu, SwitchToThread every %u-th wait step",
               static_cast<unsigned long long>(kYieldCall), static_cast<unsigned long>(spin_tid), spin_mask + 1);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_wait_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_string(L"wait", L"mode", L"off");
    const int level = splug::ini_int(L"save", L"level", 0);
    const auto spin = splug::ini_mode(L"spin", L"mode", L"off");
    splug::log("stellaris_wait_patch 0.3.0: build %s, wait mode=%ls, save level=%d, spin=%s, swp_level at rva %llx, swp_spin at rva %llx",
               splug::state().build, mode.c_str(), level, splug::mode_name(spin),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&swp_level) -
                                               reinterpret_cast<std::uintptr_t>(module)),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&swp_spin) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (spin == splug::Mode::on) install_spin();
    if (level >= 1 && level <= 9) {
        if (!splug::bytes_match(0x141d0b0b3, kDeflateArgs)) {
            splug::log("save level REFUSED: argument bytes differ; not changed");
        } else {
            swp_level = level;
            swp_deflate_init = reinterpret_cast<std::uint64_t>(splug::rva(kDeflateInit));
            if (const char* err = splug::prepare_call(splug::rva(kDeflateCall), kDeflateCallBytes,
                                                      reinterpret_cast<const void*>(&swp_level_stub), level_patch)) {
                splug::log("save level REFUSED: %s", err);
            } else if (!splug::commit_call(level_patch)) {
                splug::log("save level REFUSED: cannot write call");
            } else {
                splug::log("save level %d installed at %llx", level, static_cast<unsigned long long>(kDeflateCall));
            }
        }
    }
    if (mode != L"probe") return;
    sleep_original = reinterpret_cast<SleepFn>(splug::rva(kSleepFn));
    if (const char* err = splug::prepare_call(splug::rva(kSleepCall), kSleepCallBytes,
                                              reinterpret_cast<const void*>(&swp_stub), sleep_patch)) {
        splug::log("probe REFUSED: %s", err);
        return;
    }
    if (!splug::commit_call(sleep_patch)) {
        splug::log("probe REFUSED: cannot write call");
        return;
    }
    splug::log("probe installed at %llx", static_cast<unsigned long long>(kSleepCall));
    install_timers();
}

}  // namespace

extern "C" void swp_hook(const std::int64_t* ms_req, const std::uint8_t* const* holder) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    // новая серия (сохранение): разрыв > 5 с после прошлого сна
    if (burst_start && ms(a.QuadPart - last_end) > 5000.0) close_burst();
    const std::uint8_t* task = nullptr;
    if (holder && *holder) std::memcpy(&task, *holder + 0x10, 8);
    // новое ожидание: другая задача или разрыв > 3 мс после прошлого сна
    if (!wait_start || task != last_task || ms(a.QuadPart - last_end) > 3.0) {
        close_wait();
        ++burst_waits;
        wait_start = a.QuadPart;
        last_task = task;
        if (!burst_start) {
            burst_start = a.QuadPart;
            burst_level = swp_deflate_init ? swp_level : 5;
        }
    }
    sleep_original(ms_req);
    QueryPerformanceCounter(&b);
    ++burst_sleeps;
    last_end = b.QuadPart;
}

extern "C" void swp_yield() {
    static thread_local unsigned n = 0;
    if (swp_spin == 1 && GetCurrentThreadId() == spin_tid && (++n & spin_mask) != 0) {
        for (int i = 0; i < 64; ++i) _mm_pause();
        return;
    }
    SwitchToThread();
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
