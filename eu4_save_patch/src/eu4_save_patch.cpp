// EU4 Save Patch 0.1.0 experimental, for EU4 1.37.5.0 Windows x64.
//
// The game serializes the whole state into an in-memory stream on the main
// thread (14074ff30). 91% of that time is two loops: one write per province
// (call [vtable+8] at 140751709) and one per country (141707a20 at 140751795).
// The game is stopped while it saves, so the state can be read from several
// threads. On the first object of each loop this plugin lets worker threads
// write every object into its own recording stream (a copy of the writer
// with the same depth), then the loop replays each recording into the real
// stream with the very same stream calls the original would make.
//
// A recording that needed anything other than plain writes (stream position
// queries etc.) is not used: that object is written by the original code.
//
// Modes (eu4_save_patch.ini, [save] mode=):
//   off    - nothing is patched
//   shadow - parallel recordings are made, but the game writes as usual; every
//            object is also recorded serially and compared (mismatch counters)
//   on     - recordings are replayed into the real stream
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "call_patch.hpp"

namespace {

enum class Mode { off, shadow, on };

// gamestate global and layout used by the loops in 14074ff30
constexpr std::uintptr_t gamestate_rva = 0x233fe78;
constexpr std::size_t province_base = 0x1ca8;   // inline provinces, stride 0x2e10
constexpr std::size_t province_begin = 0x1c90;  // pointer vector, count = (end-begin)/8
constexpr std::size_t province_end = 0x1c98;
constexpr std::size_t province_stride = 0x2e10;
constexpr std::size_t country_array = 0x1ce8;
// province vtable 141cd6640: slot +8 = 140d2f410 (block write)
constexpr std::uintptr_t province_slot_rva = 0x1cd6648;
constexpr std::uintptr_t block_write_rva = 0xd2f410;
constexpr std::uintptr_t province_return_rva = 0x75170c;  // after call [rax+8]
// mov r8,rbx; lea rdx,[rsp+0x30]; mov rcx,rsi; call 141707a20
constexpr std::uintptr_t country_site_rva = 0x75178a;
constexpr std::uint8_t country_bytes[] = {0x4c, 0x8b, 0xc3, 0x48, 0x8d, 0x54, 0x24, 0x30, 0x48,
    0x8b, 0xce, 0xe8, 0x86, 0x62, 0xfb, 0x00};
constexpr std::uintptr_t country_call_rva = 0x751795;
constexpr std::uintptr_t country_write_rva = 0x1707a20;
constexpr std::uintptr_t string_from_cstr_rva = 0x95240;  // 140095240(string*, const char*)
constexpr std::uintptr_t string_free_rva = 0x95660;       // 140095660(string*)
constexpr std::uintptr_t writer_vtable_rva = 0x1dbf7d8;
constexpr std::size_t writer_size = 0x20;  // vtable, depth, stream, flags

using BlockWriteFn = void (*)(void* object, void* writer);
using CountryWriteFn = void (*)(void* writer, void* tag_string, void* country);
using StringFn = void* (*)(void* string, const void* text);
using StringFreeFn = void (*)(void* string);
using GetCountFn = int (*)(void* gamestate);

std::uintptr_t image = 0;
Mode mode = Mode::off;
BlockWriteFn block_write = nullptr;
CountryWriteFn country_write = nullptr;
StringFn string_from_cstr = nullptr;
StringFreeFn string_free = nullptr;

std::wstring log_path;
std::mutex log_mutex;
std::atomic<LONG> installation_status{0};
std::int64_t qpc_frequency = 1;

void log(const char* format, ...) noexcept {
    char text[700];
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

inline std::int64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
inline double ms(std::int64_t ticks) { return 1000.0 * static_cast<double>(ticks) / qpc_frequency; }

// ---- recording stream: same vtable layout as the game's stream (slots 7..10 write)
enum Op : std::uint8_t { op_cstr = 1, op_int = 2, op_byte = 3, op_raw = 4 };

struct Recording {
    std::vector<char> data;
    std::uint8_t writer_after[writer_size];
    bool unsupported = false;
    bool ready = false;
};

// The game stream is a seekable memory stream: +0x4c cursor, slots 6/12/13/14
// seek, 16 tell, 17 size. Writes (slots 7..9) all end in slot 10 with strlen/4/1
// bytes, except slot 7 in transcoding mode 2. The recording keeps the logical
// length so tell/size answer as the real stream would relative to the object's
// start; the save code only uses tell deltas (bytes written by a block).
struct RecStream {
    void** vtable;
    std::uint64_t reserved;
    int mode;  // +0x10, copied from the real stream
    int pad;
    std::uint8_t rest[0x58 - 0x18];
    Recording* out;
    std::int64_t length;     // logical bytes written so far
    bool length_uncertain;   // a mode-2 string with non-ASCII bytes
    bool position_used;
};

void put(Recording* r, Op op, const void* bytes, std::uint32_t size) {
    auto& d = r->data;
    const auto at = d.size();
    d.resize(at + 1 + 4 + size);
    d[at] = static_cast<char>(op);
    std::memcpy(&d[at + 1], &size, 4);
    if (size) std::memcpy(&d[at + 5], bytes, size);
}

void check_position(RecStream* s) {
    s->position_used = true;
    if (s->length_uncertain) s->out->unsupported = true;
}

std::atomic<int> date_traces{0};
[[maybe_unused]] void trace_date(const char* text) {
    // debug: log the call stack of the first few date-looking strings
    int dots = 0;
    for (const char* c = text; *c; ++c) {
        if (*c == '.') ++dots;
        else if (*c < '0' || *c > '9') return;
    }
    if (dots != 2 || date_traces.fetch_add(1) >= 3) return;
    void* frames[12]{};
    const auto n = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
    char line[400];
    int at = std::snprintf(line, sizeof(line), "DATE %s:", text);
    for (int i = 0; i < n && at < 380; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
        at += std::snprintf(line + at, sizeof(line) - at, " %llx",
            static_cast<unsigned long long>(a >= image && a < image + 0x3000000 ? a - image + 0x140000000 : a));
    }
    log("%s", line);
}

void rs_cstr(RecStream* s, const char* text) {
    // trace_date(text);  // debug: call stacks of date strings
    const auto n = static_cast<std::uint32_t>(std::strlen(text));
    if (s->mode == 2) {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (static_cast<unsigned char>(text[i]) >= 0x80) {
                s->length_uncertain = true;
                if (s->position_used) s->out->unsupported = true;
                break;
            }
        }
    }
    put(s->out, op_cstr, text, n + 1);  // keep the terminator for replay
    s->length += n;
}
void rs_int(RecStream* s, std::uint32_t value) {
    put(s->out, op_int, &value, 4);
    s->length += 4;
}
void rs_byte(RecStream* s, char value) {
    put(s->out, op_byte, &value, 1);
    s->length += 1;
}
void rs_raw(RecStream* s, const char* bytes, std::int64_t size) {
    put(s->out, op_raw, bytes, static_cast<std::uint32_t>(size));
    s->length += size;
}
std::uint64_t rs_unsupported(RecStream* s) {
    s->out->unsupported = true;
    return 0;
}
bool rs_true(RecStream*) { return true; }
bool rs_false(RecStream*) { return false; }
std::int64_t rs_zero(RecStream*) { return 0; }
std::int64_t rs_tell(RecStream* s) {  // slot 16; cursor is always at the end here
    check_position(s);
    return s->length;
}
int rs_size(RecStream* s) {  // slot 17
    check_position(s);
    return static_cast<int>(s->length);
}
bool rs_at_end(RecStream* s) {  // slot 11: cursor <= size
    check_position(s);
    return true;
}

constexpr int stream_slots = 20;  // vtable 141dc0358 has 20 entries
void* rec_vtable[stream_slots];

void init_rec_vtable() {
    for (auto& slot : rec_vtable) slot = reinterpret_cast<void*>(&rs_unsupported);
    rec_vtable[5] = reinterpret_cast<void*>(&rs_true);    // 14008f440: mov al,1
    rec_vtable[7] = reinterpret_cast<void*>(&rs_cstr);    // +0x38
    rec_vtable[8] = reinterpret_cast<void*>(&rs_int);     // +0x40
    rec_vtable[9] = reinterpret_cast<void*>(&rs_byte);    // +0x48
    rec_vtable[10] = reinterpret_cast<void*>(&rs_raw);    // +0x50
    rec_vtable[11] = reinterpret_cast<void*>(&rs_at_end); // 14153d4b0
    rec_vtable[15] = reinterpret_cast<void*>(&rs_false);  // 14008f450: xor al,al
    rec_vtable[16] = reinterpret_cast<void*>(&rs_tell);   // 14153d4a0: [+0x4c]
    rec_vtable[17] = reinterpret_cast<void*>(&rs_size);   // 1401f0d70
    rec_vtable[18] = reinterpret_cast<void*>(&rs_zero);   // 14008f480: xor eax,eax
    // 0..4, 6, 12, 13, 14, 19 (dtor, read, seek, reset): unsupported
}

// replay into the real stream with the same calls
using CstrFn = void (*)(void*, const char*);
using IntFn = void (*)(void*, std::uint32_t);
using ByteFn = void (*)(void*, char);
using RawFn = void (*)(void*, const char*, std::int64_t);

void replay(const Recording& r, void* stream) {
    void** vt = *static_cast<void***>(stream);
    const char* p = r.data.data();
    const char* end = p + r.data.size();
    while (p < end) {
        const auto op = static_cast<Op>(*p);
        std::uint32_t size;
        std::memcpy(&size, p + 1, 4);
        const char* bytes = p + 5;
        switch (op) {
        case op_cstr: reinterpret_cast<CstrFn>(vt[7])(stream, bytes); break;
        case op_int: {
            std::uint32_t v;
            std::memcpy(&v, bytes, 4);
            reinterpret_cast<IntFn>(vt[8])(stream, v);
            break;
        }
        case op_byte: reinterpret_cast<ByteFn>(vt[9])(stream, *bytes); break;
        case op_raw: reinterpret_cast<RawFn>(vt[10])(stream, bytes, size); break;
        }
        p = bytes + size;
    }
}

// ---- job: record object i with a writer copy
enum class Kind { province, country };

struct Job {
    Kind kind;
    std::uint8_t writer_template[writer_size];
    int stream_mode;
    std::uint8_t* gamestate;
    std::uint8_t* province_first;
    int count;
    std::vector<Recording>* out;
};

Job job;
std::atomic<int> next_index{0};

void* object_at(const Job& j, int i) {
    if (j.kind == Kind::province) return j.province_first + static_cast<std::size_t>(i) * province_stride;
    auto** countries = *reinterpret_cast<void***>(j.gamestate + country_array);
    return countries[i];
}

void record_object(const Job& j, int i, Recording& r) {
    r.data.clear();
    r.unsupported = false;
    RecStream stream{};
    stream.vtable = rec_vtable;
    stream.mode = j.stream_mode;
    stream.out = &r;
    alignas(16) std::uint8_t writer[writer_size];
    std::memcpy(writer, j.writer_template, writer_size);
    *reinterpret_cast<void**>(writer + 0x10) = &stream;
    writer[0x19] = 0;  // does not own the stream
    void* object = object_at(j, i);
    if (j.kind == Kind::province) {
        block_write(object, writer);
    } else {
        alignas(16) std::uint8_t tag_string[0x20];
        const auto* country = static_cast<const std::uint8_t*>(object);
        std::uint64_t tag = *reinterpret_cast<const std::uint64_t*>(country + 0x20);
        string_from_cstr(tag_string, &tag);
        country_write(writer, tag_string, object);
        string_free(tag_string);
    }
    std::memcpy(r.writer_after, writer, writer_size);
    r.ready = true;
}

void run_jobs() {
    for (;;) {
        const int i = next_index.fetch_add(1, std::memory_order_relaxed);
        if (i >= job.count) return;
        if (job.kind == Kind::province &&
            *reinterpret_cast<const int*>(static_cast<std::uint8_t*>(object_at(job, i)) + 0x20) == 0) {
            continue;  // the loop skips provinces with id 0 (index 0 is one)
        }
        record_object(job, i, (*job.out)[i]);
    }
}

constexpr int max_workers = 32;
int worker_count = 0;
std::atomic<int> active{0};
HANDLE job_semaphore = nullptr;

DWORD WINAPI worker(void*) {
    for (;;) {
        WaitForSingleObject(job_semaphore, INFINITE);
        active.fetch_add(1);
        run_jobs();
        active.fetch_sub(1);
    }
}

void run_parallel() {
    next_index.store(0);
    ReleaseSemaphore(job_semaphore, worker_count, nullptr);
    run_jobs();
    for (int spins = 0; active.load() != 0; ++spins) {
        if (spins < 2000) YieldProcessor(); else SwitchToThread();
    }
}

// ---- per-loop state (main thread only)
struct LoopState {
    std::vector<Recording> recordings;
    int count = 0;
    std::uint8_t writer_template[writer_size];
    void* stream = nullptr;
    bool prepared = false;
};
LoopState provinces, countries;

struct Stats {
    std::uint64_t saves = 0, objects = 0, replayed = 0, fallback = 0, unsupported = 0,
        compared = 0, mismatches = 0;
    double parallel_ms = 0, replay_ms = 0, original_ms = 0;
} stats;
int mismatch_logged = 0;

void prepare(LoopState& loop, Kind kind, void* writer, std::uint8_t* gamestate,
    std::uint8_t* province_first, int count) {
    const auto start = qpc_now();
    loop.count = count;
    if (static_cast<int>(loop.recordings.size()) < count) loop.recordings.resize(count);
    for (int i = 0; i < count; ++i) loop.recordings[i].ready = false;
    std::memcpy(loop.writer_template, writer, writer_size);
    loop.stream = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(writer) + 0x10);
    job.kind = kind;
    std::memcpy(job.writer_template, writer, writer_size);
    job.stream_mode = *reinterpret_cast<int*>(static_cast<std::uint8_t*>(loop.stream) + 0x10);
    job.gamestate = gamestate;
    job.province_first = province_first;
    job.count = count;
    job.out = &loop.recordings;
    run_parallel();
    loop.prepared = true;
    stats.parallel_ms += ms(qpc_now() - start);
    if (kind == Kind::province) ++stats.saves;
}

