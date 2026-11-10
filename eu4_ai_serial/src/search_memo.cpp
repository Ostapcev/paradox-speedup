// searchmemo: port of Bellows army_search_memo (MIT, see LICENSE.bellows.txt).
//
// Within one scope on one thread
//   - the AI army search 1401022b0 (entry hook; its call site 0xebb5f belongs to eu4_speed_patch),
//   - the path search 14096c520 (entry hook),
//   - the listing of a unit's moves from a province 140195750 (entry hook, main thread daily),
// two answers that depend only on the army are computed once:
//   - 140d40ae0(army, 0xe0, 0), the zone-of-control flag asked by 140d45250 (call 0xd45291),
//   - 140d40790(army, &out, 0xfb, 0, 0), the speed modifier asked by 1401a6800 (call 0x1a6988).
// A scope inside another starts empty. Outside the scopes the flag goes to flagcache::call.
// shadow: the game's function runs every time and is compared; on: every 1024th memo answer is
// compared and a mismatch turns the memo off for good.
#include "search_memo.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "call_patch.hpp"
#include "unit_flag_cache.hpp"

namespace searchmemo {
namespace {

constexpr std::uintptr_t search_rva = 0x1022b0;
constexpr std::uintptr_t path_rva = 0x96c520;
constexpr std::uintptr_t moves_rva = 0x195750;
constexpr std::uintptr_t flag_call_rva = 0xd45291;
constexpr std::uintptr_t flag_rva = 0xd40ae0;
constexpr std::uintptr_t speed_call_rva = 0x1a6988;
constexpr std::uintptr_t speed_rva = 0xd40790;
constexpr std::uint8_t prologue_r9[5] = {0x4c, 0x89, 0x4c, 0x24, 0x20};   // mov [rsp+20h], r9
constexpr std::uint8_t prologue_r9b[5] = {0x44, 0x88, 0x4c, 0x24, 0x20};  // mov [rsp+20h], r9b
// xor r8d,r8d; mov edx,0xe0; call 140d40ae0
constexpr std::uint8_t flag_context[] = {0x45, 0x33, 0xc0, 0xba, 0xe0, 0x00, 0x00, 0x00, 0xe8};
// lea rdx,[rsp+70h]; mov rcx,rbp; call 140d40790; mov ebx,[rax]
constexpr std::uint8_t speed_context[] = {0x48, 0x8d, 0x54, 0x24, 0x70, 0x48, 0x8b, 0xcd, 0xe8};
constexpr std::uint8_t speed_after[] = {0x8b, 0x18};
// 140a273a0(province): the neighbour with the same owner (controlled by it) and the strongest
// fort, or null. Asked by 140d45250 for the move's source (0xd454d0) and target (0xd4556e).
constexpr std::uintptr_t fort_rva = 0xa273a0;
constexpr std::uintptr_t fort_call_rvas[] = {0xd454d0, 0xd4556e};
constexpr std::uint8_t fort_prologue[] = {0x40, 0x53, 0x4c, 0x8b, 0x91, 0xb8, 0x00, 0x00};

using SearchFn = std::uint64_t (*)(void*, int, void*, std::uint64_t, float*, std::uint8_t,
    std::uint8_t*, std::uint8_t*, std::uint8_t*, std::uint8_t, std::uint8_t, void*);
using PathFn = std::uint8_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
using MovesFn = void (*)(void*, void*, int, std::uint8_t, void*, void*);
using FlagFn = std::uint8_t (*)(void*, std::uint32_t, char, std::int64_t);
using SpeedFn = int* (*)(void*, int*, std::uint64_t, char, void*);
using FortFn = void* (*)(void*);

Mode mode = Mode::off;
SearchFn search_next = nullptr;
PathFn path_next = nullptr;
MovesFn moves_next = nullptr;
FlagFn flag_game = nullptr;
SpeedFn speed_game = nullptr;
FortFn fort_game = nullptr;
std::atomic<bool> disabled{false};
bool fort_installed = false;

struct Memo {
    int depth = 0;
    std::uint64_t scope_id = 0;
    const void* flag_army = nullptr;
    const void* speed_army = nullptr;
    int speed = 0;
    std::uint8_t flag = 0;
    bool flag_valid = false;
    bool speed_valid = false;
};
thread_local Memo memo;
thread_local std::uint64_t scope_counter = 0;

// Fort answers of the current scope, by province (entries of other scopes are stale).
struct FortEntry {
    const void* province;
    std::uint64_t scope;
    void* value;
};
constexpr int fort_table_size = 512;
thread_local FortEntry fort_table[fort_table_size];
std::atomic<std::uint64_t> g_fort_hits{0}, g_fort_misses{0};
thread_local std::uint32_t fort_hits_local = 0, fort_misses_local = 0;

struct Local {
    std::uint32_t scopes, flag_hits, flag_misses, flag_outside, speed_hits, speed_misses,
        verifications, mismatches, pending, sequence;
};
thread_local Local local;
std::atomic<std::uint64_t> g_scopes{0}, g_flag_hits{0}, g_flag_misses{0}, g_flag_outside{0},
    g_speed_hits{0}, g_speed_misses{0}, g_verifications{0}, g_mismatches{0};

void flush() {
    g_scopes.fetch_add(local.scopes, std::memory_order_relaxed);
    g_flag_hits.fetch_add(local.flag_hits, std::memory_order_relaxed);
    g_flag_misses.fetch_add(local.flag_misses, std::memory_order_relaxed);
    g_flag_outside.fetch_add(local.flag_outside, std::memory_order_relaxed);
    g_speed_hits.fetch_add(local.speed_hits, std::memory_order_relaxed);
    g_speed_misses.fetch_add(local.speed_misses, std::memory_order_relaxed);
    g_verifications.fetch_add(local.verifications, std::memory_order_relaxed);
    g_mismatches.fetch_add(local.mismatches, std::memory_order_relaxed);
    g_fort_hits.fetch_add(fort_hits_local, std::memory_order_relaxed);
    g_fort_misses.fetch_add(fort_misses_local, std::memory_order_relaxed);
    fort_hits_local = fort_misses_local = 0;
    const auto sequence = local.sequence;
    local = Local{};
    local.sequence = sequence;
}

inline void tick() {
    if (++local.pending >= 4096) flush();
}

// true when this memo answer should also be checked against the game's function.
inline bool check_now() {
    return mode == Mode::shadow || (++local.sequence & 1023) == 0;
}

void mismatch() {
    ++local.mismatches;
    flush();
    if (mode == Mode::on) disabled.store(true, std::memory_order_relaxed);
}

struct Scope {
    Memo saved;
    Scope() : saved(memo) {
        memo = Memo{};
        memo.depth = saved.depth + 1;
        memo.scope_id = ++scope_counter;
        ++local.scopes;
        tick();
    }
    ~Scope() { memo = saved; }
};

std::uint64_t search_hook(void* a1, int a2, void* a3, std::uint64_t a4, float* a5, std::uint8_t a6,
    std::uint8_t* a7, std::uint8_t* a8, std::uint8_t* a9, std::uint8_t a10, std::uint8_t a11,
    void* a12) {
    Scope scope;
    return search_next(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
}

std::uint8_t path_hook(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
    void* a8) {
    Scope scope;
    return path_next(a1, a2, a3, a4, a5, a6, a7, a8);
}

void moves_hook(void* a1, void* unit, int province, std::uint8_t a4, void* a5, void* a6) {
    Scope scope;
    moves_next(a1, unit, province, a4, a5, a6);
}

std::uint8_t flag_hook(void* army, std::uint32_t id, char via, std::int64_t extra) {
    if (memo.depth == 0 || disabled.load(std::memory_order_relaxed)) {
        ++local.flag_outside;
        tick();
        return flagcache::call(army, id, via, extra);
    }
    if (!memo.flag_valid || memo.flag_army != army) {
        const auto value = flag_game(army, id, via, extra);
        memo.flag_army = army;
        memo.flag = value;
        memo.flag_valid = true;
        ++local.flag_misses;
        tick();
        return value;
    }
    ++local.flag_hits;
    tick();
    if (check_now()) {
        const auto value = flag_game(army, id, via, extra);
        ++local.verifications;
        if (value != memo.flag) mismatch();
        return value;
    }
    return memo.flag;
}

int* speed_hook(void* army, int* out, std::uint64_t id, char a4, void* a5) {
    if (memo.depth == 0 || disabled.load(std::memory_order_relaxed)) return speed_game(army, out, id, a4, a5);
    if (!memo.speed_valid || memo.speed_army != army) {
        int* result = speed_game(army, out, id, a4, a5);
        memo.speed_army = army;
        memo.speed = *result;
        memo.speed_valid = true;
        ++local.speed_misses;
        tick();
        return result;
    }
    ++local.speed_hits;
    tick();
    if (check_now()) {
        int* result = speed_game(army, out, id, a4, a5);
        ++local.verifications;
        if (*result != memo.speed) mismatch();
        return result;
    }
    *out = memo.speed;
    return out;
}

void* fort_hook(void* province) {
    if (memo.depth == 0 || disabled.load(std::memory_order_relaxed)) return fort_game(province);
    const auto key = reinterpret_cast<std::uintptr_t>(province);
    FortEntry& e = fort_table[(key / 0x2e10) & (fort_table_size - 1)];
    if (e.province == province && e.scope == memo.scope_id) {
        ++fort_hits_local;
        tick();
        if (check_now()) {
            void* value = fort_game(province);
            ++local.verifications;
            if (value != e.value) mismatch();
            return value;
        }
        return e.value;
    }
    void* value = fort_game(province);
    e.province = province;
    e.scope = memo.scope_id;
    e.value = value;
    ++fort_misses_local;
    tick();
    return value;
}

speedpatch::CallPatch patches[5];
speedpatch::CallPatch fort_patches[2];

}  // namespace

bool install(std::uintptr_t image, Mode selected, char* error, std::size_t error_size) {
    if (selected == Mode::off) return true;
    if (std::memcmp(reinterpret_cast<const void*>(image + flag_call_rva - 8), flag_context,
            sizeof(flag_context)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(image + speed_call_rva - 8), speed_context,
            sizeof(speed_context)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(image + speed_call_rva + 5), speed_after,
            sizeof(speed_after)) != 0) {
        std::snprintf(error, error_size, "search memo call site bytes differ");
        return false;
    }
    mode = selected;
    flag_game = reinterpret_cast<FlagFn>(image + flag_rva);
    speed_game = reinterpret_cast<SpeedFn>(image + speed_rva);
    void* trampolines[3]{};
    bool ok =
        speedpatch::prepare_entry(reinterpret_cast<std::uint8_t*>(image + search_rva), prologue_r9,
            reinterpret_cast<const void*>(&search_hook), patches[0], &trampolines[0], error, error_size) &&
        speedpatch::prepare_entry(reinterpret_cast<std::uint8_t*>(image + path_rva), prologue_r9,
            reinterpret_cast<const void*>(&path_hook), patches[1], &trampolines[1], error, error_size) &&
        speedpatch::prepare_entry(reinterpret_cast<std::uint8_t*>(image + moves_rva), prologue_r9b,
            reinterpret_cast<const void*>(&moves_hook), patches[2], &trampolines[2], error, error_size) &&
        speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + flag_call_rva),
            reinterpret_cast<const void*>(flag_game), reinterpret_cast<const void*>(&flag_hook),
            patches[3], error, error_size) &&
        speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + speed_call_rva),
            reinterpret_cast<const void*>(speed_game), reinterpret_cast<const void*>(&speed_hook),
            patches[4], error, error_size);
    if (ok) {
        search_next = reinterpret_cast<SearchFn>(trampolines[0]);
        path_next = reinterpret_cast<PathFn>(trampolines[1]);
        moves_next = reinterpret_cast<MovesFn>(trampolines[2]);
        for (auto& patch : patches) {
            if (!speedpatch::commit_call(patch, error, error_size)) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        for (auto& patch : patches) speedpatch::discard_call(patch);
        mode = Mode::off;
        return false;
    }
    // the fort memo is optional: a refusal leaves the rest installed
    fort_game = reinterpret_cast<FortFn>(image + fort_rva);
    if (std::memcmp(reinterpret_cast<const void*>(image + fort_rva), fort_prologue, sizeof(fort_prologue)) == 0) {
        bool fort_ok = true;
        for (int i = 0; i < 2 && fort_ok; ++i) {
            fort_ok = speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + fort_call_rvas[i]),
                reinterpret_cast<const void*>(fort_game), reinterpret_cast<const void*>(&fort_hook),
                fort_patches[i], error, error_size);
        }
        for (int i = 0; i < 2 && fort_ok; ++i) fort_ok = speedpatch::commit_call(fort_patches[i], error, error_size);
        if (!fort_ok) {
            for (auto& patch : fort_patches) speedpatch::discard_call(patch);
        }
        fort_installed = fort_ok;
    }
    return true;
}

bool fort_active() { return fort_installed; }
std::uint64_t fort_hits() { return g_fort_hits.load(); }
std::uint64_t fort_misses() { return g_fort_misses.load(); }

Stats stats() {
    return Stats{g_scopes.load(), g_flag_hits.load(), g_flag_misses.load(), g_flag_outside.load(),
        g_speed_hits.load(), g_speed_misses.load(), g_verifications.load(), g_mismatches.load(),
        disabled.load()};
}

const char* mode_name() {
    return mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "off";
}

}  // namespace searchmemo
