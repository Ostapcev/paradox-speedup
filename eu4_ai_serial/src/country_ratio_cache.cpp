// Cache for 1403747b0(country, int* out, flag) called from 1403b4a60.
//
// The function walks every owned province of the country and writes one
// per-mille value to *out (it writes nothing else). The AI calls it again and
// again for the same country during one daily AI phase; results are cached
// per thread and per (country, flag) only between flagcache::begin_phase and
// end_phase.
#include "country_ratio_cache.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "call_patch.hpp"
#include "eu4_build.hpp"

namespace ratiocache {
namespace {

constexpr std::uintptr_t call_rva = 0x3b4ec4;      // in 1403b4a60
constexpr std::uintptr_t function_rva = 0x3747b0;  // 1403747b0
// xor r8d,r8d; lea rdx,[rsp+0x90]; mov rcx,r13; call 1403747b0; cmp dword [rax],0xc8
constexpr std::uint8_t expected[] = {0x45, 0x33, 0xc0, 0x48, 0x8d, 0x94, 0x24, 0x90, 0x00,
    0x00, 0x00, 0x49, 0x8b, 0xcd, 0xe8, 0xe7, 0xf8, 0xfb, 0xff, 0x81, 0x38, 0xc8, 0x00, 0x00,
    0x00};
constexpr std::uintptr_t expected_rva = 0x3b4eb6;

using RatioFn = int* (*)(void* country, int* out, char flag);

Mode mode = Mode::off;
RatioFn original = nullptr;
std::atomic<std::uint64_t> hits{0};
std::atomic<std::uint64_t> misses{0};
std::atomic<std::uint64_t> mismatches{0};
std::atomic<std::uint64_t> bypassed{0};

struct Entry {
    void* country;
    std::uint64_t generation;
    char flag;
    int value;
};
constexpr int table_size = 64;
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

int* hook(void* country, int* out, char flag) {
    if (!flagcache::phase_active() || !country || !out) {
        ++local.bypassed;
        tick();
        return original(country, out, flag);
    }
    const auto current = flagcache::phase_generation();
    const auto key = reinterpret_cast<std::uintptr_t>(country);
    Entry& entry = table[(key >> 4 ^ key >> 11 ^ static_cast<unsigned char>(flag)) &
        (table_size - 1)];
    if (entry.country == country && entry.flag == flag && entry.generation == current) {
        ++local.hits;
        tick();
        if (mode == Mode::on) {
            *out = entry.value;
            return out;
        }
        auto* result = original(country, out, flag);
        if (*result != entry.value) ++local.mismatches;
        return result;
    }
    auto* result = original(country, out, flag);
    entry.country = country;
    entry.flag = flag;
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
        std::snprintf(error, error_size, "country ratio call site bytes differ (rva 0x%llx)",
            static_cast<unsigned long long>(expected_rva));
        return false;
    }
    mode = selected;
    original = reinterpret_cast<RatioFn>(image + eu4build::rva(function_rva));
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

}  // namespace ratiocache
