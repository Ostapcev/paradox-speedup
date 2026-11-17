// Stellaris Render Patch 0.2.0, Stellaris 4.5.4 x64 — реже рисовать кадры изнутри тика и между тиками.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   14024be40  кадр изнутри тика: полный проход UI и отрисовки (141a52770) посреди расчёта дня.
//              Вызывается из ~57 мест тика после проверки 14024bdb0 / 140938370: «с прошлого кадра
//              прошло >= 1/fps - 1 мс», fps = min(настройки, но не меньше 30). На TEST5000 в 2215 году
//              это ~26 % главного потока (профиль t5000_2215_wall).
// Хук: на вход 14024be40 ставится переход в плагин; пролог (3 инструкции, 15 байт, без
// RIP-адресации) переносится в трамплин. Плагин пропускает кадр, если с прошлого кадра,
// нарисованного ИЗНУТРИ тика, прошло меньше frame_interval. Кадры главного цикла между тиками
// не трогаются; игровая логика не меняется.
//
// Режимы ([render] mode=):
//   off     ничего не меняет;
//   shadow  ничего не меняет, раз в report_s секунд пишет: сколько кадров изнутри тика, сколько мс;
//   on      пропуск кадров по frame_interval + та же статистика.
//
// 0.2.0 — кадры главного цикла между тиками ([main]):
//   140336b40  обновление игрового состояния за один кадр главного цикла (vcall +0x58 из 141a52770),
//              dl = «считать тик». Сначала тик (call 1401b9dd0 в 140336b80, потом vcall +0x118),
//              затем с 140336b91 — весь кадр: ввод, GUI 141c11400, обновление карты (vcall +0x110),
//              отрисовка 141a53d60. Кадр изнутри тика — это тот же 140336b40 с dl = 0, то есть та
//              же вторая половина, которую 0.1.0 уже пропускает.
//   Плагин: обёртка на вызов тика 140336b80 меряет его длительность; на 140336b91 (call
//   _Query_perf_frequency) стоит заглушка: если тик в этом кадре был «тяжёлым», игра «бежит» — последние min_streak «тяжёлых» тиков
//   (>= min_tick_ms, то есть шёл расчёт дня) шли с промежутками меньше run_gap_ms, и последний был не
//   раньше run_gap_ms назад (на паузе и на малой скорости так не бывает), — и с прошлого нарисованного
//   кадра (любого) прошло меньше frame_interval, — сразу уходим в эпилог 140337d83
//   (на этом месте в функции ещё ничего не создано). Игровой тик выполняется всегда.
//   Итог замера (BENCH2300, 10 окон по минуте с живым переключением srp_main): 1,66 против 1,75 дня/с
//   без пропуска — выигрыша нет. Тяжёлый тик (~115 мс) — шаг дня; после него главный поток рисует кадр
//   (~4,5 мс), пока рабочие потоки доделывают свою часть. Если кадр пропустить, следующий тик выходит
//   «лёгким» (ждать нечего, кроме рабочих) и кадр рисуется на нём. Поэтому по умолчанию [main] mode=off.
//   mode=shadow — только считает, что было бы пропущено. Глобал srp_main (0/1) меняет поведение на лету.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
double srp_interval = 0.1;       // интервал кадров изнутри тика, с (живой)
std::int64_t srp_main = 0;          // 1 = пропускать кадры главного цикла (живой переключатель)
std::uint64_t srp_qpf = 0;          // 142025648 — исходная цель вызова на 140336b91
std::uint64_t srp_epilogue = 0;     // 140337d83
bool srp_main_decide(std::uint32_t tick_flag);
void srp_main_stub();
}

// вход по call из 140336b91: [rsp] = 140336b96, rbp — кадр 140336b40, [rbp+0x168] = dl на входе
__attribute__((naked)) void srp_main_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    movzx ecx, byte ptr [rbp + 0x168]\n"
        "    sub rsp, 0x28\n"
        "    call srp_main_decide\n"
        "    add rsp, 0x28\n"
        "    test al, al\n"
        "    jnz 1f\n"
        "    jmp qword ptr [rip + srp_qpf]\n"
        "1:\n"
        "    add rsp, 8\n"
        "    jmp qword ptr [rip + srp_epilogue]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kFrame = 0x14024be40;
constexpr std::uint8_t kPrologue[15] = {
    0x48, 0x89, 0x5c, 0x24, 0x08,  // mov [rsp+8], rbx
    0x48, 0x89, 0x74, 0x24, 0x18,  // mov [rsp+0x18], rsi
    0x48, 0x89, 0x7c, 0x24, 0x20,  // mov [rsp+0x20], rdi
};
constexpr std::uint8_t kNext[5] = {0x48, 0x89, 0x54, 0x24, 0x10};  // mov [rsp+0x10], rdx

