// Replaces the two child loops of the country AI root 14014b370 (at 14014b622..
// 14014b675: lists root+0x40 and root+0xc50, each child's vtable +0x58 with the
// same arg) by an identical loop in this plugin, so the army AI nodes (vtable
// 141c3cdb8, update 1400e9f30) can be studied and later run differently.
//
// mode probe: before/after each army node, snapshot the root AI object, the
// country and the sibling army nodes, and count which 8-byte words changed.
#include "tree_walk.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace treewalk {
namespace {

constexpr std::uintptr_t site_rva = 0x14b622;
constexpr std::uintptr_t resume_rva = 0x14b675;
constexpr std::uint8_t expected[] = {0x48, 0x8b, 0x5f, 0x40, 0x48, 0x85, 0xdb, 0x74, 0x1a};
constexpr std::uintptr_t army_vtable_rva = 0x1c3cdb8;
constexpr std::uintptr_t country_table_global_rva = 0x233d8d0;  // [+0x118] = Country*[]

constexpr std::size_t root_size = 0x1000;
constexpr std::size_t country_size = 0x3000;
constexpr std::size_t army_size = 0xb80;
constexpr int words_root = root_size / 8, words_country = country_size / 8, words_army = army_size / 8;

std::uintptr_t image = 0;
std::uintptr_t army_vtable = 0;
Mode mode = Mode::off;

using NodeFn = void (*)(void* node, void* arg);

struct ListNode {
    void* object;
    void* prev;
    ListNode* next;
};

std::atomic<std::uint32_t> changed_root[words_root];
std::atomic<std::uint32_t> changed_country[words_country];
std::atomic<std::uint32_t> changed_self[words_army];
std::atomic<std::uint32_t> changed_sibling[words_army];
std::atomic<std::uint64_t> probes{0}, sibling_probes{0}, armies_per_root{0}, roots{0};
std::atomic<std::uint64_t> runs_hist[8];  // consecutive army runs by length (7 = 7+)
constexpr std::uint64_t probe_limit = 20000;

void* country_of(void* root) {
    const auto index = *reinterpret_cast<const short*>(static_cast<std::uint8_t*>(root) + 0x2c);
    auto* global = *reinterpret_cast<std::uint8_t**>(image + country_table_global_rva);
    if (!global || index < 0) return nullptr;
    auto** table = *reinterpret_cast<void***>(global + 0x118);
    return table ? table[index] : nullptr;
}

void diff(const std::uint8_t* before, const std::uint8_t* after, int words,
    std::atomic<std::uint32_t>* counts) {
    for (int i = 0; i < words; ++i) {
        if (std::memcmp(before + i * 8, after + i * 8, 8) != 0) {
            counts[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void call_node(void* object, void* arg) {
    auto** vtable = *static_cast<void***>(object);
    reinterpret_cast<NodeFn>(vtable[0x58 / 8])(object, arg);
}

void probe_army(void* root, void* object, void* arg, const std::vector<void*>& siblings) {
    std::vector<std::uint8_t> root_before(root_size), country_before(country_size),
        self_before(army_size), sib_before(siblings.size() * army_size);
    void* country = country_of(root);
    std::memcpy(root_before.data(), root, root_size);
    if (country) std::memcpy(country_before.data(), country, country_size);
    std::memcpy(self_before.data(), object, army_size);
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        std::memcpy(&sib_before[i * army_size], siblings[i], army_size);
    }
    call_node(object, arg);
    diff(root_before.data(), static_cast<std::uint8_t*>(root), words_root, changed_root);
    if (country) diff(country_before.data(), static_cast<std::uint8_t*>(country), words_country,
        changed_country);
    diff(self_before.data(), static_cast<std::uint8_t*>(object), words_army, changed_self);
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        diff(&sib_before[i * army_size], static_cast<std::uint8_t*>(siblings[i]), words_army,
            changed_sibling);
        sibling_probes.fetch_add(1, std::memory_order_relaxed);
    }
    probes.fetch_add(1, std::memory_order_relaxed);
}

// ---- parallel army runs
// Removal/finish methods of the root and army nodes may unlink list nodes; they
// run under one recursive lock so two army nodes never edit a list at once.
CRITICAL_SECTION structure_lock;
thread_local int in_batch = 0;
std::atomic<std::uint64_t> structure_calls_in_batch{0}, batches{0}, batch_items{0},
    helper_items{0};

struct Wrapped {
    std::uintptr_t slot_rva;  // vtable slot
    void* original;
};
// army vtable 141c3cdb8 +0x190 (1400e46d0) +0x198 (1400e4710);
// root vtable 141c3e9e0 +0x178 (140148940) +0x208 (14014a160) +0x210 (14014a1d0)
Wrapped wrapped[5] = {{0x1c3cdb8 + 0x190, nullptr}, {0x1c3cdb8 + 0x198, nullptr},
    {0x1c3e9e0 + 0x178, nullptr}, {0x1c3e9e0 + 0x208, nullptr}, {0x1c3e9e0 + 0x210, nullptr}};
const std::uintptr_t wrapped_expected[5] = {0xe46d0, 0xe4710, 0x148940, 0x14a160, 0x14a1d0};

using Fn2 = std::uint64_t (*)(void*, void*);
template <int I>
std::uint64_t locked(void* a, void* b) {
    if (in_batch) structure_calls_in_batch.fetch_add(1, std::memory_order_relaxed);
    EnterCriticalSection(&structure_lock);
    const auto r = reinterpret_cast<Fn2>(wrapped[I].original)(a, b);
    LeaveCriticalSection(&structure_lock);
    return r;
}
void* const locked_fns[5] = {reinterpret_cast<void*>(&locked<0>),
    reinterpret_cast<void*>(&locked<1>), reinterpret_cast<void*>(&locked<2>),
    reinterpret_cast<void*>(&locked<3>), reinterpret_cast<void*>(&locked<4>)};

struct Batch {
    void* const* objects;
    int count;
    void* arg;
    void* sink;
    std::atomic<int> next{0};
    std::atomic<int> done{0};
};
SRWLOCK batches_lock = SRWLOCK_INIT;
constexpr int max_active = 64;
Batch* active[max_active];
int active_count = 0;
HANDLE wake = nullptr;
int helper_count = 0;

// The game's AI task body 140786150 sets two tbb::enumerable_thread_specific values
// around each country task: a depth counter (ETS 141fd5fa8) and a sink pointer
// (ETS 141fd1ab8) that AI code checks (e.g. 14098c19a, 1403888e0). Items on other
// threads must see the same values as the owner thread.
using EtsLocalFn = void* (*)(void* ets, bool* exists);
EtsLocalFn ets_local = nullptr;
void* ets_depth = nullptr;
void* ets_sink = nullptr;
std::atomic<std::uint64_t> serial_no_context{0};

void run_item(void* object, void* arg, void* sink) {
    bool exists = false;
    auto* depth = static_cast<int*>(ets_local(ets_depth, &exists));
    auto** slot = static_cast<void**>(ets_local(ets_sink, &exists));
    ++*depth;
    void* const old = *slot;
    *slot = sink;
    ++in_batch;
    call_node(object, arg);
    --in_batch;
    --*depth;
    *slot = old;
}

// take one item of any active batch; false if none left
bool help_once() {
    void* object = nullptr;
    void* arg = nullptr;
    void* sink = nullptr;
    Batch* owner = nullptr;
    AcquireSRWLockShared(&batches_lock);
    for (int b = 0; b < active_count && !owner; ++b) {
        Batch* batch = active[b];
        if (batch->next.load(std::memory_order_relaxed) >= batch->count) continue;
        const int i = batch->next.fetch_add(1, std::memory_order_acq_rel);
        if (i < batch->count) {
            owner = batch;
            object = batch->objects[i];
            arg = batch->arg;
            sink = batch->sink;
        }
    }
    ReleaseSRWLockShared(&batches_lock);
    if (!owner) return false;
    run_item(object, arg, sink);
    helper_items.fetch_add(1, std::memory_order_relaxed);
    owner->done.fetch_add(1, std::memory_order_acq_rel);  // owner may return after this
    return true;
}

DWORD WINAPI helper(void*) {
    for (;;) {
        WaitForSingleObject(wake, INFINITE);
        while (help_once()) {
        }
    }
}

void run_batch(void* const* objects, int count, void* arg, void* sink) {
    Batch batch;
    batch.objects = objects;
    batch.count = count;
    batch.arg = arg;
    batch.sink = sink;
    AcquireSRWLockExclusive(&batches_lock);
    const bool listed = active_count < max_active;
    if (listed) active[active_count++] = &batch;
    ReleaseSRWLockExclusive(&batches_lock);
    if (listed && helper_count > 0) {
        ReleaseSemaphore(wake, (std::min)(count - 1, helper_count), nullptr);
    }
    for (;;) {
        const int i = batch.next.fetch_add(1, std::memory_order_acq_rel);
        if (i >= count) break;
        run_item(objects[i], arg, sink);
        batch.done.fetch_add(1, std::memory_order_acq_rel);
    }
    for (int spins = 0; batch.done.load(std::memory_order_acquire) < count; ++spins) {
        if (spins < 4000) YieldProcessor(); else SwitchToThread();
    }
    if (listed) {
        AcquireSRWLockExclusive(&batches_lock);
        for (int b = 0; b < active_count; ++b) {
            if (active[b] == &batch) {
                active[b] = active[--active_count];
                break;
            }
        }
        ReleaseSRWLockExclusive(&batches_lock);
    }
    batches.fetch_add(1, std::memory_order_relaxed);
    batch_items.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
}

// ---- batch on the game's own job system (nested parallel-for, like 14077d8a0)
// context (0x40): vtable 141c41660, +8 byte* from operator new(1), +0x10/+0x18
// shared state, +0x20 mode (2 = destructor waits), +0x28.. vector; submit
// 1401798a0(ctx, -, begin, end, std::function* body, std::function* on_done);
// destructor 1401797d0 waits (the calling thread runs items too) and frees.
using GameSubmitFn = void (*)(void* ctx, void* unused, int begin, int end, void* body,
    void* on_done);
using GameDtorFn = void (*)(void* ctx);
using GameNewFn = void* (*)(std::size_t);
GameSubmitFn game_submit = nullptr;
GameDtorFn game_ctx_dtor = nullptr;
GameNewFn game_new = nullptr;
void* game_ctx_vtable = nullptr;
bool use_game_pool = false;
std::atomic<std::uint64_t> game_batches{0};

// MSVC std::function _Func_base: _Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get
struct BodyImpl {
    void* const* vtable;
    void* const* objects;
    void* arg;
    void* sink;
};
void* body_copy(BodyImpl* self, void* where) {
    auto* copy = static_cast<BodyImpl*>(where);
    *copy = *self;
    return copy;
}
void body_call(BodyImpl* self, int* index) {
    run_item(self->objects[*index], self->arg, self->sink);
}
const void* body_type(BodyImpl*) { return nullptr; }
void body_delete(BodyImpl*, bool) {}
void* body_get(BodyImpl* self) { return self; }
void* const body_vtable[6] = {reinterpret_cast<void*>(&body_copy),
    reinterpret_cast<void*>(&body_copy), reinterpret_cast<void*>(&body_call),
    reinterpret_cast<void*>(&body_type), reinterpret_cast<void*>(&body_delete),
    reinterpret_cast<void*>(&body_get)};

void run_batch_game(void* const* objects, int count, void* arg, void* sink) {
    alignas(16) std::uint8_t body[0x40]{};
    alignas(16) std::uint8_t on_done[0x40]{};  // empty std::function (impl at +0x38 = null)
    alignas(16) std::uint8_t ctx[0x40]{};
    auto* impl = reinterpret_cast<BodyImpl*>(body);
    impl->vtable = body_vtable;
    impl->objects = objects;
    impl->arg = arg;
    impl->sink = sink;
    *reinterpret_cast<void**>(body + 0x38) = body;
    *reinterpret_cast<void**>(ctx) = game_ctx_vtable;
    auto* flag = static_cast<std::uint8_t*>(game_new(1));
    *flag = 0;
    *reinterpret_cast<void**>(ctx + 8) = flag;
    *reinterpret_cast<int*>(ctx + 0x20) = 2;
    game_submit(ctx, nullptr, 0, count, body, on_done);
    game_ctx_dtor(ctx);  // waits for all items, frees the context parts
    game_batches.fetch_add(1, std::memory_order_relaxed);
    batches.fetch_add(1, std::memory_order_relaxed);
    batch_items.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
}

bool contains(const std::vector<void*>& done, void* object) {
    for (void* d : done) if (d == object) return true;
    return false;
}

// parallel: consecutive army nodes (2+) run as one batch; after a batch the list
// is walked again from its head (a node may have been unlinked meanwhile),
// skipping objects already updated in this walk.
void walk_list_parallel(ListNode** head, void* arg) {
    // owner thread's AI task context; without it (not inside a country task) stay serial
    bool exists = false;
    const int depth = *static_cast<int*>(ets_local(ets_depth, &exists));
    void* const sink = *static_cast<void**>(ets_local(ets_sink, &exists));
    const bool batching = depth > 0 && sink != nullptr;
    if (!batching) serial_no_context.fetch_add(1, std::memory_order_relaxed);
    std::vector<void*> done;
    std::vector<void*> run;
    ListNode* node = *head;
    while (node) {
        void* object = node->object;
        if (contains(done, object)) {
            node = node->next;
            continue;
        }
        run.clear();
        for (auto* n = node; n && *static_cast<std::uintptr_t*>(n->object) == army_vtable &&
             !contains(done, n->object); n = n->next) {
            run.push_back(n->object);
        }
        if (run.size() < 2 || !batching) {
            ListNode* next = node->next;  // like the game
            call_node(object, arg);
            done.push_back(object);
            node = next;
            continue;
        }
        if (use_game_pool) run_batch_game(run.data(), static_cast<int>(run.size()), arg, sink);
        else run_batch(run.data(), static_cast<int>(run.size()), arg, sink);
        done.insert(done.end(), run.begin(), run.end());
        node = *head;  // restart: the list may have changed during the batch
    }
}

void walk_list(void* root, ListNode* node, void* arg) {
    // collect the army nodes of this list (for probing siblings and run lengths)
    std::vector<void*> armies;
    int run = 0;
    for (auto* n = node; n; n = n->next) {
        if (*static_cast<std::uintptr_t*>(n->object) == army_vtable) {
            armies.push_back(n->object);
            ++run;
        } else if (run) {
            runs_hist[run < 7 ? run : 7].fetch_add(1, std::memory_order_relaxed);
            run = 0;
        }
    }
    if (run) runs_hist[run < 7 ? run : 7].fetch_add(1, std::memory_order_relaxed);
    armies_per_root.fetch_add(armies.size(), std::memory_order_relaxed);
    const bool probe = mode == Mode::probe && probes.load(std::memory_order_relaxed) < probe_limit;
    while (node) {
        void* object = node->object;
        ListNode* next = node->next;  // read before the call, like the game
        if (probe && *static_cast<std::uintptr_t*>(object) == army_vtable) {
            std::vector<void*> siblings;
            for (void* other : armies) if (other != object) siblings.push_back(other);
            probe_army(root, object, arg, siblings);
        } else {
            call_node(object, arg);
        }
        node = next;
    }
}

void walk(void* root, void* arg) {
    auto* r = static_cast<std::uint8_t*>(root);
    roots.fetch_add(1, std::memory_order_relaxed);
    if (mode == Mode::parallel) {
        walk_list_parallel(reinterpret_cast<ListNode**>(r + 0x40), arg);
        walk_list_parallel(reinterpret_cast<ListNode**>(r + 0xc50), arg);
        return;
    }
    walk_list(root, *reinterpret_cast<ListNode**>(r + 0x40), arg);
    walk_list(root, *reinterpret_cast<ListNode**>(r + 0xc50), arg);
}

void report_counts(void (*log)(const char*, ...) noexcept, const char* name,
    std::atomic<std::uint32_t>* counts, int words, std::uint64_t total) {
    char line[1200];
    int at = std::snprintf(line, sizeof(line), "tree_walk changed %s (of %llu):", name,
        static_cast<unsigned long long>(total));
    int shown = 0;
    for (int i = 0; i < words && at < 1150; ++i) {
        const auto c = counts[i].load();
        if (!c) continue;
        at += std::snprintf(line + at, sizeof(line) - at, " +%x:%u", i * 8, c);
        ++shown;
    }
    if (!shown) at += std::snprintf(line + at, sizeof(line) - at, " none");
    log("%s", line);
}

}  // namespace

bool install(std::uintptr_t base, Mode selected, int helpers, char* error,
    std::size_t error_size) {
    if (selected == Mode::off) return true;
    image = base;
    if (selected == Mode::parallel) {
        for (int i = 0; i < 5; ++i) {
            auto** slot = reinterpret_cast<void**>(base + wrapped[i].slot_rva);
            if (*slot != reinterpret_cast<void*>(base + wrapped_expected[i])) {
                std::snprintf(error, error_size, "tree walk: vtable slot %d differs", i);
                return false;
            }
        }
        // ETS local() 1400df320 (calls tbb thread_get_id) and the two ETS objects
        static const std::uint8_t ets_start[] = {0x40, 0x53, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
        static const std::uint8_t body_ets[] = {0x48, 0x8d, 0x0d, 0x25, 0xfe, 0x84, 0x01};
        if (std::memcmp(reinterpret_cast<void*>(base + 0xdf320), ets_start, sizeof(ets_start)) ||
            std::memcmp(reinterpret_cast<void*>(base + 0x78617c), body_ets, sizeof(body_ets))) {
            std::snprintf(error, error_size, "tree walk: AI task context code differs");
            return false;
        }
        ets_local = reinterpret_cast<EtsLocalFn>(base + 0xdf320);
        ets_depth = reinterpret_cast<void*>(base + 0x1fd5fa8);
        ets_sink = reinterpret_cast<void*>(base + 0x1fd1ab8);
        InitializeCriticalSection(&structure_lock);
        // helpers < 0: run batches on the game's job system
        static const std::uint8_t ctx_dtor_start[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89,
            0x74, 0x24, 0x10, 0x57};
        if (helpers < 0) {
            if (std::memcmp(reinterpret_cast<void*>(base + 0x1797d0), ctx_dtor_start,
                    sizeof(ctx_dtor_start)) != 0 ||
                *reinterpret_cast<std::uint32_t*>(base + 0x1797e5) != 0x1ac7e77) {
                std::snprintf(error, error_size, "tree walk: job context code differs");
                return false;
            }
            game_submit = reinterpret_cast<GameSubmitFn>(base + 0x1798a0);
            game_ctx_dtor = reinterpret_cast<GameDtorFn>(base + 0x1797d0);
            game_new = reinterpret_cast<GameNewFn>(base + 0x1a332d4);
            game_ctx_vtable = reinterpret_cast<void*>(base + 0x1c41660);
            use_game_pool = true;
            helpers = 0;
        }
        wake = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
        if (!wake) {
            std::snprintf(error, error_size, "tree walk: semaphore");
            return false;
        }
        for (int i = 0; i < helpers; ++i) {
            HANDLE thread = CreateThread(nullptr, 1 << 20, helper, nullptr,
                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (thread) {
                CloseHandle(thread);
                ++helper_count;
            }
        }
        for (int i = 0; i < 5; ++i) {
            auto** slot = reinterpret_cast<void**>(base + wrapped[i].slot_rva);
            wrapped[i].original = *slot;
            DWORD old = 0;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            *slot = locked_fns[i];
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
    }
    army_vtable = base + army_vtable_rva;
    auto* site = reinterpret_cast<std::uint8_t*>(base + site_rva);
    if (std::memcmp(site, expected, sizeof(expected)) != 0) {
        std::snprintf(error, error_size, "tree walk site bytes differ");
        return false;
    }
    mode = selected;
    std::uint8_t code[23];
    const auto target = reinterpret_cast<std::uint64_t>(&walk);
    code[0] = 0x48; code[1] = 0x89; code[2] = 0xf9;   // mov rcx, rdi
    code[3] = 0x48; code[4] = 0x89; code[5] = 0xea;   // mov rdx, rbp
    code[6] = 0x48; code[7] = 0xb8;                   // mov rax, imm64
    std::memcpy(code + 8, &target, 8);
    code[16] = 0xff; code[17] = 0xd0;                 // call rax
    code[18] = 0xe9;                                  // jmp resume
    const auto rel = static_cast<std::int32_t>(
        (base + resume_rva) - (base + site_rva + 23));
    std::memcpy(code + 19, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
        std::snprintf(error, error_size, "tree walk VirtualProtect %lu", GetLastError());
        return false;
    }
    std::memcpy(site, code, sizeof(code));
    VirtualProtect(site, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(code));
    return true;
}

void report(void (*log)(const char*, ...) noexcept) {
    if (mode == Mode::off) return;
    const auto r = roots.load();
    log("tree_walk roots=%llu armies_per_root=%.2f runs 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7+:%llu probes=%llu",
        static_cast<unsigned long long>(r), r ? static_cast<double>(armies_per_root.load()) / r : 0.0,
        static_cast<unsigned long long>(runs_hist[1].load()),
        static_cast<unsigned long long>(runs_hist[2].load()),
        static_cast<unsigned long long>(runs_hist[3].load()),
        static_cast<unsigned long long>(runs_hist[4].load()),
        static_cast<unsigned long long>(runs_hist[5].load()),
        static_cast<unsigned long long>(runs_hist[6].load()),
        static_cast<unsigned long long>(runs_hist[7].load()),
        static_cast<unsigned long long>(probes.load()));
    if (mode == Mode::parallel) {
        log("tree_walk parallel game_pool=%d helpers=%d batches=%llu items=%llu by_helpers=%llu "
            "structure_calls_in_batch=%llu serial_no_context=%llu", use_game_pool ? 1 : 0, helper_count,
            static_cast<unsigned long long>(batches.load()),
            static_cast<unsigned long long>(batch_items.load()),
            static_cast<unsigned long long>(helper_items.load()),
            static_cast<unsigned long long>(structure_calls_in_batch.load()),
            static_cast<unsigned long long>(serial_no_context.load()));
    }
    if (mode != Mode::probe) return;
    report_counts(log, "root", changed_root, words_root, probes.load());
    report_counts(log, "country", changed_country, words_country, probes.load());
    report_counts(log, "self", changed_self, words_army, probes.load());
    report_counts(log, "sibling", changed_sibling, words_army, sibling_probes.load());
}

const char* mode_name() {
    return mode == Mode::probe ? "probe" : mode == Mode::serial ? "serial" :
        mode == Mode::parallel ? "parallel" : "off";
}

}  // namespace treewalk
