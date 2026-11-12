// EU4 Menu Patch: Back on the country selection screen and Exit to Menu in a campaign return
// to the main menu instead of restarting the game. EU4 1.37.5 Steam x64 only.
//
// A port of Hamweard (https://github.com/happy-dwarf-team/hamweard, MIT, (c) 2026 Happy Dwarf
// Team), itself a port of EU4 Menu Patch 0.1.4 (https://github.com/YozoraTempest/EU4-Menu-Patch,
// MIT, (c) 2026 VulonLok); see LICENSE.hamweard.txt. The clean-up sequence, the deferred world
// reset, the canal snapshot and every address are theirs. What differs: Anvil's replace_block,
// mid_hook, hook and swap_pointer are replaced by hand-written stubs below (5-byte jumps to code
// within 2 GB of the image), the log is printf-style, and only the Steam build is supported.
// Everything the hooks do runs on the game's main thread.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eu4_build.hpp"

#include "map_snapshot.hpp"
#include "menu_transition.hpp"

namespace {

using Bytes = std::uint8_t*;
std::uintptr_t image = 0;
std::wstring log_path;
std::mutex log_mutex;
std::atomic<LONG> installation_status{0};

void log(const char* format, ...) noexcept {
    if (log_path.empty()) return;
    char message[1500];
    const int prefix = std::snprintf(message, sizeof(message), "[%lu:%llu] ", GetCurrentProcessId(),
        static_cast<unsigned long long>(GetTickCount64()));
    if (prefix < 0) return;
    va_list arguments;
    va_start(arguments, format);
    const int body = std::vsnprintf(message + prefix, sizeof(message) - static_cast<std::size_t>(prefix), format, arguments);
    va_end(arguments);
    if (body < 0) return;
    std::size_t used = static_cast<std::size_t>(prefix) + static_cast<std::size_t>(body);
    if (used > sizeof(message) - 2) used = sizeof(message) - 2;
    message[used] = '\n';
    std::lock_guard<std::mutex> lock(log_mutex);
    HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, message, static_cast<DWORD>(used + 1), &written, nullptr);
        CloseHandle(file);
    }
}

template <class T> T& at(void* object, std::ptrdiff_t offset) noexcept {
    return *reinterpret_cast<T*>(static_cast<Bytes>(object) + offset);
}

// ---------------------------------------------------------------- patching primitives
// The registers a block or mid hook sees and may change; rip is where execution goes on.
struct Cpu {
    std::uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax, rflags, rip;
};
static_assert(sizeof(Cpu) == 17 * 8);
using CpuHandler = void (*)(Cpu&);

struct Code {
    Bytes base = nullptr;
    std::size_t size = 0;
    void bytes(std::initializer_list<std::uint8_t> list) {
        for (std::uint8_t b : list) base[size++] = b;
    }
    void raw(const void* p, std::size_t n) {
        std::memcpy(base + size, p, n);
        size += n;
    }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void jmp_abs(std::uintptr_t target) {  // jmp [rip+0]; dq target
        bytes({0xFF, 0x25, 0, 0, 0, 0});
        u64(target);
    }
    Bytes here() const { return base + size; }
};

Bytes code_page = nullptr;
std::size_t code_used = 0;
constexpr std::size_t code_page_size = 0x10000;

Bytes allocate_near(std::size_t bytes) {
    bytes = (bytes + 15) & ~std::size_t{15};
    if (!code_page || code_used + bytes > code_page_size) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const std::uintptr_t granularity = info.dwAllocationGranularity;
        code_page = nullptr;
        for (std::uintptr_t distance = 0x10000000; distance < 0x70000000 && !code_page; distance += granularity) {
            for (int sign = -1; sign <= 1 && !code_page; sign += 2) {
                const std::uintptr_t want = (image + static_cast<std::uintptr_t>(sign) * distance) & ~(granularity - 1);
                code_page = static_cast<Bytes>(VirtualAlloc(reinterpret_cast<void*>(want), code_page_size,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            }
        }
        if (!code_page) return nullptr;
        code_used = 0;
    }
    Bytes p = code_page + code_used;
    code_used += bytes;
    std::memset(p, 0xCC, bytes);
    return p;
}

bool rel32_reachable(std::uintptr_t from_end, std::uintptr_t to) {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from_end);
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

bool expect(std::uintptr_t rva, std::initializer_list<std::uint8_t> bytes, const char* what) {
    if (std::memcmp(reinterpret_cast<const void*>(image + rva), bytes.begin(), bytes.size()) == 0) return true;
    log("REFUSED: bytes differ at 0x%llx (%s)", static_cast<unsigned long long>(rva), what);
    return false;
}

bool expect_pointer(std::uintptr_t rva, std::uintptr_t points_to, const char* what) {
    if (*reinterpret_cast<const std::uintptr_t*>(image + rva) == image + points_to) return true;
    log("REFUSED: 0x%llx does not point to 0x%llx (%s)", static_cast<unsigned long long>(rva),
        static_cast<unsigned long long>(points_to), what);
    return false;
}

// A patch prepared in full first; all are written at the end, or none.
struct Pending {
    std::uintptr_t at;
    std::uint8_t bytes[16];
    std::size_t size;
};
std::vector<Pending> pending;

bool queue_jump(std::uintptr_t at, std::size_t length, Bytes target) {
    if (!rel32_reachable(at + 5, reinterpret_cast<std::uintptr_t>(target)) || length < 5 || length > 16) return false;
    Pending p{at, {}, length};
    std::memset(p.bytes, 0x90, sizeof(p.bytes));
    p.bytes[0] = 0xE9;
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(target) - (at + 5));
    std::memcpy(p.bytes + 1, &rel, 4);
    pending.push_back(p);
    return true;
}

