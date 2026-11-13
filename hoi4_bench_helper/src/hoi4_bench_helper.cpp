// HOI4 Bench Helper — вспомогательный плагин для бенчмарка, HOI4 1.19.4.2 x64.
//
// observe=on: после загрузки сохранения, когда игровые часы прошли observe_after_hours,
// один раз выполняет консольную команду `observe` (псевдоним `spectator`) — игра
// переходит в режим наблюдателя. В HOI4, в отличие от EU4, сейв, сделанный
// наблюдателем, загружается за страну, поэтому без этого прогоны бенча шли бы за игрока.
//
// Обработчик команды: 140285120(result*) — без аргументов, result = {u8 ok; std::string text}.
// Он сам проверяет gamestate+0x520 (страна игрока; <1 = уже наблюдатель) и отправляет
// игровую команду смены страны — тот же путь, что и при вводе в консоли.
//
// Вызов делается в главном потоке из CInGameIdler::Update: перехват
// `call 1401b3b50` (тик сессии) на 140e418bb, перед оригиналом.
// Контекст сайта (140e418b0): test bl,bl; je +0xc; mov rcx,[r13+0x530]; call 1401b3b50.

#include <windows.h>

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

constexpr std::uintptr_t tick_site_rva = 0xe418b0;
constexpr std::uint8_t tick_bytes[] = {0x84, 0xdb, 0x74, 0x0c, 0x49, 0x8b, 0x8d, 0x30,
    0x05, 0x00, 0x00, 0xe8, 0x90, 0x22, 0x37, 0xff};
constexpr std::uintptr_t tick_call_rva = 0xe418bb;  // 5 байт E8 rel32
constexpr std::uintptr_t tick_function_rva = 0x1b3b50;
constexpr std::uintptr_t observe_rva = 0x285120;
constexpr std::uint8_t observe_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55};
constexpr std::uintptr_t gamestate_rva = 0x33dd6b0;
constexpr std::uintptr_t hours_offset = 0x468;
constexpr std::uintptr_t player_offset = 0x520;

using TickFn = void (*)(void*);
using ObserveFn = void* (*)(void*);

struct alignas(16) CommandResult {  // MSVC: bool + std::string (SSO 16, size, capacity)
    std::uint8_t ok;
    std::uint8_t pad[7];
    union { char buffer[16]; char* pointer; } text;
    std::uint64_t size;
    std::uint64_t capacity;
};

std::uintptr_t image = 0;
std::wstring log_path;
std::mutex log_mutex;
TickFn original_tick = nullptr;
std::int32_t observe_after_hours = 24;

// Только главный поток.
bool observe_done = false;
int observe_attempts = 0;
std::uint64_t seen_state = 0;
std::int32_t first_hours = 0;

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

void try_observe() noexcept {
    const auto state = *reinterpret_cast<const volatile std::uint64_t*>(image + gamestate_rva);
    if (state == 0) return;
    const auto hours = *reinterpret_cast<const volatile std::int32_t*>(state + hours_offset);
    if (state != seen_state) {  // новый gamestate (загрузка) — отсчёт заново
        seen_state = state;
        first_hours = hours;
        return;
    }
    if (hours - first_hours < observe_after_hours) return;
    const auto player = *reinterpret_cast<const volatile std::int32_t*>(state + player_offset);
    if (player < 1) {
        log("observe: already observing (hours=%ld)", static_cast<long>(hours));
        observe_done = true;
        return;
    }
    CommandResult result{};
    result.capacity = 15;
    reinterpret_cast<ObserveFn>(image + observe_rva)(&result);
    const char* text = result.capacity > 15 ? result.text.pointer : result.text.buffer;
    log("observe: player=%ld hours=%ld ok=%u \"%.*s\"", static_cast<long>(player),
        static_cast<long>(hours), static_cast<unsigned>(result.ok),
        static_cast<int>(result.size < 200 ? result.size : 200), text);
    // Длинный текст (ошибка) лежит в куче игры — оставляем (раз за запуск).
    first_hours = hours;  // повтор, если не вышло, — ещё через observe_after_hours
    if (result.ok || ++observe_attempts >= 3) observe_done = true;
}

// Вместо `call 1401b3b50`: rcx = [r13+0x530], как у оригинала.
void tick_hook(void* session) {
    if (!observe_done) try_observe();
    original_tick(session);
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\hoi4_bench_helper.log";
    const auto ini = directory + L"\\hoi4_bench_helper.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"hoi4.exe") != 0) return 3;

    wchar_t value[32]{};
    GetPrivateProfileStringW(L"bench", L"observe", L"off", value, 32, ini.c_str());
    if (_wcsicmp(value, L"on") != 0) {
        log("disabled by configuration (observe=off); no bytes changed");
        return 0;
    }
    observe_after_hours = GetPrivateProfileIntW(L"bench", L"observe_after_hours", 24, ini.c_str());
    if (observe_after_hours < 1 || observe_after_hours > 24 * 365) observe_after_hours = 24;

    unsigned exe_stamp = 0, exe_size = 0;
    const char* exe_build = hoi4build::detect(&exe_stamp, &exe_size);
    if (!exe_build) {
        log("REFUSED: unknown hoi4.exe build (stamp %08x, image size %x); no bytes changed",
            exe_stamp, exe_size);
        return 5;
    }
    log("hoi4.exe build %s", exe_build);
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (std::memcmp(reinterpret_cast<void*>(image + tick_site_rva), tick_bytes,
            sizeof(tick_bytes)) != 0 ||
        std::memcmp(reinterpret_cast<void*>(image + observe_rva), observe_prologue,
            sizeof(observe_prologue)) != 0) {
        log("REFUSED: code bytes differ (other game version?); no bytes changed");
        return 4;
    }
    original_tick = reinterpret_cast<TickFn>(image + tick_function_rva);
    char error[160]{};
    speedpatch::CallPatch tick;
    if (!speedpatch::prepare_call_bytes(
            reinterpret_cast<std::uint8_t*>(image + tick_call_rva), tick_bytes + 11, 5,
            reinterpret_cast<const void*>(&tick_hook), tick, error, sizeof(error)) ||
        !speedpatch::commit_call(tick, error, sizeof(error))) {
        speedpatch::discard_call(tick);
        log("REFUSED: %s", error);
        return 5;
    }
    log("initialized version=0.1.0 observe=on observe_after_hours=%ld",
        static_cast<long>(observe_after_hours));
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
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
