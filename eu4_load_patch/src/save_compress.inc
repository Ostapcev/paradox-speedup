// ---- save: compression of one ZIP member of a save (Bellows save_compress, MIT)
// 0x153C790(writer, input, buffer, size, crc, level) deflates the whole input stream into the
// archive writer: deflateInit2_(level, 8, -15, 9, 0); reads size/2 bytes at a time, deflate
// (Z_NO_FLUSH), a byte-at-a-time CRC-32 over what deflate consumed, writes; then Z_FINISH.
// It returns the compressed size (0 on error) and leaves ~crc in *crc.
//   on:      the same function call for call with the game's own zlib (bytes identical),
//            only the CRC is slice-by-8.
//   shadow:  the game's function runs into a capture, ours into another; the game's bytes are
//            written, differences are counted.
//   save_parallel=on|shadow: members of 4 MiB and more are read whole and deflated in 1 MiB
//            chunks on several threads with the game's zlib (each chunk ends with Z_SYNC_FLUSH,
//            the last with Z_FINISH): a valid member with the same CRC and sizes, other DEFLATE
//            bytes. shadow also decodes the result with the game's tinfl (0x1A08470) and
//            compares it with the input before writing; on any failure the member goes the
//            "on" way.
constexpr std::uintptr_t save_member_rva = 0x153C790;
constexpr std::uintptr_t save_init2_rva = 0x181FC70;
constexpr std::uintptr_t save_deflate_rva = 0x181F210;
constexpr std::uintptr_t save_end_rva = 0x181FB90;
constexpr std::uintptr_t save_tinfl_rva = 0x1A08470;

struct ZStream {  // zlib 1.2.8 z_stream, 64-bit Windows (uLong is 32 bits)
    const std::uint8_t* next_in;
    std::uint32_t avail_in;
    std::uint32_t total_in;
    std::uint8_t* next_out;
    std::uint32_t avail_out;
    std::uint32_t total_out;
    const char* msg;
    void* state;
    void* zalloc;
    void* zfree;
    void* opaque;
    int data_type;
    std::uint32_t adler;
    std::uint32_t reserved;
};
static_assert(sizeof(ZStream) == 0x58, "z_stream layout");
constexpr int kZOk = 0, kZStreamEnd = 1, kZSyncFlush = 2, kZFinish = 4;

using SaveMemberFn = int (*)(void* writer, void* input, std::uint8_t* buffer, std::uint64_t size, std::uint32_t* crc, int level);
using ZInit2Fn = int (*)(ZStream*, int, int, int, int, int, const char*, int);
using ZDeflateFn = int (*)(ZStream*, int);
using ZEndFn = int (*)(ZStream*);
using TinflFn = int (*)(void*, const std::uint8_t*, std::size_t*, std::uint8_t*, std::uint8_t*, std::size_t*, std::uint32_t);
SaveMemberFn save_next = nullptr;
ZInit2Fn z_init2 = nullptr;
ZDeflateFn z_deflate = nullptr;
ZEndFn z_end = nullptr;
TinflFn z_tinfl = nullptr;
Stats save_stats;
Mode save_parallel = Mode::off;
std::atomic<long long> save_parallel_members{0}, save_parallel_fallbacks{0}, save_bytes_in{0}, save_bytes_out{0};

