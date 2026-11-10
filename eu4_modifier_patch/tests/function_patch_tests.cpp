#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "function_patch.hpp"

extern "C" void* modifier_fixture(void* destination, void* source);
extern "C" void* call_modifier_fixture(void* destination, void* source);
extern "C" void fixture_return_site();
extern "C" void* modifier_copy_fixture(void* destination, void* source);
extern "C" void* call_modifier_copy_fixture(void* destination, void* source);
extern "C" void modifier_copy_return_site();
extern "C" void* three_arg_fixture(void* first, void* second, void* third);
extern "C" void* call_three_arg_fixture(void* first, void* second, void* third);
extern "C" void three_arg_return_site();
extern "C" void* cache_reset_fixture(void* cache, void* country);
extern "C" void* call_cache_reset_fixture(void* cache, void* country);
extern "C" void cache_reset_return_site();
extern "C" void* cache_rebuild_fixture(void* cache, void* country);
extern "C" void* call_cache_rebuild_fixture(void* cache, void* country);
extern "C" void cache_rebuild_return_site();

namespace {

modifierpatch::FunctionPatch* active_patch = nullptr;
void* seen_destination = nullptr;
void* seen_source = nullptr;
void* seen_third = nullptr;
const void* seen_caller = nullptr;
int hook_calls = 0;
int three_arg_hook_calls = 0;

void* fixture_hook(void* destination, void* source, const void* caller) {
    using Function = void* (*)(void*, void*);
    seen_destination = destination;
    seen_source = source;
    seen_caller = caller;
    ++hook_calls;
    return reinterpret_cast<Function>(active_patch->trampoline)(destination, source);
}

void* three_arg_hook(void* first, void* second, void* third, const void* caller) {
    using Function = void* (*)(void*, void*, void*);
    seen_destination = first;
    seen_source = second;
    seen_third = third;
    seen_caller = caller;
    ++three_arg_hook_calls;
    return reinterpret_cast<Function>(active_patch->trampoline)(first, second, third);
}

}  // namespace

