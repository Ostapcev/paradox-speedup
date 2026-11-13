// EU4 Render Patch 0.6.1 experimental, for EU4 1.37.5.0 Windows x64.
//
// Long ticks (the daily/monthly country and province loops) call 140769ea0
// every few countries; it draws a full frame (1414c25a0) when more than
// scale * table[speed] has passed since the previous such frame. The table
// (rdata 141c33e18, speeds 1..5) is read by that gate only. Raising it draws
// fewer frames inside a tick; the game logic is not touched.
//
// The main loop (ingameidler 1408137c0) runs one logic step and then draws
// the scene (1408183b0) every iteration; at speed 5 that is about one frame
// per game day. With frame_interval > 0 a scene draw is skipped while days
// come faster than frame_interval and the last drawn frame is younger than
// frame_interval. Paused or slow games draw every frame as before.
//
// Modes (eu4_render_patch.ini, [render] mode=):
//   count    - only count in-tick frames and their time
//   throttle - count, and raise table entries to tick_interval (ini, float)
//   disabled - change nothing (default)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <string>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace {

enum class Mode { count, throttle, disabled };

// movss xmm6,[scale]; mulss xmm6,[r13+rax*4+table]
constexpr std::uintptr_t gate_rva = 0x769f59;
constexpr std::uint8_t gate_bytes[] = {0xf3, 0x0f, 0x10, 0x35, 0x0f, 0x11, 0xcf, 0x01, 0xf3,
    0x41, 0x0f, 0x59, 0xb4, 0x85, 0x18, 0x3e, 0xc3, 0x01};
// xor edx,edx; mov rcx,rdi; call 1414c25a0
constexpr std::uintptr_t draw_site_rva = 0x769fad;
constexpr std::uint8_t draw_bytes[] = {0x33, 0xd2, 0x48, 0x8b, 0xcf, 0xe8, 0xe9, 0x85, 0xd5,
    0x00};
constexpr std::uintptr_t draw_call_rva = 0x769fb2;
constexpr std::uintptr_t draw_function_rva = 0x14c25a0;
constexpr std::uintptr_t table_rva = 0x1c33e18;
constexpr std::uintptr_t scale_rva = 0x245b070;  // set by the game at runtime
// mov rcx,r13; call 1408183b0 (scene draw in the main loop)
constexpr std::uintptr_t scene_site_rva = 0x815919;
constexpr std::uint8_t scene_bytes[] = {0x49, 0x8b, 0xcd, 0xe8, 0x8f, 0x2a, 0x00, 0x00};
constexpr std::uintptr_t scene_call_rva = 0x81591c;
constexpr std::uintptr_t scene_function_rva = 0x8183b0;
// mov rcx,rsi; call 140758430 (daily tick)
constexpr std::uintptr_t tick_site_rva = 0xb795d7;
constexpr std::uint8_t tick_bytes[] = {0x48, 0x8b, 0xce, 0xe8, 0x51, 0xee, 0xbd, 0xff};
constexpr std::uintptr_t tick_call_rva = 0xb795da;
constexpr std::uintptr_t tick_function_rva = 0x758430;
constexpr float expected_table[5] = {0.02f, 0.02f, 0.02f, 0.02f, 0.05f};

using DrawFn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);

DrawFn original_draw = nullptr;
std::uintptr_t image = 0;
std::atomic<LONG> installation_status{0};
std::wstring log_path;
std::mutex log_mutex;
std::int64_t qpc_frequency = 1;
std::atomic<std::uint64_t> draws{0};
std::atomic<std::uint64_t> draw_ticks{0};

using SceneFn = void (*)(void*);
using TickFn = std::uint64_t (*)(void*);
SceneFn original_scene = nullptr;
TickFn original_tick = nullptr;
std::int64_t frame_interval_ticks = 0;  // 0 = never skip
// Main thread only.
std::int64_t last_scene = 0;
std::int64_t last_day = 0;
std::int64_t last_day_gap = 1LL << 60;
std::atomic<std::uint64_t> scenes_drawn{0};
std::atomic<std::uint64_t> scenes_skipped{0};
std::atomic<std::uint64_t> scene_qpc{0};
std::atomic<std::uint64_t> days{0};

inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

void* last_state = nullptr;  // gamestate passed to the daily tick (main thread)

std::uint64_t tick_hook(void* state) {
    last_state = state;
    const auto now = qpc_now();
    if (last_day) last_day_gap = now - last_day;
    last_day = now;
    days.fetch_add(1, std::memory_order_relaxed);
    return original_tick(state);
}

// running_frame_interval > 0: a frame cap while days keep coming at any pace (slow
// modded games, where a day takes longer than frame_interval and the rule above never
// applies). "Running" = the last day started less than 2x the last day gap ago
// (at least running_min, at most running_max); a paused game stops producing days and
// returns to every frame within that window.
std::int64_t running_interval_ticks = 0;
std::int64_t running_min_ticks = 0, running_max_ticks = 0;
std::atomic<std::uint64_t> scenes_capped{0};

bool game_running(std::int64_t now) noexcept {
    // Speeds 1-4 are timer-bound (2/1/0.5/0.2 s per day, table 141c33e30): fewer frames
    // there gain no game time, so the cap applies at speed 5 (index 4 at gamestate+0x1e08,
    // written by 1407579f0) only.
    if (!last_day || !last_state) return false;
    if (*reinterpret_cast<const volatile std::int32_t*>(
            reinterpret_cast<std::uintptr_t>(last_state) + 0x1e08) != 4) {
        return false;
    }
    std::int64_t window = last_day_gap * 2;
    if (window < running_min_ticks) window = running_min_ticks;
    if (window > running_max_ticks) window = running_max_ticks;
    return now - last_day < window;
}