void dump_one(const Recording& r, const std::wstring& file) {
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return;
    const char* p = r.data.data();
    const char* end = p + r.data.size();
    while (p < end) {
        std::uint32_t size;
        std::memcpy(&size, p + 1, 4);
        const char* bytes = p + 5;
        if (*p == op_cstr) std::fwrite(bytes, 1, size ? size - 1 : 0, f);
        else if (*p == op_int) std::fprintf(f, "<i%u>", *reinterpret_cast<const std::uint32_t*>(bytes));
        else std::fwrite(bytes, 1, size, f);
        p = bytes + size;
    }
    std::fclose(f);
}
void dump_pair(int n, const Recording& parallel, const Recording& serial) {
    const auto base = log_path.substr(0, log_path.size() - 4);
    dump_one(parallel, base + L"_par" + std::to_wstring(n) + L".txt");
    dump_one(serial, base + L"_ser" + std::to_wstring(n) + L".txt");
}

// writer state left by an object, without the stream pointer (+0x10)
bool same_writer_state(const std::uint8_t* x, const std::uint8_t* y) {
    return std::memcmp(x, y, 0x10) == 0 && std::memcmp(x + 0x18, y + 0x18, writer_size - 0x18) == 0;
}

// same writer state as when the recordings were made?
bool writer_matches(const LoopState& loop, void* writer) {
    return std::memcmp(loop.writer_template, writer, writer_size) == 0;
}

