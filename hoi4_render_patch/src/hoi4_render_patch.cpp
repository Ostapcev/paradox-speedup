// HOI4 Render Patch 0.3.0 experimental, for HOI4 1.19.4.2 ("Operation Postern") Windows x64.
//
// Порт eu4_render_patch 0.6.0 на Hearts of Iron IV. Разбор адресов:
// hoi4_kit/docs/02_hot_functions.md и 03_render_loop.md.
//
// 1) Кадры. Главный цикл (CInGameIdler::Update, 140e41670, слот 4 vftable 1429e58d8)
// каждую итерацию вызывает OnFrame (слот 28, 1402b8f00), а тот — при живом
// видеоустройстве (гейт 14229f1f0) — Draw/Render (слот 29, 140e4a140) через
// `call [rax+0xe8]` на 1402b909e. Сна в цикле нет — игра и так гонит тики на
// полной скорости, поэтому пропуск кадров превращается в игровое время.
//
// 2) Update Provinces (140bae670, метка профайлера "Update Provinces (%d)") —
// пересчёт графики провинций карты, ~10% главного потока в 1940-м. Вызывается из
// апдейтера карты 140bab870 каждую итерацию цикла (прямой call на 140babc34,
// rcx=карта r15, rdx=idler rsi, r8d=счётчик). Дt-параметра нет — пропуск без
// накопления безопасен, это чисто визуальное обновление.
//
// 3) Юнит-графика карты (1416b34a0) — перерисовка юнитов/счётчиков на карте,
// ~4,5% главного потока. Прямой call на 140baba08 из того же апдейтера карты
// (rcx=карта rdi, rdx=gamestate). Тоже чисто визуальная — пропуск безопасен.
//
// 4) CSupplySystem hourly update (140f49f10 через thunk 140f45d10: mov dl,1;
// jmp) — почасовой пересчёт снабжения, в поздней игре ~21% часового тика
// (скоуп id 4 таймер-менеджера в CGameState::HourlyUpdate 1401f5230, сайт
// 1401f59af). Это ИГРОВАЯ логика, поэтому троттлинг не по времени, а по игровым
// часам: supply_interval=N (>1) пропускает вызов, пока с прошлого прогона не
// прошло N часов. Влияет на геймплей (снабжение обновляется реже) — по
// умолчанию выключено (1 = каждый час, как в оригинале).
//
// С frame_interval > 0 отрисовка пропускается, пока часы идут чаще, чем раз в
// frame_interval, и прошлый кадр моложе frame_interval; provinces_interval и
// unitgfx_interval — то же для Update Provinces и юнит-графики. На паузе
// (счётчик часов gamestate+0x468 замер), в меню и на медленных скоростях всё
// работает как в оригинале.
//
// Режимы (hoi4_render_patch.ini, [render] mode=):
//   count    - только считает кадры и их время
//   throttle - пропускает кадры/обновления по интервалам
//   disabled - ничего не меняет (default)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include "hoi4_build.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "call_patch.hpp"