bool frame_skippable(std::int64_t now) noexcept {
    if (frame_interval_ticks > 0 && now - last_scene < frame_interval_ticks &&
        last_day_gap < frame_interval_ticks && now - last_day < frame_interval_ticks) {
        return true;
    }
    if (running_interval_ticks > 0 && now - last_scene < running_interval_ticks &&
        game_running(now)) {
        scenes_capped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// Map graphics update (1410a6d00) runs before the scene draw in the same loop
// pass; when skip_map_update=1 it takes the decision for that pass, and the
// skipped frame time is handed to the next update so animations keep pace.
using MapFn = std::uint64_t (*)(void*, void*, float, void*);
MapFn original_map = nullptr;
bool skip_map_update = false;
int pass_decision = 0;  // 0 none, 1 skip, 2 draw (main thread only)
float pending_dt = 0.0f;
std::atomic<std::uint64_t> maps_skipped{0};

std::uint64_t map_hook(void* view, void* unused, float dt, void* idler) {
    const auto now = qpc_now();
    if (frame_skippable(now) && pending_dt < 1.0f) {
        pass_decision = 1;
        pending_dt += dt;
        maps_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    pass_decision = 2;
    const float total = dt + pending_dt;
    pending_dt = 0.0f;
    return original_map(view, unused, total, idler);
}

void scene_hook(void* idler) {
    const auto now = qpc_now();
    const int decision = pass_decision;
    pass_decision = 0;
    if (decision == 1 || (decision == 0 && frame_skippable(now))) {
        scenes_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    original_scene(idler);
    last_scene = now;
    scenes_drawn.fetch_add(1, std::memory_order_relaxed);
    scene_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// Top-bar alerts (140df5860), player only, every main loop pass (~3.6% of the
// main thread as Russia). The function first animates the alert icons (slide by
// frame dt, cheap), then walks 114 alerts: an alert already shown is re-checked
// every call, a hidden one only when it is the round-robin index (+0x4af8, one
// per call). The checks are the expensive part.
// alert_interval > 0: the function still runs every pass (smooth animation, new
// alerts found at the normal round-robin pace), but shown alerts are re-checked
// at most that often. Done by a flag tested at 0x140df5ed2 (`imul rax,r15,0xa8`
// after the round-robin test): when set, a shown, non-round-robin alert takes the
// game's own "not shown" branch 0x140df5ee1.
// mov rcx,[r13+0x1400]; call 140df5860
using AlertFn = void (*)(void*);
AlertFn original_alert = nullptr;
std::int64_t alert_interval_ticks = 0;
std::int64_t last_alert = 0;  // main thread only
volatile std::uint8_t alert_recheck_skip = 0;
std::atomic<std::uint64_t> alerts_run{0}, alerts_skipped{0}, alert_qpc{0}, alert_light_qpc{0};

void alert_hook(void* manager) {
    const auto now = qpc_now();
    // only while the game runs at speed 5: on pause and slow speeds fewer checks gain no game time
    if (alert_interval_ticks > 0 && last_alert && now - last_alert < alert_interval_ticks &&
        game_running(now)) {
        alert_recheck_skip = 1;
        original_alert(manager);
        alert_recheck_skip = 0;
        alerts_skipped.fetch_add(1, std::memory_order_relaxed);
        alert_light_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now),
            std::memory_order_relaxed);
        return;
    }
    original_alert(manager);
    last_alert = now;
    alerts_run.fetch_add(1, std::memory_order_relaxed);
    alert_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

// ---- Console command fps_render ----------------------------------------------
// 141721320(console, result, args) runs one console command for every caller (the
// console window, map-click debug commands, queued commands in the daily tick).
// args is a std::vector<std::string>, args[0] the command name. The result is
// {bool ok; std::string text} (built like 140eb02d0); the console prints text.
// The hook takes "fps_render" itself and hands everything else to the game.
struct MsvcString {
    union {
        char buffer[16];
        char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;
};
struct StringVector {
    MsvcString* first;
    MsvcString* last;
    MsvcString* end;
};
using ConsoleFn = void* (*)(void*, void*, void*);
using AssignFn = void* (*)(void*, const char*, std::size_t);  // std::string::assign
constexpr std::uintptr_t console_rva = 0x1721320;
constexpr std::uint8_t console_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10};
constexpr std::uintptr_t assign_rva = 0x95110;
constexpr std::uint8_t assign_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
    0x24, 0x18, 0x56, 0x57};
constexpr int max_fps = 240;
ConsoleFn original_console = nullptr;
AssignFn game_assign = nullptr;
std::wstring ini_path;

const char* text_of(const MsvcString& s) noexcept {
    return s.capacity > 15 ? s.pointer : s.buffer;
}

void* set_result(void* result, bool ok, const char* text) {
    auto* bytes = static_cast<std::uint8_t*>(result);
    bytes[0] = ok ? 1 : 0;
    auto* s = reinterpret_cast<MsvcString*>(bytes + 8);
    std::memset(s, 0, sizeof(*s));
    s->capacity = 15;
    game_assign(s, text, std::strlen(text));  // the game's allocator owns the text
    return result;
}

void log(const char* format, ...) noexcept;

// Replaces the value of `key=` lines in the ini, byte for byte elsewhere (the file is
// UTF-8 with Russian comments, which the Win32 profile writer would re-encode).
bool save_ini_value(const char* key, const char* value) {
    HANDLE file = CreateFileW(ini_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::string text;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got) text.append(chunk, got);
    CloseHandle(file);
    const std::string prefix = std::string(key) + "=";
    bool found = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, prefix.size(), prefix) == 0) {
            std::size_t value_end = end;
            if (value_end > at && text[value_end - 1] == '\r') --value_end;
            const std::string line = prefix + value;
            text.replace(at, value_end - at, line);
            end = at + line.size() + (end - value_end);
            found = true;
        }
        at = end + 1;
    }
    if (!found) {
        if (!text.empty() && text.back() != '\n') text += "\r\n";
        text += prefix + value + "\r\n";
    }
    file = CreateFileW(ini_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
        nullptr) && written == text.size();
    CloseHandle(file);
    return ok;
}