// Saves every general register, the flags and xmm0-15, calls handler(cpu) on an aligned stack,
// restores what the handler left in cpu and goes on at cpu.rip (default: resume).
Bytes make_cpu_stub(CpuHandler handler, std::uintptr_t resume) {
    constexpr std::size_t capacity = 512;  // the stub is about 390 bytes
    Code c{allocate_near(capacity)};
    if (!c.base) return nullptr;
    c.bytes({0x48, 0x83, 0xEC, 0x08});                   // sub rsp, 8          (rip slot)
    c.bytes({0x9C});                                     // pushfq
    c.bytes({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57}); // push rax rcx rdx rbx rbp rsi rdi
    c.bytes({0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    c.bytes({0x48, 0x89, 0xE3});                         // mov rbx, rsp        (&cpu)
    c.bytes({0x48, 0xB8}); c.u64(resume);                // mov rax, resume
    c.bytes({0x48, 0x89, 0x83, 0x80, 0x00, 0x00, 0x00}); // mov [rbx+80h], rax  (cpu.rip)
    c.bytes({0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00}); // sub rsp, 100h
    c.bytes({0x48, 0x83, 0xE4, 0xF0});                   // and rsp, -16
    for (int i = 0; i < 16; ++i) {                       // movups [rsp+i*16], xmm_i
        const std::uint8_t reg = static_cast<std::uint8_t>(i & 7);
        if (i >= 8) c.bytes({0x44});
        c.bytes({0x0F, 0x11, static_cast<std::uint8_t>(0x84 | (reg << 3)), 0x24});
        const std::uint32_t disp = static_cast<std::uint32_t>(i * 16);
        c.raw(&disp, 4);
    }
    c.bytes({0x48, 0x89, 0xD9});                         // mov rcx, rbx
    c.bytes({0x48, 0x83, 0xEC, 0x20});                   // sub rsp, 20h
    c.bytes({0x48, 0xB8}); c.u64(reinterpret_cast<std::uint64_t>(handler));
    c.bytes({0xFF, 0xD0});                               // call rax
    c.bytes({0x48, 0x83, 0xC4, 0x20});                   // add rsp, 20h
    for (int i = 0; i < 16; ++i) {                       // movups xmm_i, [rsp+i*16]
        const std::uint8_t reg = static_cast<std::uint8_t>(i & 7);
        if (i >= 8) c.bytes({0x44});
        c.bytes({0x0F, 0x10, static_cast<std::uint8_t>(0x84 | (reg << 3)), 0x24});
        const std::uint32_t disp = static_cast<std::uint32_t>(i * 16);
        c.raw(&disp, 4);
    }
    c.bytes({0x48, 0x89, 0xDC});                         // mov rsp, rbx
    c.bytes({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58});
    c.bytes({0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58}); // pop rdi rsi rbp rbx rdx rcx rax
    c.bytes({0x9D});                                     // popfq
    c.bytes({0xC3});                                     // ret -> cpu.rip
    if (c.size > capacity) {
        log("REFUSED: cpu stub of %zu bytes", c.size);
        return nullptr;
    }
    log("cpu stub %zu bytes", c.size);
    return c.base;
}

// The displaced instructions (position independent, or with one rip-relative disp32 at
// disp_at), then a jump back after them.
Bytes make_trampoline(std::uintptr_t at, std::size_t length, int disp_at = -1) {
    Code t{allocate_near(48)};
    if (!t.base) return nullptr;
    t.raw(reinterpret_cast<const void*>(at), length);
    if (disp_at >= 0) {
        std::int32_t disp = 0;
        std::memcpy(&disp, t.base + disp_at, 4);
        const std::uintptr_t target = at + length + static_cast<std::int64_t>(disp);  // the disp ends the copy
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(t.base) + length;
        if (!rel32_reachable(end, target)) return nullptr;
        const auto fixed = static_cast<std::int32_t>(target - end);
        std::memcpy(t.base + disp_at, &fixed, 4);
    }
    t.jmp_abs(at + length);
    return t.base;
}

// Replaces the block at rva (length bytes) with handler; execution goes on at resume_rva
// unless the handler sets cpu.rip.
bool replace_block(std::uintptr_t rva, std::size_t length, std::uintptr_t resume_rva, CpuHandler handler) {
    Bytes stub = make_cpu_stub(handler, image + resume_rva);
    return stub && queue_jump(image + rva, length, stub);
}

// Runs handler at rva, then the displaced instructions and the rest of the function.
bool mid_hook(std::uintptr_t rva, std::size_t length, CpuHandler handler) {
    Bytes trampoline = make_trampoline(image + rva, length);
    if (!trampoline) return false;
    Bytes stub = make_cpu_stub(handler, reinterpret_cast<std::uintptr_t>(trampoline));
    return stub && queue_jump(image + rva, length, stub);
}

// Entry hook: the function jumps to hook; *original is the trampoline.
template <class F> bool hook_entry(std::uintptr_t rva, std::size_t length, F* hook, F*& original, int disp_at = -1) {
    Bytes trampoline = make_trampoline(image + rva, length, disp_at);
    Code relay{allocate_near(16)};
    if (!trampoline || !relay.base) return false;
    relay.jmp_abs(reinterpret_cast<std::uintptr_t>(hook));
    original = reinterpret_cast<F*>(trampoline);
    return queue_jump(image + rva, length, relay.base);
}

bool write_all() {
    for (const Pending& p : pending) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(p.at), p.size, PAGE_EXECUTE_READWRITE, &old)) return false;
    }
    for (const Pending& p : pending) {
        std::memcpy(reinterpret_cast<void*>(p.at), p.bytes, p.size);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(p.at), p.size);
    }
    return true;
}

template <class F> bool swap_slot(std::uintptr_t rva, std::uintptr_t points_to, F* replacement, F*& previous) {
    auto* slot = reinterpret_cast<void* volatile*>(image + rva);
    DWORD old = 0;
    if (!VirtualProtect(const_cast<void**>(slot), 8, PAGE_READWRITE, &old)) return false;
    void* expected = reinterpret_cast<void*>(image + points_to);
    previous = reinterpret_cast<F*>(expected);
    const bool ok = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(replacement), expected) == expected;
    DWORD ignored = 0;
    VirtualProtect(const_cast<void**>(slot), 8, old, &ignored);
    return ok;
}

