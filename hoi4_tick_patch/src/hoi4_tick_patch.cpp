// HOI4 Tick Patch 0.1.0, HOI4 1.19.4.2 x64 — faster hourly/daily ticks without changing game logic.
//
// Module "pfor": every PDX parallel-for (208 template instances, e.g. the ~20 phases of
// CSupplySystem::UpdateSupply) first asks 142573440(range) how many jobs to start:
//   range+0x0c begin, +0x10 end, +0x14 max jobs, +0x18 grain, +0x1c mode (3 = serial)
//   jobs = min(ceil((end - begin) / grain), max jobs), or 1 for mode 3.
// With jobs == 1 the instance runs its body inline in the calling thread (the path the game
// takes for small ranges and single-thread machines). For small phases the hand-off to the
// workers and the wait (spin + SwitchToThread) cost more than the work itself.
//   pfor=probe: measures each call site (return address): calls, items, jobs and the time from
//     the job-count query to the end of the instance function; odd calls of a site run in
//     parallel, even calls serially (the instance's return address is redirected to a stub
//     that takes the time). Only for measurement.
//   pfor=on: sites listed in serial_sites= (rva of the return address) get jobs = 1.
//   pfor=off: no bytes changed.
// The answer "1 job" only selects the game's own serial path of the parallel-for, so the
// result is the same as for any other split of the range.

#include <windows.h>

#include "hoi4_build.hpp"
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

namespace {

enum class Mode { off, probe, on };

constexpr std::uintptr_t jobs_rva = 0x2573440;
constexpr std::uint8_t jobs_bytes[] = {0x83, 0x79, 0x1c, 0x03, 0x75, 0x06, 0xb8, 0x01, 0x00, 0x00,
                                       0x00, 0xc3, 0x8b, 0x41, 0x10, 0x2b, 0x41, 0x0c, 0xff, 0xc8,
                                       0x03, 0x41, 0x18, 0x99};

Mode mode = Mode::off;
std::uintptr_t image = 0, image_end = 0;
DWORD main_tid = 0;
std::wstring log_path;
std::mutex log_mutex;
std::int64_t qpc_frequency = 1;

void log(const char* format, ...) noexcept {
    char text[1024];
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
double us(double ticks) noexcept { return ticks * 1e6 / static_cast<double>(qpc_frequency); }

struct Range { std::uint8_t pad[0x0c]; std::int32_t begin, end, max_jobs, grain, kind; };

inline std::int32_t game_jobs(const Range* r) noexcept {
    if (r->kind == 3) return 1;
    const std::int32_t n = (r->end - r->begin - 1 + r->grain) / r->grain;
    return r->max_jobs < n ? r->max_jobs : n;
}

// ---- per-site statistics (site = rva of the return address, main thread / other threads)
struct SiteStats {
    std::atomic<std::uint32_t> key{0};  // rva | (worker << 31); 0 = empty
    std::atomic<std::uint64_t> calls{0}, multi{0}, items{0}, jobs{0}, grain{0};
    std::atomic<std::uint64_t> par_n{0}, par_qpc{0}, ser_n{0}, ser_qpc{0};
    std::atomic<std::uint32_t> parity{0};
};
constexpr std::size_t site_slots = 2048;
SiteStats sites[site_slots];

SiteStats* site_of(std::uint32_t key) noexcept {
    std::size_t i = (key * 2654435761u) & (site_slots - 1);
    for (std::size_t n = 0; n < site_slots; ++n, i = (i + 1) & (site_slots - 1)) {
        std::uint32_t k = sites[i].key.load(std::memory_order_acquire);
        if (k == key) return &sites[i];
        if (k == 0) {
            std::uint32_t expected = 0;
            if (sites[i].key.compare_exchange_strong(expected, key)) return &sites[i];
            if (expected == key) return &sites[i];
        }
    }
    return nullptr;
}

// ---- serial set for mode on
std::uint32_t serial_rvas[256];
int serial_count = 0;
inline bool is_serial(std::uint32_t rva) noexcept {
    for (int i = 0; i < serial_count; ++i) if (serial_rvas[i] == rva) return true;
    return false;
}

// ---- probe: return-address redirection of the parallel-for instance
struct Pending { std::uint64_t original; std::int64_t t0; SiteStats* site; bool serial; };
thread_local Pending pending[64];
thread_local int pending_depth = 0;
std::uint8_t* return_stub = nullptr;
std::atomic<std::uint64_t> hijack_fail{0}, hijacks{0};

extern "C" std::uint64_t tick_on_return() {
    const auto t1 = qpc_now();
    if (pending_depth <= 0) { std::abort(); }
    const Pending p = pending[--pending_depth];
    const auto dt = static_cast<std::uint64_t>(t1 - p.t0);
    if (p.serial) { p.site->ser_n.fetch_add(1); p.site->ser_qpc.fetch_add(dt); }
    else { p.site->par_n.fetch_add(1); p.site->par_qpc.fetch_add(dt); }
    return p.original;
}

// Redirect the return address of the function that called the job-count query (the
// parallel-for instance). Our own frame is unwound first, then the instance's.
__attribute__((noinline)) bool hijack_instance(SiteStats* site, bool serial) {
    if (pending_depth >= 64) return false;
    CONTEXT context{};
    RtlCaptureContext(&context);
    for (int frame = 0; frame < 3; ++frame) {  // hijack_instance -> jobs_hook -> instance
        DWORD64 base = 0;
        auto* entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (!entry) return false;
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context, &handler_data,
                         &establisher, nullptr);
        if (frame == 1 && (context.Rip < image || context.Rip >= image_end)) return false;
    }
    // context = caller of the instance; its return address sits just below Rsp
    auto* slot = reinterpret_cast<std::uint64_t*>(context.Rsp - 8);
    if (*slot != context.Rip || context.Rip < image || context.Rip >= image_end) return false;
    pending[pending_depth++] = Pending{*slot, qpc_now(), site, serial};
    *slot = reinterpret_cast<std::uint64_t>(return_stub);
    return true;
}

std::int32_t jobs_hook(const Range* r) {
    const std::int32_t n = game_jobs(r);
    if (n <= 1) return n;
    const auto rva = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - image);
    if (mode == Mode::on) return is_serial(rva) ? 1 : n;
    const bool worker = GetCurrentThreadId() != main_tid;
    SiteStats* s = site_of(rva | (worker ? 0x80000000u : 0));
    if (!s) return n;
    s->calls.fetch_add(1, std::memory_order_relaxed);
    s->multi.fetch_add(1, std::memory_order_relaxed);
    s->items.fetch_add(static_cast<std::uint64_t>(r->end - r->begin), std::memory_order_relaxed);
    s->jobs.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    s->grain.fetch_add(static_cast<std::uint64_t>(r->grain), std::memory_order_relaxed);
    const bool serial = (s->parity.fetch_add(1) & 1) != 0;
    if (!hijack_instance(s, serial)) { hijack_fail.fetch_add(1); return n; }
    hijacks.fetch_add(1, std::memory_order_relaxed);
    return serial ? 1 : n;
}

