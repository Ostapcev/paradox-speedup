#pragma once

#include <string>

namespace heavy {

// Starts the sampler thread; stacks are written to `path` (stacks.csv format).
// `write_every`: rewrite the file after this many begin()..end() spans.
bool start(const std::wstring& path, int write_every = 128);
// Called on the sampled thread around the span (task 0 or the monthly tick).
void begin();
void end();

}  // namespace heavy
