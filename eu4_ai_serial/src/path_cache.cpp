// Cache for the map pathfinder 14096c520 (map.h), ~21% of the heaviest daily AI
// task. Hooked at the army AI call sites in 140106320 (3) and 140109ee0 (2).
//
// char 14096c520(Policy* policy, Unit* unit, Province* from, Province* to,
//                PathList* path, Options* options, int scratch, char* out8)
//   policy:  8 bytes (vtable 141c3af98) or 16 bytes (vtable 141c3af78 + flag byte)
//   path:    list {head, tail, int count, byte}, filled by push 1400df210 (int id)
//   options: 0x41 bytes read by the policy callbacks
// The result is cached per thread for the exact inputs, only inside the daily AI
// phase (flagcache::begin_phase..end_phase) and only when path starts empty and
// out8 is null. Shadow: the original always runs; repeats are compared (return,
// path ids and node bytes, path flag, options after the call).
#include "path_cache.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "call_patch.hpp"

namespace pathcache {
namespace {

constexpr std::uintptr_t function_rva = 0x96c520;
constexpr std::uintptr_t site_rvas[] = {0x10671e, 0x106c3d, 0x107194, 0x10a035, 0x10a0f7};
constexpr int site_count = sizeof(site_rvas) / sizeof(site_rvas[0]);
constexpr std::uintptr_t policy_base_vtable_rva = 0x1c3af98;     // 8 bytes
constexpr std::uintptr_t policy_flag_vtable_rva = 0x1c3af78;     // 16 bytes
constexpr std::uintptr_t push_rva = 0xdf210;
constexpr std::size_t options_size = 0x41;
constexpr int max_ids = 40;

struct PathList {
    struct Node {
        int id;
        int pad;
        Node* prev;
        Node* next;
        std::uint8_t flag;
    };
    Node* head;
    Node* tail;
    int count;
    std::uint8_t flag;
};

using PathFn = char (*)(void* policy, void* unit, void* from, void* to, PathList* path,
    std::uint8_t* options, int scratch, char* out8);
using PushFn = void (*)(PathList* path, const int* id);

Mode mode = Mode::off;
PathFn original = nullptr;
PushFn push = nullptr;
std::uintptr_t vt_base = 0, vt_flag = 0;

std::atomic<std::uint64_t> hits{0}, misses{0}, mismatches{0}, bypassed{0};

struct Entry {
    std::uint64_t generation;
    std::uintptr_t vtable;
    void* unit;
    void* from;
    void* to;
    std::uint8_t policy_flag;
    char result;
    std::uint8_t path_flag;
    std::uint8_t count;
    std::uint8_t options[options_size];
    std::uint8_t options_after[options_size];
    int ids[max_ids];
    std::uint8_t node_flags[max_ids];
};
constexpr int table_bits = 11;
constexpr int table_size = 1 << table_bits;

struct Local {
    Entry* table = nullptr;
    std::uint32_t hits = 0, misses = 0, mismatches = 0, bypassed = 0, pending = 0;
};
thread_local Local local;

void flush() {
    hits.fetch_add(local.hits, std::memory_order_relaxed);
    misses.fetch_add(local.misses, std::memory_order_relaxed);
    mismatches.fetch_add(local.mismatches, std::memory_order_relaxed);
    bypassed.fetch_add(local.bypassed, std::memory_order_relaxed);
    local.hits = local.misses = local.mismatches = local.bypassed = local.pending = 0;
}
inline void tick() {
    if (++local.pending >= 256) flush();
}

inline std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

// fill entry output from a finished call; false if it cannot be cached
bool capture(Entry& e, char result, const PathList* path, const std::uint8_t* options) {
    if (path->count < 0 || path->count > max_ids) return false;
    int n = 0;
    for (auto* node = path->head; node; node = node->next) {
        if (n >= max_ids) return false;
        e.ids[n] = node->id;
        e.node_flags[n] = node->flag;
        ++n;
    }
    if (n != path->count) return false;
    e.count = static_cast<std::uint8_t>(n);
    e.result = result;
    e.path_flag = path->flag;
    std::memcpy(e.options_after, options, options_size);
    return true;
}

bool same_output(const Entry& e, char result, const PathList* path, const std::uint8_t* options) {
    if (result != e.result || path->count != e.count || path->flag != e.path_flag ||
        std::memcmp(options, e.options_after, options_size) != 0) {
        return false;
    }
    int n = 0;
    for (auto* node = path->head; node; node = node->next, ++n) {
        if (n >= e.count || node->id != e.ids[n] || node->flag != e.node_flags[n]) return false;
    }
    return n == e.count;
}

char hook(void* policy, void* unit, void* from, void* to, PathList* path, std::uint8_t* options,
    int scratch, char* out8) {
    const auto vtable = policy ? *static_cast<std::uintptr_t*>(policy) : 0;
    if (!flagcache::phase_active() || !path || !options || out8 || path->count != 0 ||
        path->head || (vtable != vt_base && vtable != vt_flag)) {
        ++local.bypassed;
        tick();
        return original(policy, unit, from, to, path, options, scratch, out8);
    }
    if (!local.table) {
        local.table = static_cast<Entry*>(VirtualAlloc(nullptr, sizeof(Entry) * table_size,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!local.table) return original(policy, unit, from, to, path, options, scratch, out8);
    }
    const std::uint8_t policy_flag =
        vtable == vt_flag ? static_cast<const std::uint8_t*>(policy)[8] : 0;
    const auto generation = flagcache::phase_generation();
    std::uint64_t h = vtable;
    h = mix(h, reinterpret_cast<std::uintptr_t>(unit));
    h = mix(h, reinterpret_cast<std::uintptr_t>(from));
    h = mix(h, reinterpret_cast<std::uintptr_t>(to));
    h = mix(h, policy_flag);
    for (std::size_t i = 0; i + 8 <= options_size; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, options + i, 8);
        h = mix(h, w);
    }
    h = mix(h, options[options_size - 1]);
    Entry& e = local.table[(h * 0x9e3779b97f4a7c15ull) >> (64 - table_bits)];
    const bool same = e.generation == generation && e.vtable == vtable && e.unit == unit &&
        e.from == from && e.to == to && e.policy_flag == policy_flag &&
        std::memcmp(e.options, options, options_size) == 0;
    if (same) {
        ++local.hits;
        tick();
        if (mode == Mode::on) {
            for (int i = 0; i < e.count; ++i) {
                push(path, &e.ids[i]);
                path->tail->flag = e.node_flags[i];
            }
            path->flag = e.path_flag;
            std::memcpy(options, e.options_after, options_size);
            return e.result;
        }
        const char result = original(policy, unit, from, to, path, options, scratch, out8);
        if (!same_output(e, result, path, options)) ++local.mismatches;
        return result;
    }
    std::uint8_t before[options_size];
    std::memcpy(before, options, options_size);
    const char result = original(policy, unit, from, to, path, options, scratch, out8);
    ++local.misses;
    tick();
    if (capture(e, result, path, options)) {
        e.generation = generation;
        e.vtable = vtable;
        e.unit = unit;
        e.from = from;
        e.to = to;
        e.policy_flag = policy_flag;
        std::memcpy(e.options, before, options_size);
    } else {
        e.generation = 0;  // not cacheable (long path)
    }
    return result;
}

speedpatch::CallPatch patches[site_count];

}  // namespace

bool install(std::uintptr_t image, Mode selected, char* error, std::size_t error_size) {
    if (selected == Mode::off) return true;
    original = reinterpret_cast<PathFn>(image + function_rva);
    push = reinterpret_cast<PushFn>(image + push_rva);
    vt_base = image + policy_base_vtable_rva;
    vt_flag = image + policy_flag_vtable_rva;
    mode = selected;
    int prepared = 0;
    for (; prepared < site_count; ++prepared) {
        if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + site_rvas[prepared]),
                reinterpret_cast<const void*>(original), reinterpret_cast<const void*>(&hook),
                patches[prepared], error, error_size)) {
            break;
        }
    }
    if (prepared != site_count) {
        for (int i = 0; i < prepared; ++i) speedpatch::discard_call(patches[i]);
        mode = Mode::off;
        return false;
    }
    for (int i = 0; i < site_count; ++i) {
        if (!speedpatch::commit_call(patches[i], error, error_size)) {
            for (int j = 0; j < i; ++j) speedpatch::rollback_call(patches[j]);
            for (int j = i; j < site_count; ++j) speedpatch::discard_call(patches[j]);
            mode = Mode::off;
            return false;
        }
    }
    return true;
}

Stats stats() { return Stats{hits.load(), misses.load(), mismatches.load(), bypassed.load()}; }

const char* mode_name() {
    return mode == Mode::on ? "on" : mode == Mode::shadow ? "shadow" : "off";
}

}  // namespace pathcache
