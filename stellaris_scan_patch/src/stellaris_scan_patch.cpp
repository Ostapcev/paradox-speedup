// Stellaris Scan Patch 0.1.0, Stellaris 4.5.4 x64 — поиск флота в списке 140874c70.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140874c70(obj, relation, country, a4, a5, a6, min_power) — линейный проход по списку id флотов
//   [obj+0x1040], длина [obj+0x104c]. Для каждого id: слот таблицы флотов *(1431199e8), проверка
//   [флот+0x30] == id, затем поля флота +0x32c, +0x1340/+0x134c, +0x1268, +0x1280, +0x308, +0x448
//   и отношение владельца (+0x448) к country через 140703ba0; первый подходящий флот — результат,
//   иначе флот по умолчанию *(143119ec0). Вызовы: 140874a10 из 1408777d0 и из задания 14074fa80.
//   На BENCH2300 — 2,9 % главного потока, из них 80 % — сам цикл (ожидание памяти).
//
// [scan] mode=probe — перехват входа 140874c70 (пролог 15 байт переносится в трамплин): считает
//   вызовы, длину списка, сколько элементов пройдено до результата, долю «не найдено», время.
//   Логику не меняет.
//   Замер (BENCH2300, макс. скорость): ~500 тыс. вызовов/с (13 % на главном потоке), список ~5,7,
//   найдено 2,8 % — почти всегда проходится весь список, ~50 нс на элемент (цепочка промахов:
//   слот таблицы -> строки флота). Отсекают флот его собственные поля, а не отношение к стране.
//
// [scan] prefetch=1 — подкачка: на 140874cbb (lea r15,[rsi+r10*4]; cmp rsi,r15 — 7 байт) переход
//   в заглушку, которая до цикла подкачивает слоты таблицы для первых 16 id списка, затем читает
//   эти слоты (то же чтение, что делает игра, с той же проверкой индекса) и подкачивает строки флота
//   +0x30, +0x32c, +0x1268, +0x1280, +0x1340. Промахи идут параллельно, а не друг за другом.
//   Потом выполняются вытесненные инструкции и возврат на 140874cc2. rax, rcx, rdx, r8, r9 в этой
//   точке мертвы. Глобал spf_on (0/1) переключает подкачку на лету.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>

extern "C" {
std::int64_t spf_on = 0;
std::uint64_t spf_db_global = 0;  // 1431199e8 — указатель на таблицу флотов
std::uint64_t spf_resume = 0;     // 140874cc2
void spf_stub();
}

__attribute__((naked)) void spf_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    lea r15, [rsi + r10*4]\n"
        "    cmp qword ptr [rip + spf_on], 0\n"
        "    je 9f\n"
        "    mov rax, qword ptr [rip + spf_db_global]\n"
        "    mov rax, qword ptr [rax]\n"
        "    test rax, rax\n"
        "    je 9f\n"
        "    mov r8d, dword ptr [rax + 0x20]\n"
        "    mov r9, qword ptr [rax + 0x18]\n"
        "    lea rdx, [rsi + 64]\n"
        "    cmp rdx, r15\n"
        "    cmova rdx, r15\n"
        "    mov rcx, rsi\n"
        "1:\n"
        "    cmp rcx, rdx\n"
        "    jae 2f\n"
        "    mov eax, dword ptr [rcx]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r8d\n"
        "    jae 3f\n"
        "    shl rax, 4\n"
        "    prefetcht0 [r9 + rax + 8]\n"
        "3:\n"
        "    add rcx, 4\n"
        "    jmp 1b\n"
        "2:\n"
        "    mov rcx, rsi\n"
        "4:\n"
        "    cmp rcx, rdx\n"
        "    jae 9f\n"
        "    mov eax, dword ptr [rcx]\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, r8d\n"
        "    jae 5f\n"
        "    shl rax, 4\n"
        "    mov rax, qword ptr [r9 + rax + 8]\n"
        "    test rax, rax\n"
        "    je 5f\n"
        "    prefetcht0 [rax + 0x30]\n"
        "    prefetcht0 [rax + 0x32c]\n"
        "    prefetcht0 [rax + 0x1268]\n"
        "    prefetcht0 [rax + 0x1280]\n"
        "    prefetcht0 [rax + 0x1340]\n"
        "5:\n"
        "    add rcx, 4\n"
        "    jmp 4b\n"
        "9:\n"
        "    cmp rsi, r15\n"
        "    jmp qword ptr [rip + spf_resume]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kPfSite = 0x140874cbb;
