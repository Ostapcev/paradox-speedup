#include "eu4_build.hpp"
// ---- provview: 0x2FFC60 without its copy of the province's modifier list (Bellows
// province_modifier_view, MIT). 0x2FFC60(country, out, province, culture) copies the province's
// modifier list (+0x998) without the placeholders 0x286 into a fresh sparse list only so that
// 0x3003B0 -> 0x300720 can read one value from it (the first entry with id 0x266 or 0x267).
// Filtering keeps the order, so a sparse view of the province's own entries gives the same value
// while the registry's value objects for 0x266 / 0x267 are unset (0x300720 hands the list to
// them otherwise); then, and for calls without a province, the game's function runs. Called for
// every owned province by the daily knowledge refresh (0x93FE30, parallel under
// eu4_knowledge_patch) and by AI code. shadow: both run, the game's value is returned; on:
// every 1024th call is compared and a mismatch turns the module off.
constexpr std::uintptr_t provview_rva = 0x2FFC60;
constexpr std::uintptr_t provview_value_rva = 0x3003B0;
constexpr std::uintptr_t provview_list_rva = 0x2FFCB7;      // lea r12,[r8+998h]
constexpr std::uintptr_t provview_registry_rva = 0x3007BF;  // mov r8,[registry]
constexpr std::uintptr_t provview_post267_rva = 0x30080A;   // mov rcx,[r8+0E6E0h]
constexpr std::uintptr_t provview_post266_rva = 0x300959;   // mov rcx,[r8+0E680h]
constexpr std::size_t kViewDense = 0x10, kViewEntries = 0x58, kViewCount = 0x64;

using ProvValueFn = std::int32_t* (*)(void* country, std::int32_t* out, void* list, void* culture);
ProvValueFn provview_next = nullptr;
ProvValueFn provview_value = nullptr;
const unsigned char* const* provview_registry = nullptr;
std::size_t provview_list = 0, provview_post266 = 0, provview_post267 = 0;
Stats provview_stats;

struct alignas(8) ModListView {
    unsigned char bytes[0xC8] = {};
};

std::int32_t* provview_through_view(void* country, std::int32_t* out, void* province, void* culture) {
    const auto* list = static_cast<const unsigned char*>(province) + provview_list;
    ModListView view;
    view.bytes[kViewDense] = 0;
    const auto* entries = rd<const std::int32_t*>(list, kViewEntries);
    const auto count = rd<std::int32_t>(list, kViewCount);
    std::memcpy(view.bytes + kViewEntries, &entries, sizeof(entries));
    std::memcpy(view.bytes + kViewCount, &count, sizeof(count));
    return provview_value(country, out, view.bytes, culture);
}

std::int32_t* provview_hook(void* country, std::int32_t* out, void* province, void* culture) {
    Stats& s = provview_stats;
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned char* registry = *provview_registry;
    if (!province || s.disabled.load(std::memory_order_relaxed) || !registry ||
        rd<const void*>(registry, provview_post266) != nullptr || rd<const void*>(registry, provview_post267) != nullptr) {
        s.skips.fetch_add(1, std::memory_order_relaxed);
        return provview_next(country, out, province, culture);
    }
    if (s.mode == Mode::shadow || (s.sequence.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) {
        std::int32_t fast = 0;
        {
            Timer t(s.ticks);
            provview_through_view(country, &fast, province, culture);
        }
        std::int32_t* result;
        {
            Timer t(s.original_ticks);
            result = provview_next(country, out, province, culture);
        }
        s.compared.fetch_add(1, std::memory_order_relaxed);
        if (*out != fast) {
            if (s.mismatches.fetch_add(1) < 10) log("PROVVIEW MISMATCH: ours %d game %d", fast, *out);
            if (s.mode == Mode::on) s.disabled.store(true);
        }
        return result;
    }
    s.ours.fetch_add(1, std::memory_order_relaxed);
    return provview_through_view(country, out, province, culture);
}

bool install_provview() {
    const RangeBytes checks[] = {
        {provview_value_rva, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57}},
        {provview_list_rva, {0x4D, 0x8D, 0xA0, 0x98, 0x09, 0x00, 0x00}},
        {provview_registry_rva, {0x4C, 0x8B, 0x05, 0xBA, 0x78, 0x05, 0x02}},
        {provview_post267_rva, {0x49, 0x8B, 0x88, 0xE0, 0xE6, 0x00, 0x00}},
        {provview_post266_rva, {0x49, 0x8B, 0x88, 0x80, 0xE6, 0x00, 0x00}},
    };
    for (const auto& c : checks) {
        if (!eu4build::code_matches(image, c.rva, c.bytes.begin(), c.bytes.size())) {
            log("provview REFUSED: bytes differ at 0x%llx", static_cast<unsigned long long>(c.rva));
            return false;
        }
    }
    provview_value = reinterpret_cast<ProvValueFn>(image + eu4build::rva(provview_value_rva));
    provview_list = rd<std::uint32_t>(reinterpret_cast<const void*>(image + eu4build::rva(provview_list_rva)), 3);
    provview_registry = rip_target<const unsigned char*>(image + eu4build::rva(provview_registry_rva), 3, 7);
    provview_post267 = rd<std::uint32_t>(reinterpret_cast<const void*>(image + eu4build::rva(provview_post267_rva)), 3);
    provview_post266 = rd<std::uint32_t>(reinterpret_cast<const void*>(image + eu4build::rva(provview_post266_rva)), 3);
    void* next = hook_entry(provview_rva,
        {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54},
        5, reinterpret_cast<const void*>(&provview_hook), "provview");
    if (!next) return false;
    provview_next = reinterpret_cast<ProvValueFn>(next);
    return true;
}
