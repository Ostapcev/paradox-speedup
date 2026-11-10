// ---- provfree: the old province modifier cache freed off the main thread
// Province CalculateModifier 0x9F9500 builds a fresh cache (default ctor 0x95C560, no heap)
// and move-assigns it into province+0x988 with 0x9F95D0. The move frees the old cache's
// buffers on the calling thread: strings, vectors of strings and plain vectors in 26 modifier
// lists (about half of 0x9F95D0's time is std::string frees). Any move-assign destroys the
// destination's old resources, so the hook takes them out first (leaving the empty state the
// game's own move would write) and calls the game's 0x9F95D0, which then has nothing to free.
// Small vectors (+0x50, reused by the game) and vectors of shared pointers (+0xA8) stay with
// the game. Every container is checked for a sane shape first; one bad shape leaves the whole
// call to the game.
//   shadow: the taken buffers are freed right after the call, on the same thread (layout test)
//   on:     they are freed on a background thread
constexpr std::uintptr_t provfree_rva = 0x9F95D0;
constexpr std::uintptr_t provfree_string_free_rva = 0x95660;   // std::string tidy
constexpr std::uintptr_t provfree_strvec_free_rva = 0x94E60;   // vector<string> free
constexpr std::uintptr_t provfree_vec8_free_rva = 0x8EAC0;     // vector<8-byte> free
constexpr std::uintptr_t provfree_vec4_free_rva = 0x943E0;     // vector<4-byte> free
constexpr std::uintptr_t provfree_list_assign_rva = 0x32A150;  // modifier list move (layout check)
constexpr std::uintptr_t provfree_blist_assign_rva = 0x329F70; // list with names move (layout check)

using ProvAssignFn = void* (*)(void* dest, void* src);
using FreeFn = void (*)(void*);
ProvAssignFn provfree_next = nullptr;
FreeFn free_string = nullptr, free_strvec = nullptr, free_vec8 = nullptr, free_vec4 = nullptr;
Stats provfree_stats;
std::atomic<long long> provfree_items{0}, provfree_refused{0}, provfree_bg_ticks{0};

struct FreeItem {
    std::uint8_t kind;  // 0 string, 1 vector<string>, 2 vector<8>, 3 vector<4>
    std::uint64_t a, b, c, d;
};

// The 32-byte MSVC string / 24-byte vector shapes the game's free functions take.
void free_item(const FreeItem& item) {
    alignas(16) std::uint64_t box[4] = {item.a, item.b, item.c, item.d};
    switch (item.kind) {
    case 0: free_string(box); break;
    case 1: free_strvec(box); break;
    case 2: free_vec8(box); break;
    default: free_vec4(box); break;
    }
}

constexpr std::size_t kListSize = 0xC0, kBListSize = 0x178;
constexpr std::size_t kTopString = 0x198;
constexpr std::size_t kAListOffsets[] = {0x10, 0xD8, 0x2090};
constexpr std::size_t kBListFirst = 0x1B8, kBListCount = 21;
constexpr std::size_t kBListExtra[] = {0x2150, 0x22C8};

bool sane_string(const unsigned char* s) {
    const auto size = rd<std::uint64_t>(s, 0x10), cap = rd<std::uint64_t>(s, 0x18);
    return cap >= 15 && size <= cap && cap < (1ull << 40) && (cap == 15 || rd<const void*>(s, 0) != nullptr);
}
template <std::size_t Elem> bool sane_vector(const unsigned char* v) {
    const auto b = rd<std::uintptr_t>(v, 0), e = rd<std::uintptr_t>(v, 8), c = rd<std::uintptr_t>(v, 0x10);
    if (b == 0) return e == 0 && c == 0;
    return b <= e && e <= c && (c - b) % Elem == 0 && (e - b) % Elem == 0 && c - b < (1ull << 36);
}