struct CrcTables {
    std::uint32_t t[8][256];
    CrcTables() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (std::uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
const CrcTables crc_tables;

// The game's register update (no inversion), byte by byte and slice-by-8.
std::uint32_t crc_bytewise(std::uint32_t reg, const std::uint8_t* p, std::size_t n) noexcept {
    for (; n; --n) reg = (reg >> 8) ^ crc_tables.t[0][(reg ^ *p++) & 0xFF];
    return reg;
}
std::uint32_t crc_update(std::uint32_t reg, const std::uint8_t* p, std::size_t n) noexcept {
    const auto& t = crc_tables.t;
    for (; n >= 8; n -= 8, p += 8) {
        std::uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= reg;
        reg = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
    }
    return crc_bytewise(reg, p, n);
}

// CRC of A followed by B (both in the usual ~update(~0, data) form) from crc(A), crc(B), len(B).
std::uint32_t gf2_times(const std::uint32_t* m, std::uint32_t v) {
    std::uint32_t sum = 0;
    for (int i = 0; v; v >>= 1, ++i)
        if (v & 1) sum ^= m[i];
    return sum;
}
void gf2_square(std::uint32_t* out, const std::uint32_t* m) {
    for (int n = 0; n < 32; ++n) out[n] = gf2_times(m, m[n]);
}
std::uint32_t crc_combine(std::uint32_t crc1, std::uint32_t crc2, std::uint64_t len2) {
    if (len2 == 0) return crc1;
    std::uint32_t even[32], odd[32];
    odd[0] = 0xEDB88320u;
    std::uint32_t row = 1;
    for (int n = 1; n < 32; ++n, row <<= 1) odd[n] = row;
    gf2_square(even, odd);
    gf2_square(odd, even);
    do {
        gf2_square(even, odd);
        if (len2 & 1) crc1 = gf2_times(even, crc1);
        len2 >>= 1;
        if (!len2) break;
        gf2_square(odd, even);
        if (len2 & 1) crc1 = gf2_times(odd, crc1);
        len2 >>= 1;
    } while (len2);
    return crc1 ^ crc2;
}

void** vtable_of(void* object) { return *static_cast<void***>(object); }
int stream_size(void* input) { return reinterpret_cast<int (*)(void*)>(vtable_of(input)[0x88 / 8])(input); }
void stream_seek(void* input, int position) { reinterpret_cast<void (*)(void*, int)>(vtable_of(input)[0x60 / 8])(input, position); }
void stream_read(void* input, std::uint8_t* destination, int length) {
    reinterpret_cast<void (*)(void*, std::uint8_t*, int)>(vtable_of(input)[0x20 / 8])(input, destination, length);
}

struct SaveIo {
    void* input;
    void* writer;
    void (*write)(void* writer, const std::uint8_t* data, std::int64_t length);
};
void writer_write(void* writer, const std::uint8_t* data, std::int64_t length) {
    reinterpret_cast<void (*)(void*, const std::uint8_t*, std::int64_t)>(vtable_of(writer)[0x50 / 8])(writer, data, length);
}

// 0x153C790 call for call. reference_crc: also run the game's byte loop and keep its value.
int compress_entry(const SaveIo& io, std::uint8_t* buffer, std::uint64_t size, std::uint32_t* crc, int level,
    bool reference_crc, bool* crc_differs) {
    ZStream s{};
    if (z_init2(&s, level, 8, -15, 9, 0, "1.2.3", 0x58) != kZOk) return 0;
    const std::uint64_t half = size >> 1;
    const int input_half = static_cast<int>(half);
    const auto output_half = static_cast<std::uint32_t>(size - half);
    std::uint8_t* const output = buffer + half;
    int written = 0;
    int left = stream_size(io.input);
    while (left > 0) {
        stream_seek(io.input, stream_size(io.input) - left);
        const int chunk = left < input_half ? left : input_half;
        stream_read(io.input, buffer, chunk);
        const std::uint32_t before = s.total_in;
        s.next_in = buffer;
        s.avail_in = static_cast<std::uint32_t>(chunk);
        s.next_out = output;
        s.avail_out = output_half;
        if (z_deflate(&s, 0) != kZOk) {
            z_end(&s);
            return 0;
        }
        const std::uint32_t consumed = s.total_in - before;
        std::uint32_t reg = crc_update(*crc, buffer, consumed);
        if (reference_crc) {
            const std::uint32_t game = crc_bytewise(*crc, buffer, consumed);
            if (game != reg && crc_differs) *crc_differs = true;
            reg = game;
        }
        *crc = reg;
        const int produced = static_cast<int>(s.total_out) - written;
        left = stream_size(io.input) - static_cast<int>(s.total_in);
        if (produced > 0) {
            written += produced;
            io.write(io.writer, output, produced);
        }
    }
    s.next_in = nullptr;
    s.avail_in = 0;
    s.next_out = output;
    s.avail_out = output_half;
    int result;
    do {
        result = z_deflate(&s, kZFinish);
    } while (result == kZOk);
    if (result != kZStreamEnd) return 0;  // as the game: without deflateEnd
    const int produced = static_cast<int>(s.total_out) - written;
    if (produced > 0) {
        written += produced;
        io.write(io.writer, output, produced);
    }
    z_end(&s);
    *crc = ~*crc;
    return written;
}

// A writer that keeps what is written to it (shadow): only the +0x50 slot is valid.
struct Capture {
    void** vtable;
    std::vector<std::uint8_t> bytes;
    std::vector<std::int64_t> writes;
    bool failed = false;
};
void capture_write(void* self, const std::uint8_t* data, std::int64_t length) {
    auto* c = static_cast<Capture*>(self);
    try {
        c->bytes.insert(c->bytes.end(), data, data + length);
        c->writes.push_back(length);
    } catch (...) {
        c->failed = true;
    }
}
void capture_other(void* self) { static_cast<Capture*>(self)->failed = true; }
void* capture_vtable[32] = {};

int verify_entry(const SaveIo& io, std::uint8_t* buffer, std::uint64_t size, std::uint32_t* crc, int level) {
    Capture game{capture_vtable, {}, {}, false};
    Capture ours{capture_vtable, {}, {}, false};
    std::uint32_t game_crc = *crc, our_crc = *crc;
    void* game_writer = &game;
    int game_written;
    {
        Timer t(save_stats.original_ticks);
        game_written = save_next(&game_writer, io.input, buffer, size, &game_crc, level);
    }
    if (game.failed) return compress_entry(io, buffer, size, crc, level, false, nullptr);
    SaveIo captured{io.input, &ours, capture_write};
    bool crc_differs = false;
    int our_written;
    {
        Timer t(save_stats.ticks);
        our_written = compress_entry(captured, buffer, size, &our_crc, level, true, &crc_differs);
    }
    if (!ours.failed) {
        save_stats.compared.fetch_add(1);
        if (our_written != game_written || our_crc != game_crc || ours.bytes != game.bytes || crc_differs) {
            save_stats.mismatches.fetch_add(1);
            log("SAVE MISMATCH: %d / %d bytes, crc %08X / %08X, crc loop %s", our_written, game_written,
                our_crc, game_crc, crc_differs ? "differs" : "same");
        }
    }
    std::int64_t offset = 0;
    for (const std::int64_t length : game.writes) {
        io.write(io.writer, game.bytes.data() + offset, length);
        offset += length;
    }
    *crc = game_crc;
    return game_written;
}

constexpr std::size_t kSaveChunk = 1 << 20;
constexpr std::size_t kSaveMinChunks = 4;

struct SavePiece {
    std::vector<std::uint8_t> bytes;
    std::uint32_t crc = 0;  // ~update(~0, chunk)
};

bool deflate_chunk(const std::uint8_t* data, std::size_t length, bool last, int level, SavePiece& piece) {
    ZStream s{};
    if (z_init2(&s, level, 8, -15, 9, 0, "1.2.3", 0x58) != kZOk) return false;
    bool ok = true;
    try {
        piece.bytes.resize(length + length / 8 + 4096);
    } catch (...) {
        ok = false;
    }
    std::size_t produced = 0;
    s.next_in = data;
    s.avail_in = static_cast<std::uint32_t>(length);
    while (ok) {
        if (produced == piece.bytes.size()) {
            try {
                piece.bytes.resize(piece.bytes.size() * 2);
            } catch (...) {
                ok = false;
                break;
            }
        }
        s.next_out = piece.bytes.data() + produced;
        s.avail_out = static_cast<std::uint32_t>(piece.bytes.size() - produced);
        const int result = z_deflate(&s, last ? kZFinish : kZSyncFlush);
        produced = piece.bytes.size() - s.avail_out;
        if (last) {
            if (result == kZStreamEnd) break;
            if (result != kZOk) ok = false;
        } else {
            if (result != kZOk) ok = false;
            else if (s.avail_in == 0 && s.avail_out != 0) break;  // flush complete
        }
    }
    z_end(&s);
    if (!ok) return false;
    piece.bytes.resize(produced);
    piece.crc = ~crc_update(0xFFFFFFFFu, data, length);
    return true;
}

bool tinfl_matches(const std::vector<SavePiece>& pieces, const std::uint8_t* data, std::size_t size) {
    std::size_t total = 0;
    for (const auto& p : pieces) total += p.bytes.size();
    std::vector<std::uint8_t> joined, out;
    void* state = nullptr;
    try {
        joined.reserve(total);
        for (const auto& p : pieces) joined.insert(joined.end(), p.bytes.begin(), p.bytes.end());
        out.resize(size + 1);
        state = std::calloc(1, 64 * 1024);  // tinfl_decompressor, m_state = 0 (fresh)
    } catch (...) {
        return false;
    }
    if (!state) return false;
    std::size_t in_size = joined.size(), out_size = out.size();
    const int status = z_tinfl(state, joined.data(), &in_size, out.data(), out.data(), &out_size, 4);
    std::free(state);
    return status == 0 && in_size == joined.size() && out_size == size && std::memcmp(out.data(), data, size) == 0;
}

// The parallel way; false (nothing written) when the member goes the "on" way instead.
bool compress_parallel(const SaveIo& io, std::uint32_t* crc, int level, int& written) {
    const int size = stream_size(io.input);
    if (size <= 0 || static_cast<std::size_t>(size) < kSaveMinChunks * kSaveChunk) return false;
    std::uint8_t* data = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!data) return false;
    struct Free {
        std::uint8_t* p;
        ~Free() { VirtualFree(p, 0, MEM_RELEASE); }
    } guard{data};
    stream_seek(io.input, 0);
    stream_read(io.input, data, size);
    const std::size_t chunks = (static_cast<std::size_t>(size) + kSaveChunk - 1) / kSaveChunk;
    std::vector<SavePiece> pieces;
    try {
        pieces.resize(chunks);
    } catch (...) {
        return false;
    }
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    auto work = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= chunks || failed.load(std::memory_order_relaxed)) return;
            const std::size_t start = i * kSaveChunk;
            const std::size_t length = (std::min)(kSaveChunk, static_cast<std::size_t>(size) - start);
            if (!deflate_chunk(data + start, length, i + 1 == chunks, level, pieces[i])) failed.store(true);
        }
    };
    DWORD_PTR process = 0, system = 0;
    unsigned cpus = 1;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        cpus = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++cpus;
    }
    const unsigned workers = static_cast<unsigned>((std::min<std::size_t>)((std::max)(1u, (std::min)(cpus, 8u)), chunks));
    std::vector<std::thread> helpers;
    try {
        for (unsigned i = 1; i < workers; ++i) helpers.emplace_back(work);
    } catch (...) {
    }
    work();
    for (auto& h : helpers) h.join();
    if (failed.load()) return false;
    std::uint32_t whole = pieces[0].crc;
    std::uint64_t out = pieces[0].bytes.size();
    for (std::size_t i = 1; i < chunks; ++i) {
        const std::size_t length = (std::min)(kSaveChunk, static_cast<std::size_t>(size) - i * kSaveChunk);
        whole = crc_combine(whole, pieces[i].crc, length);
        out += pieces[i].bytes.size();
    }
    if (out > 0x7FFFFFFFu) return false;
    if (save_parallel == Mode::shadow) {
        save_stats.compared.fetch_add(1);
        if (!tinfl_matches(pieces, data, static_cast<std::size_t>(size))) {
            save_stats.mismatches.fetch_add(1);
            log("SAVE MISMATCH: a parallel member did not decode to its input; the game's way instead");
            return false;
        }
    }
    // The game's convention: *crc in is the register (~0 for a fresh member), ~register out.
    // crc of the whole = combine(crc of what came before, ...); a member starts fresh, so the
    // register before is ~crc_prefix.
    const std::uint32_t prefix = ~*crc;  // CRC (usual form) of the bytes before this input
    *crc = crc_combine(prefix, whole, static_cast<std::uint64_t>(size));
    for (const auto& p : pieces) io.write(io.writer, p.bytes.data(), static_cast<std::int64_t>(p.bytes.size()));
    written = static_cast<int>(out);
    return true;
}

