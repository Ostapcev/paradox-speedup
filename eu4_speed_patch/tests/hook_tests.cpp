#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "call_patch.hpp"

namespace {

__declspec(noinline) int replacement_hook() {
    return 99;
}

}  // namespace

int main() {
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!page) {
        std::fprintf(stderr, "VirtualAlloc failed\n");
        return 1;
    }

    auto* target = page + 0x100;
    const std::uint8_t target_code[] = {0xB8, 42, 0, 0, 0, 0xC3};  // return 42
    std::memcpy(target, target_code, sizeof(target_code));
    page[0] = 0xE8;
    const std::int32_t displacement = static_cast<std::int32_t>(target - (page + 5));
    std::memcpy(page + 1, &displacement, sizeof(displacement));
    page[5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), page, 4096);

    using TestFn = int (*)();
    const auto invoke = reinterpret_cast<TestFn>(page);
    if (invoke() != 42) {
        std::fprintf(stderr, "test fixture did not return 42\n");
        return 2;
    }

    speedpatch::CallPatch patch;
    char error[160]{};
    if (!speedpatch::prepare_call(page, target, reinterpret_cast<const void*>(&replacement_hook),
            patch, error, sizeof(error))) {
        std::fprintf(stderr, "prepare_call failed: %s\n", error);
        return 3;
    }
    if (!speedpatch::commit_call(patch, error, sizeof(error)) || invoke() != 99) {
        std::fprintf(stderr, "committed hook failed: %s\n", error);
        return 4;
    }
    speedpatch::rollback_call(patch);
    if (invoke() != 42) {
        std::fprintf(stderr, "rollback did not restore the original call\n");
        return 5;
    }
    speedpatch::discard_call(patch);
    VirtualFree(page, 0, MEM_RELEASE);
    std::puts("hook tests passed: prepare, commit, execution, rollback");
    return 0;
}