namespace {

enum class Mode { count, throttle, disabled };

// Контекст сайта (1402b9094): test al,al; je +0xc; mov rax,[rsi]; mov rcx,rsi;
// дальше сам вызов (1402b909e): call [rax+0xe8]  (Render, слот 29 CInGameIdler)
constexpr std::uintptr_t render_site_rva = 0x2b9094;
constexpr std::uint8_t render_bytes[] = {0x84, 0xc0, 0x74, 0x0c, 0x48, 0x8b, 0x06, 0x48,
    0x8b, 0xce, 0xff, 0x90, 0xe8, 0x00, 0x00, 0x00};
constexpr std::uintptr_t render_call_rva = 0x2b909e;  // 6 байт FF 90 E8 00 00 00
// Update Provinces: movaps xmm2,xmm6; mov rdx,rsi; mov rcx,r15; call 140bae670
constexpr std::uintptr_t provinces_site_rva = 0xbabc2b;
constexpr std::uint8_t provinces_bytes[] = {0x0f, 0x28, 0xd6, 0x48, 0x8b, 0xd6, 0x49, 0x8b,
    0xcf, 0xe8, 0x37, 0x2a, 0x00, 0x00};
constexpr std::uintptr_t provinces_call_rva = 0xbabc34;  // 5 байт E8 rel32
constexpr std::uintptr_t provinces_function_rva = 0xbae670;
// Юнит-графика карты: mov ebx,[rip+..]; mov rdx,[rip+gamestate]; mov rcx,rdi;
// call 1416b34a0
constexpr std::uintptr_t unitgfx_site_rva = 0xbab9f8;
constexpr std::uint8_t unitgfx_bytes[] = {0x8b, 0x1d, 0x9a, 0x80, 0xae, 0x02, 0x48, 0x8b,
    0x15, 0xab, 0x1c, 0x83, 0x02, 0x48, 0x8b, 0xcf, 0xe8, 0x93, 0x7a, 0xb0, 0x00};
constexpr std::uintptr_t unitgfx_call_rva = 0xbaba08;  // 5 байт E8 rel32
constexpr std::uintptr_t unitgfx_function_rva = 0x16b34a0;
// CSupplySystem hourly: lea rcx,[r15+0x3d8]; call 1401dd780; mov rcx,rax;
// call 140f45d10 (thunk: mov dl,1; jmp 140f49f10)
constexpr std::uintptr_t supply_site_rva = 0x1f59a0;
constexpr std::uint8_t supply_bytes[] = {0x49, 0x8d, 0x8f, 0xd8, 0x03, 0x00, 0x00, 0xe8,
    0xd4, 0x7d, 0xfe, 0xff, 0x48, 0x8b, 0xc8, 0xe8, 0x5c, 0x03, 0xd5, 0x00};
constexpr std::uintptr_t supply_call_rva = 0x1f59af;  // 5 байт E8 rel32
constexpr std::uintptr_t supply_function_rva = 0xf45d10;
// CGameState::_pInstance (записывается игрой при создании gamestate)
// Обновление 3D-сцены движка (камеры, анимации, частицы) из CInGameIdler::Update:
// movaps xmm1,xmm6 (dt); mov rcx,rax; call 1402b6480 -> 142294050. Идёт на каждой
// итерации, даже когда кадр пропущен. scene_interval: пропуск, пока часы идут быстро и
// прошлое обновление моложе интервала; пропущенное время копится и уходит в следующий
// вызов, так что анимации идут с той же скоростью. Чистая графика (код движка).
constexpr std::uintptr_t scene_site_rva = 0xe43b70;
constexpr std::uint8_t scene_bytes[] = {0x0f, 0x28, 0xce, 0x48, 0x8b, 0xc8, 0xe8, 0x05, 0x29,
    0x47, 0xff};
constexpr std::uintptr_t scene_call_rva = 0xe43b76;
constexpr std::uintptr_t scene_function_rva = 0x2b6480;
constexpr std::uintptr_t gamestate_rva = 0x33dd6b0;
// ai_frames: цикл ожидания ИИ 1401ee390 (главный поток крутит SwitchToThread, пока задача ИИ
// считается на воркерах). Игра сама рисует в нём вложенный кадр (механизм debug_smooth), если
// 1402ba690 скажет, что кадр просрочен. Мы подменяем это решение: пока игра идёт быстро, кадр
// рисуется здесь, как только с прошлого кадра прошло frame_interval, — и отрисовка идёт
// одновременно с ИИ, а не после тика. Сайт 1401ee6b2: call 1402ba690 (rcx движок, edx [r13+4bc]);
// сайт 1401ee6fc: call 1422943b0 (сам вложенный кадр; обёртка только считает и меряет).
constexpr std::uintptr_t due_call_rva = 0x1ee6b2;
constexpr std::uintptr_t due_function_rva = 0x2ba690;
constexpr std::uintptr_t nested_call_rva = 0x1ee6fc;
constexpr std::uintptr_t nested_function_rva = 0x22943b0;
// tick_interval: вход 1402ba690 («пора ли нарисовать кадр посреди тика», механизм debug_smooth;
// ~60 мест в часовом/дневном тике и цикл ожидания ИИ). Пока с прошлого «да» прошло меньше
// tick_interval, ответ «нет» без вызова игры; иначе решает игра. Только графика.
// Пролог: mov [rsp+10],rbx | push rdi | sub rsp,30 | mov rax,[rcx]  (13 байт, без rip)
constexpr std::uint8_t due_prologue[13] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec,
                                           0x30, 0x48, 0x8b, 0x01};
// ai_wait: в цикле ожидания ИИ вместо `call SwitchToThread` (1401ee72f -> thunk 1425a3678)
// короткая пауза на pause-инструкциях (~3 мкс), SwitchToThread раз в ~50 мкс. Главный поток
// меньше мешает соседнему логическому ядру, на котором может считаться ИИ.
constexpr std::uintptr_t yield_call_rva = 0x1ee72f;
constexpr std::uintptr_t yield_function_rva = 0x25a3678;
// int: монотонный счётчик часов в gamestate (дата = (hours - 43800000) / 8760)
constexpr std::uintptr_t hours_offset = 0x468;

