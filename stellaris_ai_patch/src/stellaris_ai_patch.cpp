// Stellaris AI Patch 0.2.0, Stellaris 4.5.4 x64 — фаза ИИ стран (параллельная) и её хвост.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   1402640b0 (из дня 140251780) собирает ИИ-объекты стран в массив [r13+0x90] (число [r13+0x9c]) и в
//   14026429b вызывает 140b8db40(массив, n) — параллельный for: задачи и главный поток берут куски
//   индексов атомарным счётчиком (140b8f8b0) и зовут у объекта vcall +0x38 (обновление ИИ страны).
//   Свою долю главный поток считает сам, затем ждёт остальные задачи, опрашивая часы кадра (140938370)
//   и крутясь в спине. На BENCH2300 вся фаза — 8 % главного потока, из них ~3,7 % — ожидание хвоста.
//
// [ai] mode=probe — вызов 14026429b подменяется обёрткой; при первом вызове слот +0x38 vtable
//   первого объекта массива заменяется таймером. По каждой фазе: длительность, сумма и максимум
//   задач, число потоков, нижняя граница max(макс. задача, сумма/потоки), позиция самой тяжёлой
//   задачи в массиве и повторяется ли она между фазами. Логику не меняет.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>

extern "C" {
volatile int sai_group1 = 0;  // желаемое число потоков группы 1 (0 — не трогать)
volatile int sai_group0 = 0;  // то же для группы 0
}

namespace {

constexpr std::uint64_t kCall = 0x14026429b;
constexpr std::uint8_t kCallBytes[5] = {0xe8, 0xa0, 0x98, 0x92, 0x00};
constexpr std::uint64_t kParallelFor = 0x140b8db40;
constexpr int kMax = 1024;
constexpr std::uint64_t kTaskManager = 0x1435f92a0;
constexpr std::uint64_t kSetThreads = 0x141b483b0;  // (manager, count, group)

using ForFn = void (*)(void**, int);
using TaskFn = void (*)(void*);
ForFn for_original = nullptr;
TaskFn task_original = nullptr;
void** task_slot = nullptr;
splug::CallPatch call_patch;
LARGE_INTEGER freq{};
DWORD main_tid = 0;
double report_s = 30.0;

// текущая фаза (пишут задачи; читает главный поток после конца фазы)
void** cur_arr = nullptr;
int cur_n = 0;
LONG64 task_start[kMax], task_end[kMax];
DWORD task_tid[kMax];
volatile LONG64 unmatched = 0;

// накопление за окно отчёта (только главный поток)
long long phases = 0, tasks = 0, threads_sum = 0, heavy_repeat = 0, other_vtable = 0;
double wall_ms = 0, sum_ms = 0, max_ms = 0, lb_ms = 0, main_ms = 0, heavy_pos = 0, wait_ms = 0;
void* last_heavy = nullptr;
LONG64 window_start = 0;

double ms(LONG64 d) { return 1000.0 * static_cast<double>(d) / static_cast<double>(freq.QuadPart); }

void task_wrap(void* self) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    task_original(self);
    QueryPerformanceCounter(&b);
    int i = 0;
    while (i < cur_n && cur_arr[i] != self) ++i;
    if (i < cur_n && i < kMax) {
        task_start[i] = a.QuadPart;
        task_end[i] = b.QuadPart;
        task_tid[i] = GetCurrentThreadId();
    } else {
        InterlockedIncrement64(&unmatched);
    }
}

void report(LONG64 now) {
    const double span = ms(now - window_start) / 1000.0;
    if (span < report_s || phases == 0) return;
    const double p = static_cast<double>(phases);
    splug::log("%.0f s: %lld phases, %.1f tasks, %.1f threads; per phase: wall %.2f ms, tasks sum %.2f ms, "
               "max task %.2f ms, lower bound %.2f ms, main thread work %.2f ms, main idle %.2f ms; "
               "heaviest at %.0f%% of array, same as last phase %.0f%%; total wall %.0f ms, idle %.0f ms; "
               "objects with other vtable %lld, unmatched %lld",
               span, phases, tasks / p, threads_sum / p, wall_ms / p, sum_ms / p, max_ms / p, lb_ms / p, main_ms / p,
               wait_ms / p, 100.0 * heavy_pos / p, 100.0 * heavy_repeat / p, wall_ms, wait_ms, other_vtable,
               static_cast<long long>(unmatched));
    phases = tasks = threads_sum = heavy_repeat = other_vtable = 0;
    wall_ms = sum_ms = max_ms = lb_ms = main_ms = heavy_pos = wait_ms = 0;
    unmatched = 0;
    window_start = now;
}

using SetThreadsFn = void (*)(void*, int, int);

int group_count(char* mgr, int group) {
    return *reinterpret_cast<int*>(mgr + (group == 2 ? 0x3c8 : group == 1 ? 0x6c8 : 0x9c8));
}