void report() noexcept {
    struct Row { std::uint32_t key; double calls, items, jobs, par, ser, saving, grain; };
    static Row rows[site_slots];
    int count = 0;
    for (auto& s : sites) {
        const auto k = s.key.load();
        if (!k) continue;
        const double pn = static_cast<double>(s.par_n.load()), sn = static_cast<double>(s.ser_n.load());
        const double c = static_cast<double>(s.calls.load());
        if (c == 0) continue;
        const double par = pn ? us(static_cast<double>(s.par_qpc.load())) / pn : 0;
        const double ser = sn ? us(static_cast<double>(s.ser_qpc.load())) / sn : 0;
        rows[count++] = Row{k, c, static_cast<double>(s.items.load()) / c,
                            static_cast<double>(s.jobs.load()) / c, par, ser,
                            (pn && sn) ? (par - ser) * c : 0, static_cast<double>(s.grain.load()) / c};
    }
    // sort by saving (descending), simple insertion sort
    for (int i = 1; i < count; ++i) {
        Row x = rows[i]; int j = i - 1;
        while (j >= 0 && rows[j].saving < x.saving) { rows[j + 1] = rows[j]; --j; }
        rows[j + 1] = x;
    }
    log("pfor probe: sites=%d hijacks=%llu hijack_fail=%llu (site, thread, calls, items, jobs, "
        "parallel us, serial us, saving ms total)", count,
        static_cast<unsigned long long>(hijacks.load()), static_cast<unsigned long long>(hijack_fail.load()));
    for (int i = 0; i < count; ++i) {
        const auto& r = rows[i];
        log("  site %llx %s calls=%.0f items=%.1f grain=%.1f jobs=%.1f par=%.1f ser=%.1f saving=%.1f ms",
            0x140000000ull + (r.key & 0x7fffffffu), (r.key & 0x80000000u) ? "worker" : "main  ",
            r.calls, r.items, r.grain, r.jobs, r.par, r.ser, r.saving / 1000.0);
    }
}

bool write_jump(std::uint8_t* at, const void* target);
#include "mods.inc"
#include "aiprof.inc"

bool probe_on = false, mods_on = false;
DWORD WINAPI reporter(void*) {
    for (;;) {
        Sleep(30000);
        if (probe_on) report();
        if (mods_on) mods_report();
        if (ai_on) ai_report();
    }
}

bool write_jump(std::uint8_t* at, const void* target) {
    std::uint8_t patch[12] = {0x48, 0xb8};
    const auto address = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(patch + 2, &address, 8);
    patch[10] = 0xff; patch[11] = 0xe0;  // mov rax, target; jmp rax
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, patch, sizeof(patch));
    VirtualProtect(at, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(patch));
    return true;
}