using RenderFn = void (*)(void*);
using ProvincesFn = void (*)(void*, void*, std::uint32_t);
using UnitGfxFn = void (*)(void*, void*);
using SupplyFn = void (*)(void*);
using SceneFn = void (*)(void*, float);
using DueFn = bool (*)(void*, std::int32_t);
using NestedFn = void (*)(void*, char);

std::uintptr_t image = 0;
std::atomic<LONG> installation_status{0};
std::wstring log_path;
std::mutex log_mutex;
std::int64_t qpc_frequency = 1;
std::int64_t frame_interval_ticks = 0;      // 0 = не пропускать кадры
std::int64_t provinces_interval_ticks = 0;  // 0 = не пропускать Update Provinces
std::int64_t unitgfx_interval_ticks = 0;    // 0 = не пропускать юнит-графику
std::int32_t supply_interval_hours = 1;     // 1 = каждый час, как в оригинале
ProvincesFn original_provinces = nullptr;
UnitGfxFn original_unitgfx = nullptr;
SupplyFn original_supply = nullptr;
SceneFn original_scene = nullptr;
std::int64_t scene_interval_ticks = 0;
bool ai_frames = false;
std::int64_t tick_interval_ticks = 0;
std::int64_t last_due_yes = 0, last_due_asked = 0, ask_ticks = 1;
DueFn due_game = nullptr;  // трамплин на оригинал 1402ba690 (если вход перехвачен)
std::atomic<std::uint64_t> entry_calls{0}, entry_asked{0}, entry_yes{0};
bool ai_wait = false;
std::int64_t pause_ticks = 1, yield_ticks = 1;
std::atomic<std::uint64_t> pauses{0}, pause_yields{0};
DueFn original_due = nullptr;
NestedFn original_nested = nullptr;
bool in_ai_frame = false;
std::atomic<std::uint64_t> due_calls{0}, due_game_yes{0}, due_ours_yes{0}, nested_calls{0},
    nested_qpc{0}, nested_drawn{0};
std::int64_t last_scene = 0;
float scene_pending_dt = 0.0f;
std::atomic<std::uint64_t> scene_run{0}, scene_skipped{0}, scene_qpc{0};

// Только главный поток.
std::int64_t last_render = 0;
std::int64_t last_provinces = 0;
std::int64_t last_unitgfx = 0;
std::int64_t last_hours = -1;
std::int64_t last_hour_change = 0;
std::int64_t hour_gap = 1LL << 60;
std::int32_t last_supply_hour = 0;
bool last_supply_hour_valid = false;

std::atomic<std::uint64_t> frames_drawn{0};
std::atomic<std::uint64_t> frames_skipped{0};
std::atomic<std::uint64_t> render_qpc{0};
std::atomic<std::uint64_t> provinces_run{0};
std::atomic<std::uint64_t> provinces_skipped{0};
std::atomic<std::uint64_t> provinces_qpc{0};
std::atomic<std::uint64_t> unitgfx_run{0};
std::atomic<std::uint64_t> unitgfx_skipped{0};
std::atomic<std::uint64_t> unitgfx_qpc{0};
std::atomic<std::uint64_t> supply_run{0};
std::atomic<std::uint64_t> supply_skipped{0};
std::atomic<std::uint64_t> supply_qpc{0};

inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void log(const char* format, ...) noexcept {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> lock(log_mutex);
    FILE* file = _wfopen(log_path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "[%lu:%lu] %s\n", GetCurrentProcessId(), GetTickCount(), text);
    std::fclose(file);
}

// Игровые часы идут (не пауза, не меню) и идут чаще, чем раз в interval_ticks.
// На паузе счётчик часов замерает и now - last_hour_change перерастает интервал.
bool game_running_fast(std::int64_t now, std::int64_t interval_ticks) noexcept {
    if (interval_ticks <= 0) return false;
    const auto state = *reinterpret_cast<const volatile std::uint64_t*>(image + gamestate_rva);
    if (state == 0) return false;  // меню: gamestate ещё нет
    const auto hours = *reinterpret_cast<const volatile std::int32_t*>(state + hours_offset);
    if (hours != static_cast<std::int32_t>(last_hours)) {
        if (last_hour_change) hour_gap = now - last_hour_change;
        last_hour_change = now;
        last_hours = hours;
    }
    return hour_gap < interval_ticks && now - last_hour_change < interval_ticks;
}

