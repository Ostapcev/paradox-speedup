// ---- pipe: save checksum and file write run behind the serializer (pipe=off|verify|on)
//
// Uncompressed save (1405ce5f0): the serializer 1414e3f00 (call 0x5CE8E2) writes the whole
// text into a memory stream (vtable 141dc0358; +0x38 overflow vector, +0x40 buffer of
// +0x50 = 0x5A00000 bytes, +0x48 length, +0x4c cursor). It only appends, then seeks to 0 and
// computes the stream checksum (slot +0x98 = 14171f3f0, a serial byte loop), appends the
// "checksum" line, and 1405ce5f0 writes buffer + overflow to the .tmp file with
// file slot +0x50 (141710a70 -> PHYSFS_write).
// A background thread follows the growing length (a margin behind it): it folds the bytes
// into the checksum state and writes them to the file. The checksum hook and the file write
// hook then only do the tail. The overflow vector (reallocates) is always left to the game.
// verify: the game's own checksum is computed too and returned; a 64-bit hash of the
// prefix taken by the thread is compared with the buffer at file-write time; a mismatch
// rewrites the whole file from the buffer.
enum class PipeMode { off, verify, on };
PipeMode pipe_mode = PipeMode::off;

constexpr std::uintptr_t serialize_call_rva = 0x5CE8E2;
constexpr std::uintptr_t serialize_rva = 0x14E3F00;
constexpr std::uint8_t serialize_site[] = {0x48, 0x8d, 0x4c, 0x24, 0x60, 0xe8};  // lea rcx,[rsp+60h]; call
constexpr std::uintptr_t mem_vtable_rva = 0x1DC0358;
constexpr std::uintptr_t checksum_rva = 0x171F3F0;  // mem slot +0x98
constexpr std::uintptr_t mem_rewind_rva = 0x171F010;  // mem slot +0x70
constexpr std::uintptr_t file_vtable_rva = 0x1DBFA98;
constexpr std::uintptr_t file_write_rva = 0x1710A70;  // file slot +0x50
constexpr std::uintptr_t physfs_write_rva = 0x19F8E60;
constexpr std::uintptr_t physfs_seek_rva = 0x19F8310;
constexpr int kPipeMargin = 1 << 16;
constexpr int kPipeChunk = 1 << 20;

using SerializeFn = std::uint64_t (*)(void*, void*, void*, void*);
using ChecksumFn = std::uint32_t (*)(void*, int);
using RewindFn = void (*)(void*);
using FileWriteFn = void (*)(void*, const void*, std::uint64_t);
using PhysWriteFn = std::int64_t (*)(void*, const void*, std::uint64_t, std::uint32_t);
using PhysSeekFn = int (*)(void*, std::uint64_t);
SerializeFn original_serialize = nullptr;
ChecksumFn original_checksum = nullptr;
RewindFn mem_rewind = nullptr;
FileWriteFn original_file_write = nullptr;
PhysWriteFn physfs_write = nullptr;
PhysSeekFn physfs_seek = nullptr;

template <class T> inline T pipe_rd(const void* p, std::size_t at) {
    T v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof(T));
    return v;
}

struct PipeState {
    void* writer = nullptr;
    void* file = nullptr;
    void* handle = nullptr;  // PHYSFS_File*
    const std::uint8_t* buffer = nullptr;
    int capacity = 0;
    std::atomic<bool> stop{false};
    // written by the thread, read by the main thread after done_event
    int done = 0;
    std::uint32_t a = 0, b = 0;
    std::uint64_t hash = 0;
    bool write_ok = true;
    bool stalled = false;
    std::int64_t busy_ticks = 0;
    int chunks = 0;
};
PipeState pipe;
void* pipe_helper = nullptr;  // save helper of the running save (main thread)
bool pipe_running = false;    // thread started for this save and not yet stopped
bool pipe_armed = false;      // prefix valid for the checksum / file write hooks
HANDLE pipe_start_event = nullptr, pipe_done_event = nullptr;
std::uint64_t pipe_saves = 0, pipe_checksum_used = 0, pipe_checksum_mismatch = 0,
    pipe_hash_mismatch = 0, pipe_rewrites = 0;
double pipe_tail_ms = 0;

