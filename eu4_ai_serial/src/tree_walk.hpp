#pragma once

#include <cstddef>
#include <cstdint>

namespace treewalk {

// [ai] tree_walk=off|serial|probe|parallel. serial = same loop in the plugin;
// probe = serial plus write tracking of army AI nodes;
// parallel = consecutive army AI nodes run on helper threads ([ai] tree_workers).
enum class Mode { off, serial, probe, parallel };

bool install(std::uintptr_t image, Mode mode, int helpers, char* error, std::size_t error_size);
void report(void (*log)(const char*, ...) noexcept);
const char* mode_name();

}  // namespace treewalk