// ---------------------------------------------------------------- game layout (1.37.5 Steam)
namespace off {
constexpr std::ptrdiff_t app_current_idler = 0x40;
constexpr std::ptrdiff_t app_next_idler = 0x70;
constexpr std::ptrdiff_t app_exit = 0x83;
constexpr std::ptrdiff_t app_restart = 0x84;
constexpr std::ptrdiff_t app_checksum_ready = 0x328;
constexpr std::ptrdiff_t app_services = 0x348;
constexpr std::ptrdiff_t app_graphical_map = 0x360;
constexpr std::ptrdiff_t app_menu_arg_1 = 0x350;
constexpr std::ptrdiff_t app_menu_arg_2 = 0x358;
constexpr std::ptrdiff_t window_widget = 0x30;
constexpr std::ptrdiff_t window_destroy_slot = 0x130;
constexpr std::ptrdiff_t widget_dead = 0xC4;
constexpr std::ptrdiff_t widget_name_slot = 0xA0;
constexpr std::ptrdiff_t widget_visible = 0xC5;
constexpr std::ptrdiff_t widget_hide_slot = 0x90;
constexpr std::ptrdiff_t dialog_name = 0x118;
constexpr std::ptrdiff_t lobby_id = 0x1C0;
constexpr std::ptrdiff_t lobby_hosting = 0x299;
constexpr std::ptrdiff_t lobby_flag_29b = 0x29B;
constexpr std::ptrdiff_t lobby_searching = 0x29C;
constexpr std::ptrdiff_t lobby_create_call = 0x2B0;
constexpr std::ptrdiff_t lobby_join_call = 0x2E0;
constexpr std::ptrdiff_t lobby_search_callback = 0x300;
constexpr std::ptrdiff_t lobby_search_call = 0x310;
constexpr std::ptrdiff_t world_provinces_begin = 0x1C90;
constexpr std::ptrdiff_t world_provinces_end = 0x1C98;
constexpr std::ptrdiff_t world_graphics_owner = 0x1E00;
constexpr std::ptrdiff_t world_potential_corruption = 0x23B4;
constexpr std::ptrdiff_t map_province_graphics = 0x70;
constexpr std::ptrdiff_t province_graphics_size = 0x1400;
constexpr std::ptrdiff_t canal_list_count = 0x14;
constexpr std::ptrdiff_t map_templates_begin = 0x6C0;
constexpr std::ptrdiff_t map_templates_end = 0x6C8;
constexpr std::ptrdiff_t map_naval_count = 0x17C0;
constexpr std::ptrdiff_t map_naval_distance = 0x17C8;
constexpr std::ptrdiff_t template_size = 0x230;
constexpr std::ptrdiff_t template_neighbours_begin = 0x168;
constexpr std::ptrdiff_t template_neighbours_end = 0x170;
constexpr std::ptrdiff_t idler_session_slot = 0x80;
constexpr std::ptrdiff_t session_is_dummy = 0x55;
constexpr std::ptrdiff_t achievements_loaded_begin = 0x88;
constexpr std::ptrdiff_t achievements_loaded_end = 0x90;
// CInGameIdler / CFrontEnd (Steam values of Hamweard's Layout)
constexpr std::ptrdiff_t idler_app = 0x330;
constexpr std::ptrdiff_t idler_minimap = 0x10E0;
constexpr std::ptrdiff_t idler_exit_flag = 0x12E9;
constexpr std::ptrdiff_t idler_notifications = 0x1400;
constexpr std::ptrdiff_t menu_pending = 0x900;
constexpr std::size_t main_menu_size = 0xB48;
}  // namespace off

namespace rva {
constexpr std::uintptr_t in_game_menu_block = 0x815CAE, in_game_idle_epilogue = 0x8165F2;
constexpr std::uintptr_t pregame_menu_block = 0x111DFD7, pregame_menu_continuation = 0x111E000;
constexpr std::uintptr_t frontend_tick_slot = 0x1D5A8E8, frontend_tick = 0x10D2E80;
constexpr std::uintptr_t frontend_restore_slot = 0x1D5A8F8, frontend_restore = 0x10D5BC0;
constexpr std::uintptr_t steam_lobby_vtable = 0x1DA6D38, steam_lobby_slot0 = 0x1658A80;
constexpr std::uintptr_t minimap_vtable = 0x1D6C620, minimap_slot0 = 0x1248740;
constexpr std::uintptr_t busy_enter = 0x470AF0, busy_leave = 0x470B30;
constexpr std::uintptr_t lobby_close = 0x1658D50, lobby_disconnect = 0x165B0C0, lobby_initialize = 0x1658CB0;
constexpr std::uintptr_t in_game_exit = 0x80FEE0, campaign_exit_cleanup = 0x5CC160;
constexpr std::uintptr_t notifications_destroy = 0xDFFCF0, minimap_destroy = 0x1248740;
constexpr std::uintptr_t continue_record_read = 0x5D1670, set_save_metadata = 0x2110C0;
constexpr std::uintptr_t string_free = 0x95660, game_alloc = 0x1A332D4;
constexpr std::uintptr_t main_menu_construct = 0x10D23C0, set_next_idler = 0x14C2530;
constexpr std::uintptr_t reset_game = 0x210000, history_post_validate = 0x7E9B70, history_execute = 0x7E8470;
constexpr std::uintptr_t multiplayer_click = 0x10F0510, multiplayer_availability = 0x10EB5C0;
constexpr std::uintptr_t canal_logic_apply = 0x78F250, province_graphics_clear = 0x1329BA0;
constexpr std::uintptr_t achievements_instance = 0x96080;
constexpr std::uintptr_t in_game_gui_init = 0x81AF10, in_game_idler_construct = 0x803270;
constexpr std::uintptr_t gui_create_window = 0x14E9030, dialog_window_store = 0x10A0735;
constexpr std::uintptr_t gui_manager_store = 0x14E8904, widget_registry = 0x14E9E16;
constexpr std::uintptr_t map = 0x233FED8, world = 0x233FE78, history = 0x233FEA0;
constexpr std::uintptr_t trigger_db = 0x2340010, trigger_db_create = 0xB89520, trigger_db_destroy = 0xB89560;
constexpr std::uintptr_t trigger_db_validate = 0xB89690, map_randomize = 0x890D00;
constexpr std::uintptr_t custom_countries_clear = 0x468430, custom_nation_used = 0x233D911;
}  // namespace rva