int current_fps(std::int64_t ticks) noexcept {
    return ticks > 0 ? static_cast<int>(static_cast<double>(qpc_frequency) /
        static_cast<double>(ticks) + 0.5) : 0;
}

void* fps_render_command(void* result, const StringVector* args) {
    char text[256];
    const auto count = args->last - args->first;
    if (count < 2) {
        const int frame = current_fps(frame_interval_ticks);
        const int running = current_fps(running_interval_ticks);
        if (!frame && !running) {
            std::snprintf(text, sizeof(text),
                "fps_render: no cap (every frame is drawn). Usage: fps_render <1-%d>, 0 = no cap",
                max_fps);
        } else if (frame == running || !frame || !running) {
            std::snprintf(text, sizeof(text),
                "fps_render: %d FPS while the game runs fast; paused or slow speeds draw every "
                "frame. Usage: fps_render <1-%d>, 0 = no cap", frame ? frame : running, max_fps);
        } else {
            std::snprintf(text, sizeof(text),
                "fps_render: %d FPS when days come faster than a frame, %d FPS at speed 5. "
                "Usage: fps_render <1-%d>, 0 = no cap", frame, running, max_fps);
        }
        return set_result(result, true, text);
    }
    const char* word = text_of(args->first[1]);
    char* end = nullptr;
    long fps = std::strtol(word, &end, 10);
    if (_stricmp(word, "off") == 0) {
        fps = 0;
    } else if (end == word || *end != '\0' || fps < 0 || fps > max_fps) {
        std::snprintf(text, sizeof(text), "fps_render: expected a number 0-%d (0 = no cap)",
            max_fps);
        return set_result(result, false, text);
    }
    const double interval = fps > 0 ? 1.0 / static_cast<double>(fps) : 0.0;
    const auto ticks = static_cast<std::int64_t>(interval * static_cast<double>(qpc_frequency));
    // only the main-loop cap: running_frame_interval (any day length) made speed 5 choppy
    frame_interval_ticks = ticks;
    char value[32];
    std::snprintf(value, sizeof(value), "%.6g", interval);
    const bool saved = save_ini_value("frame_interval", value);
    log("console fps_render %ld (interval %s) saved=%d", fps, value, saved ? 1 : 0);
    if (fps > 0) {
        std::snprintf(text, sizeof(text),
            "fps_render: %ld FPS while the game runs fast (paused: every frame)%s", fps,
            saved ? "" : "; could not save to eu4_render_patch.ini");
    } else {
        std::snprintf(text, sizeof(text), "fps_render: no cap%s",
            saved ? "" : "; could not save to eu4_render_patch.ini");
    }
    return set_result(result, true, text);
}

void* console_hook(void* console, void* result, void* raw_args) {
    const auto* args = static_cast<const StringVector*>(raw_args);
    if (args && args->first && args->last > args->first && args->first[0].size == 10 &&
        _strnicmp(text_of(args->first[0]), "fps_render", 10) == 0) {
        return fps_render_command(result, args);
    }
    return original_console(console, result, raw_args);
}

std::uint8_t* allocate_stub_near(std::uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto step = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = target & ~(step - 1);
    for (std::uintptr_t delta = step; delta < 0x70000000; delta += step) {
        if (delta < origin) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(origin - delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(p);
            }
        }
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(origin + delta), 4096,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            return static_cast<std::uint8_t*>(p);
        }
    }
    return nullptr;
}

// Returns false (and changes nothing) if the bytes differ or memory is unavailable.
bool install_alert_recheck_gate(char* error, std::size_t error_size) {
    // 140df5ec8: cmp ebx,[r12+0x4af8]; je +0x20; imul rax,r15,0xa8;
    //            cmp qword [rax+rdi+0x20],0; jne +0x11; lea rcx,[rbp+0xc60]; call; jmp
    constexpr std::uint8_t expected[] = {0x41, 0x3b, 0x9c, 0x24, 0xf8, 0x4a, 0x00, 0x00, 0x74,
        0x20, 0x49, 0x69, 0xc7, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x83, 0x7c, 0x38, 0x20, 0x00, 0x75,
        0x11, 0x48, 0x8d, 0x8d, 0x60, 0x0c, 0x00, 0x00};
    if (!eu4build::code_matches(image, 0xdf5ec8, expected, sizeof(expected))) {
        std::snprintf(error, error_size, "alert loop bytes differ");
        return false;
    }
    auto* site = reinterpret_cast<std::uint8_t*>(image + eu4build::rva(0xdf5ed2));
    const auto resume = image + eu4build::rva(0xdf5ed9);  // cmp qword [rax+rdi+0x20],0
    const auto not_shown = image + eu4build::rva(0xdf5ee1);  // lea rcx,[rbp+0xc60]; call; jmp next alert
    auto* stub = allocate_stub_near(reinterpret_cast<std::uintptr_t>(site));
    if (!stub) {
        std::snprintf(error, error_size, "no memory near the alert loop");
        return false;
    }
    std::uint8_t code[64]{};
    int n = 0;
    auto put = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) code[n++] = b;
    };
    auto put64 = [&](std::uint64_t v) {
        std::memcpy(code + n, &v, 8);
        n += 8;
    };
    put({0x48, 0xb8});  // mov rax, &alert_recheck_skip
    put64(reinterpret_cast<std::uint64_t>(&alert_recheck_skip));
    put({0x80, 0x38, 0x00});  // cmp byte [rax],0
    put({0x75, 0x15});  // jne skip (imul 7 + jmp 14)
    put({0x49, 0x69, 0xc7, 0xa8, 0x00, 0x00, 0x00});  // imul rax,r15,0xa8
    put({0xff, 0x25, 0, 0, 0, 0});  // jmp [rip]
    put64(resume);
    put({0xff, 0x25, 0, 0, 0, 0});  // skip: jmp [rip]
    put64(not_shown);
    std::memcpy(stub, code, static_cast<std::size_t>(n));
    DWORD old = 0;
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        std::snprintf(error, error_size, "cannot make the alert stub executable");
        return false;
    }
    const auto rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site) + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        VirtualFree(stub, 0, MEM_RELEASE);
        std::snprintf(error, error_size, "alert stub out of rel32 range");
        return false;
    }
    std::uint8_t jump[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(jump + 1, &rel32, 4);
    if (!VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        std::snprintf(error, error_size, "cannot unprotect the alert loop");
        return false;
    }
    std::memcpy(site, jump, sizeof(jump));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(jump), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    return true;
}