int zip_member_hook(void* writer, void* input, std::uint8_t* buffer, std::uint64_t size, std::uint32_t* crc, int level) {
    const std::int64_t start = qpc_now();
    save_stats.calls.fetch_add(1);
    SaveIo io{input, *static_cast<void**>(writer), writer_write};
    const int in_bytes = stream_size(input);
    int written = 0;
    bool done = false;
    const char* path = "on";
    if (save_parallel != Mode::off) {
        try {
            done = compress_parallel(io, crc, level, written);
        } catch (...) {
            done = false;
        }
        if (done) {
            save_parallel_members.fetch_add(1);
            path = "parallel";
        } else if (static_cast<std::size_t>(in_bytes) >= kSaveMinChunks * kSaveChunk) {
            save_parallel_fallbacks.fetch_add(1);
        }
    }
    if (!done) {
        if (save_stats.mode == Mode::shadow) {
            written = verify_entry(io, buffer, size, crc, level);
            path = "shadow";
        } else {
            written = compress_entry(io, buffer, size, crc, level, false, nullptr);
        }
    }
    save_stats.ours.fetch_add(1);
    save_bytes_in.fetch_add(in_bytes);
    save_bytes_out.fetch_add(written);
    log("save member path=%s level=%d in=%d out=%d took_ms=%.1f", path, level, in_bytes, written,
        ms(qpc_now() - start));
    return written;
}

