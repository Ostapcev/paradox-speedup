#include "hot_loop_patch.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <limits>

extern "C" const std::uint8_t knowledge_hook_begin[];
extern "C" const std::uint8_t knowledge_hook_end[];
extern "C" const std::uint8_t knowledge_hook_found_displacement[];
extern "C" const std::uint8_t knowledge_hook_out_displacement[];

namespace knowledgepatch {
namespace {

void set_error(char* output, std::size_t size, const char* message) noexcept {
    if (output && size) std::snprintf(output, size, "%s", message);
}

std::uint8_t* allocate_near(std::uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto step = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = target & ~(step - 1);
    for (std::uintptr_t delta = step; delta < 0x70000000; delta += step) {
        if (delta < origin) {
            if (void* result = VirtualAlloc(reinterpret_cast<void*>(origin - delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(result);
            }
        }
        if (origin <= (std::numeric_limits<std::uintptr_t>::max)() - delta) {
            if (void* result = VirtualAlloc(reinterpret_cast<void*>(origin + delta), 4096,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(result);
            }
        }
    }
    return nullptr;
}

bool write_relative(
    std::uint8_t* displacement,
    const void* target,
    char* error,
    std::size_t error_size) noexcept {
    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(target)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(displacement + 4));
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)()) {
        set_error(error, error_size, "loop continuation is outside rel32 range");
        return false;
    }
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(displacement, &relative, sizeof(relative));
    return true;
}

}  // namespace

bool prepare_loop(
    std::uint8_t* site,
    const void* found,
    const void* out,
    const std::uint8_t expected[loop_site_size],
    LoopPatch& patch,
    char* error,
    std::size_t error_size) noexcept {
    if (!site || !found || !out || !expected ||
        std::memcmp(site, expected, loop_site_size) != 0) {
        set_error(error, error_size, "hot loop signature mismatch");
        return false;
    }
    const auto hook_size = static_cast<std::size_t>(knowledge_hook_end - knowledge_hook_begin);
    if (!hook_size || hook_size > 512) {
        set_error(error, error_size, "invalid generated hook size");
        return false;
    }
    auto* stub = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!stub) {
        set_error(error, error_size, "no memory within rel32 range of the hot loop");
        return false;
    }
    std::memcpy(stub, knowledge_hook_begin, hook_size);
    const auto found_offset = static_cast<std::size_t>(
        knowledge_hook_found_displacement - knowledge_hook_begin);
    const auto out_offset = static_cast<std::size_t>(
        knowledge_hook_out_displacement - knowledge_hook_begin);
    if (found_offset + 4 > hook_size || out_offset + 4 > hook_size ||
        !write_relative(stub + found_offset, found, error, error_size) ||
        !write_relative(stub + out_offset, out, error, error_size)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        set_error(error, error_size, "cannot make the loop stub executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, hook_size);

    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site + 5));
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)()) {
        VirtualFree(stub, 0, MEM_RELEASE);
        set_error(error, error_size, "loop stub is outside rel32 range");
        return false;
    }
    patch.site = site;
    patch.stub = stub;
    std::memcpy(patch.original, site, loop_site_size);
    patch.replacement[0] = 0xE9;
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(patch.replacement + 1, &relative, sizeof(relative));
    std::memset(patch.replacement + 5, 0x90, loop_site_size - 5);
    return true;
}

bool commit_loop(LoopPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!patch.site || !patch.stub || patch.committed ||
        std::memcmp(patch.site, patch.original, loop_site_size) != 0) {
        set_error(error, error_size, "hot loop changed before commit");
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(patch.site, loop_site_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        set_error(error, error_size, "cannot make the hot loop writable");
        return false;
    }
    std::memcpy(patch.site, patch.replacement, loop_site_size);
    patch.committed = true;
    FlushInstructionCache(GetCurrentProcess(), patch.site, loop_site_size);
    DWORD ignored = 0;
    VirtualProtect(patch.site, loop_site_size, old_protection, &ignored);
    return true;
}

void rollback_loop(LoopPatch& patch) noexcept {
    if (!patch.committed || !patch.site) return;
    DWORD old_protection = 0;
    if (VirtualProtect(patch.site, loop_site_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::memcpy(patch.site, patch.original, loop_site_size);
        FlushInstructionCache(GetCurrentProcess(), patch.site, loop_site_size);
        DWORD ignored = 0;
        VirtualProtect(patch.site, loop_site_size, old_protection, &ignored);
        patch.committed = false;
    }
}

void discard_loop(LoopPatch& patch) noexcept {
    rollback_loop(patch);
    if (patch.stub) VirtualFree(patch.stub, 0, MEM_RELEASE);
    patch.stub = nullptr;
    patch.site = nullptr;
}

}  // namespace knowledgepatch
