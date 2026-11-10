// EU4 Pops Patch 0.1.0 experimental, for EU4 1.37.5.0 Windows x64.
//
// pops_api.dll (Paradox online services: account, achievements, social) is
// polled twice per frame on the main thread. Its poll loop receives a 1 ms
// timeout and, when it has nothing to do, calls Sleep(1). That idle sleep is
// ~5% of the main thread. This plugin changes the timeout argument to 0, so the
// poll still runs every frame but returns at once. Game logic is untouched.
//
// eu4_pops_patch.ini: [pops] poll_sleep=off (plugin does nothing) | zero (default off)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

// mov rcx,[rdi+0x30]; mov edx,1; call ... (timeout for the poll loop at +0x1bf9f0)
constexpr std::uintptr_t site_rva = 0x6bdd1;
constexpr std::uint8_t site_bytes[] = {0x48, 0x8b, 0x4f, 0x30, 0xba, 0x01, 0x00, 0x00, 0x00, 0xe8};
constexpr std::uintptr_t imm_offset = 5;  // low byte of the edx immediate

std::atomic<LONG> installation_status{0};
std::wstring log_path;

void log(const char* format, ...) noexcept {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    FILE* file = _wfopen(log_path.c_str(), L"ab");
    if (!file) return;
    std::fprintf(file, "[%lu:%lu] %s\n", GetCurrentProcessId(), GetTickCount(), text);
    std::fclose(file);
}

DWORD install(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 1;
    auto* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    const std::wstring directory = path;
    log_path = directory + L"\\eu4_pops_patch.log";
    const auto ini = directory + L"\\eu4_pops_patch.ini";

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto* exe_name = wcsrchr(exe_path, L'\\');
    if (_wcsicmp(exe_name ? exe_name + 1 : exe_path, L"eu4.exe") != 0) {
        installation_status.store(-3);
        return 3;
    }
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"pops", L"poll_sleep", L"off", value, 32, ini.c_str());
    if (_wcsicmp(value, L"zero") != 0) {
        log("disabled by configuration; no bytes changed");
        installation_status.store(3);
        return 0;
    }
    auto* pops = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"pops_api.dll"));
    if (!pops) {
        log("REFUSED: pops_api.dll not loaded; no bytes changed");
        installation_status.store(-4);
        return 4;
    }
    std::uint8_t* site = pops + site_rva;
    if (std::memcmp(site, site_bytes, sizeof(site_bytes)) != 0) {
        log("REFUSED: bytes differ; no bytes changed");
        installation_status.store(-5);
        return 5;
    }
    DWORD old = 0;
    if (!VirtualProtect(site + imm_offset, 1, PAGE_EXECUTE_READWRITE, &old)) {
        log("REFUSED: VirtualProtect failed %lu", GetLastError());
        installation_status.store(-6);
        return 6;
    }
    site[imm_offset] = 0x00;
    VirtualProtect(site + imm_offset, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(site_bytes));
    installation_status.store(1);
    log("initialized version=0.1.0-experimental poll_sleep=zero");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);  // one byte, no threads, no CRT heap
    }
    return TRUE;
}

extern "C" __declspec(dllexport) LONG EU4PopsPatchStatus() {
    return installation_status.load();
}
