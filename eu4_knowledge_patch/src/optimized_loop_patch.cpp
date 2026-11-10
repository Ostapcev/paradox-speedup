#include "optimized_loop_patch.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <limits>

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

bool write_relative(std::uint8_t* displacement, const void* target,
    char* error, std::size_t error_size) noexcept {
    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(target)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(displacement + 4));
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)()) {
        set_error(error, error_size, "loop destination is outside rel32 range");
        return false;
    }
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(displacement, &relative, sizeof(relative));
    return true;
}

bool valid_image(const LoopHookImage& image, std::size_t& size) noexcept {
    if (!image.begin || !image.end || !image.tls_displacement ||
        !image.found_displacement || !image.out_displacement ||
        !image.continuation_displacement || image.end <= image.begin) {
        return false;
    }
    size = static_cast<std::size_t>(image.end - image.begin);
    const auto in_range = [&](const std::uint8_t* displacement) {
        return displacement >= image.begin && displacement + 4 <= image.end;
    };
    return size <= 512 && in_range(image.tls_displacement) &&
        in_range(image.found_displacement) && in_range(image.out_displacement) &&
        in_range(image.continuation_displacement);
}

}  // namespace

bool prepare_loop(std::uint8_t* site, const void* continuation,
    const void* found, const void* out, const std::uint8_t* expected, std::size_t site_size,
    const LoopHookImage& image, std::uint32_t tls_byte_offset, LoopPatch& patch,
    char* error, std::size_t error_size) noexcept {
    std::size_t hook_size = 0;
    if ((site_size != core_loop_site_size && site_size != claim_loop_site_size) ||
        !site || !continuation || !found || !out || !expected ||
        std::memcmp(site, expected, site_size) != 0) {
        set_error(error, error_size, "hot loop signature mismatch");
        return false;
    }
    if (!valid_image(image, hook_size) || tls_byte_offset > 0x7FFFFFFF) {
        set_error(error, error_size, "invalid generated hook image or TLS offset");
        return false;
    }
    auto* stub = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!stub) {
        set_error(error, error_size, "no memory within rel32 range of the hot loop");
        return false;
    }
    std::memcpy(stub, image.begin, hook_size);
    const auto offset = [&](const std::uint8_t* symbol) {
        return static_cast<std::size_t>(symbol - image.begin);
    };
    const auto tls_offset = offset(image.tls_displacement);
    const auto found_offset = offset(image.found_displacement);
    const auto out_offset = offset(image.out_displacement);
    const auto continuation_offset = offset(image.continuation_displacement);
    std::memcpy(stub + tls_offset, &tls_byte_offset, sizeof(tls_byte_offset));
    if (!write_relative(stub + found_offset, found, error, error_size) ||
        !write_relative(stub + out_offset, out, error, error_size) ||
        !write_relative(stub + continuation_offset, continuation, error, error_size)) {
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
    patch.size = site_size;
    std::memcpy(patch.original, site, patch.size);
    patch.replacement[0] = 0xE9;
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(patch.replacement + 1, &relative, sizeof(relative));
    std::memset(patch.replacement + 5, 0x90, patch.size - 5);
    return true;
}

bool commit_loop(LoopPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!patch.site || !patch.stub || patch.committed ||
        (patch.size != core_loop_site_size && patch.size != claim_loop_site_size) ||
        std::memcmp(patch.site, patch.original, patch.size) != 0) {
        set_error(error, error_size, "hot loop changed before commit");
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(patch.site, patch.size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        set_error(error, error_size, "cannot make the hot loop writable");
        return false;
    }
    std::memcpy(patch.site, patch.replacement, patch.size);
    patch.committed = true;
    FlushInstructionCache(GetCurrentProcess(), patch.site, patch.size);
    DWORD ignored = 0;
    VirtualProtect(patch.site, patch.size, old_protection, &ignored);
    return true;
}

void rollback_loop(LoopPatch& patch) noexcept {
    if (!patch.committed || !patch.site) return;
    DWORD old_protection = 0;
    if (VirtualProtect(patch.site, patch.size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::memcpy(patch.site, patch.original, patch.size);
        FlushInstructionCache(GetCurrentProcess(), patch.site, patch.size);
        DWORD ignored = 0;
        VirtualProtect(patch.site, patch.size, old_protection, &ignored);
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
