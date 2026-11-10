#include "function_patch.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <limits>

namespace modifierpatch {
namespace {

constexpr std::size_t trampoline_offset = 64;

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

void write_absolute_jump(std::uint8_t* output, const void* target) noexcept {
    output[0] = 0x49;
    output[1] = 0xBB;  // mov r11, imm64
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(output + 2, &address, sizeof(address));
    output[10] = 0x41;
    output[11] = 0xFF;
    output[12] = 0xE3;  // jmp r11
    // R11 is caller-saved and unused by the supported prologues. In particular,
    // do not use RAX here: modifier_copy loads its source vtable into RAX before
    // the trampoline's jump back to the original function.
}

}  // namespace

bool prepare_function(std::uint8_t* site, const EntryBytes& expected, std::size_t entry_size,
    const void* hook, FunctionPatch& patch, char* error, std::size_t error_size,
    CallerArgument caller_argument) noexcept {
    if (!site || !hook || entry_size < 5 || entry_size > maximum_entry_size ||
        std::memcmp(site, expected.data(), entry_size) != 0) {
        set_error(error, error_size, "function entry signature or length mismatch");
        return false;
    }
    auto* allocation = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!allocation) {
        set_error(error, error_size, "no memory within rel32 range of function");
        return false;
    }

    // Pass the game's return address in an unused argument register. The caller
    // selects R8 for two-argument functions or R9 to preserve a third argument.
    allocation[0] = 0x4C;
    allocation[1] = 0x8B;
    allocation[2] = caller_argument == CallerArgument::fourth ? 0x0C : 0x04;
    allocation[3] = 0x24;  // mov r8/r9, qword ptr [rsp]
    write_absolute_jump(allocation + 4, hook);
    auto* trampoline = allocation + trampoline_offset;
    std::memcpy(trampoline, expected.data(), entry_size);
    write_absolute_jump(trampoline + entry_size, site + entry_size);
    DWORD old_protection = 0;
    if (!VirtualProtect(allocation, 4096, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        set_error(error, error_size, "cannot make function relay executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), allocation, trampoline_offset + 48);

    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(allocation)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site + 5));
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)()) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        set_error(error, error_size, "function relay is outside rel32 range");
        return false;
    }

    patch.site = site;
    patch.allocation = allocation;
    patch.trampoline = trampoline;
    patch.original = expected;
    patch.entry_size = entry_size;
    patch.replacement.fill(0x90);
    patch.replacement[0] = 0xE9;
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(patch.replacement.data() + 1, &relative, sizeof(relative));
    return true;
}

bool commit_function(FunctionPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!patch.site || !patch.allocation || !patch.trampoline || patch.committed ||
        patch.entry_size < 5 || patch.entry_size > maximum_entry_size ||
        std::memcmp(patch.site, patch.original.data(), patch.entry_size) != 0) {
        set_error(error, error_size, "function changed before commit");
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(patch.site, patch.entry_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        set_error(error, error_size, "cannot make function writable");
        return false;
    }
    std::memcpy(patch.site, patch.replacement.data(), patch.entry_size);
    patch.committed = true;
    FlushInstructionCache(GetCurrentProcess(), patch.site, patch.entry_size);
    DWORD ignored = 0;
    VirtualProtect(patch.site, patch.entry_size, old_protection, &ignored);
    return true;
}

void rollback_function(FunctionPatch& patch) noexcept {
    if (!patch.committed || !patch.site) return;
    DWORD old_protection = 0;
    if (VirtualProtect(patch.site, patch.entry_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::memcpy(patch.site, patch.original.data(), patch.entry_size);
        FlushInstructionCache(GetCurrentProcess(), patch.site, patch.entry_size);
        DWORD ignored = 0;
        VirtualProtect(patch.site, patch.entry_size, old_protection, &ignored);
        patch.committed = false;
    }
}

void discard_function(FunctionPatch& patch) noexcept {
    rollback_function(patch);
    if (patch.allocation) VirtualFree(patch.allocation, 0, MEM_RELEASE);
    patch.allocation = nullptr;
    patch.trampoline = nullptr;
    patch.site = nullptr;
    patch.entry_size = 0;
}

bool prepare_search(std::uint8_t* site, const SearchBytes& expected, std::size_t entry_size,
    const void* relay, SearchPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!site || !relay || entry_size < 5 || entry_size > maximum_search_size ||
        std::memcmp(site, expected.data(), entry_size) != 0) {
        set_error(error, error_size, "search loop signature or length mismatch");
        return false;
    }
    auto* allocation = allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!allocation) {
        set_error(error, error_size, "no memory within rel32 range of search loop");
        return false;
    }
    write_absolute_jump(allocation, relay);
    DWORD old_protection = 0;
    if (!VirtualProtect(allocation, 4096, PAGE_EXECUTE_READ, &old_protection)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        set_error(error, error_size, "cannot make search relay executable");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), allocation, 16);
    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(allocation)) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site + 5));
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)()) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        set_error(error, error_size, "search relay is outside rel32 range");
        return false;
    }
    patch.site = site;
    patch.allocation = allocation;
    patch.original = expected;
    patch.entry_size = entry_size;
    patch.replacement.fill(0x90);
    patch.replacement[0] = 0xE9;
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(patch.replacement.data() + 1, &relative, sizeof(relative));
    return true;
}

bool commit_search(SearchPatch& patch, char* error, std::size_t error_size) noexcept {
    if (!patch.site || !patch.allocation || patch.committed || patch.entry_size < 5 ||
        patch.entry_size > maximum_search_size ||
        std::memcmp(patch.site, patch.original.data(), patch.entry_size) != 0) {
        set_error(error, error_size, "search loop changed before commit");
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(patch.site, patch.entry_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        set_error(error, error_size, "cannot make search loop writable");
        return false;
    }
    std::memcpy(patch.site, patch.replacement.data(), patch.entry_size);
    patch.committed = true;
    FlushInstructionCache(GetCurrentProcess(), patch.site, patch.entry_size);
    DWORD ignored = 0;
    VirtualProtect(patch.site, patch.entry_size, old_protection, &ignored);
    return true;
}

void rollback_search(SearchPatch& patch) noexcept {
    if (!patch.committed || !patch.site) return;
    DWORD old_protection = 0;
    if (VirtualProtect(patch.site, patch.entry_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::memcpy(patch.site, patch.original.data(), patch.entry_size);
        FlushInstructionCache(GetCurrentProcess(), patch.site, patch.entry_size);
        DWORD ignored = 0;
        VirtualProtect(patch.site, patch.entry_size, old_protection, &ignored);
        patch.committed = false;
    }
}

void discard_search(SearchPatch& patch) noexcept {
    rollback_search(patch);
    if (patch.allocation) VirtualFree(patch.allocation, 0, MEM_RELEASE);
    patch.allocation = nullptr;
    patch.site = nullptr;
    patch.entry_size = 0;
}

}  // namespace modifierpatch
