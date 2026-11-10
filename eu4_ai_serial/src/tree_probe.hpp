#pragma once

#include <cstddef>
#include <cstdint>

namespace treeprobe {

// Patches slot +0x60 of the 13 AI node vtables (diagnostic, [ai] tree_probe=1).
bool install(std::uintptr_t image, char* error, std::size_t error_size);
void begin_task(int index);
void end_task();
void phase_end(int longest_index, std::uint64_t longest_ticks);
void begin_heavy();
void end_heavy();
void report(void (*log)(const char*, ...) noexcept);

}  // namespace treeprobe