template <class F> F* fn(std::uintptr_t r) { return reinterpret_cast<F*>(image + r); }

using TickFn = void(void* idler, bool update);
using RestoreFn = void(void* idler);
using UnregisterCallResultFn = void(void* callback, std::uint64_t call);
using CreateWindowFn = void*(void* manager, const void* name);
using HistoryExecuteFn = void(void* history, const void* from, const void* to);
using MultiplayerClickFn = void(void* state);
using MultiplayerAvailabilityFn = int(void* view);

TickFn* original_tick = nullptr;
RestoreFn* original_restore = nullptr;
CreateWindowFn* original_create_window = nullptr;
HistoryExecuteFn* original_history_execute = nullptr;
MultiplayerClickFn* original_multiplayer_click = nullptr;
MultiplayerAvailabilityFn* original_multiplayer_availability = nullptr;

std::uintptr_t* g_world = nullptr;
std::uintptr_t* g_history = nullptr;
std::uintptr_t* g_trigger_db = nullptr;
std::uint8_t* g_custom_nation_used = nullptr;
std::uintptr_t* g_map = nullptr;
void* g_lobby_vtable = nullptr;
void* g_minimap_vtable = nullptr;
UnregisterCallResultFn* g_unregister_call_result = nullptr;
bool g_unregister_resolved = false;

std::atomic<void*> g_pending_frontend_app{nullptr};
Bytes g_pending_menu_app = nullptr;
void* g_pending_menu_idler = nullptr;
std::uint32_t g_previous_corruption = 0;
std::uint32_t g_returns = 0;

hamweard::MapSnapshot g_map_before_canals;
bool g_map_changed = false;
bool g_map_randomized = false;
std::uintptr_t g_in_game_stock_path = 0;
std::uintptr_t g_pregame_stock_path = 0;

bool g_returned_to_menu = false;
std::uintptr_t* g_manager_global = nullptr;
std::ptrdiff_t g_registry_begin = 0;
std::ptrdiff_t g_registry_end = 0;
bool g_recording_windows = false;
bool g_campaign_seen = false;
std::unordered_map<void*, std::string> g_campaign_windows;
std::unordered_set<std::string> g_frontend_names;
hamweard::WorldResetGate g_reset_gate;

class BusyScope {
    std::uint8_t previous_[16] = {};

public:
    BusyScope() { fn<void*(void*)>(rva::busy_enter)(previous_); }
    ~BusyScope() { fn<void(void*)>(rva::busy_leave)(previous_); }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;
};

Bytes world() noexcept { return reinterpret_cast<Bytes>(*g_world); }

std::uint32_t potential_corruption() noexcept {
    Bytes w = world();
    return w ? at<std::uint32_t>(w, off::world_potential_corruption) : 0;
}

Bytes steam_lobby(Bytes app) noexcept {
    if (!g_lobby_vtable) return nullptr;
    Bytes services = at<Bytes>(app, off::app_services);
    Bytes lobby = services ? at<Bytes>(services, 0x10) : nullptr;
    return lobby && at<void*>(lobby, 0) == g_lobby_vtable ? lobby : nullptr;
}

