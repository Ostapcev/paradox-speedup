// EU4 Det Patch 0.1.0 (test tool), for EU4 1.37.5.0 Windows x64.
//
// Single-player picks a fresh random seed on load, so two runs of the same save
// diverge. For A/B checks of logic changes this tool makes runs reproducible:
// once the save is loaded (game paused, date stable for 3 s) it writes a fixed
// seed and count into the gamestate. Test use only.
//
// eu4_det_patch.ini [det]:
//   mode=off | dump (write the first 0x10000 gamestate bytes to a file) | seed
//   seed_offset=0x... count_offset=0x...  seed=N  count=N
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

constexpr std::uintptr_t gamestate_rva = 0x233fe78;
constexpr std::size_t date_offset = 0x1dd0;
constexpr std::size_t dump_size = 0x10000;

std::wstring directory;
std::wstring ini;

void log(const char* format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    FILE* file = _wfopen((directory + L"\\eu4_det_patch.log").c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "[%lu] %s\n", GetTickCount(), text);
    std::fclose(file);
}

unsigned long long read_number(const wchar_t* key) {
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"det", key, L"0", value, 64, ini.c_str());
    return std::wcstoull(value, nullptr, 0);
}

DWORD WINAPI watcher(void* parameter) {
    const bool dump = reinterpret_cast<std::uintptr_t>(parameter) == 1;
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto** global = reinterpret_cast<std::uint8_t**>(image + gamestate_rva);
    int last_date = 0;
    int stable_ms = 0;
    for (;;) {
        Sleep(50);
        auto* gs = *global;
        const int date = gs ? *reinterpret_cast<volatile int*>(gs + date_offset) : 0;
        if (!date || date != last_date) {
            last_date = date;
            stable_ms = 0;
            continue;
        }
        stable_ms += 50;
        if (stable_ms < 3000) continue;
        if (dump) {
            FILE* file = _wfopen((directory + L"\\eu4_det_gamestate.bin").c_str(), L"wb");
            if (file) {
                std::fwrite(gs, 1, dump_size, file);
                std::fclose(file);
            }
            file = _wfopen((directory + L"\\eu4_det_data.bin").c_str(), L"wb");
            if (file) {
                std::fwrite(reinterpret_cast<void*>(image + 0x1fb0000), 1, 0x4b4fb4, file);
                std::fclose(file);
            }
            log("dumped gamestate %p date=%d", static_cast<void*>(gs), date);
        } else {
            const auto seed_offset = read_number(L"seed_offset");
            const auto count_offset = read_number(L"count_offset");
            auto* seed = reinterpret_cast<std::uint32_t*>(gs + seed_offset);
            auto* count = reinterpret_cast<std::uint32_t*>(gs + count_offset);
            log("date=%d before seed=%u count=%u", date, *seed, *count);
            *seed = static_cast<std::uint32_t>(read_number(L"seed"));
            *count = static_cast<std::uint32_t>(read_number(L"count"));
            log("set seed=%u count=%u", *seed, *count);
        }
        return 0;
    }
}

// mode=trace: log the call stack when the save writes the random seed key
using CstrFn = void (*)(void*, const char*);
using RawFn = void (*)(void*, const char*, std::int64_t);
CstrFn original_cstr = nullptr;
RawFn original_raw = nullptr;
std::uintptr_t image_base = 0;
int traces = 0;

