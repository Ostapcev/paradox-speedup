// call_patch.hpp — подмена 5-байтного `call rel32` в коде игры (header-only).
// Перенесено из hoi4_bench_helper/src/call_patch.cpp: на месте вызова пишется `call stub`,
// stub (в пределах rel32 от места) = `mov rax, hook; jmp rax`. Исходная функция игры не меняется.
// Перед записью байты места сверяются с ожидаемыми.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <limits>

namespace splug {

struct CallPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t original[5]{};
    std::uint8_t replacement[5]{};
    bool committed = false;
};

inline std::uint8_t* allocate_near(std::uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto step = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = target & ~(step - 1);
    for (std::uintptr_t delta = step; delta < 0x70000000; delta += step) {
        if (delta < origin)
            if (void* r = VirtualAlloc(reinterpret_cast<void*>(origin - delta), 4096,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<std::uint8_t*>(r);
        if (origin <= (std::numeric_limits<std::uintptr_t>::max)() - delta)
            if (void* r = VirtualAlloc(reinterpret_cast<void*>(origin + delta), 4096,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<std::uint8_t*>(r);
    }
    return nullptr;
}

// site — адрес инструкции call (E8 rel32); expected — её 5 исходных байтов.
// Возвращает nullptr при успехе или текст ошибки.
inline const char* prepare_call(std::uint8_t* site, const std::uint8_t (&expected)[5], const void* hook,
                                CallPatch& patch) noexcept {
    if (std::memcmp(site, expected, 5) != 0) return "call site bytes differ";
    auto* stub = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!stub) return "no memory within rel32 range";
    std::uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const auto address = reinterpret_cast<std::uintptr_t>(hook);
    std::memcpy(code + 2, &address, sizeof(address));
    std::memcpy(stub, code, sizeof(code));
    DWORD old = 0;
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return "cannot make stub executable";
    }
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));
    const auto disp = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) -
                      static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site) + 5);
    if (disp < (std::numeric_limits<std::int32_t>::min)() || disp > (std::numeric_limits<std::int32_t>::max)()) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return "stub outside rel32 range";
    }
    const auto rel = static_cast<std::int32_t>(disp);
    patch.site = site;
    patch.stub = stub;
    std::memcpy(patch.original, site, 5);
    patch.replacement[0] = 0xE8;
    std::memcpy(patch.replacement + 1, &rel, 4);
    return nullptr;
}

inline bool commit_call(CallPatch& patch) noexcept {
    if (!patch.site || patch.committed || std::memcmp(patch.site, patch.original, 5) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(patch.site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(patch.site, patch.replacement, 5);
    FlushInstructionCache(GetCurrentProcess(), patch.site, 5);
    DWORD ignored = 0;
    VirtualProtect(patch.site, 5, old, &ignored);
    patch.committed = true;
    return true;
}

// Адрес, на который указывал исходный call (для вызова оригинала из хука).
inline void* call_target(const std::uint8_t* site) noexcept {
    std::int32_t rel = 0;
    std::memcpy(&rel, site + 1, 4);
    return const_cast<std::uint8_t*>(site + 5 + rel);
}

}  // namespace splug