// ---- Map icons over provinces (build menu, trade, sieges...) -------------------
// 1410a6d00 (map update) -> 1410a7a90 walks every province and calls 141321880,
// which places/fills that province's icon widgets each frame (8-9% of a paused
// frame with the build menu open). The widgets are persistent GUI objects, so a
// skipped update leaves them where they were.
// map_icons_interval > 0: while the game runs at speed 5, run the per-province pass at most
// that often, but at once when the map camera matrix changes or a mouse button is down.
// map_icons_hide: icon kinds to remove; 14132e300 called with a null anchor
// destroys the icon (the game's own path for icons that should not exist).
using IconPassFn = void (*)(void*, std::uint64_t, std::uint64_t);
using IconProvinceFn = void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using IconPlaceFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t);
IconPassFn original_icon_pass = nullptr;
IconProvinceFn original_icon_province = nullptr;
IconPlaceFn original_icon_place = nullptr;
std::int64_t icon_interval_ticks = 0;
std::int64_t last_icon_pass = 0;  // main thread only
bool icon_pass_run = true;
bool icon_force_next = true;
constexpr int icon_camera_slots = 8;
std::uint64_t icon_camera_owner[icon_camera_slots]{};
unsigned char icon_camera[icon_camera_slots][64]{};
std::uint64_t icon_camera_next = 0;
std::atomic<std::uint64_t> icon_passes_run{0}, icon_passes_skipped{0}, icon_camera_runs{0},
    icon_qpc{0}, icons_hidden{0};
constexpr int max_kinds = 48;
struct KindFilter {
    char kind[max_kinds][48]{};
    int count = 0;
    bool all = false;
    bool active() const noexcept { return all || count > 0; }
};
KindFilter hide_filter;  // map_icons_hide: whole icon removed
KindFilter text_filter;  // map_icons_text_hide: text boxes inside the icon not drawn

// Text boxes of instantTextBox (vtable 141da1d40, draw = slot +0x58 1415f3f20) that
// belong to map icons of text_filter kinds. Filled when an icon is placed, emptied
// at the start of every icon pass that runs (generation), main thread only.
constexpr std::uintptr_t textbox_vtable_rva = 0x1da1d40;
constexpr std::uintptr_t textbox_draw_slot_rva = 0x1da1d98;
constexpr std::uintptr_t textbox_draw_rva = 0x15f3f20;
using TextDrawFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t);
TextDrawFn original_text_draw = nullptr;
constexpr std::uint32_t text_set_size = 8192;  // power of two
struct TextMark {
    std::uint64_t box;
    std::uint32_t generation;
};
TextMark text_set[text_set_size]{};
std::uint32_t text_generation = 1;
std::uint32_t text_marked_now = 0;
std::atomic<std::uint64_t> texts_skipped{0}, texts_marked{0}, text_set_full{0};

inline std::uint32_t text_slot(std::uint64_t box) noexcept {
    return static_cast<std::uint32_t>((box >> 4) * 0x9e3779b97f4a7c15ull >> 40) &
        (text_set_size - 1);
}

bool text_marked(std::uint64_t box) noexcept {
    for (std::uint32_t i = text_slot(box), n = 0; n < 32; ++n, i = (i + 1) & (text_set_size - 1)) {
        const TextMark& m = text_set[i];
        if (m.generation != text_generation) return false;
        if (m.box == box) return true;
    }
    return false;
}