bool frame_skippable(std::int64_t now) noexcept {
    return game_running_fast(now, frame_interval_ticks) &&
        now - last_render < frame_interval_ticks;
}

// Вызывается вместо `call [rax+0xe8]`: rcx = CInGameIdler (this), как у оригинала.
void render_hook(void* idler) {
    const auto now = qpc_now();
    if (frame_skippable(now)) {
        frames_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto vtable = *reinterpret_cast<const std::uint8_t* const*>(idler);
    const auto render = *reinterpret_cast<const RenderFn*>(vtable + 0xe8);
    render(idler);
    last_render = now;
    frames_drawn.fetch_add(1, std::memory_order_relaxed);
    if (in_ai_frame) nested_drawn.fetch_add(1, std::memory_order_relaxed);
    render_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// Вызывается вместо `call 140bae670`: rcx = карта, rdx = idler, r8d = счётчик.
void provinces_hook(void* map, void* idler, std::uint32_t counter) {
    const auto now = qpc_now();
    if (game_running_fast(now, provinces_interval_ticks) &&
        now - last_provinces < provinces_interval_ticks) {
        provinces_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    original_provinces(map, idler, counter);
    last_provinces = now;
    provinces_run.fetch_add(1, std::memory_order_relaxed);
    provinces_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// Вызывается вместо `call 1416b34a0`: rcx = карта (rdi), rdx = gamestate.
void unitgfx_hook(void* map, void* gamestate) {
    const auto now = qpc_now();
    if (game_running_fast(now, unitgfx_interval_ticks) &&
        now - last_unitgfx < unitgfx_interval_ticks) {
        unitgfx_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    original_unitgfx(map, gamestate);
    last_unitgfx = now;
    unitgfx_run.fetch_add(1, std::memory_order_relaxed);
    unitgfx_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// Вызывается вместо `call 140f45d10` из CGameState::HourlyUpdate:
// rcx = CSupplySystem (результат доступа к gamestate+0x3d8). Оригинал — thunk,
// сам ставящий dl=1 перед jmp в 140f49f10. Троттлинг по игровым часам, а не по
// времени: пропуск, пока с прошлого прогона прошло меньше supply_interval_hours.
// Загрузка сейва/откат часов (hours < last) трактуется как «давно» — прогон.
void supply_hook(void* supply) {
    const auto now = qpc_now();
    if (supply_interval_hours > 1) {
        const auto state = *reinterpret_cast<const volatile std::uint64_t*>(image + gamestate_rva);
        if (state != 0) {
            const auto hours =
                *reinterpret_cast<const volatile std::int32_t*>(state + hours_offset);
            if (last_supply_hour_valid &&
                static_cast<std::uint32_t>(hours - last_supply_hour) <
                    static_cast<std::uint32_t>(supply_interval_hours)) {
                supply_skipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            last_supply_hour = hours;
            last_supply_hour_valid = true;
        }
    }
    original_supply(supply);
    supply_run.fetch_add(1, std::memory_order_relaxed);
    supply_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// Вызывается вместо `call 1402b6480`: rcx = объект idler'а (vfunc +0x130), xmm1 = dt.
void scene_hook(void* object, float dt) {
    const auto now = qpc_now();
    if (game_running_fast(now, scene_interval_ticks) && now - last_scene < scene_interval_ticks) {
        scene_pending_dt += dt;
        scene_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float total = dt + scene_pending_dt;
    scene_pending_dt = 0.0f;
    original_scene(object, total);
    last_scene = now;
    scene_run.fetch_add(1, std::memory_order_relaxed);
    scene_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// Вход 1402ba690 (главный поток): ограничение вложенных кадров по tick_interval.
bool due_entry_hook(void* engine, std::int32_t arg) {
    entry_calls.fetch_add(1, std::memory_order_relaxed);
    const auto now = qpc_now();
    if (now - last_due_yes < tick_interval_ticks) return false;
    // и спрашивать игру не чаще раза в ~2 мс: кадр посреди тика начнётся не более чем на 2 мс позже
    if (now - last_due_asked < ask_ticks) return false;
    last_due_asked = now;
    entry_asked.fetch_add(1, std::memory_order_relaxed);
    const bool yes = due_game(engine, arg);
    if (yes) { last_due_yes = now; entry_yes.fetch_add(1, std::memory_order_relaxed); }
    return yes;
}

// Вместо SwitchToThread в цикле ожидания ИИ.
void ai_pause_hook() {
    static std::int64_t spin_start = 0, last_seen = 0;
    const auto t0 = qpc_now();
    if (t0 - last_seen > yield_ticks) spin_start = t0;  // новое ожидание
    for (auto t = t0; t - t0 < pause_ticks; t = qpc_now())
        for (int i = 0; i < 16; ++i) _mm_pause();
    last_seen = qpc_now();
    pauses.fetch_add(1, std::memory_order_relaxed);
    if (last_seen - spin_start >= yield_ticks) {
        spin_start = last_seen;
        pause_yields.fetch_add(1, std::memory_order_relaxed);
        SwitchToThread();
    }
}

// Вместо `call 1402ba690` в цикле ожидания ИИ (главный поток).
bool due_hook(void* engine, std::int32_t arg) {
    const bool game = (due_game ? due_entry_hook : original_due)(engine, arg);
    due_calls.fetch_add(1, std::memory_order_relaxed);
    if (game) due_game_yes.fetch_add(1, std::memory_order_relaxed);
    if (!ai_frames) return game;
    const auto now = qpc_now();
    if (!game_running_fast(now, frame_interval_ticks)) return game;
    const bool ours = now - last_render >= frame_interval_ticks;
    if (ours) due_ours_yes.fetch_add(1, std::memory_order_relaxed);
    return ours;
}

// Вместо `call 1422943b0` (вложенный кадр в цикле ожидания ИИ): только счёт и время.
void nested_hook(void* engine, char arg) {
    const auto t0 = qpc_now();
    const bool outer = in_ai_frame;
    in_ai_frame = true;
    original_nested(engine, arg);
    in_ai_frame = outer;
    nested_calls.fetch_add(1, std::memory_order_relaxed);
    nested_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - t0), std::memory_order_relaxed);
}

bool patch_call(std::uintptr_t call_rva, std::uintptr_t target_rva, const void* hook,
                const char* name) {
    std::uint8_t expected[5] = {0xe8};
    const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(target_rva) -
                                               static_cast<std::int64_t>(call_rva + 5));
    std::memcpy(expected + 1, &rel, 4);
    auto* site = reinterpret_cast<std::uint8_t*>(image + call_rva);
    if (std::memcmp(site, expected, 5) != 0) {
        log("%s hook REFUSED (call site bytes differ)", name);
        return false;
    }
    char error[160]{};
    speedpatch::CallPatch patch;
    if (!speedpatch::prepare_call_bytes(site, expected, 5, hook, patch, error, sizeof(error)) ||
        !speedpatch::commit_call(patch, error, sizeof(error))) {
        speedpatch::discard_call(patch);
        log("%s hook REFUSED (%s)", name, error);
        return false;
    }
    return true;
}

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        const auto drawn = frames_drawn.load();
        const auto skipped = frames_skipped.load();
        if (!drawn && !skipped) continue;
        const auto ms = render_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency);
        log("stats frames_drawn=%llu frames_skipped=%llu render_ms=%llu avg_us=%llu",
            static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(skipped),
            static_cast<unsigned long long>(ms),
            static_cast<unsigned long long>(drawn ? ms * 1000 / drawn : 0));
        if (original_provinces) {
            const auto runs = provinces_run.load();
            const auto pms = provinces_qpc.load() * 1000 /
                static_cast<std::uint64_t>(qpc_frequency);
            log("stats provinces_run=%llu provinces_skipped=%llu provinces_ms=%llu avg_us=%llu",
                static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(provinces_skipped.load()),
                static_cast<unsigned long long>(pms),
                static_cast<unsigned long long>(runs ? pms * 1000 / runs : 0));
        }
        if (original_unitgfx) {
            const auto runs = unitgfx_run.load();
            const auto ums = unitgfx_qpc.load() * 1000 /
                static_cast<std::uint64_t>(qpc_frequency);
            log("stats unitgfx_run=%llu unitgfx_skipped=%llu unitgfx_ms=%llu avg_us=%llu",
                static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(unitgfx_skipped.load()),
                static_cast<unsigned long long>(ums),
                static_cast<unsigned long long>(runs ? ums * 1000 / runs : 0));
        }
        if (original_scene) {
            const auto runs = scene_run.load();
            const auto sms = scene_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency);
            log("stats scene_run=%llu scene_skipped=%llu scene_ms=%llu avg_us=%llu",
                static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(scene_skipped.load()),
                static_cast<unsigned long long>(sms),
                static_cast<unsigned long long>(runs ? sms * 1000 / runs : 0));
        }
        if (original_due) {
            const auto n = nested_calls.load();
            const auto nms = nested_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency);
            log("stats ai_wait due_checks=%llu game_due=%llu ours_due=%llu nested_frames=%llu "
                "drawn_inside=%llu nested_ms=%llu avg_us=%llu",
                static_cast<unsigned long long>(due_calls.load()),
                static_cast<unsigned long long>(due_game_yes.load()),
                static_cast<unsigned long long>(due_ours_yes.load()),
                static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(nested_drawn.load()),
                static_cast<unsigned long long>(nms),
                static_cast<unsigned long long>(n ? nms * 1000 / n : 0));
        }
        if (due_game)
            log("stats tick_frames calls=%llu asked_game=%llu yes=%llu",
                static_cast<unsigned long long>(entry_calls.load()),
                static_cast<unsigned long long>(entry_asked.load()),
                static_cast<unsigned long long>(entry_yes.load()));
        if (ai_wait)
            log("stats ai_wait pauses=%llu yields=%llu", static_cast<unsigned long long>(pauses.load()),
                static_cast<unsigned long long>(pause_yields.load()));
        if (original_supply) {
            const auto runs = supply_run.load();
            const auto sms = supply_qpc.load() * 1000 /
                static_cast<std::uint64_t>(qpc_frequency);
            log("stats supply_run=%llu supply_skipped=%llu supply_ms=%llu avg_us=%llu",
                static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(supply_skipped.load()),
                static_cast<unsigned long long>(sms),
                static_cast<unsigned long long>(runs ? sms * 1000 / runs : 0));
        }
    }
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\hoi4_render_patch.log";
    const auto ini = directory + L"\\hoi4_render_patch.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"hoi4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }

    wchar_t value[32]{};
    GetPrivateProfileStringW(L"render", L"mode", L"disabled", value, 32, ini.c_str());
    const Mode mode = _wcsicmp(value, L"count") == 0 ? Mode::count :
        _wcsicmp(value, L"throttle") == 0 ? Mode::throttle : Mode::disabled;
    if (mode == Mode::disabled) {
        log("disabled by configuration; no bytes changed");
        installation_status.store(3);
        return 0;
    }

    unsigned exe_stamp = 0, exe_size = 0;
    const char* exe_build = hoi4build::detect(&exe_stamp, &exe_size);
    if (!exe_build) {
        log("REFUSED: unknown hoi4.exe build (stamp %08x, image size %x); no bytes changed",
            exe_stamp, exe_size);
        installation_status.store(-5);
        return 5;
    }
    log("hoi4.exe build %s", exe_build);
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (std::memcmp(reinterpret_cast<void*>(image + render_site_rva), render_bytes,
            sizeof(render_bytes)) != 0) {
        log("REFUSED: render call site bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

    float frame_interval = 0.0f;
    float provinces_interval = 0.0f;
    if (mode == Mode::throttle) {
        GetPrivateProfileStringW(L"render", L"frame_interval", L"0.1", value, 32, ini.c_str());
        frame_interval = std::wcstof(value, nullptr);
        if (!(frame_interval > 0.0f && frame_interval <= 2.0f)) frame_interval = 0.1f;
        frame_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(frame_interval) * static_cast<double>(qpc_frequency));
        GetPrivateProfileStringW(L"render", L"provinces_interval", L"0.25", value, 32, ini.c_str());
        provinces_interval = std::wcstof(value, nullptr);
        if (!(provinces_interval >= 0.0f && provinces_interval <= 10.0f)) {
            provinces_interval = 0.25f;
        }
        provinces_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(provinces_interval) * static_cast<double>(qpc_frequency));
        GetPrivateProfileStringW(L"render", L"unitgfx_interval", L"0.1", value, 32, ini.c_str());
        float unitgfx_interval = std::wcstof(value, nullptr);
        if (!(unitgfx_interval >= 0.0f && unitgfx_interval <= 10.0f)) {
            unitgfx_interval = 0.1f;
        }
        unitgfx_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(unitgfx_interval) * static_cast<double>(qpc_frequency));
        GetPrivateProfileStringW(L"render", L"scene_interval", L"0", value, 32, ini.c_str());
        float scene_interval = std::wcstof(value, nullptr);
        if (!(scene_interval >= 0.0f && scene_interval <= 10.0f)) scene_interval = 0.0f;
        scene_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(scene_interval) * static_cast<double>(qpc_frequency));
        ai_frames = GetPrivateProfileIntW(L"render", L"ai_frames", 0, ini.c_str()) != 0;
        ai_wait = GetPrivateProfileIntW(L"render", L"ai_wait", 0, ini.c_str()) != 0;
        GetPrivateProfileStringW(L"render", L"tick_interval", L"0", value, 32, ini.c_str());
        float tick_interval = std::wcstof(value, nullptr);
        if (!(tick_interval >= 0.0f && tick_interval <= 10.0f)) tick_interval = 0.0f;
        tick_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(tick_interval) * static_cast<double>(qpc_frequency));
        pause_ticks = qpc_frequency * 3 / 1000000 + 1;   // ~3 мкс
        ask_ticks = qpc_frequency * 2 / 1000 + 1;         // ~2 мс
        yield_ticks = qpc_frequency * 50 / 1000000 + 1;  // ~50 мкс
        supply_interval_hours = GetPrivateProfileIntW(L"render", L"supply_interval", 1, ini.c_str());
        if (supply_interval_hours < 1 || supply_interval_hours > 168) supply_interval_hours = 1;
    }

    char error[160]{};
    speedpatch::CallPatch render;
    if (!speedpatch::prepare_call_bytes(
            reinterpret_cast<std::uint8_t*>(image + render_call_rva), render_bytes + 10, 6,
            reinterpret_cast<const void*>(&render_hook), render, error, sizeof(error)) ||
        !speedpatch::commit_call(render, error, sizeof(error))) {
        speedpatch::discard_call(render);
        log("REFUSED: %s", error);
        installation_status.store(-5);
        return 5;
    }

    // Update Provinces: отдельный отказ не мешает троттлингу кадров.
    if (std::memcmp(reinterpret_cast<void*>(image + provinces_site_rva), provinces_bytes,
            sizeof(provinces_bytes)) != 0) {
        log("provinces hook REFUSED (call site bytes differ); frame throttle stays");
    } else {
        speedpatch::CallPatch provinces;
        original_provinces = reinterpret_cast<ProvincesFn>(image + provinces_function_rva);
        if (!speedpatch::prepare_call_bytes(
                reinterpret_cast<std::uint8_t*>(image + provinces_call_rva), provinces_bytes + 9, 5,
                reinterpret_cast<const void*>(&provinces_hook), provinces, error, sizeof(error)) ||
            !speedpatch::commit_call(provinces, error, sizeof(error))) {
            speedpatch::discard_call(provinces);
            original_provinces = nullptr;
            log("provinces hook REFUSED (%s); frame throttle stays", error);
        }
    }

    // Юнит-графика карты: отдельный отказ не мешает остальным.
    if (std::memcmp(reinterpret_cast<void*>(image + unitgfx_site_rva), unitgfx_bytes,
            sizeof(unitgfx_bytes)) != 0) {
        log("unitgfx hook REFUSED (call site bytes differ)");
    } else {
        speedpatch::CallPatch unitgfx;
        original_unitgfx = reinterpret_cast<UnitGfxFn>(image + unitgfx_function_rva);
        if (!speedpatch::prepare_call_bytes(
                reinterpret_cast<std::uint8_t*>(image + unitgfx_call_rva), unitgfx_bytes + 16, 5,
                reinterpret_cast<const void*>(&unitgfx_hook), unitgfx, error, sizeof(error)) ||
            !speedpatch::commit_call(unitgfx, error, sizeof(error))) {
            speedpatch::discard_call(unitgfx);
            original_unitgfx = nullptr;
            log("unitgfx hook REFUSED (%s)", error);
        }
    }

    // Сцена движка: только при scene_interval > 0.
    if (mode == Mode::throttle && scene_interval_ticks > 0) {
        if (std::memcmp(reinterpret_cast<void*>(image + scene_site_rva), scene_bytes,
                sizeof(scene_bytes)) != 0) {
            log("scene hook REFUSED (call site bytes differ)");
        } else {
            speedpatch::CallPatch scene;
            original_scene = reinterpret_cast<SceneFn>(image + scene_function_rva);
            if (!speedpatch::prepare_call_bytes(
                    reinterpret_cast<std::uint8_t*>(image + scene_call_rva), scene_bytes + 6, 5,
                    reinterpret_cast<const void*>(&scene_hook), scene, error, sizeof(error)) ||
                !speedpatch::commit_call(scene, error, sizeof(error))) {
                speedpatch::discard_call(scene);
                original_scene = nullptr;
                log("scene hook REFUSED (%s)", error);
            }
        }
    }

    // Вход 1402ba690: tick_interval > 0 (до ai_frames, который вызывает уже перехваченный вход).
    if (mode == Mode::throttle && tick_interval_ticks > 0) {
        auto* entry = reinterpret_cast<std::uint8_t*>(image + due_function_rva);
        auto* tramp = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        DWORD old = 0;
        if (std::memcmp(entry, due_prologue, sizeof(due_prologue)) != 0 || !tramp) {
            log("tick_interval REFUSED (1402ba690 bytes differ)");
        } else if (VirtualProtect(entry, 13, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(tramp, due_prologue, 13);
            const std::uint64_t back = image + due_function_rva + 13;
            tramp[13] = 0xff; tramp[14] = 0x25; std::memset(tramp + 15, 0, 4);  // jmp [rip+0]
            std::memcpy(tramp + 19, &back, 8);
            FlushInstructionCache(GetCurrentProcess(), tramp, 64);
            due_game = reinterpret_cast<DueFn>(tramp);
            std::uint8_t patch[13] = {0x48, 0xb8};
            const auto target = reinterpret_cast<std::uint64_t>(&due_entry_hook);
            std::memcpy(patch + 2, &target, 8);
            patch[10] = 0xff; patch[11] = 0xe0; patch[12] = 0xcc;  // jmp rax; int3
            std::memcpy(entry, patch, 13);
            VirtualProtect(entry, 13, old, &old);
            FlushInstructionCache(GetCurrentProcess(), entry, 13);
            log("tick_interval=%g installed", static_cast<double>(tick_interval_ticks) / static_cast<double>(qpc_frequency));
        }
    }
    if (mode == Mode::throttle && ai_wait) {
        if (!patch_call(yield_call_rva, yield_function_rva, reinterpret_cast<const void*>(&ai_pause_hook), "ai_wait"))
            ai_wait = false;
        else
            log("ai_wait installed");
    }

    // Кадры во время ожидания ИИ: только в throttle и при ai_frames=1 (в count — только счёт).
    if (mode == Mode::count || ai_frames) {
        original_due = reinterpret_cast<DueFn>(image + due_function_rva);
        original_nested = reinterpret_cast<NestedFn>(image + nested_function_rva);
        if (!patch_call(due_call_rva, due_function_rva, reinterpret_cast<const void*>(&due_hook), "ai_frames due")) {
            original_due = nullptr;
            ai_frames = false;
        } else if (!patch_call(nested_call_rva, nested_function_rva,
                               reinterpret_cast<const void*>(&nested_hook), "ai_frames nested")) {
            original_nested = nullptr;  // решение подменено, счёт вложенных кадров недоступен
        }
        if (original_due) log("ai_frames=%d installed", ai_frames ? 1 : 0);
    }

    // CSupplySystem hourly: игровая логика, патчим только когда supply_interval > 1.
    if (mode == Mode::throttle && supply_interval_hours > 1) {
        if (std::memcmp(reinterpret_cast<void*>(image + supply_site_rva), supply_bytes,
                sizeof(supply_bytes)) != 0) {
            log("supply hook REFUSED (call site bytes differ)");
        } else {
            speedpatch::CallPatch supply;
            original_supply = reinterpret_cast<SupplyFn>(image + supply_function_rva);
            if (!speedpatch::prepare_call_bytes(
                    reinterpret_cast<std::uint8_t*>(image + supply_call_rva), supply_bytes + 15, 5,
                    reinterpret_cast<const void*>(&supply_hook), supply, error, sizeof(error)) ||
                !speedpatch::commit_call(supply, error, sizeof(error))) {
                speedpatch::discard_call(supply);
                original_supply = nullptr;
                log("supply hook REFUSED (%s)", error);
            }
        }
    }

    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    installation_status.store(mode == Mode::throttle ? 1 : 2);
    log("initialized version=0.3.0-experimental mode=%s frame_interval=%g provinces_interval=%g"
        " unitgfx_interval=%g supply_interval=%ld%s%s%s",
        mode == Mode::throttle ? "throttle" : "count", static_cast<double>(frame_interval),
        static_cast<double>(provinces_interval),
        static_cast<double>(unitgfx_interval_ticks) /
            static_cast<double>(qpc_frequency),
        static_cast<long>(supply_interval_hours),
        original_provinces ? "" : " (provinces off)",
        original_unitgfx ? "" : " (unitgfx off)",
        original_supply ? "" : " (supply off)");
    if (original_scene) log("scene_interval=%g", static_cast<double>(scene_interval_ticks) / static_cast<double>(qpc_frequency));
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
        installation_status.store(-7);
        return 7;
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG HOI4RenderPatchStatus() {
    return installation_status.load();
}