inline void pipe_fold(const std::uint8_t* p, const std::uint8_t* e, std::uint32_t& a, std::uint32_t& b) {
    std::uint32_t x = a, y = b;
    for (; p != e; ++p) {
        std::uint32_t s = *p + x;
        x = s < 0x10000 ? s : s >> 16;
        s = x + y;
        y = s < 0x10000 ? s : s >> 16;
    }
    a = x;
    b = y;
}

inline std::uint64_t pipe_hash_bytes(const std::uint8_t* p, std::size_t n, std::uint64_t h) {
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void pipe_run() {
    PipeState& s = pipe;
    int done = 0;
    std::uint32_t a = 0, b = 0;
    std::uint64_t h = 0x243F6A8885A308D3ull;
    const auto* length = reinterpret_cast<volatile int*>(static_cast<std::uint8_t*>(s.writer) + 0x48);
    int last_length = -1;
    DWORD last_change = GetTickCount();
    while (!s.stop.load(std::memory_order_acquire)) {
        const int len = *length;
        std::atomic_thread_fence(std::memory_order_acquire);
        const int limit = ((len < s.capacity ? len : s.capacity) - kPipeMargin) & ~7;  // 8-aligned: chained hash == whole-prefix hash
        if (len != last_length) {
            last_length = len;
            last_change = GetTickCount();
        } else if (GetTickCount() - last_change > 5000) {
            s.stalled = true;  // the save never came back (exception?): stop touching the file
            break;
        }
        if (limit - done < kPipeChunk) {
            Sleep(1);
            continue;
        }
        const auto t0 = qpc_now();
        const std::uint8_t* from = s.buffer + done;
        const std::uint8_t* to = s.buffer + limit;
        pipe_fold(from, to, a, b);
        if (pipe_mode == PipeMode::verify) h = pipe_hash_bytes(from, static_cast<std::size_t>(to - from), h);
        if (physfs_write(s.handle, from, static_cast<std::uint64_t>(to - from), 1) != 1) {
            s.write_ok = false;
            break;
        }
        done = limit;
        ++s.chunks;
        s.busy_ticks += qpc_now() - t0;
    }
    s.done = done;
    s.a = a;
    s.b = b;
    s.hash = h;
}

DWORD WINAPI pipe_thread(void*) {
    for (;;) {
        WaitForSingleObject(pipe_start_event, INFINITE);
        pipe_run();
        SetEvent(pipe_done_event);
    }
}

void pipe_stop() {
    if (!pipe_running) return;
    pipe.stop.store(true, std::memory_order_release);
    WaitForSingleObject(pipe_done_event, INFINITE);
    pipe_running = false;
}

bool pipe_start(void* writer, void* file) {
    auto* fs = static_cast<std::uint8_t*>(file);
    void** handle_box = pipe_rd<void**>(fs, 0x38);
    if (!handle_box || !*handle_box) return false;
    PipeState& s = pipe;
    s.writer = writer;
    s.file = file;
    s.handle = *handle_box;
    s.buffer = pipe_rd<const std::uint8_t*>(writer, 0x40);
    s.capacity = pipe_rd<int>(writer, 0x50);
    s.done = 0;
    s.a = s.b = 0;
    s.hash = 0;
    s.write_ok = true;
    s.stalled = false;
    s.busy_ticks = 0;
    s.chunks = 0;
    s.stop.store(false, std::memory_order_relaxed);
    pipe_running = true;
    pipe_armed = true;
    SetEvent(pipe_start_event);
    return true;
}

std::uint64_t serialize_hook(void* writer, void* gamestate, void* a3, void* a4) {
    auto* h = static_cast<std::uint8_t*>(pipe_helper);
    pipe_armed = false;
    if (h && GetCurrentThreadId() == save_thread && pipe_rd<std::uint8_t>(h, 0x10) == 0) {
        void* file = pipe_rd<void*>(h, 8);
        const auto* vec = pipe_rd<const std::uint64_t*>(writer, 0x38);
        if (file && pipe_rd<std::uintptr_t>(file, 0) == image + file_vtable_rva &&
            pipe_rd<std::uintptr_t>(writer, 0) == image + mem_vtable_rva &&
            pipe_rd<const void*>(writer, 0x40) && pipe_rd<int>(writer, 0x48) == 0 &&
            pipe_rd<int>(writer, 0x4c) == 0 && pipe_rd<int>(writer, 0x50) > (kPipeMargin + kPipeChunk) &&
            vec && vec[0] == vec[1]) {
            pipe_start(writer, file);
        }
    }
    const std::uint64_t r = original_serialize(writer, gamestate, a3, a4);
    pipe_stop();
    return r;
}

// replica of 14171f3f0 starting from the thread's prefix state
std::uint32_t checksum_hook(void* writer, int n) {
    if (!pipe_armed || writer != pipe.writer || GetCurrentThreadId() != save_thread) {
        return original_checksum(writer, n);
    }
    const auto t0 = qpc_now();
    pipe_stop();
    auto* w = static_cast<std::uint8_t*>(writer);
    if (n == -1) {
        const auto* vec = pipe_rd<const int*>(w, 0x38);
        n = (vec ? vec[2] - vec[0] : 0) + pipe_rd<int>(w, 0x48);
    }
    const int requested = n;
    mem_rewind(writer);
    std::uint32_t a = 0, b = 0;
    const int len = pipe_rd<int>(w, 0x48);
    bool used = false;
    if (len > 0) {
        const int count = len < n ? len : n;
        int start = 0;
        if (pipe.done > 0 && pipe.done <= count && pipe_rd<const std::uint8_t*>(w, 0x40) == pipe.buffer) {
            a = pipe.a;
            b = pipe.b;
            start = pipe.done;
            used = true;
        }
        if (count > start) {
            const auto* buf = pipe_rd<const std::uint8_t*>(w, 0x40);
            pipe_fold(buf + start, buf + count, a, b);
        }
        int consumed = count > 0 ? count : 0;
        *reinterpret_cast<int*>(w + 0x4c) += consumed;
        n -= count;
    }
    const auto* vec = pipe_rd<const std::uint64_t*>(w, 0x38);
    if (vec) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(vec[0]);
        const int size = static_cast<int>(static_cast<std::uint32_t>(vec[1]) - static_cast<std::uint32_t>(vec[0]));
        int count = size < n ? size : n;
        int consumed = 0;
        if (count > 0) {
            pipe_fold(p, p + count, a, b);
            consumed = count;
        }
        *reinterpret_cast<int*>(w + 0x4c) += consumed;
    }
    std::uint32_t result = a << 16 | b;
    if (used) ++pipe_checksum_used;
    pipe_tail_ms += ms(qpc_now() - t0);
    if (pipe_mode == PipeMode::verify) {
        const std::uint32_t game = original_checksum(writer, requested);
        if (game != result) {
            ++pipe_checksum_mismatch;
            log("pipe checksum MISMATCH game=%08x pipe=%08x prefix=%d len=%d", game, result, pipe.done, len);
        }
        result = game;
    }
    return result;
}