void leave_steam_session(Bytes app) {
    Bytes lobby = steam_lobby(app);
    if (!lobby) return;
    auto& search_call = at<std::uint64_t>(lobby, off::lobby_search_call);
    const bool active = at<std::uint64_t>(lobby, off::lobby_id) || at<std::uint64_t>(lobby, off::lobby_create_call) ||
        at<std::uint64_t>(lobby, off::lobby_join_call) || search_call || at<std::uint8_t>(lobby, off::lobby_hosting) ||
        at<std::uint8_t>(lobby, off::lobby_flag_29b);
    if (!active) return;
    log("Steam lobby clean-up: lobby %#llx, create %d, join %d, search %d, hosting %d",
        static_cast<unsigned long long>(at<std::uint64_t>(lobby, off::lobby_id)),
        at<std::uint64_t>(lobby, off::lobby_create_call) != 0, at<std::uint64_t>(lobby, off::lobby_join_call) != 0,
        search_call != 0, at<std::uint8_t>(lobby, off::lobby_hosting));
    if (!g_unregister_resolved) {
        g_unregister_resolved = true;
        if (HMODULE steam = GetModuleHandleW(L"steam_api64.dll")) {
            g_unregister_call_result = reinterpret_cast<UnregisterCallResultFn*>(
                reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_UnregisterCallResult")));
        }
        if (!g_unregister_call_result) log("SteamAPI_UnregisterCallResult not found: a lobby search is not cancelled");
    }
    if (search_call && g_unregister_call_result) {
        g_unregister_call_result(lobby + off::lobby_search_callback, search_call);
        search_call = 0;
        at<std::uint8_t>(lobby, off::lobby_searching) = 0;
    }
    fn<void(void*)>(rva::lobby_close)(lobby);
    fn<void(void*)>(rva::lobby_disconnect)(lobby);
    fn<void(void*)>(rva::lobby_initialize)(lobby);
}

void finish_return(void* idler) {
    if (idler != g_pending_menu_idler || !g_pending_menu_app) return;
    Bytes app = g_pending_menu_app;
    if (at<void*>(app, off::app_current_idler) != idler || at<void*>(app, off::app_next_idler) != nullptr ||
        at<std::uint32_t>(idler, off::menu_pending) != 0 || !at<std::uint8_t>(app, off::app_checksum_ready))
        return;
    Bytes w = world();
    if (!w) return;
    auto& corruption = at<std::uint32_t>(w, off::world_potential_corruption);
    const std::uint32_t before = corruption;
    corruption = hamweard::corruption_after_return(before, g_previous_corruption);
    log("main menu active: potential corruption %u -> %u (before the return %u), checksum ready", before, corruption,
        g_previous_corruption);
    g_pending_menu_app = nullptr;
    g_pending_menu_idler = nullptr;
    g_returned_to_menu = true;
}

struct GameString {
    char storage[16]{};
    std::size_t size = 0;
    std::size_t capacity = 15;
};
static_assert(sizeof(GameString) == 0x20);

struct ContinueRecord {
    GameString strings[4];
    bool valid = false;
    ContinueRecord() = default;
    ContinueRecord(const ContinueRecord&) = delete;
    ContinueRecord& operator=(const ContinueRecord&) = delete;
    ~ContinueRecord() {
        for (auto& s : strings) fn<void(void*)>(rva::string_free)(&s);
    }
};
static_assert(offsetof(ContinueRecord, valid) == 0x80);

void construct_menu(Bytes app, std::uint32_t reason) {
    {
        ContinueRecord record;
        fn<void(void*)>(rva::continue_record_read)(&record);
        const GameString empty;
        fn<void(void*, const void*)>(rva::set_save_metadata)(app, record.valid ? &record.strings[3] : &empty);
        log("Continue game refreshed (record valid: %d)", record.valid ? 1 : 0);
    }
    void* memory = fn<void*(std::uint64_t)>(rva::game_alloc)(off::main_menu_size);
    void* menu = fn<void*(void*, void*, void*, void*)>(rva::main_menu_construct)(memory,
        at<void*>(app, off::app_menu_arg_1), at<void*>(app, off::app_menu_arg_2), app);
    fn<void(void*, void**, bool)>(rva::set_next_idler)(app, &menu, false);
    g_pending_menu_app = app;
    g_pending_menu_idler = at<void*>(app, off::app_next_idler);
    g_reset_gate.queue(app, g_pending_menu_idler);
    g_previous_corruption = reason;
    ++g_returns;
    log("return to the main menu #%u queued (menu %p, exit %u, restart %u)", g_returns, g_pending_menu_idler,
        at<std::uint8_t>(app, off::app_exit), at<std::uint8_t>(app, off::app_restart));
}

void release_minimap(Bytes idler) {
    auto& slot = at<void*>(idler, off::idler_minimap);
    void* controller = slot;
    if (!controller) return;
    if (at<void*>(controller, 0) != g_minimap_vtable) {
        log("minimap left alone: unexpected controller vtable");
        return;
    }
    slot = nullptr;
    fn<void(void*, std::uint32_t)>(rva::minimap_destroy)(controller, 1u);
}

std::string_view game_string(const void* string) noexcept {
    const auto* s = static_cast<const GameString*>(string);
    const char* data = s->capacity > 15 ? *reinterpret_cast<char* const*>(s->storage) : s->storage;
    return {data, s->size};
}

void clear_province_graphics(Bytes app, void* idler) {
    Bytes map = at<Bytes>(app, off::app_graphical_map);
    Bytes w = world();
    if (!map || !w) return;
    at<void*>(w, off::world_graphics_owner) = idler;
    const std::ptrdiff_t provinces =
        (at<Bytes>(w, off::world_provinces_end) - at<Bytes>(w, off::world_provinces_begin)) / 8;
    Bytes graphics = at<Bytes>(map, off::map_province_graphics);
    for (std::ptrdiff_t i = 0; i < provinces; ++i)
        fn<void(void*)>(rva::province_graphics_clear)(graphics + i * off::province_graphics_size);
}

bool g_history_reloading = false;

void validate_reloaded_history() {
    if (!g_history_reloading) return;
    g_history_reloading = false;
    if (void* triggers = reinterpret_cast<void*>(*g_trigger_db)) {
        fn<void(void*)>(rva::trigger_db_validate)(triggers);
        fn<void()>(rva::trigger_db_destroy)();
    }
    fn<void(void*)>(rva::history_post_validate)(reinterpret_cast<void*>(*g_history));
}

void on_history_execute(void* history, const void* from, const void* to) {
    if (g_reset_gate.pending()) {
        log("history walk skipped: the world is about to be reset");
        return;
    }
    validate_reloaded_history();
    original_history_execute(history, from, to);
}

void note_window(void* window, std::string_view name) {
    if (!g_campaign_seen) {
        g_frontend_names.emplace(name);
    } else if (g_recording_windows && name != "load_screen" && name != "console_wnd" &&
               !g_frontend_names.contains(std::string(name))) {
        g_campaign_windows.insert_or_assign(window, std::string(name));
    } else {
        g_campaign_windows.erase(window);
    }
}

void* on_create_window(void* manager, const void* name) {
    void* window = original_create_window(manager, name);
    if (window) note_window(window, game_string(name));
    return window;
}

void destroy_campaign_windows() {
    std::unordered_map<void*, std::string> windows = std::exchange(g_campaign_windows, {});
    Bytes manager = reinterpret_cast<Bytes>(*g_manager_global);
    if (windows.empty() || !manager) return;
    std::size_t destroyed = 0, gone = 0;
    for (const auto& [window, name] : windows) {
        Bytes widget = static_cast<Bytes>(window) + off::window_widget;
        void** begin = at<void**>(manager, g_registry_begin);
        void** end = at<void**>(manager, g_registry_end);
        if (std::find(begin, end, widget) == end || at<std::uint8_t>(widget, off::widget_dead) != 0) {
            ++gone;
            continue;
        }
        using NameFn = const void* (*)(void* widget);
        const void* own = at<NameFn>(at<void*>(widget, 0), off::widget_name_slot)(widget);
        if (!own || game_string(own) != name) {
            ++gone;
            continue;
        }
        if (at<std::uint8_t>(widget, off::widget_visible) != 0) {
            using HideFn = void (*)(void* widget);
            at<HideFn>(at<void*>(widget, 0), off::widget_hide_slot)(widget);
        }
        using DestroyFn = void(void* window);
        at<DestroyFn*>(at<void*>(window, 0), off::window_destroy_slot)(window);
        ++destroyed;
    }
    log("windows of the last campaign: %zu destroyed, %zu already gone", destroyed, gone);
}

hamweard::MapView map_view() {
    hamweard::MapView view;
    auto* map = reinterpret_cast<Bytes>(*g_map);
    if (!map) return view;
    Bytes begin = at<Bytes>(map, off::map_templates_begin);
    Bytes end = at<Bytes>(map, off::map_templates_end);
    view.neighbours.reserve(static_cast<std::size_t>((end - begin) / off::template_size));
    for (Bytes t = begin; t != end; t += off::template_size) {
        auto* first = at<std::uint8_t*>(t, off::template_neighbours_begin);
        auto* last = at<std::uint8_t*>(t, off::template_neighbours_end);
        view.neighbours.emplace_back(first, last);
    }
    const auto count = static_cast<std::size_t>(at<std::int32_t>(map, off::map_naval_count));
    view.naval = {at<std::uint8_t*>(map, off::map_naval_distance), count * count * 4};
    return view;
}

void forget_loaded_achievements() {
    auto* manager = static_cast<Bytes>(fn<void*()>(rva::achievements_instance)());
    at<void*>(manager, off::achievements_loaded_end) = at<void*>(manager, off::achievements_loaded_begin);
}

bool in_multiplayer_session() {
    Bytes w = world();
    void* idler = w ? at<void*>(w, off::world_graphics_owner) : nullptr;
    if (!idler) return false;
    using GetSessionFn = Bytes (*)(void* idler);
    Bytes session = at<GetSessionFn>(at<void*>(idler, 0), off::idler_session_slot)(idler);
    return session && !at<std::uint8_t>(session, off::session_is_dummy);
}

void on_multiplayer_click(void* state) {
    if (hamweard::multiplayer_needs_restart(g_returned_to_menu, potential_corruption())) {
        log("Multiplayer after a return: the game has to restart first");
        return;
    }
    original_multiplayer_click(state);
}

int on_multiplayer_availability(void* view) {
    return hamweard::multiplayer_availability(original_multiplayer_availability(view), g_returned_to_menu,
        potential_corruption());
}

const char* restart_reason() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    const std::uint64_t commit = GlobalMemoryStatusEx(&memory) ? memory.ullAvailPageFile : UINT64_MAX;
    return hamweard::restart_reason(in_multiplayer_session(), g_map_randomized, commit);
}

