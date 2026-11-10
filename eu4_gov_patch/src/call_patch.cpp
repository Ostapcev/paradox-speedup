#include "call_patch.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <limits>

namespace speedpatch {
namespace {

void set_error(char* output, std::size_t size, const char* message) noexcept {
    if (output && size) {
        std::snprintf(output, size, "%s", message);
    }
}

std::uint8_t* allocate_near(std::uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto step = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = target & ~(step - 1);
    for (std::uintptr_t delta = step; delta < 0x70000000; delta += step) {
        if (delta < origin) {
            if (void* result = VirtualAlloc(
                    reinterpret_cast<void*>(origin - delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(result);
            }
        }
        if (origin <= (std::numeric_limits<std::uintptr_t>::max)() - delta) {
            if (void* result = VirtualAlloc(
                    reinterpret_cast<void*>(origin + delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(result);
            }
        }
    }
    return nullptr;
}

}  // namespace

bool prepare_call(
    std::uint8_t* site,
    const void* expected_target,
    const void* hook,
    CallPatch& patch,
    char* error,
    std::size_t error_size) noexcept {
    if (!site || !expected_target || !hook || site[0] != 0xE8) {
        set_error(error, error_size, "call site is missing or no longer starts with CALL");
        return false;
    }
    std::int32_t current_displacement = 0;
    std::memcpy(&current_displacement, site + 1, sizeof(current_displacement));
    const auto current_target = reinterpret_cast<std::uintptr_t>(site + 5) + current_displacement;
    if (current_target != reinterpret_cast<std::uintptr_t>(expected_target)) {
        set_error(error, error_size, "CALL destination differs from the EU4 1.37.5 profile");
        return false;
    }

    auto* stub = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!stub) {
        set_error(error, error_size, "no executable memory is available within rel32 range");
        return false;
    }

    // mov rax, absolute_hook; jmp rax. The original engine function is untouched.
    std::uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const auto hook_address = reinterpret_cast<std::uintptr_t>(hook);
    std::memcpy(code + 2, &hook_address, sizeof(hook_address));
    std::memcpy(stub, code, sizeof(code));
    DWORD old_protection = 0;
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        set_error(error, error_size, "cannot make the hook stub executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));

    const auto displacement = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site) + 5);
    if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
        displacement > (std::numeric_limits<std::int32_t>::max)()) {
        VirtualFree(stub, 0, MEM_RELEASE);
        set_error(error, error_size, "hook stub is outside rel32 range");
        return false;
    }
    const auto relative = static_cast<std::int32_t>(displacement);

    patch.site = site;
    patch.stub = stub;
    std::memcpy(patch.original, site, sizeof(patch.original));
    patch.replacement[0] = 0xE8;
    std::memcpy(patch.replacement + 1, &relative, sizeof(relative));
    return true;
}

bool commit_call(CallPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!patch.site || !patch.stub || patch.committed ||
        std::memcmp(patch.site, patch.original, sizeof(patch.original)) != 0) {
        set_error(error, error_size, "call site changed while preparing the patch");
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(patch.site, sizeof(patch.original), PAGE_EXECUTE_READWRITE, &old_protection)) {
        set_error(error, error_size, "cannot make the call site writable");
        return false;
    }
    std::memcpy(patch.site, patch.replacement, sizeof(patch.original));
    patch.committed = true;
    FlushInstructionCache(GetCurrentProcess(), patch.site, sizeof(patch.original));
    DWORD ignored = 0;
    VirtualProtect(patch.site, sizeof(patch.original), old_protection, &ignored);
    return true;
}

void rollback_call(CallPatch& patch) noexcept {
    if (!patch.committed || !patch.site) return;
    DWORD old_protection = 0;
    if (VirtualProtect(patch.site, sizeof(patch.original), PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::memcpy(patch.site, patch.original, sizeof(patch.original));
        FlushInstructionCache(GetCurrentProcess(), patch.site, sizeof(patch.original));
        DWORD ignored = 0;
        VirtualProtect(patch.site, sizeof(patch.original), old_protection, &ignored);
        patch.committed = false;
    }
}

void discard_call(CallPatch& patch) noexcept {
    rollback_call(patch);
    if (patch.stub) VirtualFree(patch.stub, 0, MEM_RELEASE);
    patch.stub = nullptr;
    patch.site = nullptr;
}

}  // namespace speedpatch