struct Taker {
    unsigned char* base;
    std::vector<FreeItem>& out;
    bool ok = true;
    void check_string(std::size_t at) { ok = ok && sane_string(base + at); }
    template <std::size_t Elem> void check_vector(std::size_t at) { ok = ok && sane_vector<Elem>(base + at); }
    void take_string(std::size_t at) {
        unsigned char* s = base + at;
        if (rd<std::uint64_t>(s, 0x18) <= 15) return;  // in-place buffer, nothing on the heap
        FreeItem item{0, rd<std::uint64_t>(s, 0), rd<std::uint64_t>(s, 8), rd<std::uint64_t>(s, 0x10), rd<std::uint64_t>(s, 0x18)};
        out.push_back(item);
        const std::uint64_t empty[4] = {0, 0, 0, 15};  // what the game's move leaves: size 0, capacity 15
        std::memcpy(s, empty, sizeof(empty));
    }
    void take_vector(std::size_t at, std::uint8_t kind) {
        unsigned char* v = base + at;
        if (rd<std::uint64_t>(v, 0) == 0) return;
        FreeItem item{kind, rd<std::uint64_t>(v, 0), rd<std::uint64_t>(v, 8), rd<std::uint64_t>(v, 0x10), 0};
        out.push_back(item);
        std::memset(v, 0, 24);
    }
    // a modifier list: string +0x18, vector<8> +0x38 (small vector +0x50, shared vector +0xA8 left)
    void list(std::size_t at, bool take) {
        if (!take) {
            check_string(at + 0x18);
            check_vector<8>(at + 0x38);
        } else {
            take_string(at + 0x18);
            take_vector(at + 0x38, 2);
        }
    }
    // a list with names: list +0, vector<4> +0xC8, vector<string> +0xE0 +0xF8, strings +0x110 +0x130 +0x158
    void blist(std::size_t at, bool take) {
        list(at, take);
        if (!take) {
            check_vector<4>(at + 0xC8);
            check_vector<32>(at + 0xE0);
            check_vector<32>(at + 0xF8);
            check_string(at + 0x110);
            check_string(at + 0x130);
            check_string(at + 0x158);
        } else {
            take_vector(at + 0xC8, 3);
            take_vector(at + 0xE0, 1);
            take_vector(at + 0xF8, 1);
            take_string(at + 0x110);
            take_string(at + 0x130);
            take_string(at + 0x158);
        }
    }
    void all(bool take) {
        if (take) take_string(kTopString); else check_string(kTopString);
        for (std::size_t a : kAListOffsets) list(a, take);
        for (std::size_t k = 0; k < kBListCount; ++k) blist(kBListFirst + k * kBListSize, take);
        for (std::size_t a : kBListExtra) blist(a, take);
    }
};

// background freeing
SRWLOCK provfree_lock = SRWLOCK_INIT;
std::vector<FreeItem>* provfree_queue = nullptr;
HANDLE provfree_event = nullptr;
constexpr std::size_t kProvfreeWakeItems = 1u << 18;
bool provfree_woken = false;  // under provfree_lock: an early wake is pending

DWORD WINAPI provfree_worker(void*) {
    std::vector<FreeItem> batch;
    for (;;) {
        WaitForSingleObject(provfree_event, 50);
        AcquireSRWLockExclusive(&provfree_lock);
        batch.swap(*provfree_queue);
        provfree_woken = false;
        ReleaseSRWLockExclusive(&provfree_lock);
        if (batch.empty()) continue;
        const std::int64_t start = qpc_now();
        for (const auto& item : batch) free_item(item);
        provfree_bg_ticks.fetch_add(qpc_now() - start, std::memory_order_relaxed);
        batch.clear();
    }
}

thread_local std::vector<FreeItem>* provfree_local = nullptr;

void* provfree_hook(void* dest, void* src) {
    Stats& s = provfree_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    if (s.disabled.load(std::memory_order_relaxed) || dest == src) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return provfree_next(dest, src);
    }
    if (!provfree_local) provfree_local = new std::vector<FreeItem>();
    std::vector<FreeItem>& items = *provfree_local;
    items.clear();
    Taker taker{static_cast<unsigned char*>(dest), items};
    taker.all(false);
    if (!taker.ok) {
        if (provfree_refused.fetch_add(1) < 5) log("provfree: a container of %p has an unexpected shape; left to the game", dest);
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return provfree_next(dest, src);
    }
    taker.all(true);
    void* result;
    {
        Timer t(s.original_ticks);
        result = provfree_next(dest, src);
    }
    s.ours.fetch_add(1, std::memory_order_relaxed);
    provfree_items.fetch_add(static_cast<long long>(items.size()), std::memory_order_relaxed);
    if (items.empty()) return result;
    if (s.mode == Mode::shadow) {
        Timer t(s.ticks);
        for (const auto& item : items) free_item(item);
        return result;
    }
    // No SetEvent per call: waking the worker for every province cost more than the frees
    // (profile 2026-11-09). The worker polls every 50 ms; it is woken early only when the
    // queue gets large (a monthly province loop queues ~80k items).
    bool wake;
    {
        Timer t(s.ticks);
        AcquireSRWLockExclusive(&provfree_lock);
        provfree_queue->insert(provfree_queue->end(), items.begin(), items.end());
        wake = provfree_queue->size() >= kProvfreeWakeItems && !provfree_woken;
        if (wake) provfree_woken = true;
        ReleaseSRWLockExclusive(&provfree_lock);
    }
    if (wake) SetEvent(provfree_event);
    return result;
}