void file_write_hook(void* file, const void* data, std::uint64_t size) {
    if (!pipe_armed || file != pipe.file || data != pipe.buffer || GetCurrentThreadId() != save_thread) {
        original_file_write(file, data, size);
        return;
    }
    const auto t0 = qpc_now();
    pipe_stop();
    pipe_armed = false;
    const std::uint64_t done = static_cast<std::uint64_t>(pipe.done);
    bool rewrite = !pipe.write_ok || pipe.stalled || done > size;
    if (!rewrite && pipe_mode == PipeMode::verify && done > 0) {
        const std::uint64_t now = pipe_hash_bytes(pipe.buffer, done, 0x243F6A8885A308D3ull);
        if (now != pipe.hash) {
            ++pipe_hash_mismatch;
            log("pipe prefix MISMATCH: buffer changed after the thread wrote it (prefix=%llu)",
                static_cast<unsigned long long>(done));
            rewrite = true;
        }
    }
    if (rewrite && done > 0) {
        ++pipe_rewrites;
        log("pipe rewrite: write_ok=%d stalled=%d prefix=%llu size=%llu", pipe.write_ok ? 1 : 0,
            pipe.stalled ? 1 : 0, static_cast<unsigned long long>(done), static_cast<unsigned long long>(size));
        physfs_seek(pipe.handle, 0);
        original_file_write(file, data, size);
    } else if (rewrite) {
        original_file_write(file, data, size);
    } else if (size > done) {
        original_file_write(file, static_cast<const std::uint8_t*>(data) + done, size - done);
    }
    ++pipe_saves;
    pipe_tail_ms += ms(qpc_now() - t0);
}