constexpr std::uint8_t kPfBytes[7] = {0x4e, 0x8d, 0x3c, 0x96, 0x49, 0x3b, 0xf7};
constexpr std::uint64_t kPfResume = 0x140874cc2;
constexpr std::uint64_t kFleetDb = 0x1431199e8;

void install_prefetch(HMODULE module) {
    if (!splug::bytes_match(kPfSite, kPfBytes)) {
        splug::log("prefetch REFUSED: bytes at %llx differ", static_cast<unsigned long long>(kPfSite));
        return;
    }
    spf_db_global = reinterpret_cast<std::uint64_t>(splug::rva(kFleetDb));
    spf_resume = reinterpret_cast<std::uint64_t>(splug::rva(kPfResume));
    auto* site = splug::rva<std::uint8_t>(kPfSite);
    std::uint8_t* tramp = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!tramp) {
        splug::log("prefetch REFUSED: no memory near the site");
        return;
    }
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(tramp, jmp, sizeof(jmp));
    const auto stub = reinterpret_cast<std::uint64_t>(&spf_stub);
    std::memcpy(tramp + 6, &stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 14);
    std::uint8_t patch[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(tramp) -
                                               reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    spf_on = 1;
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        spf_on = 0;
        splug::log("prefetch REFUSED: cannot write");
        return;
    }
    splug::log("prefetch installed at %llx, spf_on at rva %llx", static_cast<unsigned long long>(kPfSite),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&spf_on) -
                                               reinterpret_cast<std::uintptr_t>(module)));
}

constexpr std::uint64_t kFn = 0x140874c70;
constexpr std::uint8_t kPrologue[15] = {
    0x44, 0x88, 0x4c, 0x24, 0x20,  // mov [rsp+0x20], r9b
    0x89, 0x54, 0x24, 0x10,        // mov [rsp+0x10], edx
    0x48, 0x89, 0x4c, 0x24, 0x08,  // mov [rsp+8], rcx
    0x53,                          // push rbx
};
constexpr std::uint64_t kDefaultFleet = 0x143119ec0;

using ScanFn = std::uint8_t* (*)(std::uint8_t*, int, std::uint8_t*, char, char, char, int);
ScanFn original = nullptr;
LARGE_INTEGER freq{};

struct Totals {
    LONG64 calls, elems, visited, found, ticks, main_calls;
};
volatile Totals g{};
volatile LONG64 last_report = 0;
DWORD main_tid = 0;
double report_s = 30.0;

struct Local {
    long long calls = 0, elems = 0, visited = 0, found = 0, ticks = 0, main_calls = 0;
};
thread_local Local t_local;

void flush(Local& l) {
    InterlockedAdd64(&g.calls, l.calls);
    InterlockedAdd64(&g.elems, l.elems);
    InterlockedAdd64(&g.visited, l.visited);
    InterlockedAdd64(&g.found, l.found);
    InterlockedAdd64(&g.ticks, l.ticks);
    InterlockedAdd64(&g.main_calls, l.main_calls);
    l = Local{};
}

volatile LONG64 window_pf = -1;  // значение spf_on в текущем окне статистики