using FrameFn = void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using ClockFn = double (*)(void*);

// Проверка игры перед кадром (140938370): clock() - *(double*)(*(gfx + 0xe8) + 0xd8) >= 1/fps - 0.001,
// clock = (*DAT_14315fd20)->vfunc[0x28]. Пропущенный кадр эту метку не сдвигает, и игра зовёт кадр
// снова на каждой проверке (~700 тыс. раз/с). quiet_skip: при пропуске ставим метку = clock().
constexpr std::uint64_t kGfx = 0x14311bf10;    // указатель на графический объект
constexpr std::uint64_t kClock = 0x14315fd20;  // указатель на часы

splug::Mode mode = splug::Mode::off;
bool quiet_skip = true;
double report_s = 30.0;
FrameFn original = nullptr;

LARGE_INTEGER freq{};
std::int64_t last_drawn = 0;  // конец последнего кадра изнутри тика
std::int64_t last_any = 0;    // последний нарисованный кадр любого вида (для [main])
std::int64_t last_report = 0;
long long calls = 0, drawn = 0, total_calls = 0, total_drawn = 0;
double drawn_ms = 0.0;

std::int64_t now() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double seconds(std::int64_t ticks) { return static_cast<double>(ticks) / static_cast<double>(freq.QuadPart); }

std::int32_t last_day = 0;
std::int32_t day_now() {
    const std::uint8_t* c = *splug::rva<const std::uint8_t*>(0x14311b708);  // часы игры, +0xc4 — день
    std::int32_t d = 0;
    if (c) std::memcpy(&d, c + 0xc4, 4);
    return d;
}

void report(std::int64_t t) {
    const double span = seconds(t - last_report);
    if (span < report_s) return;
    const std::int32_t day = day_now();
    splug::log("%.0f s: in-tick frames %lld (%.1f/s), drawn %lld (%.1f/s), %.0f ms drawing (%.1f%% of wall); interval %.3f s, days %d",
               span, calls, calls / span, drawn, drawn / span, drawn_ms, drawn_ms / (10.0 * span), srp_interval,
               last_day ? day - last_day : 0);
    last_day = day;
    calls = drawn = 0;
    drawn_ms = 0.0;
    last_report = t;
}

void frame_hook(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    const std::int64_t t0 = now();
    ++calls;
    ++total_calls;
    // при включённом [main] интервал считается от любого кадра: общий потолок частоты кадров
    const std::int64_t ref = srp_main ? last_any : last_drawn;
    if (mode == splug::Mode::on && ref != 0 && seconds(t0 - ref) < srp_interval) {
        if (quiet_skip) {
            auto* gfx = *splug::rva<std::uint8_t*>(kGfx);
            auto* clock = *splug::rva<void*>(kClock);
            if (gfx && clock) {
                auto* window = *reinterpret_cast<std::uint8_t**>(gfx + 0xe8);
                const auto fn = reinterpret_cast<ClockFn>((*reinterpret_cast<void***>(clock))[0x28 / 8]);
                if (window) *reinterpret_cast<double*>(window + 0xd8) = fn(clock);
            }
        }
        report(t0);
        return;
    }
    original(a, b, c, d);
    const std::int64_t t1 = now();
    last_drawn = t1;
    last_any = t1;
    ++drawn;
    ++total_drawn;
    drawn_ms += 1000.0 * seconds(t1 - t0);
    report(t1);
}

// ---- 0.2.0: кадры главного цикла ----
constexpr std::uint64_t kTickCall = 0x140336b80;   // call 1401b9dd0 (тик)
constexpr std::uint8_t kTickCallBytes[5] = {0xe8, 0x4b, 0x32, 0xe8, 0xff};
constexpr std::uint64_t kTickFn = 0x1401b9dd0;
constexpr std::uint64_t kFrameCall = 0x140336b91;  // call 142025648 — первая инструкция кадра
constexpr std::uint8_t kFrameCallBytes[5] = {0xe8, 0xb2, 0xea, 0xce, 0x01};
constexpr std::uint64_t kQpf = 0x142025648;
constexpr std::uint64_t kEpilogue = 0x140337d83;
// эпилог: movaps xmm6,[rsp+0x200]; movaps xmm7,[rsp+0x1f0]; add rsp,0x218
constexpr std::uint8_t kEpilogueBytes[23] = {0x0f, 0x28, 0xb4, 0x24, 0x00, 0x02, 0x00, 0x00, 0x0f, 0x28, 0xbc, 0x24,
                                             0xf0, 0x01, 0x00, 0x00, 0x48, 0x81, 0xc4, 0x18, 0x02, 0x00, 0x00};
