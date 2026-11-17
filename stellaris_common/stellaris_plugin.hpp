// stellaris_plugin.hpp — common plugin boilerplate for Stellaris plugins (header-only).
//
// What every plugin needs, in one place (in the HoI4 plugins each file has its own copy):
//   splug::init(module, L"stellaris_foo")  — log/ini next to the DLL, checks that the host is
//       stellaris.exe of a known build; returns false (and logs REFUSED) otherwise.
//   splug::log("fmt", ...)                 — <name>.log next to the DLL, thread-safe.
//   splug::ini_string / ini_int / ini_mode — <name>.ini next to the DLL.
//   splug::rva(x)                          — pointer to image + RVA (x may also be a VA 0x14...).
//   splug::bytes_match(rva, bytes)         — the expected original bytes at a hook site.
//   splug::write_jump(at, target)          — 12-byte absolute jump (mov rax, imm64; jmp rax).
#pragma once

#include <windows.h>

#include "stellaris_build.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

namespace splug {

inline constexpr std::uint64_t kImageBase = 0x140000000ull;

struct State {
    std::wstring dir, log_path, ini_path;
    std::uintptr_t image = 0, image_end = 0;
    const char* build = nullptr;
    DWORD main_tid = 0;
    std::mutex log_mutex;
};

inline State& state() {
    static State s;
    return s;
}

inline void log(const char* format, ...) noexcept {
    char text[2048];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    State& s = state();
    std::lock_guard<std::mutex> lock(s.log_mutex);
    FILE* file = _wfopen(s.log_path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "[%lu:%lu:%lu] %s\n", GetCurrentProcessId(), GetCurrentThreadId(),
                 GetTickCount(), text);
    std::fclose(file);
}

inline std::wstring ini_string(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t value[4096]{};
    GetPrivateProfileStringW(section, key, def, value, 4096, state().ini_path.c_str());
    return value;
}

inline int ini_int(const wchar_t* section, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, state().ini_path.c_str()));
}

// off | shadow | on (also accepts "disabled" as off). Unknown values -> off.
enum class Mode { off, shadow, on };

inline Mode ini_mode(const wchar_t* section, const wchar_t* key, const wchar_t* def = L"off") {
    const std::wstring v = ini_string(section, key, def);
    if (_wcsicmp(v.c_str(), L"on") == 0) return Mode::on;
    if (_wcsicmp(v.c_str(), L"shadow") == 0) return Mode::shadow;
    return Mode::off;
}

inline const char* mode_name(Mode m) {
    return m == Mode::on ? "on" : m == Mode::shadow ? "shadow" : "off";
}

template <class T = std::uint8_t>
inline T* rva(std::uint64_t value) noexcept {
    if (value >= kImageBase) value -= kImageBase;
    return reinterpret_cast<T*>(state().image + value);
}

template <std::size_t N>
inline bool bytes_match(std::uint64_t site, const std::uint8_t (&expected)[N]) noexcept {
    return std::memcmp(rva(site), expected, N) == 0;
}

inline bool write_bytes(void* at, const void* data, std::size_t size) noexcept {
    DWORD old = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, data, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// Overwrites 12 bytes at `at` with an absolute jump to `target` (clobbers rax).
inline bool write_jump(void* at, const void* target) noexcept {
    std::uint8_t patch[12] = {0x48, 0xb8};
    const auto address = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(patch + 2, &address, 8);
    patch[10] = 0xff;
    patch[11] = 0xe0;  // mov rax, target; jmp rax
    return write_bytes(at, patch, sizeof(patch));
}

// Sets up log/ini paths from the plugin DLL location and verifies the host process.
// Returns false when the plugin must not change anything.
inline bool init(HMODULE module, const wchar_t* name) {
    State& s = state();
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    s.dir = path;
    s.log_path = s.dir + L"\\" + name + L".log";
    s.ini_path = s.dir + L"\\" + name + L".ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"stellaris.exe") != 0) return false;

    unsigned stamp = 0, size = 0;
    s.build = stellarisbuild::detect(&stamp, &size);
    if (!s.build) {
        log("REFUSED: unknown stellaris.exe build (stamp %08x, image size %x); no bytes changed",
            stamp, size);
        return false;
    }
    s.image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    s.image_end = s.image + size;
    s.main_tid = GetCurrentThreadId();
    return true;
}

}  // namespace splug