bool install_save() {
    const RangeBytes checks[] = {
        {save_init2_rva, {0x40, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20}},
        {save_deflate_rva, {0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0xF2, 0x48, 0x8B, 0xF9}},
        {save_end_rva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9}},
        {save_tinfl_rva, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
        {save_member_rva + 0x6E, {0xE8, 0x6D, 0x34, 0x2E, 0x00, 0x85, 0xC0}},  // deflateInit2_ call
        {save_member_rva + 0x111, {0x4C, 0x8D, 0x3D, 0x28, 0x54, 0x85, 0x00}},  // CRC table load
    };
    for (const auto& c : checks) {
        if (std::memcmp(reinterpret_cast<const void*>(image + c.rva), c.bytes.begin(), c.bytes.size()) != 0) {
            log("save REFUSED: bytes differ at 0x%llx", static_cast<unsigned long long>(c.rva));
            return false;
        }
    }
    z_init2 = reinterpret_cast<ZInit2Fn>(image + save_init2_rva);
    z_deflate = reinterpret_cast<ZDeflateFn>(image + save_deflate_rva);
    z_end = reinterpret_cast<ZEndFn>(image + save_end_rva);
    z_tinfl = reinterpret_cast<TinflFn>(image + save_tinfl_rva);
    for (auto& slot : capture_vtable) slot = reinterpret_cast<void*>(&capture_other);
    capture_vtable[0x50 / 8] = reinterpret_cast<void*>(&capture_write);
    void* next = hook_entry(save_member_rva,
        {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20},
        7, reinterpret_cast<const void*>(&zip_member_hook), "save");
    if (!next) return false;
    save_next = reinterpret_cast<SaveMemberFn>(next);
    return true;
}