// 140336b40: пролог кладёт dl в [rbp+0x168] (mov [rax+0x10],dl; lea rbp,[rax-0x158])
constexpr std::uint8_t kEntryBytes[25] = {0x48, 0x8b, 0xc4, 0x88, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                          0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xa8, 0xfe, 0xff, 0xff};

using TickFn = void (*)(void*);
TickFn tick_original = nullptr;
splug::CallPatch tick_patch, frame_patch;

splug::Mode main_mode = splug::Mode::off;
double main_interval_s = 0.1;
double min_tick_s = 0.005;
int min_streak = 2;
double run_gap_s = 0.3;
std::int64_t last_heavy = 0;  // момент решения после последнего тяжёлого тика

std::int64_t last_tick = 0;    // длительность последнего тика главного цикла, такты QPC
std::int64_t post_start = 0;   // момент решения по кадру главного цикла
bool post_skipped = false;
int streak = 0;
std::int64_t main_report = 0;
long long m_iter = 0, m_heavy = 0, m_would = 0, m_skip = 0;
double m_tick_ms = 0, m_post_drawn_ms = 0, m_post_skip_ms = 0;
long long m_post_drawn = 0, m_post_skip = 0;

void tick_wrap(void* session) {
    const std::int64_t t0 = now();
    if (post_start) {
        const double ms = 1000.0 * seconds(t0 - post_start);
        if (post_skipped) {
            m_post_skip_ms += ms;
            ++m_post_skip;
        } else {
            m_post_drawn_ms += ms;
            ++m_post_drawn;
        }
        post_start = 0;
    }
    tick_original(session);
    last_tick = now() - t0;
}

void report_main(std::int64_t t) {
    const double span = seconds(t - main_report);
    if (span < report_s) return;
    splug::log("%.0f s main loop: %lld frames with tick (%.1f/s), heavy %lld, tick %.0f ms (%.1f%% of wall); "
               "drawn %lld (%.1f/s, %.1f ms until next tick), skipped %lld (%.1f/s, %.2f ms), would skip %lld",
               span, m_iter, m_iter / span, m_heavy, m_tick_ms, m_tick_ms / (10.0 * span), m_post_drawn,
               m_post_drawn / span, m_post_drawn ? m_post_drawn_ms / m_post_drawn : 0.0, m_post_skip,
               m_post_skip / span, m_post_skip ? m_post_skip_ms / m_post_skip : 0.0, m_would);
    m_iter = m_heavy = m_would = m_skip = m_post_drawn = m_post_skip = 0;
    m_tick_ms = m_post_drawn_ms = m_post_skip_ms = 0;
    main_report = t;
}

}  // namespace

extern "C" bool srp_main_decide(std::uint32_t tick_flag) {
    if (!tick_flag) return false;  // кадр изнутри тика или кадр без тика — не трогаем
    const std::int64_t t = now();
    ++m_iter;
    const double tick_s = seconds(last_tick);
    m_tick_ms += 1000.0 * tick_s;
    const bool heavy = tick_s >= min_tick_s;
    if (heavy) {
        ++m_heavy;
        streak = (last_heavy != 0 && seconds(t - last_heavy) < run_gap_s) ? streak + 1 : 1;
        last_heavy = t;
    }
    const bool running = streak >= min_streak && seconds(t - last_heavy) < run_gap_s;
    // только после тяжёлого тика: лёгкий тик значит, что игра ждёт (рабочие потоки, темп скорости), и
    // без кадра главный цикл крутится вхолостую (замер: 80 тыс. итераций/с и -8 % дней в секунду)
    const bool want = heavy && running && last_any != 0 && seconds(t - last_any) < main_interval_s;
    if (want) ++m_would;
    const bool skip = want && main_mode == splug::Mode::on && srp_main != 0;
    last_tick = 0;
    if (skip) ++m_skip;
    else last_any = t;
    post_start = t;
    post_skipped = skip;
    report_main(t);
    return skip;
}