void apply_threads() {
    char* mgr = *splug::rva<char*>(kTaskManager);
    if (!mgr) return;
    static bool logged = false;
    if (!logged) {
        logged = true;
        splug::log("task groups at start: group2 %d, group1 %d, group0 %d", group_count(mgr, 2), group_count(mgr, 1),
                   group_count(mgr, 0));
    }
    const int want[2] = {sai_group0, sai_group1};
    for (int g = 1; g >= 0; --g) {
        const int w = want[g];
        if (w <= 0 || w > 64) continue;
        const int have = group_count(mgr, g);
        if (have == w) continue;
        reinterpret_cast<SetThreadsFn>(splug::rva(kSetThreads))(mgr, w, g);
        splug::log("group%d threads %d -> %d (now %d)", g, have, w, group_count(mgr, g));
    }
}

bool probing = false;

void for_wrap(void** arr, int n) {
    apply_threads();
    if (!probing) {
        for_original(arr, n);
        return;
    }
    if (!task_slot && n > 0) {
        auto** vt = *reinterpret_cast<void***>(arr[0]);
        void** slot = vt + 0x38 / 8;
        task_original = reinterpret_cast<TaskFn>(*slot);
        DWORD old = 0;
        if (VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
            *slot = reinterpret_cast<void*>(&task_wrap);
            VirtualProtect(slot, 8, old, &old);
            task_slot = slot;
            splug::log("task timer on vtable %p slot +0x38 (was %p)", static_cast<void*>(vt),
                       reinterpret_cast<void*>(task_original));
        }
    }
    const int m = n < kMax ? n : kMax;
    for (int i = 0; i < m; ++i) task_start[i] = task_end[i] = 0, task_tid[i] = 0;
    cur_arr = arr;
    cur_n = m;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    for_original(arr, n);
    QueryPerformanceCounter(&b);
    cur_n = 0;
    if (task_slot) {
        for (int i = 0; i < n; ++i)
            if (reinterpret_cast<void**>(*reinterpret_cast<void***>(arr[i]) + 0x38 / 8) != task_slot) ++other_vtable;
    }
    DWORD tids[64];
    int nt = 0;
    double sum = 0, mx = 0, main_work = 0;
    int heavy = -1;
    for (int i = 0; i < m; ++i) {
        if (!task_tid[i]) continue;
        const double d = ms(task_end[i] - task_start[i]);
        sum += d;
        if (d > mx) mx = d, heavy = i;
        if (task_tid[i] == main_tid) main_work += d;
        int k = 0;
        while (k < nt && tids[k] != task_tid[i]) ++k;
        if (k == nt && nt < 64) tids[nt++] = task_tid[i];
    }
    const double wall = ms(b.QuadPart - a.QuadPart);
    ++phases;
    tasks += n;
    threads_sum += nt;
    wall_ms += wall;
    sum_ms += sum;
    max_ms += mx;
    lb_ms += (nt ? (sum / nt > mx ? sum / nt : mx) : 0);
    main_ms += main_work;
    wait_ms += wall - main_work;
    if (heavy >= 0) {
        heavy_pos += static_cast<double>(heavy) / (m > 1 ? m - 1 : 1);
        if (arr[heavy] == last_heavy) ++heavy_repeat;
        last_heavy = arr[heavy];
    }
    report(b.QuadPart);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_ai_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_string(L"ai", L"mode", L"off");
    report_s = splug::ini_int(L"ai", L"report_s", 30);
    main_tid = GetCurrentThreadId();
    splug::log("stellaris_ai_patch 0.2.0: build %s, mode=%ls", splug::state().build, mode.c_str());
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const int autoc = static_cast<int>(si.dwNumberOfProcessors) - 2;
    int g1 = splug::ini_int(L"threads", L"group1", -1), g0 = splug::ini_int(L"threads", L"group0", -1);
    sai_group1 = g1 < 0 ? autoc : g1;
    sai_group0 = g0 < 0 ? autoc : g0;
    splug::log("threads: group1=%d group0=%d (0 = keep; %lu logical processors)", sai_group1, sai_group0,
               si.dwNumberOfProcessors);
    probing = mode == L"probe";
    if (!probing && sai_group1 <= 0 && sai_group0 <= 0) return;
    for_original = reinterpret_cast<ForFn>(splug::rva(kParallelFor));
    if (const char* err = splug::prepare_call(splug::rva<std::uint8_t>(kCall), kCallBytes,
                                              reinterpret_cast<const void*>(&for_wrap), call_patch)) {
        splug::log("probe REFUSED: %s", err);
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    window_start = now.QuadPart;
    if (!splug::commit_call(call_patch)) {
        splug::log("probe REFUSED: cannot write call");
        return;
    }
    splug::log(probing ? "probe installed at %llx" : "thread control installed at %llx", static_cast<unsigned long long>(kCall));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
