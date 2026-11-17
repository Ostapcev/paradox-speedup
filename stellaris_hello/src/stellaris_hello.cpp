// Stellaris Hello 0.1.0 — проверка цепочки «загрузчик → плагин → проверка сборки».
//
// Ничего в игре не меняет. Пишет в stellaris_hello.log:
//   - сборку stellaris.exe (или REFUSED для неизвестной);
//   - командную строку игры (видно, передан ли -gamestatetimer и т.п.);
//   - число логических процессоров;
//   - контрольную сверку байтов в одном известном месте (регистратор скриптовых имён
//     FUN_14016d510) — на нём будут проверяться все будущие хуки.
//   - раз в interval_s секунд: время работы процесса, CPU главного потока и всего процесса.

#include <windows.h>

#include "stellaris_plugin.hpp"

namespace {

// Первые байты FUN_14016d510 (регистратор скриптовых имён) в сборке 4.5.4.1b4f
// (прочитаны из stellaris.exe на диске: sub rsp,28h; mov eax,14h; ... mov rcx, gs:[58h]).
constexpr std::uint64_t check_rva = 0x16d510;
constexpr std::uint8_t check_bytes[] = {0x48, 0x83, 0xec, 0x28, 0xb8, 0x14, 0x00, 0x00,
                                        0x00, 0x8b, 0xc0, 0x65, 0x48, 0x8b, 0x0c, 0x25,
                                        0x58, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x09, 0x48};

double filetime_s(const FILETIME& t) {
    ULARGE_INTEGER v;
    v.LowPart = t.dwLowDateTime;
    v.HighPart = t.dwHighDateTime;
    return static_cast<double>(v.QuadPart) / 1e7;
}

DWORD WINAPI reporter(void* param) {
    const int interval = static_cast<int>(reinterpret_cast<std::intptr_t>(param));
    HANDLE main_thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, splug::state().main_tid);
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        Sleep(static_cast<DWORD>(interval) * 1000);
        FILETIME c, e, k, u;
        double main_cpu = -1, proc_cpu = -1;
        if (main_thread && GetThreadTimes(main_thread, &c, &e, &k, &u)) main_cpu = filetime_s(k) + filetime_s(u);
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) proc_cpu = filetime_s(k) + filetime_s(u);
        splug::log("uptime=%.0fs main_thread_cpu=%.1fs process_cpu=%.1fs",
                   (GetTickCount64() - start) / 1000.0, main_cpu, proc_cpu);
    }
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_hello")) return;
    auto& s = splug::state();
    splug::log("stellaris_hello 0.1.0: build %s, image %p..%p, main_tid=%lu", s.build,
               reinterpret_cast<void*>(s.image), reinterpret_cast<void*>(s.image_end), s.main_tid);
    splug::log("command line: %ls", GetCommandLineW());
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    splug::log("logical processors: %lu", si.dwNumberOfProcessors);

    const bool ok = splug::bytes_match(check_rva, check_bytes);
    splug::log("byte check at %llx: %s", static_cast<unsigned long long>(splug::kImageBase + check_rva),
               ok ? "OK" : "MISMATCH (hooks would be REFUSED)");

    const int interval = splug::ini_int(L"hello", L"interval_s", 30);
    if (interval > 0) {
        HANDLE thread = CreateThread(nullptr, 0, reporter, reinterpret_cast<void*>(static_cast<std::intptr_t>(interval)), 0, nullptr);
        if (thread) CloseHandle(thread);
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