// Shadow: serial recording now, compare with the parallel one, then original.
// On: replay and copy back the writer state the object left.
template <typename Original>
void handle(LoopState& loop, int index, void* writer, Original original) {
    ++stats.objects;
    if (!loop.prepared || index < 0 || index >= loop.count || !loop.recordings[index].ready ||
        !writer_matches(loop, writer) ||
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(writer) + 0x10) != loop.stream) {
        ++stats.fallback;
        const auto t = qpc_now();
        original();
        stats.original_ms += ms(qpc_now() - t);
        return;
    }
    Recording& r = loop.recordings[index];
    if (r.unsupported) ++stats.unsupported;
    if (mode == Mode::shadow || r.unsupported) {
        if (mode == Mode::shadow && !r.unsupported) {
            Recording serial;
            record_object(job, index, serial);  // job describes the current loop
            ++stats.compared;
            if (serial.unsupported || serial.data != r.data ||
                !same_writer_state(serial.writer_after, r.writer_after)) {
                ++stats.mismatches;
                if (mismatch_logged < 3) dump_pair(mismatch_logged, r, serial);
                if (mismatch_logged++ < 20) {
                    log("MISMATCH %s index=%d parallel=%zu serial=%zu",
                        &loop == &provinces ? "province" : "country", index, r.data.size(),
                        serial.data.size());
                }
            }
        }
        const auto t = qpc_now();
        original();
        stats.original_ms += ms(qpc_now() - t);
        return;
    }
    const auto t = qpc_now();
    replay(r, loop.stream);
    // copy back fields the object may have changed (depth, flags); keep vtable/stream
    auto* w = static_cast<std::uint8_t*>(writer);
    std::memcpy(w + 8, r.writer_after + 8, 8);
    std::memcpy(w + 0x18, r.writer_after + 0x18, writer_size - 0x18);
    w[0x19] = loop.writer_template[0x19];
    stats.replay_ms += ms(qpc_now() - t);
    ++stats.replayed;
}

