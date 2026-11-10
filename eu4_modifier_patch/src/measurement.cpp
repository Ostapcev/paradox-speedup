#include "measurement.hpp"

#include <cstring>
#include <limits>

namespace modifierpatch {
namespace {

template <typename T>
T read_memory(std::uintptr_t address) noexcept {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

int key_at(const void* container, std::int32_t index) noexcept {
    const auto entries = read_memory<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(container) + 0x58);
    return read_memory<int>(entries + static_cast<std::uintptr_t>(index) * 8);
}

bool prepare_hash(Scratch& scratch, std::size_t expected) {
    scratch.keys.clear();
    scratch.keys.reserve(expected);
    std::size_t capacity = 16;
    while (capacity < expected * 2 && capacity <= (std::numeric_limits<std::size_t>::max)() / 2) {
        capacity *= 2;
    }
    if (capacity < expected * 2 || capacity > 0x1000000) return false;
    if (scratch.slots.size() != capacity) {
        scratch.slots.assign(capacity, Scratch::Slot{});
        scratch.generation = 1;
    } else {
        ++scratch.generation;
        if (scratch.generation == 0) {
            scratch.slots.assign(capacity, Scratch::Slot{});
            scratch.generation = 1;
        }
    }
    return true;
}

}  // namespace

Measurement measure_merge(const void* destination, const void* source, Scratch& scratch) {
    Measurement result;
    if (!destination || !source) {
        result.valid = false;
        return result;
    }
    if (destination == source) {
        result.alias = true;
        return result;
    }
    const auto destination_address = reinterpret_cast<std::uintptr_t>(destination);
    const auto source_address = reinterpret_cast<std::uintptr_t>(source);
    result.destination_count = read_memory<std::int32_t>(destination_address + 0x64);
    result.source_count = read_memory<std::int32_t>(source_address + 0x64);
    if (result.destination_count < 0 || result.source_count < 0 ||
        result.destination_count > 0x100000 || result.source_count > 0x100000) {
        result.valid = false;
        return result;
    }
    if (result.source_count == 0) return result;

    const bool dense_destination = read_memory<std::uint8_t>(destination_address + 0x10) != 0;
    for (std::int32_t index = 0; index < result.source_count; ++index) {
        const int key = key_at(source, index);
        if (key == modifier_sentinel) continue;
        if (dense_destination && key < modifier_sentinel) {
            ++result.direct_lookups;
        } else {
            result.dynamic = true;
        }
    }
    if (!result.dynamic) return result;
    if (!prepare_hash(scratch, static_cast<std::size_t>(result.destination_count) +
            static_cast<std::size_t>(result.source_count))) {
        result.valid = false;
        return result;
    }

    const auto mask = scratch.slots.size() - 1;
    const auto generation = scratch.generation;
    const auto locate = [&](int key, bool insert, int& index) {
        auto position = (static_cast<std::uint32_t>(key) * 0x9E3779B1U) & mask;
        for (;;) {
            auto& slot = scratch.slots[position];
            if (slot.generation != generation) {
                if (!insert) return false;
                slot.generation = generation;
                slot.key = key;
                slot.index = index;
                return false;
            }
            if (slot.key == key) {
                index = slot.index;
                return true;
            }
            position = (position + 1) & mask;
        }
    };

    for (std::int32_t index = 0; index < result.destination_count; ++index) {
        const int key = key_at(destination, index);
        int found_index = 0;
        if (!locate(key, false, found_index)) {
            found_index = static_cast<int>(scratch.keys.size());
            locate(key, true, found_index);
        }
        scratch.keys.push_back(key);
    }

    for (std::int32_t index = 0; index < result.source_count; ++index) {
        const int key = key_at(source, index);
        if (key == modifier_sentinel || (dense_destination && key < modifier_sentinel)) continue;
        ++result.dynamic_inputs;
        int found_index = 0;
        if (locate(key, false, found_index)) {
            result.comparisons += static_cast<std::uint64_t>(found_index) + 1;
        } else {
            result.comparisons += scratch.keys.size();
            found_index = static_cast<int>(scratch.keys.size());
            locate(key, true, found_index);
            scratch.keys.push_back(key);
            ++result.appended;
        }
    }
    return result;
}

}  // namespace modifierpatch