void return_to_menu(Bytes idler) {
    Bytes app = at<Bytes>(idler, off::idler_app);
    const std::uint32_t reason = potential_corruption();
    log("Exit to Menu: idler %p, world %p, potential corruption %u", static_cast<void*>(idler),
        static_cast<void*>(world()), reason);
    g_recording_windows = false;
    at<std::uint8_t>(idler, off::idler_exit_flag) = 0;
    BusyScope busy;
    fn<void(void*)>(rva::in_game_exit)(idler);
    leave_steam_session(app);
    fn<void()>(rva::campaign_exit_cleanup)();
    if (void* notices = at<void*>(idler, off::idler_notifications)) fn<void(void*)>(rva::notifications_destroy)(notices);
    release_minimap(idler);
    construct_menu(app, reason);
}

void on_frontend_restore(void* idler) {
    if (g_pending_menu_app && idler == g_pending_menu_idler) clear_province_graphics(g_pending_menu_app, idler);
    original_restore(idler);
}

long long ms_between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

void on_frontend_tick(void* idler, bool update) {
    if (auto* app = static_cast<Bytes>(g_pending_frontend_app.exchange(nullptr))) {
        log("Back: returning from the country selection");
        const std::uint32_t reason = potential_corruption();
        BusyScope busy;
        leave_steam_session(app);
        construct_menu(app, reason);
        return;
    }
    if (Bytes app = g_pending_menu_app) {
        g_reset_gate.run(app, idler, at<void*>(app, off::app_current_idler), at<void*>(app, off::app_next_idler), [&] {
            using Clock = std::chrono::steady_clock;
            BusyScope busy;
            const auto start = Clock::now();
            destroy_campaign_windows();
            const auto windows = Clock::now();
            if (!*g_trigger_db) fn<void()>(rva::trigger_db_create)();
            fn<void()>(rva::custom_countries_clear)();
            *g_custom_nation_used = 0;
            g_history_reloading = true;
            fn<void(void*, bool)>(rva::reset_game)(app, false);
            validate_reloaded_history();
            forget_loaded_achievements();
            if (g_map_changed) {
                g_map_changed = false;
                if (g_map_before_canals.restore(map_view())) {
                    log("map put back as it was before the canals");
                } else {
                    log("WARNING the map could not be put back after a canal: the game restarts");
                    at<std::uint16_t>(app, off::app_exit) = 0x0101;
                }
            }
            log("world reset (world %p): windows %lld ms, reset %lld ms", static_cast<void*>(world()),
                ms_between(start, windows), ms_between(windows, Clock::now()));
        });
    }
    original_tick(idler, update);
    finish_return(idler);
}

// ---------------------------------------------------------------- block and mid-hook handlers
void in_game_block(Cpu& cpu) {
    if (const char* why = restart_reason()) {
        log("Exit to Menu %s: the game restarts", why);
        cpu.rcx = cpu.rbp + 0x240;  // the block's own lea
        cpu.rip = g_in_game_stock_path;
        return;
    }
    return_to_menu(reinterpret_cast<Bytes>(cpu.r13));
}

void pregame_block(Cpu& cpu) {
    if (const char* why = restart_reason()) {
        log("Back %s: the game restarts", why);
        at<std::uint16_t>(reinterpret_cast<void*>(cpu.rbx), off::app_exit) = 0x0101;  // the block's own mov
        cpu.rip = g_pregame_stock_path;
        return;
    }
    g_pending_frontend_app.store(reinterpret_cast<void*>(cpu.rbx));
}

