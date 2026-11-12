// Cache for 1401998f0(int* out, record*, int days, void* map, Args* args) called
// from 140198c70 (army AI province evaluation, ~20% of the army search 1401022b0).
//
// It scores the enemy units remembered in one province record against our unit
// (args holds our unit, a flag, a strength evaluator and a float) and writes
// one int to *out; the caller reads only that int. A search may evaluate the
// same province with the same arguments more than once, so results are cached
// per thread for the exact argument tuple (pointers, days and the 32 argument
// bytes), only between flagcache::begin_phase and end_phase (daily AI phase).
// Use shadow first: the original still runs and every repeat is compared.
#include "province_score_cache.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace scorecache {
namespace {

constexpr std::uintptr_t call_rva = 0x198d27;      // in 140198c70
constexpr std::uintptr_t function_rva = 0x1998f0;  // 1401998f0
// lea rax,[rsp+0x40]; mov [rsp+0x20],rax; movaps [rsp+0x40],xmm0; movaps [rsp+0x50],xmm1;
// call 1401998f0
constexpr std::uint8_t expected[] = {0x48, 0x8d, 0x44, 0x24, 0x40, 0x48, 0x89, 0x44, 0x24,
    0x20, 0x0f, 0x29, 0x44, 0x24, 0x40, 0x0f, 0x29, 0x4c, 0x24, 0x50, 0xe8, 0xc4, 0x0b, 0x00,
    0x00};
constexpr std::uintptr_t expected_rva = 0x198d13;

struct Args {
    std::uint64_t word[4];
};
using ScoreFn = int* (*)(int* out, void* record, int days, void* map, Args* args);

Mode mode = Mode::off;
ScoreFn original = nullptr;
std::atomic<std::uint64_t> hits{0};
std::atomic<std::uint64_t> misses{0};
std::atomic<std::uint64_t> mismatches{0};
std::atomic<std::uint64_t> bypassed{0};

struct Entry {
    void* record;
    void* map;
    Args args;
    std::uint64_t generation;
    int days;
    int value;
};
constexpr int table_size = 4096;
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
    if (++local.pending >= 256) flush();
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

int* hook(int* out, void* record, int days, void* map, Args* args) {
    if (!flagcache::phase_active() || !out || !args) {
        ++local.bypassed;
        tick();
        return original(out, record, days, map, args);
    }
    const auto current = flagcache::phase_generation();
    std::uint64_t h = reinterpret_cast<std::uintptr_t>(record);
    h = mix(h, reinterpret_cast<std::uintptr_t>(map));
    h = mix(h, static_cast<std::uint32_t>(days));
    for (auto w : args->word) h = mix(h, w);
    Entry& entry = table[(h * 0x9e3779b97f4a7c15ull) >> 52];
    const bool same = entry.generation == current && entry.record == record &&
        entry.map == map && entry.days == days &&
        std::memcmp(&entry.args, args, sizeof(Args)) == 0;
    if (same) {
        ++local.hits;
        tick();
        if (mode == Mode::on) {
            *out = entry.value;
            return out;
        }
        Args copy = *args;  // the callee receives a scratch copy in the original too
        auto* result = original(out, record, days, map, &copy);
        if (*result != entry.value) ++local.mismatches;
        return result;
    }
    const Args saved = *args;
    auto* result = original(out, record, days, map, args);
    entry.record = record;
    entry.map = map;
    entry.args = saved;
    entry.days = days;
    entry.generation = current;
    entry.value = *result;
    ++local.misses;
    tick();
    return result;
}

speedpatch::CallPatch patch;

}  // namespace

bool install(std::uintptr_t image, Mode selected, char* error, std::size_t error_size) {
    if (selected == Mode::off) return true;
    if (!eu4build::code_matches(image, expected_rva, expected, sizeof(expected))) {
        std::snprintf(error, error_size, "province score call site bytes differ (rva 0x%llx)",
            static_cast<unsigned long long>(expected_rva));
        return false;
    }
    mode = selected;
    original = reinterpret_cast<ScoreFn>(image + eu4build::rva(function_rva));
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

Stats stats() {
    return Stats{hits.load(), misses.load(), mismatches.load(), bypassed.load()};
}

const char* mode_name() {
    return mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "off";
}

}  // namespace scorecache