void log_stats(const char* when) {
    log("%s saves=%llu objects=%llu replayed=%llu fallback=%llu unsupported=%llu compared=%llu "
        "mismatches=%llu parallel_ms=%.1f replay_ms=%.1f original_ms=%.1f workers=%d", when,
        static_cast<unsigned long long>(stats.saves), static_cast<unsigned long long>(stats.objects),
        static_cast<unsigned long long>(stats.replayed),
        static_cast<unsigned long long>(stats.fallback),
        static_cast<unsigned long long>(stats.unsupported),
        static_cast<unsigned long long>(stats.compared),
        static_cast<unsigned long long>(stats.mismatches), stats.parallel_ms, stats.replay_ms,
        stats.original_ms, worker_count);
}

std::uint8_t* gamestate_now() {
    return *reinterpret_cast<std::uint8_t**>(image + gamestate_rva);
}

// ---- hooks
void province_hook(void* province, void* writer) {
    if (reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) != image + province_return_rva) {
        block_write(province, writer);
        return;
    }
    auto* gs = gamestate_now();
    auto* first = *reinterpret_cast<std::uint8_t**>(gs + province_base);
    const auto begin = *reinterpret_cast<std::uint8_t**>(gs + province_begin);
    const auto end = *reinterpret_cast<std::uint8_t**>(gs + province_end);
    const int count = static_cast<int>((end - begin) / 8);
    const auto offset = static_cast<std::uint8_t*>(province) - first;
    const int index = offset >= 0 && offset % province_stride == 0 ?
        static_cast<int>(offset / province_stride) : -1;
    // a new pass starts when the index does not grow (index 0 is never written)
    static int last_index = -1;
    if (index >= 0 && (!provinces.prepared || index <= last_index)) {
        countries.prepared = false;
        prepare(provinces, Kind::province, writer, gs, first, count);
    }
    last_index = index;
    job.kind = Kind::province;
    job.province_first = first;
    handle(provinces, index, writer, [&] { block_write(province, writer); });
}