void trace_stack(const char* what) {
    void* frames[16]{};
    const auto n = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
    char line[480];
    int at = std::snprintf(line, sizeof(line), "TRACE %s:", what);
    for (int i = 0; i < n && at < 460; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
        at += std::snprintf(line + at, sizeof(line) - at, " %llx", static_cast<unsigned long long>(
            a >= image_base && a < image_base + 0x3000000 ? a - image_base + 0x140000000 : a));
    }
    log("%s", line);
}
bool interesting(const char* text, std::size_t n) {
    static const char key[] = "multiplayer_random";
    return n >= sizeof(key) - 1 && std::memcmp(text, key, sizeof(key) - 1) == 0;
}
int after = 0;
void cstr_hook(void* stream, const char* text) {
    if (traces < 8 && (interesting(text, std::strlen(text)) || after > 0)) {
        ++traces;
        after = interesting(text, std::strlen(text)) ? 3 : after - 1;
        log("cstr '%.40s'", text);
        trace_stack("cstr");
    }
    original_cstr(stream, text);
}
void raw_hook(void* stream, const char* text, std::int64_t n) {
    if (traces < 8 && n > 0 && (interesting(text, static_cast<std::size_t>(n)) || after > 0)) {
        ++traces;
        after = interesting(text, static_cast<std::size_t>(n)) ? 3 : after - 1;
        log("raw '%.*s'", static_cast<int>(n < 40 ? n : 40), text);
        trace_stack("raw");
    }
    original_raw(stream, text, n);
}
void install_trace() {
    image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto** vt = reinterpret_cast<void**>(image_base + 0x1dc0358);
    DWORD old = 0;
    VirtualProtect(vt, 16 * 8, PAGE_READWRITE, &old);
    original_cstr = reinterpret_cast<CstrFn>(vt[7]);
    original_raw = reinterpret_cast<RawFn>(vt[10]);
    vt[7] = reinterpret_cast<void*>(&cstr_hook);
    vt[10] = reinterpret_cast<void*>(&raw_hook);
    VirtualProtect(vt, 16 * 8, old, &old);
    log("trace installed");
}

// mode=seed: the reseed function 140ad0880 (seed 0x1423494b4 from time, count
// 0x1423494b0 = 0) becomes: seed = fixed; count = 0; ret
void install_fixed_seed(std::uint32_t seed) {
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* code = reinterpret_cast<std::uint8_t*>(image + 0xad0880);
    static const std::uint8_t expected[] = {0x48, 0x83, 0xec, 0x28, 0x80, 0x3d};
    if (std::memcmp(code, expected, sizeof(expected)) != 0) {
        log("REFUSED: reseed bytes differ");
        return;
    }
    std::uint8_t patch[21];
    const auto at = reinterpret_cast<std::uintptr_t>(code);
    const auto seed_disp = static_cast<std::int32_t>((image + 0x23494b4) - (at + 10));
    const auto count_disp = static_cast<std::int32_t>((image + 0x23494b0) - (at + 20));
    const std::uint32_t zero = 0;
    patch[0] = 0xc7; patch[1] = 0x05;
    std::memcpy(patch + 2, &seed_disp, 4);
    std::memcpy(patch + 6, &seed, 4);
    patch[10] = 0xc7; patch[11] = 0x05;
    std::memcpy(patch + 12, &count_disp, 4);
    std::memcpy(patch + 16, &zero, 4);
    patch[20] = 0xc3;
    DWORD old = 0;
    VirtualProtect(code, sizeof(patch), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(code, patch, sizeof(patch));
    VirtualProtect(code, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(patch));
    log("fixed seed %u installed", seed);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    if (auto* slash = wcsrchr(path, L'\\')) *slash = L'\0';
    directory = path;
    ini = directory + L"\\eu4_det_patch.ini";
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto* name = wcsrchr(exe, L'\\');
    if (_wcsicmp(name ? name + 1 : exe, L"eu4.exe") != 0) return TRUE;
    wchar_t mode[16]{};
    GetPrivateProfileStringW(L"det", L"mode", L"off", mode, 16, ini.c_str());
    std::uintptr_t kind = 0;
    if (_wcsicmp(mode, L"dump") == 0) kind = 1;
    else if (_wcsicmp(mode, L"seed") == 0) {
        install_fixed_seed(static_cast<std::uint32_t>(read_number(L"seed")));
        return TRUE;
    }
    else if (_wcsicmp(mode, L"trace") == 0) {
        install_trace();
        return TRUE;
    }
    if (!kind) return TRUE;
    HANDLE thread = CreateThread(nullptr, 0, watcher, reinterpret_cast<void*>(kind), 0, nullptr);
    if (thread) CloseHandle(thread);
    return TRUE;
}