void provfree_report() {
    report("provfree", provfree_stats);
    log("provfree items=%lld refused=%lld background_ms=%.1f", provfree_items.load(), provfree_refused.load(),
        ms(provfree_bg_ticks.load()));
}

bool install_provfree(bool hook_it = true) {
    // the move functions must still have the layout read above
    struct Expect {
        std::uintptr_t rva;
        std::initializer_list<std::uint8_t> bytes;
    };
    const Expect expect[] = {
        // 0x9F95D0: list +0x10, list +0xD8, string +0x198, first named list +0x1B8 ... +0x22C8
        {0x9F95F3, {0x48, 0x83, 0xC2, 0x10}},
        {0x9F960B, {0x49, 0x8D, 0x9E, 0xD8, 0x00, 0x00, 0x00}},
        {0x9F962B, {0x48, 0x81, 0xC3, 0xC0, 0x00, 0x00, 0x00}},
        {0x9F9667, {0x49, 0x8D, 0x96, 0xB8, 0x01, 0x00, 0x00}},
        {0x9F97F6, {0x49, 0x8D, 0x96, 0x90, 0x20, 0x00, 0x00}},
        {0x9F9809, {0x49, 0x8D, 0x96, 0x50, 0x21, 0x00, 0x00}},
        {0x9F981C, {0x49, 0x8D, 0x96, 0xC8, 0x22, 0x00, 0x00}},
        // 0x32A150: string +0x18, vector +0x38 freed by 0x8EAC0, small vector +0x50, vector +0xA8
        {0x32A16B, {0x48, 0x8D, 0x7A, 0x18}},
        {0x32A1BF, {0x48, 0x8D, 0x7E, 0x38}},
        {0x32A1CF, {0xE8}},
        {0x32A23F, {0x48, 0x8D, 0xBE, 0xA8, 0x00, 0x00, 0x00}},
        // 0x329F70: vector +0xC8 (0x943E0), +0xE0 +0xF8 (0x94E60), strings +0x110 +0x130 +0x158
        {0x329F9A, {0x48, 0x8D, 0xB7, 0xC8, 0x00, 0x00, 0x00}},
        {0x329FDF, {0x48, 0x8D, 0xB7, 0xE0, 0x00, 0x00, 0x00}},
        {0x32A01B, {0x48, 0x8D, 0xB7, 0xF8, 0x00, 0x00, 0x00}},
        {0x32A057, {0x48, 0x8D, 0xB7, 0x10, 0x01, 0x00, 0x00}},
        {0x32A090, {0x48, 0x8D, 0xB7, 0x30, 0x01, 0x00, 0x00}},
        {0x32A0D0, {0x48, 0x8D, 0xB3, 0x58, 0x01, 0x00, 0x00}},
    };
    for (const auto& e : expect) {
        if (std::memcmp(reinterpret_cast<const void*>(image + e.rva), e.bytes.begin(), e.bytes.size()) != 0) {
            log("provfree REFUSED: bytes differ at 0x%llx", static_cast<unsigned long long>(e.rva));
            return false;
        }
    }
    // the called free functions
    auto call_target = [](std::uintptr_t at) {
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(image + at + 1), 4);
        return at + 5 + rel;
    };
    if (call_target(0x329FB9) != provfree_vec4_free_rva || call_target(0x329FF5) != provfree_strvec_free_rva ||
        call_target(0x32A06D) != provfree_string_free_rva || call_target(0x32A1CF) != provfree_vec8_free_rva ||
        call_target(0x9F95FF) != provfree_list_assign_rva || call_target(0x9F9675) != provfree_blist_assign_rva) {
        log("provfree REFUSED: call targets differ");
        return false;
    }
    free_string = reinterpret_cast<FreeFn>(image + provfree_string_free_rva);
    free_strvec = reinterpret_cast<FreeFn>(image + provfree_strvec_free_rva);
    free_vec8 = reinterpret_cast<FreeFn>(image + provfree_vec8_free_rva);
    free_vec4 = reinterpret_cast<FreeFn>(image + provfree_vec4_free_rva);
    if (!hook_it) return true;  // provreuse: layout checks and free functions only
    if (provfree_stats.mode == Mode::on) {
        provfree_queue = new std::vector<FreeItem>();
        provfree_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE thread = provfree_event ? CreateThread(nullptr, 0, provfree_worker, nullptr, 0, nullptr) : nullptr;
        if (!thread) {
            log("provfree REFUSED: no background thread");
            return false;
        }
        CloseHandle(thread);
    }
    void* next = hook_entry(provfree_rva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10}, 5,
        reinterpret_cast<const void*>(&provfree_hook), "provfree");
    if (!next) return false;
    provfree_next = reinterpret_cast<ProvAssignFn>(next);
    return true;
}