int country_count(std::uint8_t* gs) {
    void** vt = *reinterpret_cast<void***>(gs);
    return reinterpret_cast<GetCountFn>(vt[0x40 / 8])(gs);
}

void country_hook(void* writer, void* tag_string, void* country) {
    auto* gs = gamestate_now();
    auto** array = *reinterpret_cast<void***>(gs + country_array);
    const int count = country_count(gs);
    int index = -1;
    if (count > 0 && array[0] == country) index = 0;
    else if (countries.prepared && countries.count == count) {
        // the loop walks the array in order; find the next one cheaply
        static int last = 0;
        if (last + 1 < count && array[last + 1] == country) index = last + 1;
        else for (int i = 0; i < count; ++i) if (array[i] == country) { index = i; break; }
        last = index < 0 ? 0 : index;
    }
    if (index == 0) {
        prepare(countries, Kind::country, writer, gs, nullptr, count);
    }
    job.kind = Kind::country;
    job.gamestate = gs;
    handle(countries, index, writer, [&] { country_write(writer, tag_string, country); });
    if (index == count - 1) {
        countries.prepared = false;
        log_stats("save");
    }
}

// ---- save timing and fast number formatting --------------------------------
// 1405cdfc0: call 1405ce5f0 (the savegame helper that serializes the whole state
// into memory and writes the file; autosave and manual save). Timed always when
// the plugin is loaded with format != off.
// 14015c230 = sprintf(buf, fmt, ...) and 141657e40 = snprintf(buf, size, fmt, ...)
// are thin wrappers over the UCRT core 141a461c4 (common_vsprintf, options from
// 140092fa0 | 1). During a save they format most numbers (fixed point "%d.%03d",
// "%.3f" ...). The hooks replace the wrappers (same core call), and while the main
// thread is inside the save they take an exact fast path for simple formats:
// literal text, %d %i %u (optional '0' flag and width, l/ll/I64), %s, %%,
// %.Nf (N <= 9, no width; exact decimal rounding, ties to even like UCRT).
// Anything else, or output longer than the buffer, goes to the UCRT core.
// format = off | count | shadow | on
//   count  - only time saves and count formats used during saves
//   shadow - fast path computed and compared with UCRT output; UCRT output is used
//   on     - fast path output is used
constexpr std::uintptr_t save_call_rva = 0x5cdfc0;
constexpr std::uintptr_t save_function_rva = 0x5ce5f0;
constexpr std::uintptr_t sprintf_rva = 0x15c230;
constexpr std::uintptr_t snprintf_rva = 0x1657e40;
constexpr std::uintptr_t vsprintf_core_rva = 0x1a461c4;
constexpr std::uintptr_t printf_options_rva = 0x92fa0;
constexpr std::uint8_t sprintf_bytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x4c, 0x89, 0x44, 0x24,
    0x18, 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
    0xda, 0x48, 0x8d, 0x74, 0x24, 0x60, 0x48, 0x8b, 0xf9, 0xe8, 0x4a, 0x6d, 0xf3, 0xff};
constexpr std::uint8_t snprintf_bytes[] = {0x4c, 0x89, 0x44, 0x24, 0x18, 0x4c, 0x89, 0x4c, 0x24,
    0x20, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x38, 0x49, 0x8b, 0xf0, 0x48, 0x8d, 0x6c,
    0x24, 0x78, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9, 0xe8, 0x3b, 0xb1, 0xa3, 0xfe};

enum class FormatMode { off, count, shadow, on, ab };
bool ab_fast = false;  // ab: this save uses the fast path
FormatMode format_mode = FormatMode::off;
using SaveFn = void (*)(void*);
using CoreFn = int (*)(std::uint64_t, char*, std::size_t, const char*, void*, va_list);
using OptionsFn = std::uint64_t* (*)();
SaveFn original_save = nullptr;
CoreFn vsprintf_core = nullptr;
OptionsFn printf_options = nullptr;
volatile DWORD save_thread = 0;  // thread inside the save, 0 = none
std::atomic<std::uint64_t> fmt_fast{0}, fmt_slow{0}, fmt_mismatch{0};
int save_number = 0;
double save_total_ms = 0;

inline int call_core(char* buffer, std::size_t size, const char* format, va_list args) {
    const int result = vsprintf_core(*printf_options() | 1, buffer, size, format, nullptr, args);
    return result < 0 ? -1 : result;
}