int main() {
    constexpr modifierpatch::EntryBytes expected{
        0x40, 0x55, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x48};
    auto wrong = expected;
    wrong[0] = 0;
    modifierpatch::FunctionPatch patch;
    active_patch = &patch;
    char error[256]{};
    auto* site = reinterpret_cast<std::uint8_t*>(&modifier_fixture);
    int destination = 10;
    int source = 7;
    if (modifier_fixture(&destination, &source) != &destination || destination != 17) {
        std::fputs("Original fixture failed\n", stderr);
        return 1;
    }
    const auto hook = reinterpret_cast<const void*>(&fixture_hook);
    if (modifierpatch::prepare_function(site, wrong, 8, hook, patch, error, sizeof(error))) {
        std::fputs("Signature guard accepted wrong bytes\n", stderr);
        return 2;
    }
    if (!modifierpatch::prepare_function(site, expected, 8, hook, patch,
            error, sizeof(error))) {
        std::fprintf(stderr, "prepare_function failed: %s\n", error);
        return 3;
    }
    if (!modifierpatch::commit_function(patch, error, sizeof(error))) {
        std::fprintf(stderr, "commit_function failed: %s\n", error);
        modifierpatch::discard_function(patch);
        return 4;
    }
    if (call_modifier_fixture(&destination, &source) != &destination || destination != 24 ||
        hook_calls != 1 || seen_destination != &destination || seen_source != &source ||
        seen_caller != reinterpret_cast<const void*>(&fixture_return_site)) {
        std::fputs("Hook, caller capture, or trampoline changed arguments/results\n", stderr);
        modifierpatch::discard_function(patch);
        return 5;
    }
    modifierpatch::rollback_function(patch);
    if (std::memcmp(site, expected.data(), 8) != 0 ||
        call_modifier_fixture(&destination, &source) != &destination || destination != 31 ||
        hook_calls != 1) {
        std::fputs("Rollback did not restore the original function\n", stderr);
        modifierpatch::discard_function(patch);
        return 6;
    }
    modifierpatch::discard_function(patch);

    // Regression test for the 0.3.0 crash: the copied prologue loads RAX from
    // the source object, and the jump back must not overwrite that live value.
    constexpr modifierpatch::EntryBytes modifier_copy_expected{
        0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x02};
    if (call_modifier_copy_fixture(&destination, &source) != &destination) {
        std::fputs("Original RAX-live fixture failed\n", stderr);
        return 11;
    }
    active_patch = &patch;
    if (!modifierpatch::prepare_function(
            reinterpret_cast<std::uint8_t*>(&modifier_copy_fixture), modifier_copy_expected, 11,
            hook, patch, error, sizeof(error)) ||
        !modifierpatch::commit_function(patch, error, sizeof(error))) {
        std::fprintf(stderr, "RAX-live patch failed: %s\n", error);
        modifierpatch::discard_function(patch);
        return 12;
    }
    if (call_modifier_copy_fixture(&destination, &source) != &destination || hook_calls != 2 ||
        seen_destination != &destination || seen_source != &source ||
        seen_caller != reinterpret_cast<const void*>(&modifier_copy_return_site)) {
        std::fputs("Trampoline overwrote RAX from the copied prologue\n", stderr);
        modifierpatch::discard_function(patch);
        return 13;
    }
    modifierpatch::rollback_function(patch);
    if (std::memcmp(reinterpret_cast<const void*>(&modifier_copy_fixture),
            modifier_copy_expected.data(), 11) != 0 ||
        call_modifier_copy_fixture(&destination, &source) != &destination || hook_calls != 2) {
        std::fputs("RAX-live rollback failed\n", stderr);
        modifierpatch::discard_function(patch);
        return 14;
    }
    modifierpatch::discard_function(patch);

    constexpr modifierpatch::EntryBytes cache_reset_expected{
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
    auto* high_cache = reinterpret_cast<void*>(0x123456789abcdef0ULL);
    auto* high_country = reinterpret_cast<void*>(0x1020304050607080ULL);
    active_patch = &patch;
    if (!modifierpatch::prepare_function(reinterpret_cast<std::uint8_t*>(&cache_reset_fixture),
            cache_reset_expected, 9, hook, patch, error, sizeof(error)) ||
        !modifierpatch::commit_function(patch, error, sizeof(error))) {
        std::fprintf(stderr, "Nine-byte cache reset patch failed: %s\n", error);
        modifierpatch::discard_function(patch);
        return 15;
    }
    if (call_cache_reset_fixture(high_cache, high_country) != high_cache || hook_calls != 3 ||
        seen_destination != high_cache || seen_source != high_country ||
        seen_caller != reinterpret_cast<const void*>(&cache_reset_return_site)) {
        std::fputs("Cache reset trampoline truncated a 64-bit argument\n", stderr);
        modifierpatch::discard_function(patch);
        return 16;
    }
    modifierpatch::rollback_function(patch);
    if (std::memcmp(reinterpret_cast<const void*>(&cache_reset_fixture),
            cache_reset_expected.data(), 9) != 0 ||
        call_cache_reset_fixture(high_cache, high_country) != high_cache || hook_calls != 3) {
        std::fputs("Nine-byte cache reset rollback failed\n", stderr);
        modifierpatch::discard_function(patch);
        return 17;
    }
    modifierpatch::discard_function(patch);

    constexpr modifierpatch::EntryBytes cache_rebuild_expected{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55};
    active_patch = &patch;
    if (!modifierpatch::prepare_function(reinterpret_cast<std::uint8_t*>(&cache_rebuild_fixture),
            cache_rebuild_expected, 11, hook, patch, error, sizeof(error)) ||
        !modifierpatch::commit_function(patch, error, sizeof(error))) {
        std::fprintf(stderr, "Cache rebuild patch failed: %s\n", error);
        modifierpatch::discard_function(patch);
        return 18;
    }
    if (call_cache_rebuild_fixture(high_cache, high_country) != high_country || hook_calls != 4 ||
        seen_destination != high_cache || seen_source != high_country ||
        seen_caller != reinterpret_cast<const void*>(&cache_rebuild_return_site)) {
        std::fputs("Cache rebuild trampoline changed 64-bit arguments/results\n", stderr);
        modifierpatch::discard_function(patch);
        return 19;
    }
    modifierpatch::rollback_function(patch);
    if (std::memcmp(reinterpret_cast<const void*>(&cache_rebuild_fixture),
            cache_rebuild_expected.data(), 11) != 0 ||
        call_cache_rebuild_fixture(high_cache, high_country) != high_country || hook_calls != 4) {
        std::fputs("Cache rebuild rollback failed\n", stderr);
        modifierpatch::discard_function(patch);
        return 20;
    }
    modifierpatch::discard_function(patch);

    constexpr modifierpatch::EntryBytes three_arg_expected{
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55};
    int third = 99;
    if (three_arg_fixture(&destination, &source, &third) != &third) {
        std::fputs("Original three-argument fixture failed\n", stderr);
        return 7;
    }
    active_patch = &patch;
    if (!modifierpatch::prepare_function(reinterpret_cast<std::uint8_t*>(&three_arg_fixture),
            three_arg_expected, 11, reinterpret_cast<const void*>(&three_arg_hook), patch,
            error, sizeof(error), modifierpatch::CallerArgument::fourth) ||
        !modifierpatch::commit_function(patch, error, sizeof(error))) {
        std::fprintf(stderr, "Three-argument patch failed: %s\n", error);
        modifierpatch::discard_function(patch);
        return 8;
    }
    if (call_three_arg_fixture(&destination, &source, &third) != &third ||
        three_arg_hook_calls != 1 || seen_destination != &destination || seen_source != &source ||
        seen_third != &third || seen_caller != reinterpret_cast<const void*>(&three_arg_return_site)) {
        std::fputs("Fourth-argument relay did not preserve the original third argument\n", stderr);
        modifierpatch::discard_function(patch);
        return 9;
    }
    modifierpatch::rollback_function(patch);
    if (std::memcmp(reinterpret_cast<const void*>(&three_arg_fixture), three_arg_expected.data(), 11) != 0 ||
        call_three_arg_fixture(&destination, &source, &third) != &third || three_arg_hook_calls != 1) {
        std::fputs("Three-argument rollback failed\n", stderr);
        modifierpatch::discard_function(patch);
        return 10;
    }
    modifierpatch::discard_function(patch);
    std::puts("function patch tests passed: signatures, all arguments, trampolines, results, rollback");
    return 0;
}