void text_mark(std::uint64_t box) noexcept {
    if (text_marked_now > text_set_size / 2) {
        text_set_full.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (std::uint32_t i = text_slot(box), n = 0; n < 32; ++n, i = (i + 1) & (text_set_size - 1)) {
        TextMark& m = text_set[i];
        if (m.generation != text_generation) {
            m.box = box;
            m.generation = text_generation;
            ++text_marked_now;
            texts_marked.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (m.box == box) return;
    }
    text_set_full.fetch_add(1, std::memory_order_relaxed);
}

void text_clear() noexcept {
    if (++text_generation == 0) {  // wrapped: really clear
        std::memset(text_set, 0, sizeof(text_set));
        text_generation = 1;
    }
    text_marked_now = 0;
}

// Text boxes are found the way the game fills icons (14122c170): the icon window is
// holder+8; its sub-window "big" comes from window->vtbl[+0x1c0](std::string*, 1);
// a text box widget by name from window->vtbl[+0x98](const char*); its render
// object is at widget+0x118 and is marked only if its vtable is the text one. Names: instantTextBoxType entries
// of interface/macrobuild_mapicons.gui and mapicons.gui.
const char* const icon_text_names[] = {"autonomy", "building_cost", "building_info",
    "building_slots", "construction_info", "cost", "influence", "info_left", "info_right",
    "manpower", "production", "queue_info", "sailor", "tax", "unit_info", "unrest",
    "#fortlevel", "#suppley_limit", "#tax_value", "attacker_unit_strength", "branch_income",
    "current_loot", "defender_unit_strength", "siege_progress", "total_value",
    "trade_ship_count", "unit_strength"};
struct MsvcSmallString {
    char buffer[16];
    std::uint64_t size;
    std::uint64_t capacity;
};
using FindChildWindowFn = std::uint64_t (*)(std::uint64_t, MsvcSmallString*, std::uint64_t);
using FindTextFn = std::uint64_t (*)(std::uint64_t, const char*);

void mark_texts_in(std::uint64_t window) noexcept {
    const auto vtable = *reinterpret_cast<const std::uint64_t*>(window);
    const auto find_text = *reinterpret_cast<const FindTextFn*>(vtable + 0x98);
    const auto box_vtable = image + eu4build::rva(textbox_vtable_rva);
    for (const char* name : icon_text_names) {
        // the widget found by name (vtable 141d8fd10) keeps its text render object at +0x118;
        // that object (vtable 141da1d40) is what 1414dd350 draws
        const auto widget = find_text(window, name);
        if (!widget) continue;
        const auto box = *reinterpret_cast<const std::uint64_t*>(widget + 0x118);
        if (box && *reinterpret_cast<const std::uint64_t*>(box) == box_vtable) text_mark(box);
    }
}

void mark_icon_texts(std::uint64_t window) noexcept {
    mark_texts_in(window);
    MsvcSmallString big{};
    std::memcpy(big.buffer, "big", 4);
    big.size = 3;
    big.capacity = 15;
    const auto vtable = *reinterpret_cast<const std::uint64_t*>(window);
    const auto find_window = *reinterpret_cast<const FindChildWindowFn*>(vtable + 0x1c0);
    const auto child = find_window(window, &big, 1);
    if (child && child != window) mark_texts_in(child);
}

std::uint64_t text_draw_hook(std::uint64_t box, std::uint64_t a, std::uint64_t b,
    std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f) {
    if (text_marked(box)) {
        texts_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return original_text_draw(box, a, b, c, d, e, f);
}

void icon_pass_hook(void* map, std::uint64_t a, std::uint64_t b) {
    const auto now = qpc_now();
    const bool mouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    // Throttled only while the game runs at speed 5 (game_running). On pause and at slow speeds
    // every pass runs as in the game: fewer passes there gain no game time, and the skipped
    // passes made movement arrows and their animation update twice a second.
    icon_pass_run = icon_interval_ticks <= 0 || icon_force_next || mouse ||
        now - last_icon_pass >= icon_interval_ticks || !game_running(now);
    icon_force_next = mouse;  // one more full pass after the button is released
    if (icon_pass_run) {
        if (original_text_draw) text_clear();
        last_icon_pass = now;
        icon_passes_run.fetch_add(1, std::memory_order_relaxed);
    } else {
        icon_passes_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    original_icon_pass(map, a, b);
    icon_qpc.fetch_add(static_cast<std::uint64_t>(qpc_now() - now), std::memory_order_relaxed);
}

void icon_province_hook(std::uint64_t p1, std::uint64_t p2, std::uint64_t p3, std::uint64_t p4,
    std::uint64_t p5, std::uint64_t p6, std::uint64_t p7, std::uint64_t p8, std::uint64_t p9,
    std::uint64_t p10, std::uint64_t p11, std::uint64_t camera, std::uint64_t p13,
    std::uint64_t p14) {
    if (camera) {
        // provinces use one of a few view objects (map wrap copies); keep one matrix each
        int slot = 0;
        while (slot < icon_camera_slots && icon_camera_owner[slot] != camera) ++slot;
        if (slot == icon_camera_slots) {
            slot = static_cast<int>(icon_camera_next++ % icon_camera_slots);
            icon_camera_owner[slot] = camera;
            std::memset(icon_camera[slot], 0, sizeof(icon_camera[slot]));
        }
        const auto* matrix = reinterpret_cast<const unsigned char*>(camera + 0x80);
        if (std::memcmp(matrix, icon_camera[slot], sizeof(icon_camera[slot])) != 0) {
            std::memcpy(icon_camera[slot], matrix, sizeof(icon_camera[slot]));
            if (!icon_pass_run) {
                icon_pass_run = true;  // rest of this pass
                icon_force_next = true;  // provinces already skipped in this pass
                icon_camera_runs.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!icon_pass_run) return;
    original_icon_province(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, camera, p13, p14);
}

bool kind_matches(const KindFilter& filter, std::uint64_t name_string) noexcept {
    if (filter.all) return true;
    if (!name_string || filter.count == 0) return false;
    const auto* s = reinterpret_cast<const std::uint8_t*>(name_string);
    const auto size = *reinterpret_cast<const std::uint64_t*>(s + 0x10);
    const auto capacity = *reinterpret_cast<const std::uint64_t*>(s + 0x18);
    const char* text = capacity > 15 ? *reinterpret_cast<const char* const*>(s) :
        reinterpret_cast<const char*>(s);
    for (int i = 0; i < filter.count; ++i) {
        if (std::strlen(filter.kind[i]) == size && std::memcmp(filter.kind[i], text, size) == 0) {
            return true;
        }
    }
    return false;
}

std::uint64_t icon_place_hook(std::uint64_t p1, std::uint64_t p2, std::uint64_t slot,
    std::uint64_t anchor, std::uint64_t p5, std::uint64_t name, std::uint64_t p7,
    std::uint64_t p8, std::uint64_t p9, std::uint64_t p10, std::uint64_t p11, std::uint64_t p12,
    std::uint64_t p13) {
    if (anchor && kind_matches(hide_filter, name)) {
        anchor = 0;
        icons_hidden.fetch_add(1, std::memory_order_relaxed);
    }
    const auto placed = original_icon_place(p1, p2, slot, anchor, p5, name, p7, p8, p9, p10, p11,
        p12, p13);
    if (original_text_draw && slot && static_cast<std::uint8_t>(placed) != 0 &&
        kind_matches(text_filter, name)) {
        const auto holder = *reinterpret_cast<const std::uint64_t*>(slot);
        const auto window = holder ? *reinterpret_cast<const std::uint64_t*>(holder + 8) : 0;
        if (window) mark_icon_texts(window);
    }
    return placed;
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

std::uint64_t draw_hook(void* a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    const auto value = original_draw(a, b, c, d);
    QueryPerformanceCounter(&end);
    draws.fetch_add(1, std::memory_order_relaxed);
    draw_ticks.fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart),
        std::memory_order_relaxed);
    return value;
}

DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        const auto n = draws.load();
        const auto ms = draw_ticks.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency);
        const float* table = reinterpret_cast<const float*>(image + eu4build::rva(table_rva));
        log("stats in_tick_frames=%llu in_tick_ms=%llu avg_us=%llu scale=%g table=%g,%g,%g,%g,%g",
            static_cast<unsigned long long>(n), static_cast<unsigned long long>(ms),
            static_cast<unsigned long long>(n ? ms * 1000 / n : 0),
            static_cast<double>(*reinterpret_cast<const float*>(image + eu4build::rva(scale_rva))),
            static_cast<double>(table[0]), static_cast<double>(table[1]),
            static_cast<double>(table[2]), static_cast<double>(table[3]),
            static_cast<double>(table[4]));
        log("scene days=%llu drawn=%llu skipped=%llu map_skipped=%llu scene_ms=%llu",
            static_cast<unsigned long long>(days.load()),
            static_cast<unsigned long long>(scenes_drawn.load()),
            static_cast<unsigned long long>(scenes_skipped.load()),
            static_cast<unsigned long long>(maps_skipped.load()),
            static_cast<unsigned long long>(
                scene_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency)));
        if (running_interval_ticks > 0) {
            log("scene running_capped=%llu",
                static_cast<unsigned long long>(scenes_capped.load()));
        }
        if (original_alert) {
            log("alerts full=%llu light=%llu full_ms=%llu light_ms=%llu",
                static_cast<unsigned long long>(alerts_run.load()),
                static_cast<unsigned long long>(alerts_skipped.load()),
                static_cast<unsigned long long>(
                    alert_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency)),
                static_cast<unsigned long long>(
                    alert_light_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency)));
        }
        if (original_icon_pass || original_icon_place) {
            log("map_icons texts marked=%llu skipped=%llu set_full=%llu",
                static_cast<unsigned long long>(texts_marked.load()),
                static_cast<unsigned long long>(texts_skipped.load()),
                static_cast<unsigned long long>(text_set_full.load()));
            log("map_icons passes_run=%llu skipped=%llu camera_runs=%llu pass_ms=%llu hidden=%llu",
                static_cast<unsigned long long>(icon_passes_run.load()),
                static_cast<unsigned long long>(icon_passes_skipped.load()),
                static_cast<unsigned long long>(icon_camera_runs.load()),
                static_cast<unsigned long long>(
                    icon_qpc.load() * 1000 / static_cast<std::uint64_t>(qpc_frequency)),
                static_cast<unsigned long long>(icons_hidden.load()));
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
    log_path = directory + L"\\eu4_render_patch.log";
    const auto ini = directory + L"\\eu4_render_patch.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
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

    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (eu4build::detect() == eu4build::Build::unknown) {
        log("REFUSED: unknown eu4.exe build (needs Steam or GOG 1.37.5); no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    if (!eu4build::code_matches(image, gate_rva, gate_bytes, sizeof(gate_bytes)) ||
        !eu4build::code_matches(image, draw_site_rva, draw_bytes, sizeof(draw_bytes)) ||
        std::memcmp(reinterpret_cast<void*>(image + eu4build::rva(table_rva)), expected_table,
            sizeof(expected_table)) != 0 ||
        !eu4build::code_matches(image, scene_site_rva, scene_bytes, sizeof(scene_bytes)) ||
        !eu4build::code_matches(image, tick_site_rva, tick_bytes, sizeof(tick_bytes))) {
        log("REFUSED: bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    // set always: fps_render can switch the running cap on at runtime
    running_min_ticks = qpc_frequency / 4;  // 0.25 s
    running_max_ticks = qpc_frequency * 2;  // 2 s
    ini_path = ini;

    char error[160]{};
    speedpatch::CallPatch draw;
    original_draw = reinterpret_cast<DrawFn>(image + eu4build::rva(draw_function_rva));
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(draw_call_rva)),
            reinterpret_cast<const void*>(original_draw),
            reinterpret_cast<const void*>(&draw_hook), draw, error, sizeof(error)) ||
        !speedpatch::commit_call(draw, error, sizeof(error))) {
        speedpatch::discard_call(draw);
        log("REFUSED: %s", error);
        installation_status.store(-5);
        return 5;
    }

    speedpatch::CallPatch scene;
    speedpatch::CallPatch tick;
    original_scene = reinterpret_cast<SceneFn>(image + eu4build::rva(scene_function_rva));
    original_tick = reinterpret_cast<TickFn>(image + eu4build::rva(tick_function_rva));
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(scene_call_rva)),
            reinterpret_cast<const void*>(original_scene),
            reinterpret_cast<const void*>(&scene_hook), scene, error, sizeof(error)) ||
        !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(tick_call_rva)),
            reinterpret_cast<const void*>(original_tick),
            reinterpret_cast<const void*>(&tick_hook), tick, error, sizeof(error)) ||
        !speedpatch::commit_call(tick, error, sizeof(error)) ||
        !speedpatch::commit_call(scene, error, sizeof(error))) {
        speedpatch::discard_call(scene);
        speedpatch::discard_call(tick);
        log("REFUSED: %s (in-tick frame hook stays)", error);
        installation_status.store(-8);
        return 8;
    }
    float frame_interval = 0.0f;
    if (mode == Mode::throttle) {
        GetPrivateProfileStringW(L"render", L"frame_interval", L"0", value, 32, ini.c_str());
        frame_interval = std::wcstof(value, nullptr);
        if (!(frame_interval > 0.0f && frame_interval <= 2.0f)) frame_interval = 0.0f;
        frame_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(frame_interval) * static_cast<double>(qpc_frequency));
        GetPrivateProfileStringW(L"render", L"running_frame_interval", L"0", value, 32,
            ini.c_str());
        const float running = std::wcstof(value, nullptr);
        if (running > 0.0f && running <= 1.0f) {
            running_interval_ticks = static_cast<std::int64_t>(
                static_cast<double>(running) * static_cast<double>(qpc_frequency));
        }
        // the map hook does nothing while both intervals are 0 (frame_skippable is false)
        skip_map_update =
            GetPrivateProfileIntW(L"render", L"skip_map_update", 0, ini.c_str()) != 0;
    }
    {
        speedpatch::CallPatch console;
        void* trampoline = nullptr;
        game_assign = reinterpret_cast<AssignFn>(image + eu4build::rva(assign_rva));
        if (!eu4build::code_matches(image, assign_rva, assign_prologue, sizeof(assign_prologue))) {
            log("console fps_render REFUSED (string assign bytes differ)");
        } else if (!speedpatch::prepare_entry(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(console_rva)),
                       console_prologue, reinterpret_cast<const void*>(&console_hook), console,
                       &trampoline, error, sizeof(error))) {
            log("console fps_render REFUSED (%s)", error);
        } else {
            original_console = reinterpret_cast<ConsoleFn>(trampoline);
            if (!speedpatch::commit_call(console, error, sizeof(error))) {
                speedpatch::discard_call(console);
                original_console = nullptr;
                log("console fps_render REFUSED (%s)", error);
            } else {
                log("console command fps_render installed");
            }
        }
    }

    if (skip_map_update) {
        // mov rcx,[rax+0x360]; call 1410a6d00 (map graphics update in the main loop)
        constexpr std::uint8_t map_bytes[] = {0x48, 0x8b, 0x88, 0x60, 0x03, 0x00, 0x00, 0xe8,
            0xd2, 0x14, 0x89, 0x00};
        speedpatch::CallPatch map;
        original_map = reinterpret_cast<MapFn>(image + eu4build::rva(0x10a6d00));
        if (!eu4build::code_matches(image, 0x815822, map_bytes, sizeof(map_bytes)) ||
            !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(0x815829)),
                reinterpret_cast<const void*>(original_map),
                reinterpret_cast<const void*>(&map_hook), map, error, sizeof(error)) ||
            !speedpatch::commit_call(map, error, sizeof(error))) {
            speedpatch::discard_call(map);
            skip_map_update = false;
            log("skip_map_update REFUSED (%s); scene throttle stays", error);
        }
    }

    {
        constexpr std::uint8_t alert_bytes[] = {0x49, 0x8b, 0x8d, 0x00, 0x14, 0x00, 0x00, 0xe8,
            0x92, 0x04, 0x5e, 0x00};
        GetPrivateProfileStringW(L"render", L"alert_interval", L"0", value, 32, ini.c_str());
        float alert_interval = std::wcstof(value, nullptr);
        if (!(alert_interval >= 0.0f && alert_interval <= 10.0f)) alert_interval = 0.0f;
        alert_interval_ticks = static_cast<std::int64_t>(
            static_cast<double>(alert_interval) * static_cast<double>(qpc_frequency));
        speedpatch::CallPatch alert;
        original_alert = reinterpret_cast<AlertFn>(image + eu4build::rva(0xdf5860));
        // GOG: same code, but the main-loop object keeps the alert manager at +0x13f0
        constexpr std::uint8_t alert_load_gog[] = {0x49, 0x8b, 0x8d, 0xf0, 0x13, 0x00, 0x00};
        const bool alert_site_ok = eu4build::detect() == eu4build::Build::gog
            ? eu4build::rva(0x8153c9) != 0 &&
                std::memcmp(reinterpret_cast<const void*>(image + eu4build::rva(0x8153c9) - 7),
                    alert_load_gog, sizeof(alert_load_gog)) == 0 &&
                eu4build::code_matches(image, 0x8153c9, alert_bytes + 7, 5)
            : eu4build::code_matches(image, 0x8153c2, alert_bytes, sizeof(alert_bytes));
        if (!alert_site_ok ||
            !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(0x8153c9)),
                reinterpret_cast<const void*>(original_alert),
                reinterpret_cast<const void*>(&alert_hook), alert, error, sizeof(error)) ||
            !speedpatch::commit_call(alert, error, sizeof(error))) {
            speedpatch::discard_call(alert);
            original_alert = nullptr;
            alert_interval_ticks = 0;
            log("alert hook REFUSED (%s)", error);
        } else {
            if (alert_interval_ticks > 0 && !install_alert_recheck_gate(error, sizeof(error))) {
                // the hook then only counts; every call runs the full original function
                alert_interval_ticks = 0;
                log("alert recheck gate REFUSED (%s); alerts run as in the original", error);
            }
            log("alert_interval=%g (animation every pass, shown alerts re-checked at this interval)",
                static_cast<double>(alert_interval));
        }
    }

    {
        GetPrivateProfileStringW(L"render", L"map_icons_interval", L"0", value, 32, ini.c_str());
        float icon_interval = std::wcstof(value, nullptr);
        if (!(icon_interval >= 0.0f && icon_interval <= 10.0f)) icon_interval = 0.0f;
        if (icon_interval > 0.0f) {
            icon_interval_ticks = static_cast<std::int64_t>(
                static_cast<double>(icon_interval) * static_cast<double>(qpc_frequency));
            speedpatch::CallPatch pass;
            speedpatch::CallPatch province;
            original_icon_pass = reinterpret_cast<IconPassFn>(image + eu4build::rva(0x10a7a90));
            original_icon_province = reinterpret_cast<IconProvinceFn>(image + eu4build::rva(0x1321880));
            if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(0x10a6d6a)),
                    reinterpret_cast<const void*>(original_icon_pass),
                    reinterpret_cast<const void*>(&icon_pass_hook), pass, error, sizeof(error)) ||
                !speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(0x10a7f82)),
                    reinterpret_cast<const void*>(original_icon_province),
                    reinterpret_cast<const void*>(&icon_province_hook), province, error,
                    sizeof(error)) ||
                !speedpatch::commit_call(province, error, sizeof(error)) ||
                !speedpatch::commit_call(pass, error, sizeof(error))) {
                speedpatch::discard_call(pass);
                speedpatch::discard_call(province);
                original_icon_pass = nullptr;
                log("map_icons_interval REFUSED (%s)", error);
            } else {
                log("map_icons_interval=%g", static_cast<double>(icon_interval));
            }
        }

        auto read_filter = [&](const wchar_t* key, KindFilter& filter) {
            wchar_t list[2048]{};
            GetPrivateProfileStringW(L"render", key, L"", list, 2048, ini.c_str());
            std::wstring names = list;
            if (_wcsicmp(names.c_str(), L"build") == 0) {
                names = L"building_improvements_mapicon,build_unit_mapicon,estates_mapicon,"
                    L"build_template_mapicon,development_mapicon,exploit_mapicon,"
                    L"local_autonomy_mapicon,mutually_exclusive_mapicon";
            }
            if (_wcsicmp(names.c_str(), L"all") == 0) {
                filter.all = true;
                return;
            }
            std::size_t start = 0;
            while (start < names.size() && filter.count < max_kinds) {
                auto end = names.find(L',', start);
                if (end == std::wstring::npos) end = names.size();
                std::string item;
                for (auto i = start; i < end; ++i) {
                    const wchar_t c = names[i];
                    if (c != L' ' && c != L'\t' && c < 128) item.push_back(static_cast<char>(c));
                }
                if (!item.empty() && item.size() < sizeof(filter.kind[0])) {
                    std::memcpy(filter.kind[filter.count++], item.c_str(), item.size() + 1);
                }
                start = end + 1;
            }
        };
        read_filter(L"map_icons_hide", hide_filter);
        read_filter(L"map_icons_text_hide", text_filter);
        if (text_filter.active()) {
            if (!original_icon_pass) {
                text_filter = KindFilter{};
                log("map_icons_text_hide needs map_icons_interval > 0 (icon pass hook); off");
            } else {
                auto* slot_ptr = reinterpret_cast<std::uint64_t*>(image + eu4build::rva(textbox_draw_slot_rva));
                DWORD old = 0;
                if (*slot_ptr != image + eu4build::rva(textbox_draw_rva) ||
                    !VirtualProtect(slot_ptr, 8, PAGE_READWRITE, &old)) {
                    text_filter = KindFilter{};
                    log("map_icons_text_hide REFUSED (text box vtable differs)");
                } else {
                    original_text_draw = reinterpret_cast<TextDrawFn>(image + eu4build::rva(textbox_draw_rva));
                    *slot_ptr = reinterpret_cast<std::uint64_t>(&text_draw_hook);
                    DWORD ignored = 0;
                    VirtualProtect(slot_ptr, 8, old, &ignored);
                }
            }
        }
        if (hide_filter.active() || text_filter.active()) {
            constexpr std::uintptr_t place_sites[] = {0x1328885, 0x13289d1, 0x1328be7, 0x1329fda,
                0x132a2f4, 0x132a6e8, 0x132a9a6, 0x132aef5, 0x132e94a, 0x132ec8a, 0x132ef9a,
                0x132f28f, 0x132f835, 0x132fb4f, 0x132fec4, 0x13303a4, 0x133077d, 0x1330c15,
                0x133157e, 0x1331bd4, 0x1331e47, 0x1332205, 0x133524c, 0x133662f, 0x1337101,
                0x1337989, 0x1337e3f};
            constexpr int site_count = sizeof(place_sites) / sizeof(place_sites[0]);
            speedpatch::CallPatch place[site_count];
            original_icon_place = reinterpret_cast<IconPlaceFn>(image + eu4build::rva(0x132e300));
            bool ok = true;
            for (int i = 0; i < site_count && ok; ++i) {
                ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(place_sites[i])),
                    reinterpret_cast<const void*>(original_icon_place),
                    reinterpret_cast<const void*>(&icon_place_hook), place[i], error,
                    sizeof(error));
            }
            for (int i = 0; i < site_count && ok; ++i) {
                ok = speedpatch::commit_call(place[i], error, sizeof(error));
            }
            if (!ok) {
                for (auto& patch : place) speedpatch::discard_call(patch);
                original_icon_place = nullptr;
                hide_filter = KindFilter{};
                text_filter = KindFilter{};  // nothing gets marked; the draw hook passes through
                log("map_icons_hide/text_hide REFUSED (%s)", error);
            } else {
                log("map_icons_hide=%s kinds=%d map_icons_text_hide=%s kinds=%d",
                    hide_filter.all ? "all" : "list", hide_filter.count,
                    text_filter.all ? "all" : "list", text_filter.count);
            }
        }
    }

    float interval = 0.0f;
    if (mode == Mode::throttle) {
        GetPrivateProfileStringW(L"render", L"tick_interval", L"0.5", value, 32, ini.c_str());
        interval = std::wcstof(value, nullptr);
        if (!(interval > 0.0f && interval <= 10.0f)) interval = 0.5f;
        auto* table = reinterpret_cast<float*>(image + eu4build::rva(table_rva));
        DWORD old_protection = 0;
        if (!VirtualProtect(table, sizeof(expected_table), PAGE_READWRITE, &old_protection)) {
            log("REFUSED: cannot make the table writable (in-tick frames still counted)");
            installation_status.store(-6);
            return 6;
        }
        for (int i = 0; i < 5; ++i) {
            if (table[i] < interval) table[i] = interval;
        }
        DWORD ignored = 0;
        VirtualProtect(table, sizeof(expected_table), old_protection, &ignored);
    }
    HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    installation_status.store(mode == Mode::throttle ? 1 : 2);
    log("initialized version=0.6.1-experimental build=%s mode=%s tick_interval=%g "
        "frame_interval=%g running_frame_interval=%g skip_map_update=%d", eu4build::name(),
        mode == Mode::throttle ? "throttle" : "count",
        static_cast<double>(interval), static_cast<double>(frame_interval),
        static_cast<double>(running_interval_ticks) / static_cast<double>(qpc_frequency),
        skip_map_update ? 1 : 0);
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

extern "C" __declspec(dllexport) LONG EU4RenderPatchStatus() {
    return installation_status.load();
}
