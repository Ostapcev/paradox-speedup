// Stellaris Sched Patch 0.1.0, Stellaris 4.5.4 x64 — приоритет и ядро главного потока.
//
// Главный поток (тот, что грузит плагины) — узкое место: рабочих потоков 28 на 16 логических
// процессорах (8 ядер × 2 SMT на 7940H), и главный поток делит ядро с соседом по SMT и с рабочими.
// Плагин держит фоновый поток, который раз в 200 мс смотрит sch_mode (rva в логе) и применяет:
//   0 — как в игре (приоритет обычный, все процессоры);
//   1 — главный поток THREAD_PRIORITY_HIGHEST;
//   2 — главному потоку отдельное физическое ядро: он на логическом 0, остальные потоки процесса
//       на всех, кроме 0 и 1 (сосед по SMT пустует);
//   3 — 1 и 2 вместе.
// [sched] mode=0..3 — начальное значение. Игровую логику не меняет.

#include <windows.h>
#include <tlhelp32.h>

#include "stellaris_plugin.hpp"

#include <cstdint>

extern "C" {
std::int64_t sch_mode = 0;
}

namespace {

HANDLE main_thread = nullptr;
DWORD main_tid = 0;
DWORD_PTR all_mask = 0;

void apply_others(DWORD_PTR mask) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == main_tid || te.th32ThreadID == self) continue;
        if (HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) {
            if (SetThreadAffinityMask(h, mask)) ++n;
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    (void)n;
}

void apply(std::int64_t m) {
    SetThreadPriority(main_thread, (m & 1) ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL);
    if (m & 2) {
        SetThreadAffinityMask(main_thread, 1);
        apply_others(all_mask & ~static_cast<DWORD_PTR>(3));
    } else {
        SetThreadAffinityMask(main_thread, all_mask);
        apply_others(all_mask);
    }
}

DWORD WINAPI watcher(LPVOID) {
    std::int64_t cur = -1;
    int tick = 0;
    for (;;) {
        const std::int64_t m = sch_mode;
        // раз в ~5 с повторить, чтобы новые потоки игры тоже попали под маску
        if (m != cur || (m & 2 && ++tick % 25 == 0)) {
            if (m != cur) splug::log("sched mode %lld", static_cast<long long>(m));
            apply(m);
            cur = m;
        }
        Sleep(200);
    }
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_sched_patch")) return;
    DWORD_PTR sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &all_mask, &sys);
    const int mode = splug::ini_int(L"sched", L"mode", 0);
    splug::log("stellaris_sched_patch 0.1.0: build %s, mode=%d, process mask %llx, sch_mode at rva %llx",
               splug::state().build, mode, static_cast<unsigned long long>(all_mask),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&sch_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    main_tid = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        splug::log("REFUSED: cannot duplicate main thread handle");
        return;
    }
    sch_mode = mode;
    if (HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr)) CloseHandle(t);
    splug::log("watcher started, main thread %lu", static_cast<unsigned long>(main_tid));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