void maybe_report(LONG64 now) {
    const LONG64 last = last_report;
    const double span = static_cast<double>(now - last) / static_cast<double>(freq.QuadPart);
    const LONG64 pf = window_pf;
    // окно закрывается по времени или сразу при переключении spf_on (окна не смешивают состояния)
    if (span < report_s && (pf == spf_on || pf < 0)) return;
    if (InterlockedCompareExchange64(&last_report, now, last) != last) return;
    window_pf = spf_on;
    const LONG64 calls = InterlockedExchange64(&g.calls, 0), elems = InterlockedExchange64(&g.elems, 0);
    const LONG64 visited = InterlockedExchange64(&g.visited, 0), found = InterlockedExchange64(&g.found, 0);
    const LONG64 ticks = InterlockedExchange64(&g.ticks, 0), mc = InterlockedExchange64(&g.main_calls, 0);
    const double ms = 1000.0 * static_cast<double>(ticks) / static_cast<double>(freq.QuadPart);
    splug::log("%.0f s pf=%lld: calls %lld (%.0f/s, main thread %lld), list avg %.1f, visited avg %.1f, found %.1f%%, "
               "%.0f ms total (%.2f us/call, %.1f ns/visited)",
               span, static_cast<long long>(pf), calls, calls / span, mc, calls ? double(elems) / calls : 0.0,
               calls ? double(visited) / calls : 0.0, calls ? 100.0 * found / calls : 0.0, ms,
               calls ? 1000.0 * ms / calls : 0.0, visited ? 1e6 * ms / visited : 0.0);
}

std::uint8_t* scan_hook(std::uint8_t* obj, int relation, std::uint8_t* country, char a4, char a5, char a6, int power) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    std::uint8_t* r = original(obj, relation, country, a4, a5, a6, power);
    QueryPerformanceCounter(&t1);
    Local& l = t_local;
    const int n = *reinterpret_cast<int*>(obj + 0x104c);
    const auto* ids = *reinterpret_cast<std::uint32_t**>(obj + 0x1040);
    ++l.calls;
    l.elems += n;
    l.ticks += t1.QuadPart - t0.QuadPart;
    if (GetCurrentThreadId() == main_tid) ++l.main_calls;
    const auto* def = *splug::rva<std::uint8_t*>(kDefaultFleet);
    if (r && r != def) {
        ++l.found;
        const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(r + 0x30);
        int i = 0;
        while (i < n && ids[i] != id) ++i;
        l.visited += i + 1;
    } else {
        l.visited += n;
    }
    if (l.calls >= 1024) {
        flush(l);
        maybe_report(t1.QuadPart);
    }
    return r;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_scan_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_string(L"scan", L"mode", L"off");
    report_s = splug::ini_int(L"scan", L"report_s", 30);
    main_tid = GetCurrentThreadId();  // DllMain идёт в главном потоке игры
    const bool prefetch = splug::ini_int(L"scan", L"prefetch", 0) != 0;
    splug::log("stellaris_scan_patch 0.1.0: build %s, mode=%ls, prefetch=%d", splug::state().build, mode.c_str(),
               prefetch ? 1 : 0);
    if (prefetch) install_prefetch(module);
    if (mode != L"probe") return;
    auto* site = splug::rva<std::uint8_t>(kFn);
    if (std::memcmp(site, kPrologue, sizeof(kPrologue)) != 0) {
        splug::log("REFUSED: bytes at %llx differ", static_cast<unsigned long long>(kFn));
        return;
    }
    auto* tramp = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!tramp) return;
    std::memcpy(tramp, kPrologue, sizeof(kPrologue));
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(tramp + 15, jmp, sizeof(jmp));
    const auto back = reinterpret_cast<std::uint64_t>(site + 15);
    std::memcpy(tramp + 21, &back, sizeof(back));
    DWORD old = 0;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 29);
    original = reinterpret_cast<ScanFn>(tramp);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    last_report = now.QuadPart;
    std::uint8_t patch[15];
    std::memset(patch, 0xcc, sizeof(patch));
    const auto hook = reinterpret_cast<std::uint64_t>(&scan_hook);
    patch[0] = 0x48;
    patch[1] = 0xb8;
    std::memcpy(patch + 2, &hook, sizeof(hook));
    patch[10] = 0xff;
    patch[11] = 0xe0;
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("REFUSED: cannot write");
        return;
    }
    splug::log("probe installed at %llx", static_cast<unsigned long long>(kFn));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