void canal_hook(Cpu& cpu) {
    if (at<std::int32_t>(reinterpret_cast<void*>(cpu.rcx), off::canal_list_count) > 0) {
        if (!g_map_before_canals.taken()) {
            g_map_before_canals.take(map_view());
            log("map kept before the first canal");
        }
        g_map_changed = true;
    }
}

void randomize_hook(Cpu&) {
    g_map_randomized = true;
    log("Random New World: the next return restarts the game");
}

void record_hook(Cpu&) {
    g_campaign_seen = true;
    g_recording_windows = true;
}

void dialog_hook(Cpu& cpu) {
    if (void* window = reinterpret_cast<void*>(cpu.rdi))
        note_window(window, game_string(reinterpret_cast<Bytes>(cpu.rbx) + off::dialog_name));
}

// ---------------------------------------------------------------- install
bool check_all() {
    bool ok = true;
    ok &= expect(rva::in_game_menu_block, {0x48, 0x8D, 0x8D, 0x40, 0x02, 0x00, 0x00}, "in_game_menu_block");
    ok &= expect(rva::in_game_idle_epilogue, {0x49, 0x8B, 0x85, 0x30, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x88, 0x70, 0x03,
        0x00, 0x00, 0x0F, 0xB6, 0x85, 0x48, 0x02, 0x00, 0x00}, "in_game_idle_epilogue");
    ok &= expect(rva::pregame_menu_block, {0x66, 0xC7, 0x83, 0x83, 0x00, 0x00, 0x00, 0x01, 0x01}, "pregame_menu_block");
    ok &= expect(rva::pregame_menu_continuation, {0x48, 0x8B, 0x5D, 0xB0, 0x48, 0x85, 0xDB, 0x74, 0x5C},
        "pregame_menu_continuation");
    ok &= expect_pointer(rva::frontend_tick_slot, rva::frontend_tick, "frontend_tick_slot");
    ok &= expect_pointer(rva::frontend_restore_slot, rva::frontend_restore, "frontend_restore_slot");
    ok &= expect_pointer(rva::steam_lobby_vtable, rva::steam_lobby_slot0, "steam_lobby_vtable");
    ok &= expect_pointer(rva::minimap_vtable, rva::minimap_slot0, "minimap_vtable");
    const std::initializer_list<std::uint8_t> small = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
    ok &= expect(rva::busy_enter, small, "busy_enter");
    ok &= expect(rva::busy_leave, small, "busy_leave");
    ok &= expect(rva::lobby_close, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x99, 0x02, 0x00, 0x00, 0x00}, "lobby_close");
    ok &= expect(rva::lobby_disconnect, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x8B, 0x89, 0xC4, 0x01,
        0x00, 0x00}, "lobby_disconnect");
    ok &= expect(rva::lobby_initialize, small, "lobby_initialize");
    ok &= expect(rva::in_game_exit, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9}, "in_game_exit");
    ok &= expect(rva::campaign_exit_cleanup, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
        0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC}, "campaign_exit_cleanup");
    ok &= expect(rva::notifications_destroy, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
        0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56}, "notifications_destroy");
    ok &= expect(rva::minimap_destroy, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xF1}, "minimap_destroy");
    ok &= expect(rva::continue_record_read, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
        0x7C, 0x24, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}, "continue_record_read");
    ok &= expect(rva::set_save_metadata, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
        0x48, 0x8B, 0xFA}, "set_save_metadata");
    ok &= expect(rva::string_free, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x51, 0x18, 0x48, 0x8B, 0xD9}, "string_free");
    {
        // operator new is only called, never patched here; eu4_alloc_patch (direct=1) may have
        // replaced its entry with "mov rax, hook; jmp rax", which is still a valid operator new
        const auto* entry = reinterpret_cast<const std::uint8_t*>(image + rva::game_alloc);
        const bool alloc_hook = entry[0] == 0x48 && entry[1] == 0xB8 && entry[10] == 0xFF &&
            entry[11] == 0xE0;
        if (!alloc_hook) ok &= expect(rva::game_alloc, small, "game_alloc");
    }
    ok &= expect(rva::main_menu_construct, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
        0x89, 0x78, 0x20, 0x48, 0x89, 0x48, 0x08, 0x55}, "main_menu_construct");
    ok &= expect(rva::set_next_idler, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x48, 0x8B, 0xDA}, "set_next_idler");
    const std::initializer_list<std::uint8_t> big = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
        0x55, 0x41, 0x56, 0x41, 0x57};
    ok &= expect(rva::reset_game, big, "reset_game");
    ok &= expect(rva::history_post_validate, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
        0x74, 0x24, 0x20, 0x57}, "history_post_validate");
    ok &= expect(rva::history_execute, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54,
        0x24, 0x10, 0x55}, "history_execute");
    ok &= expect(rva::multiplayer_click, {0x48, 0x83, 0xEC, 0x28, 0x80, 0x3D, 0x81, 0xF9, 0x24, 0x01, 0x00, 0x75, 0x5E},
        "multiplayer_click");
    ok &= expect(rva::multiplayer_availability, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
        0x41, 0x08, 0x48, 0x8B, 0x88, 0x30, 0x03, 0x00, 0x00}, "multiplayer_availability");
    ok &= expect(rva::canal_logic_apply, {0x4C, 0x8B, 0xDC, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00,
        0x00}, "canal_logic_apply");
    ok &= expect(rva::province_graphics_clear, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
        0x74, 0x24, 0x18, 0x57}, "province_graphics_clear");
    ok &= expect(rva::achievements_instance, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0x05, 0x78, 0x2A, 0x02, 0x48,
        0x85, 0xC0}, "achievements_instance");
    ok &= expect(rva::in_game_gui_init, big, "in_game_gui_init");
    ok &= expect(rva::in_game_idler_construct, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48,
        0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56}, "in_game_idler_construct");
    ok &= expect(rva::gui_create_window, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF9,
        0x48, 0x8B, 0xDA}, "gui_create_window");
    ok &= expect(rva::dialog_window_store, {0x48, 0x89, 0x7B, 0x08, 0x48, 0x85, 0xFF}, "dialog_window_store");
    ok &= expect(rva::gui_manager_store, {0x48, 0x89, 0x35, 0xE5, 0x0B, 0xE6, 0x00}, "gui_manager_store");
    ok &= expect(rva::widget_registry, {0x48, 0x8B, 0xBE, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x86, 0x18, 0x01, 0x00,
        0x00}, "widget_registry");
    ok &= expect(rva::trigger_db_create, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xB9, 0x80, 0x00, 0x00, 0x00}, "trigger_db_create");
    ok &= expect(rva::trigger_db_destroy, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20}, "trigger_db_destroy");
    ok &= expect(rva::trigger_db_validate, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9},
        "trigger_db_validate");
    ok &= expect(rva::map_randomize, {0x44, 0x88, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54,
        0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08}, "map_randomize");
    ok &= expect(rva::custom_countries_clear, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20},
        "custom_countries_clear");
    return ok;
}

DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_menu_patch.log";
    const std::wstring ini = directory + L"\\eu4_menu_patch.ini";
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    wchar_t mode[16]{};
    GetPrivateProfileStringW(L"menu", L"mode", L"off", mode, 16, ini.c_str());
    if (_wcsicmp(mode, L"on") != 0) {
        log("disabled by configuration; no bytes changed");
        installation_status.store(2);
        return 0;
    }
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (eu4build::detect() != eu4build::Build::steam) {
        // the main-loop object and the Steam lobby code differ in other builds
        log("REFUSED: build=%s; only the Steam build of eu4.exe 1.37.5 is supported; no bytes changed",
            eu4build::name());
        installation_status.store(-4);
        return 4;
    }
    if (!check_all()) {
        installation_status.store(-4);
        return 4;
    }
    g_world = reinterpret_cast<std::uintptr_t*>(image + rva::world);
    g_history = reinterpret_cast<std::uintptr_t*>(image + rva::history);
    g_trigger_db = reinterpret_cast<std::uintptr_t*>(image + rva::trigger_db);
    g_custom_nation_used = reinterpret_cast<std::uint8_t*>(image + rva::custom_nation_used);
    g_map = reinterpret_cast<std::uintptr_t*>(image + rva::map);
    g_minimap_vtable = reinterpret_cast<void*>(image + rva::minimap_vtable);
    g_lobby_vtable = reinterpret_cast<void*>(image + rva::steam_lobby_vtable);
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, reinterpret_cast<const void*>(image + rva::gui_manager_store + 3), 4);
        g_manager_global = reinterpret_cast<std::uintptr_t*>(image + rva::gui_manager_store + 7 + disp);
        std::uint32_t begin = 0, end = 0;
        std::memcpy(&begin, reinterpret_cast<const void*>(image + rva::widget_registry + 3), 4);
        std::memcpy(&end, reinterpret_cast<const void*>(image + rva::widget_registry + 10), 4);
        g_registry_begin = static_cast<std::ptrdiff_t>(begin);
        g_registry_end = static_cast<std::ptrdiff_t>(end);
    }
    g_in_game_stock_path = image + rva::in_game_menu_block + 7;
    g_pregame_stock_path = image + rva::pregame_menu_block + 9;

    bool ok = replace_block(rva::in_game_menu_block, 7, rva::in_game_idle_epilogue, &in_game_block) &&
        replace_block(rva::pregame_menu_block, 9, rva::pregame_menu_continuation, &pregame_block) &&
        mid_hook(rva::canal_logic_apply, 7, &canal_hook) &&       // mov r11,rsp; push rbp; mov rbp,rsp
        mid_hook(rva::map_randomize, 5, &randomize_hook) &&       // mov [rsp+20h],r9b
        mid_hook(rva::in_game_idler_construct, 7, &record_hook) && // mov rax,rsp; mov [rax+18h],rbx
        mid_hook(rva::in_game_gui_init, 5, &record_hook) &&       // mov [rsp+8],rbx
        mid_hook(rva::dialog_window_store, 7, &dialog_hook) &&    // mov [rbx+8],rdi; test rdi,rdi
        hook_entry(rva::gui_create_window, 5, &on_create_window, original_create_window) &&
        hook_entry(rva::history_execute, 5, &on_history_execute, original_history_execute) &&
        // sub rsp,28h; cmp byte [rip+disp32],0 (the disp at +6 is moved with the copy)
        hook_entry(rva::multiplayer_click, 11, &on_multiplayer_click, original_multiplayer_click, 6) &&
        hook_entry(rva::multiplayer_availability, 5, &on_multiplayer_availability, original_multiplayer_availability);
    if (!ok) {
        log("REFUSED: a stub could not be prepared; no bytes changed");
        installation_status.store(-5);
        return 5;
    }
    // The slots first (they only act once a return is queued), then the code.
    if (!swap_slot(rva::frontend_tick_slot, rva::frontend_tick, &on_frontend_tick, original_tick) ||
        !swap_slot(rva::frontend_restore_slot, rva::frontend_restore, &on_frontend_restore, original_restore)) {
        if (original_tick && *reinterpret_cast<void**>(image + rva::frontend_tick_slot) == reinterpret_cast<void*>(&on_frontend_tick)) {
            TickFn* ignored = nullptr;
            swap_slot(rva::frontend_tick_slot, reinterpret_cast<std::uintptr_t>(&on_frontend_tick) - image,
                fn<TickFn>(rva::frontend_tick), ignored);
        }
        log("REFUSED: frontend idler slots differ; no code bytes changed");
        installation_status.store(-6);
        return 6;
    }
    if (!write_all()) {
        log("FAILED: code write");
        installation_status.store(-7);
        return 7;
    }
    installation_status.store(1);
    log("initialized version=0.1.0 (Hamweard port): %zu code patches, 2 slots", pending.size());
    return 0;
}

DWORD WINAPI install(void* module) {
    try {
        return install_checked(static_cast<HMODULE>(module));
    } catch (...) {
        installation_status.store(-8);
        return 8;
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

extern "C" __declspec(dllexport) LONG EU4MenuPatchStatus() {
    return installation_status.load();
}