// formats counted during saves: pointer -> count (main thread only while saving)
constexpr int fmt_table_size = 512;
struct FmtCount {
    const char* format;
    std::uint64_t count;
    std::uint64_t slow;
};
FmtCount fmt_table[fmt_table_size]{};

void count_format(const char* format, bool slow) {
    auto slot = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(format) >> 3) %
        fmt_table_size;
    for (int n = 0; n < fmt_table_size; ++n, slot = (slot + 1) % fmt_table_size) {
        FmtCount& e = fmt_table[slot];
        if (e.format == format || e.format == nullptr) {
            e.format = format;
            ++e.count;
            if (slow) ++e.slow;
            return;
        }
    }
}

char* put_unsigned(char* out, std::uint64_t value, int min_digits) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (n < min_digits) digits[n++] = '0';
    while (n) *out++ = digits[--n];
    return out;
}

// x * 10^precision rounded to an integer exactly (ties to even). false if out of range.
bool scaled_round(double x, int precision, std::uint64_t& result) {
    std::uint64_t bits;
    std::memcpy(&bits, &x, 8);
    const int exponent_bits = static_cast<int>((bits >> 52) & 0x7ff);
    std::uint64_t mantissa = bits & ((1ull << 52) - 1);
    if (exponent_bits == 0x7ff) return false;  // inf / nan
    int exponent;
    if (exponent_bits == 0) {
        exponent = -1074;
    } else {
        mantissa |= 1ull << 52;
        exponent = exponent_bits - 1075;
    }
    unsigned __int128 scale = 1;
    for (int i = 0; i < precision; ++i) scale *= 10;
    if (exponent >= 0) {
        if (exponent > 10) return false;  // > 2^63: leave to the CRT
        const unsigned __int128 value = (static_cast<unsigned __int128>(mantissa) << exponent) *
            scale;
        if (value >> 63) return false;
        result = static_cast<std::uint64_t>(value);
        return true;
    }
    const int k = -exponent;
    const unsigned __int128 product = static_cast<unsigned __int128>(mantissa) * scale;  // < 2^83
    if (k > 120) {
        result = 0;  // product < 2^83 << 2^(k-1): below one half
        return true;
    }
    unsigned __int128 q = product >> k;
    const unsigned __int128 r = product & ((static_cast<unsigned __int128>(1) << k) - 1);
    const unsigned __int128 half = static_cast<unsigned __int128>(1) << (k - 1);
    if (r > half || (r == half && (q & 1))) ++q;
    if (q >> 63) return false;
    result = static_cast<std::uint64_t>(q);
    return true;
}

// Returns length, or -1 when the format is not handled. out has room for `room` bytes
// including the terminating zero.
int fast_format(char* out_start, std::size_t room, const char* format, va_list args) {
    char* out = out_start;
    char* const end = out_start + (room > 4096 ? 4096 : room);
    for (const char* p = format; *p; ++p) {
        if (out + 72 >= end) return -1;
        if (*p != '%') {
            *out++ = *p;
            continue;
        }
        ++p;
        if (*p == '%') {
            *out++ = '%';
            continue;
        }
        bool zero = false;
        if (*p == '0') {
            zero = true;
            ++p;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int precision = -1;
        if (*p == '.') {
            ++p;
            precision = 0;
            while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
        }
        int length = 0;  // 0 int, 2 64-bit
        if (*p == 'l') {
            ++p;
            if (*p == 'l') {
                length = 2;
                ++p;
            }
        } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            length = 2;
            p += 3;
        }
        const char conversion = *p;
        if (conversion == 'd' || conversion == 'i' || conversion == 'u') {
            if (precision >= 0 || width > 40) return -1;
            std::uint64_t magnitude;
            bool negative = false;
            if (conversion == 'u') {
                magnitude = length == 2 ? va_arg(args, std::uint64_t) : va_arg(args, unsigned int);
            } else {
                const std::int64_t v = length == 2 ? va_arg(args, std::int64_t) : va_arg(args, int);
                negative = v < 0;
                magnitude = negative ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
            }
            char digits[24];
            char* d = put_unsigned(digits, magnitude, 1);
            const int count = static_cast<int>(d - digits) + (negative ? 1 : 0);
            int pad = width > count ? width - count : 0;
            if (!zero) while (pad-- > 0) *out++ = ' ';
            if (negative) *out++ = '-';
            if (zero) while (pad-- > 0) *out++ = '0';
            std::memcpy(out, digits, d - digits);
            out += d - digits;
        } else if (conversion == 'f') {
            if (precision < 0) precision = 6;
            if (width || zero || length || precision < 0 || precision > 9) return -1;
            const double x = va_arg(args, double);
            std::uint64_t scaled;
            if (!scaled_round(x < 0 ? -x : x, precision, scaled)) return -1;
            if (std::signbit(x)) *out++ = '-';
            std::uint64_t divisor = 1;
            for (int i = 0; i < precision; ++i) divisor *= 10;
            out = put_unsigned(out, scaled / divisor, 1);
            if (precision > 0) {
                *out++ = '.';
                out = put_unsigned(out, scaled % divisor, precision);
            }
        } else if (conversion == 's') {
            if (width || zero || length || precision >= 0) return -1;
            const char* text = va_arg(args, const char*);
            if (!text) return -1;
            const std::size_t n = std::strlen(text);
            if (out + n + 72 >= end) return -1;
            std::memcpy(out, text, n);
            out += n;
        } else {
            return -1;
        }
    }
    *out = '\0';
    return static_cast<int>(out - out_start);
}

