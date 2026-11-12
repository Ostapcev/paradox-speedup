// Cache for 140d40ae0(unit, 0xe0, 0) called from 140d45250 (unit passability).
//
// The check combines modifiers of the leader, the unit, its regiments and the
// country; it does not depend on the provinces, yet path search calls it for
// every edge. During the daily AI phase (submit .. end of wait) results are
// cached per thread and per unit. Outside that phase the call is untouched.
#include "unit_flag_cache.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace flagcache {
namespace {

constexpr std::uintptr_t call_rva = 0xd45291;      // in 140d45250
constexpr std::uintptr_t function_rva = 0xd40ae0;  // 140d40ae0
constexpr std::uint32_t cached_id = 0xe0;
// xor r8d,r8d; mov edx,0xe0; call 140d40ae0; test al,al
constexpr std::uint8_t expected[] = {0x45, 0x33, 0xc0, 0xba, 0xe0, 0x00, 0x00, 0x00,
    0xe8, 0x4a, 0xb8, 0xff, 0xff, 0x84, 0xc0};
constexpr std::uintptr_t expected_rva = 0xd45289;

using FlagFn = std::uint8_t (*)(void* unit, std::uint32_t id, char flag, std::int64_t extra);

Mode mode = Mode::off;
FlagFn original = nullptr;
std::atomic<std::uint64_t> generation{0};
std::atomic<bool> active{false};
std::atomic<std::uint64_t> hits{0};
std::atomic<std::uint64_t> misses{0};
std::atomic<std::uint64_t> mismatches{0};
std::atomic<std::uint64_t> bypassed{0};

struct Entry {
    void* unit;
    std::uint64_t generation;
    std::uint8_t value;
};
constexpr int table_size = 512;
thread_local Entry table[table_size];

struct LocalCounters {
    std::uint32_t hits, misses, mismatches, bypassed, pending;
};
thread_local LocalCounters local;

void flush() {
    hits.fetch_add(local.hits, std::memory_order_relaxed);
    misses.fetch_add(local.misses, std::memory_order_relaxed);
    mismatches.fetch_add(local.mismatches, std::memory_order_relaxed);
    bypassed.fetch_add(local.bypassed, std::memory_order_relaxed);
    local = LocalCounters{};
}

inline void tick() {
    if (++local.pending >= 1024) flush();
}

std::uint8_t hook(void* unit, std::uint32_t id, char flag, std::int64_t extra) {
    if (!active.load(std::memory_order_acquire) || id != cached_id || flag != 0 || !unit) {
        ++local.bypassed;
        tick();
        return original(unit, id, flag, extra);
    }
    const auto current = generation.load(std::memory_order_relaxed);
    const auto key = reinterpret_cast<std::uintptr_t>(unit);
    Entry& entry = table[(key >> 4 ^ key >> 13) & (table_size - 1)];
    if (entry.unit == unit && entry.generation == current) {
        ++local.hits;
        tick();
        if (mode == Mode::on) return entry.value;
        const auto value = original(unit, id, flag, extra);
        if (value != entry.value) ++local.mismatches;
        return value;
    }
    const auto value = original(unit, id, flag, extra);
    entry.unit = unit;
    entry.generation = current;
    entry.value = value;
    ++local.misses;
    tick();
    return value;
}

speedpatch::CallPatch patch;

}  // namespace

std::uint8_t call(void* unit, std::uint32_t id, char flag, std::int64_t extra) {
    return mode == Mode::off ? original(unit, id, flag, extra) : hook(unit, id, flag, extra);
}

bool install(std::uintptr_t image, Mode selected, char* error, std::size_t error_size, bool patch_site) {
    original = reinterpret_cast<FlagFn>(image + eu4build::rva(function_rva));
    if (!patch_site) {
        mode = selected;
        return true;
    }
    if (selected == Mode::off) return true;
    if (!eu4build::code_matches(image, expected_rva, expected, sizeof(expected))) {
        std::snprintf(error, error_size, "unit flag call site bytes differ (rva 0x%llx)",
            static_cast<unsigned long long>(expected_rva));
        return false;
    }
    mode = selected;
    original = reinterpret_cast<FlagFn>(image + eu4build::rva(function_rva));
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + eu4build::rva(call_rva)),
            reinterpret_cast<const void*>(original), reinterpret_cast<const void*>(&hook),
            patch, error, error_size)) {
        return false;
    }
    if (!speedpatch::commit_call(patch, error, error_size)) {
        speedpatch::discard_call(patch);
        return false;
    }
    return true;
}

void begin_phase() {
    generation.fetch_add(1, std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
}

void end_phase() {
    active.store(false, std::memory_order_release);
}

bool phase_active() {
    return active.load(std::memory_order_acquire);
}

std::uint64_t phase_generation() {
    return generation.load(std::memory_order_relaxed);
}

Stats stats() {
    return Stats{hits.load(), misses.load(), mismatches.load(), bypassed.load()};
}

const char* mode_name() {
    return mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "off";
}

}  // namespace flagcache