void pipe_report(double save_ms) {
    if (pipe_mode == PipeMode::off) return;
    log("pipe saves=%llu prefix_bytes=%d chunks=%d thread_ms=%.1f tail_ms=%.1f save_ms=%.1f "
        "checksum_used=%llu checksum_mismatch=%llu prefix_mismatch=%llu rewrites=%llu",
        static_cast<unsigned long long>(pipe_saves), pipe.done, pipe.chunks, ms(pipe.busy_ticks),
        pipe_tail_ms, save_ms, static_cast<unsigned long long>(pipe_checksum_used),
        static_cast<unsigned long long>(pipe_checksum_mismatch),
        static_cast<unsigned long long>(pipe_hash_mismatch), static_cast<unsigned long long>(pipe_rewrites));
    pipe_tail_ms = 0;
}

bool patch_slot(std::uintptr_t slot_rva, std::uintptr_t expect_rva, const void* hook) {
    auto** slot = reinterpret_cast<void**>(image + slot_rva);
    if (*slot != reinterpret_cast<void*>(image + expect_rva)) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = const_cast<void*>(hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

void install_pipe(const std::wstring& ini) {
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"save", L"pipe", L"off", value, 32, ini.c_str());
    pipe_mode = _wcsicmp(value, L"on") == 0 ? PipeMode::on :
        _wcsicmp(value, L"verify") == 0 ? PipeMode::verify : PipeMode::off;
    if (pipe_mode == PipeMode::off) return;
    if (format_mode == FormatMode::off) {
        log("pipe REFUSED: needs format != off (save hook)");
        pipe_mode = PipeMode::off;
        return;
    }
    const auto mem_slot = [](std::size_t at) { return mem_vtable_rva + at; };
    const auto file_slot = [](std::size_t at) { return file_vtable_rva + at; };
    if (std::memcmp(reinterpret_cast<void*>(image + serialize_call_rva - 5), serialize_site, sizeof(serialize_site)) != 0 ||
        pipe_rd<std::uintptr_t>(reinterpret_cast<void*>(image + mem_slot(0x98)), 0) != image + checksum_rva ||
        pipe_rd<std::uintptr_t>(reinterpret_cast<void*>(image + mem_slot(0x70)), 0) != image + mem_rewind_rva ||
        pipe_rd<std::uintptr_t>(reinterpret_cast<void*>(image + file_slot(0x50)), 0) != image + file_write_rva) {
        log("pipe REFUSED: bytes differ");
        pipe_mode = PipeMode::off;
        return;
    }
    original_serialize = reinterpret_cast<SerializeFn>(image + serialize_rva);
    original_checksum = reinterpret_cast<ChecksumFn>(image + checksum_rva);
    mem_rewind = reinterpret_cast<RewindFn>(image + mem_rewind_rva);
    original_file_write = reinterpret_cast<FileWriteFn>(image + file_write_rva);
    physfs_write = reinterpret_cast<PhysWriteFn>(image + physfs_write_rva);
    physfs_seek = reinterpret_cast<PhysSeekFn>(image + physfs_seek_rva);
    pipe_start_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    pipe_done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = pipe_start_event && pipe_done_event ? CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr) : nullptr;
    if (!thread) {
        log("pipe REFUSED: thread");
        pipe_mode = PipeMode::off;
        return;
    }
    CloseHandle(thread);
    char error[160]{};
    speedpatch::CallPatch call;
    if (!speedpatch::prepare_call(reinterpret_cast<std::uint8_t*>(image + serialize_call_rva),
            reinterpret_cast<const void*>(original_serialize),
            reinterpret_cast<const void*>(&serialize_hook), call, error, sizeof(error))) {
        speedpatch::discard_call(call);
        log("pipe REFUSED: %s", error);
        pipe_mode = PipeMode::off;
        return;
    }
    if (!patch_slot(mem_slot(0x98), checksum_rva, reinterpret_cast<const void*>(&checksum_hook)) ||
        !patch_slot(file_slot(0x50), file_write_rva, reinterpret_cast<const void*>(&file_write_hook))) {
        speedpatch::discard_call(call);
        log("pipe REFUSED: vtable slot");
        pipe_mode = PipeMode::off;
        return;
    }
    if (!speedpatch::commit_call(call, error, sizeof(error))) {
        speedpatch::discard_call(call);
        log("pipe: serializer call not patched (%s); slots stay but are inert", error);
        pipe_mode = PipeMode::off;
        return;
    }
    log("pipe=%s installed", pipe_mode == PipeMode::on ? "on" : "verify");
}