int mismatch_logs = 0;

int format_hook_body(char* buffer, std::size_t size, const char* format, va_list args) {
    if (save_thread == 0 || save_thread != GetCurrentThreadId() || format_mode == FormatMode::off) {
        return call_core(buffer, size, format, args);
    }
    if (format_mode == FormatMode::count || (format_mode == FormatMode::ab && !ab_fast)) {
        const int result = call_core(buffer, size, format, args);
        count_format(format, false);
        return result;
    }
    va_list copy;
    va_copy(copy, args);
    char fast[4096];
    const int fast_length = fast_format(fast, size, format, copy);
    va_end(copy);
    count_format(format, fast_length < 0);
    if (fast_length < 0) {
        fmt_slow.fetch_add(1, std::memory_order_relaxed);
        return call_core(buffer, size, format, args);
    }
    fmt_fast.fetch_add(1, std::memory_order_relaxed);
    if (format_mode == FormatMode::on || format_mode == FormatMode::ab) {
        std::memcpy(buffer, fast, static_cast<std::size_t>(fast_length) + 1);
        return fast_length;
    }
    const int result = call_core(buffer, size, format, args);
    if (result != fast_length || std::memcmp(buffer, fast, static_cast<std::size_t>(fast_length)) != 0) {
        fmt_mismatch.fetch_add(1, std::memory_order_relaxed);
        if (mismatch_logs < 20) {
            ++mismatch_logs;
            log("format MISMATCH fmt=\"%s\" crt=\"%.80s\" fast=\"%.80s\"", format,
                result >= 0 ? buffer : "", fast);
        }
    }
    return result;
}

int sprintf_hook(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = format_hook_body(buffer, static_cast<std::size_t>(-1), format, args);
    va_end(args);
    return result;
}

int snprintf_hook(char* buffer, std::size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = format_hook_body(buffer, size, format, args);
    va_end(args);
    return result;
}

#include "pipe.inc"

void save_hook(void* helper) {
    ab_fast = (save_number % 2) == 1;  // saves 2, 4, ... fast
    const auto start = qpc_now();
    save_thread = GetCurrentThreadId();
    pipe_helper = helper;
    original_save(helper);
    pipe_stop();
    pipe_armed = false;
    pipe_helper = nullptr;
    save_thread = 0;
    const double took = ms(qpc_now() - start);
    ++save_number;
    save_total_ms += took;
    log("save n=%d%s ms=%.1f total_ms=%.1f fast=%llu slow=%llu mismatch=%llu", save_number,
        format_mode == FormatMode::ab ? (ab_fast ? " ab=fast" : " ab=crt") : "", took, save_total_ms, static_cast<unsigned long long>(fmt_fast.load()),
        static_cast<unsigned long long>(fmt_slow.load()),
        static_cast<unsigned long long>(fmt_mismatch.load()));
    pipe_report(took);
    if (save_number <= 2 || format_mode == FormatMode::count) {
        // top formats by count
        for (int round = 0; round < 12; ++round) {
            int best = -1;
            for (int i = 0; i < fmt_table_size; ++i) {
                if (fmt_table[i].format && fmt_table[i].count &&
                    (best < 0 || fmt_table[i].count > fmt_table[best].count)) {
                    best = i;
                }
            }
            if (best < 0) break;
            log("  format count=%llu slow=%llu \"%s\"",
                static_cast<unsigned long long>(fmt_table[best].count),
                static_cast<unsigned long long>(fmt_table[best].slow), fmt_table[best].format);
            fmt_table[best].count = 0;
        }
        std::memset(fmt_table, 0, sizeof(fmt_table));
    }
}

// 14-byte absolute jump written over a function entry
bool write_jump(std::uintptr_t at, const void* target) {
    std::uint8_t code[14] = {0xff, 0x25, 0, 0, 0, 0};
    const auto address = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(code + 6, &address, 8);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(at), code, sizeof(code));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), sizeof(code), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof(code));
    return true;
}