bool make_return_stub() {
    return_stub = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!return_stub) return false;
    static const std::uint8_t code[] = {
        0x50,                                // push rax
        0x52,                                // push rdx
        0x48, 0x83, 0xec, 0x30,              // sub rsp, 0x30 (rsp stays 16-aligned)
        0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x20,  // movdqu [rsp+0x20], xmm0
        0xff, 0x15, 0x16, 0x00, 0x00, 0x00,  // call [rip+0x16] -> tick_on_return
        0x49, 0x89, 0xc3,                    // mov r11, rax
        0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20,  // movdqu xmm0, [rsp+0x20]
        0x48, 0x83, 0xc4, 0x30,              // add rsp, 0x30
        0x5a,                                // pop rdx
        0x58,                                // pop rax
        0x41, 0xff, 0xe3,                    // jmp r11
    };
    static_assert(sizeof(code) == 36, "stub size");
    std::memcpy(return_stub, code, sizeof(code));
    // call [rip+0x16]: next instruction at +18, pointer at +18+0x16 = +40
    const auto target = reinterpret_cast<std::uint64_t>(&tick_on_return);
    std::memcpy(return_stub + 40, &target, 8);
    FlushInstructionCache(GetCurrentProcess(), return_stub, 64);
    return true;
}

void install_pfor(const std::wstring& ini) {
    wchar_t value[4096]{};
    GetPrivateProfileStringW(L"tick", L"serial_sites", L"", value, 4096, ini.c_str());
    for (wchar_t* p = value; *p && serial_count < 256;) {
        wchar_t* next = nullptr;
        const unsigned long long v = std::wcstoull(p, &next, 16);
        if (next == p) { ++p; continue; }
        if (v > 0x140000000ull) serial_rvas[serial_count++] = static_cast<std::uint32_t>(v - 0x140000000ull);
        else if (v) serial_rvas[serial_count++] = static_cast<std::uint32_t>(v);
        p = next;
    }
    if (std::memcmp(reinterpret_cast<void*>(image + jobs_rva), jobs_bytes, sizeof(jobs_bytes)) != 0) {
        log("pfor REFUSED (142573440 bytes differ: other game version?)");
        return;
    }
    if (mode == Mode::probe && !make_return_stub()) { log("pfor REFUSED (no stub memory)"); return; }
    if (!write_jump(reinterpret_cast<std::uint8_t*>(image + jobs_rva), reinterpret_cast<const void*>(&jobs_hook))) {
        log("pfor REFUSED (VirtualProtect)");
        return;
    }
    probe_on = mode == Mode::probe;
    log("pfor=%s serial_sites=%d", mode == Mode::on ? "on" : "probe", serial_count);
}

void install(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\hoi4_tick_patch.log";
    const auto ini = directory + L"\\hoi4_tick_patch.ini";
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"hoi4.exe") != 0) return;
    unsigned exe_stamp = 0, exe_size = 0;
    const char* exe_build = hoi4build::detect(&exe_stamp, &exe_size);
    if (!exe_build) {
        log("REFUSED: unknown hoi4.exe build (stamp %08x, image size %x); no bytes changed",
            exe_stamp, exe_size);
        return;
    }
    log("hoi4.exe build %s", exe_build);
    image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    image_end = image + nt->OptionalHeader.SizeOfImage;
    main_tid = GetCurrentThreadId();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency = frequency.QuadPart > 0 ? frequency.QuadPart : 1;

    wchar_t value[4096]{};
    GetPrivateProfileStringW(L"tick", L"mods", L"off", value, 64, ini.c_str());
    const ModsMode mm = _wcsicmp(value, L"on") == 0 ? ModsMode::on : _wcsicmp(value, L"shadow") == 0
        ? ModsMode::shadow : _wcsicmp(value, L"measure") == 0 ? ModsMode::measure : ModsMode::off;
    linear_limit = static_cast<int>(GetPrivateProfileIntW(L"tick", L"mods_linear_limit", 64, ini.c_str()));
    if (mm != ModsMode::off) {
        const bool ok = install_mods(mm);
        mods_on = ok && mm != ModsMode::on;
        log("mods=%ls %s (linear_limit=%d)", value, ok ? "installed" : "NOT installed", linear_limit);
    }
    GetPrivateProfileStringW(L"tick", L"pfor", L"off", value, 64, ini.c_str());
    mode = _wcsicmp(value, L"on") == 0 ? Mode::on : _wcsicmp(value, L"probe") == 0 ? Mode::probe : Mode::off;
    if (mode != Mode::off) install_pfor(ini);
    if (GetPrivateProfileIntW(L"tick", L"aiprof", 0, ini.c_str()) != 0) install_aiprof();
    if (probe_on || mods_on || ai_on) {
        HANDLE thread = CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    log("initialized version=0.2.0 main_tid=%lu", main_tid);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