namespace {

void install_main() {
    if (main_mode == splug::Mode::off) return;
    if (!splug::bytes_match(0x140336b40, kEntryBytes) || !splug::bytes_match(kEpilogue, kEpilogueBytes)) {
        splug::log("main REFUSED: bytes of 140336b40 differ; no bytes changed");
        return;
    }
    tick_original = reinterpret_cast<TickFn>(splug::rva(kTickFn));
    srp_qpf = reinterpret_cast<std::uint64_t>(splug::rva(kQpf));
    srp_epilogue = reinterpret_cast<std::uint64_t>(splug::rva(kEpilogue));
    if (const char* err = splug::prepare_call(splug::rva<std::uint8_t>(kTickCall), kTickCallBytes,
                                              reinterpret_cast<const void*>(&tick_wrap), tick_patch)) {
        splug::log("main REFUSED: %s", err);
        return;
    }
    if (const char* err = splug::prepare_call(splug::rva<std::uint8_t>(kFrameCall), kFrameCallBytes,
                                              reinterpret_cast<const void*>(&srp_main_stub), frame_patch)) {
        splug::log("main REFUSED: %s", err);
        return;
    }
    main_report = now();
    if (!splug::commit_call(tick_patch) || !splug::commit_call(frame_patch)) {
        splug::log("main REFUSED: cannot write calls");
        return;
    }
    srp_main = main_mode == splug::Mode::on ? 1 : 0;
    splug::log("main hooks installed at %llx / %llx", static_cast<unsigned long long>(kTickCall),
               static_cast<unsigned long long>(kFrameCall));
}

double ini_double(const wchar_t* section, const wchar_t* key, double def, double lo, double hi) {
    const std::wstring s = splug::ini_string(section, key, L"");
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const double v = std::wcstod(s.c_str(), &end);
    if (end == s.c_str() || v < lo || v > hi) {
        splug::log("ini: %ls=%ls is not a number in [%g, %g]; using %g", key, s.c_str(), lo, hi, def);
        return def;
    }
    return v;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_render_patch")) return;
    QueryPerformanceFrequency(&freq);
    mode = splug::ini_mode(L"render", L"mode", L"off");
    srp_interval = ini_double(L"render", L"frame_interval", 0.1, 0.0, 2.0);
    report_s = ini_double(L"render", L"report_s", 30.0, 1.0, 3600.0);
    quiet_skip = splug::ini_int(L"render", L"quiet_skip", 1) != 0;
    main_mode = splug::ini_mode(L"main", L"mode", L"off");
    main_interval_s = ini_double(L"main", L"frame_interval", 0.1, 0.0, 2.0);
    min_tick_s = ini_double(L"main", L"min_tick_ms", 5.0, 0.0, 10000.0) / 1000.0;
    min_streak = splug::ini_int(L"main", L"min_streak", 2);
    run_gap_s = ini_double(L"main", L"run_gap_ms", 300.0, 1.0, 10000.0) / 1000.0;
    splug::log("stellaris_render_patch 0.2.0: build %s, mode=%s, frame_interval=%.3f s, quiet_skip=%d, report every %.0f s; "
               "main mode=%s, frame_interval=%.3f s, min_tick_ms=%.1f, min_streak=%d, run_gap_ms=%.0f, srp_main at rva %llx, srp_interval at rva %llx",
               splug::state().build, splug::mode_name(mode), srp_interval, quiet_skip ? 1 : 0, report_s,
               splug::mode_name(main_mode), main_interval_s, min_tick_s * 1000.0, min_streak, run_gap_s * 1000.0,
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&srp_main) -
                                               reinterpret_cast<std::uintptr_t>(module)),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&srp_interval) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    install_main();
    if (mode == splug::Mode::off) return;

    auto* site = splug::rva<std::uint8_t>(kFrame);
    if (std::memcmp(site, kPrologue, sizeof(kPrologue)) != 0 || std::memcmp(site + 15, kNext, sizeof(kNext)) != 0) {
        splug::log("REFUSED: bytes at %llx differ; no bytes changed", static_cast<unsigned long long>(kFrame));
        return;
    }
    // трамплин: пролог + jmp [rip+0] -> site+15
    auto* tramp = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!tramp) {
        splug::log("REFUSED: no memory for trampoline");
        return;
    }
    std::memcpy(tramp, kPrologue, sizeof(kPrologue));
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(tramp + 15, jmp, sizeof(jmp));
    const auto back = reinterpret_cast<std::uint64_t>(site + 15);
    std::memcpy(tramp + 21, &back, sizeof(back));
    DWORD old = 0;
    if (!VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old)) {
        splug::log("REFUSED: cannot make trampoline executable");
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, 29);
    original = reinterpret_cast<FrameFn>(tramp);
    last_report = now();
    // 12 байт mov rax, hook; jmp rax — rax на входе функции свободен; оставшиеся 3 байта пролога — int3
    std::uint8_t patch[15];
    std::memset(patch, 0xcc, sizeof(patch));
    const auto hook = reinterpret_cast<std::uint64_t>(&frame_hook);
    patch[0] = 0x48;
    patch[1] = 0xb8;
    std::memcpy(patch + 2, &hook, sizeof(hook));
    patch[10] = 0xff;
    patch[11] = 0xe0;
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("REFUSED: cannot write %llx", static_cast<unsigned long long>(kFrame));
        return;
    }
    splug::log("hook installed at %llx", static_cast<unsigned long long>(kFrame));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