void install_format(const std::wstring& ini) {
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"save", L"format", L"off", value, 32, ini.c_str());
    format_mode = _wcsicmp(value, L"on") == 0 ? FormatMode::on :
        _wcsicmp(value, L"ab") == 0 ? FormatMode::ab :
        _wcsicmp(value, L"shadow") == 0 ? FormatMode::shadow :
        _wcsicmp(value, L"count") == 0 ? FormatMode::count : FormatMode::off;
    if (format_mode == FormatMode::off) return;
    if (std::memcmp(reinterpret_cast<void*>(image + sprintf_rva), sprintf_bytes,
            sizeof(sprintf_bytes)) != 0 ||
        std::memcmp(reinterpret_cast<void*>(image + snprintf_rva), snprintf_bytes,
            sizeof(snprintf_bytes)) != 0) {
        format_mode = FormatMode::off;
        log("format REFUSED: printf wrapper bytes differ");
        return;
    }
    vsprintf_core = reinterpret_cast<CoreFn>(image + vsprintf_core_rva);
    printf_options = reinterpret_cast<OptionsFn>(image + printf_options_rva);
    original_save = reinterpret_cast<SaveFn>(image + save_function_rva);
    char error[160]{};
    speedpatch::CallPatch save;
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + save_call_rva),
            reinterpret_cast<const void*>(original_save),
            reinterpret_cast<const void*>(&save_hook), save, error, sizeof(error)) ||
        !speedpatch::commit_call(save, error, sizeof(error))) {
        speedpatch::discard_call(save);
        format_mode = FormatMode::off;
        log("format REFUSED: %s", error);
        return;
    }
    if (!write_jump(image + sprintf_rva, reinterpret_cast<const void*>(&sprintf_hook)) ||
        !write_jump(image + snprintf_rva, reinterpret_cast<const void*>(&snprintf_hook))) {
        log("format: VirtualProtect failed %lu (save timing only)", GetLastError());
        format_mode = FormatMode::count;
    }
    log("format=%s installed",
        format_mode == FormatMode::on ? "on" : format_mode == FormatMode::ab ? "ab" : format_mode == FormatMode::shadow ? "shadow" : "count");
}

// ---- install
DWORD install_checked(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_save_patch.log";
    const auto ini = directory + L"\\eu4_save_patch.ini";
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"save", L"mode", L"off", value, 32, ini.c_str());
    mode = _wcsicmp(value, L"on") == 0 ? Mode::on :
        _wcsicmp(value, L"shadow") == 0 ? Mode::shadow : Mode::off;
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    install_format(ini);
    install_pipe(ini);
    if (mode == Mode::off) {
        log("parallel write disabled by configuration");
        installation_status.store(3);
        return 0;
    }
    auto** slot = reinterpret_cast<void**>(image + province_slot_rva);
    block_write = reinterpret_cast<BlockWriteFn>(image + block_write_rva);
    if (*slot != reinterpret_cast<void*>(block_write) ||
        std::memcmp(reinterpret_cast<void*>(image + country_site_rva), country_bytes,
            sizeof(country_bytes)) != 0 ||
        *reinterpret_cast<void**>(image + writer_vtable_rva) !=
            reinterpret_cast<void*>(image + 0x1707960)) {
        log("REFUSED: bytes differ; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    country_write = reinterpret_cast<CountryWriteFn>(image + country_write_rva);
    string_from_cstr = reinterpret_cast<StringFn>(image + string_from_cstr_rva);
    string_free = reinterpret_cast<StringFreeFn>(image + string_free_rva);
    init_rec_vtable();

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    int workers = static_cast<int>(info.dwNumberOfProcessors) - 2;
    workers = GetPrivateProfileIntW(L"save", L"workers", workers, ini.c_str());
    if (workers < 1) workers = 1;
    if (workers > max_workers) workers = max_workers;
    job_semaphore = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    if (!job_semaphore) {
        installation_status.store(-6);
        return 6;
    }
    for (int i = 0; i < workers; ++i) {
        HANDLE thread = CreateThread(nullptr, 8 << 20, worker, nullptr,
            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread) {
            CloseHandle(thread);
            ++worker_count;
        }
    }
    char error[160]{};
    speedpatch::CallPatch country;
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + country_call_rva),
            reinterpret_cast<const void*>(country_write),
            reinterpret_cast<const void*>(&country_hook), country, error, sizeof(error)) ||
        !speedpatch::commit_call(country, error, sizeof(error))) {
        speedpatch::discard_call(country);
        log("REFUSED: %s", error);
        installation_status.store(-5);
        return 5;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        speedpatch::rollback_call(country);
        log("REFUSED: VirtualProtect %lu", GetLastError());
        installation_status.store(-6);
        return 6;
    }
    *slot = reinterpret_cast<void*>(&province_hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    installation_status.store(mode == Mode::on ? 1 : 2);
    log("initialized version=0.1.0-experimental mode=%s workers=%d",
        mode == Mode::on ? "on" : "shadow", worker_count);
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

extern "C" __declspec(dllexport) LONG EU4SavePatchStatus() {
    return installation_status.load();
}
