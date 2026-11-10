#include "parallel_patch.hpp"

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
        set_error(error, error_size, "parallel continuation is outside rel32 range");
        return false;
    }
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(displacement, &relative, sizeof(relative));
    return true;
}

}  // namespace

bool prepare_parallel_patch(std::uint8_t* site, const void* continuation,
    const void* secondary, const std::uint8_t* expected, std::size_t site_size,
    const ParallelHookImage& image, std::uint32_t tls_byte_offset, ParallelPatch& patch,
    char* error, std::size_t error_size) noexcept {
    if (!site || !continuation || !expected || site_size < 5 ||
        site_size > parallel_patch_capacity || std::memcmp(site, expected, site_size) != 0) {
        set_error(error, error_size, "parallel site signature or length mismatch");
        return false;
    }
    if (!image.begin || !image.end || !image.continuation_displacement ||
        image.end <= image.begin) {
        set_error(error, error_size, "invalid parallel hook image");
        return false;
    }
    const auto size = static_cast<std::size_t>(image.end - image.begin);
    const auto in_range = [&](const std::uint8_t* pointer) {
        return pointer >= image.begin && pointer + 4 <= image.end;
    };
    if (!size || size > 1024 || !in_range(image.continuation_displacement) ||
        (image.tls_displacement && !in_range(image.tls_displacement)) ||
        (image.secondary_displacement &&
            (!secondary || !in_range(image.secondary_displacement)))) {
        set_error(error, error_size, "invalid parallel hook relocation");
        return false;
    }
    auto* stub = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!stub) {
        set_error(error, error_size, "no memory within rel32 range of parallel site");
        return false;
    }
    std::memcpy(stub, image.begin, size);
    if (image.tls_displacement) {
        const auto offset = static_cast<std::size_t>(image.tls_displacement - image.begin);
        std::memcpy(stub + offset, &tls_byte_offset, sizeof(tls_byte_offset));
    }
    const auto continuation_offset = static_cast<std::size_t>(
        image.continuation_displacement - image.begin);
    if (!write_relative(stub + continuation_offset, continuation, error, error_size)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    if (image.secondary_displacement) {
        const auto secondary_offset = static_cast<std::size_t>(
            image.secondary_displacement - image.begin);
        if (!write_relative(stub + secondary_offset, secondary, error, error_size)) {
            VirtualFree(stub, 0, MEM_RELEASE);
            return false;
        }
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        set_error(error, error_size, "cannot make parallel stub executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, size);

    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site + 5));
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)()) {
        VirtualFree(stub, 0, MEM_RELEASE);
        set_error(error, error_size, "parallel stub is outside rel32 range");
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

bool commit_parallel_patch(ParallelPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!patch.site || !patch.stub || patch.committed || patch.size < 5 ||
        patch.size > parallel_patch_capacity ||
        std::memcmp(patch.site, patch.original, patch.size) != 0) {
        set_error(error, error_size, "parallel site changed before commit");
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(patch.site, patch.size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        set_error(error, error_size, "cannot make parallel site writable");
        return false;
    }
    std::memcpy(patch.site, patch.replacement, patch.size);
    patch.committed = true;
    FlushInstructionCache(GetCurrentProcess(), patch.site, patch.size);
    DWORD ignored = 0;
    VirtualProtect(patch.site, patch.size, old_protection, &ignored);
    return true;
}

void rollback_parallel_patch(ParallelPatch& patch) noexcept {
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

void discard_parallel_patch(ParallelPatch& patch) noexcept {
    rollback_parallel_patch(patch);
    if (patch.stub) VirtualFree(patch.stub, 0, MEM_RELEASE);
    patch.stub = nullptr;
    patch.site = nullptr;
    patch.size = 0;
}

}  // namespace knowledgepatch
