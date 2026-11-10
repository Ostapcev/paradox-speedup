// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/zip_reader.hpp; see ../../LICENSE.bellows.txt.
#pragma once

// Minimal, fail-closed ZIP reader for ram_prefetch (ported from the lab's
// startup_ram_zip_backend.cpp): central directory of a single-disk, non-ZIP64
// archive; stored (0) and DEFLATE (8) members; local header cross-checked with
// the central directory; full CRC-32 of the output. Anything else fails.

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sb::zip {

struct Entry {
    std::uint64_t localOffset = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint32_t crc32 = 0;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    bool unsupported = false;  // encrypted, other method, ZIP64 or multi-disk
    bool duplicate = false;    // the name occurs more than once
};

// Positional reads from a file handle (thread-safe on one handle).
bool ReadAt(HANDLE file, std::uint64_t offset, void* destination, std::size_t length);

// Parses the central directory and returns the entries whose names are in
// targets (all entries when targets is empty). Returns false for a malformed
// or unsupported archive.
bool ParseCentralDirectory(HANDLE file, const std::unordered_set<std::string>& targets,
                           std::unordered_map<std::string, Entry>& out);

// Raw-DEFLATE decoder: whole input to whole output. Returns true when the
// stream ended exactly at the end of both buffers.
using InflateFn = bool (*)(const std::uint8_t* input, std::size_t inputSize, std::uint8_t* output,
                           std::size_t outputSize);

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size);

// Reads member `name` described by `entry` into output (entry.uncompressedSize
// bytes), checks the local header, decodes and verifies CRC-32.
bool ReadMember(HANDLE file, const std::string& name, const Entry& entry, InflateFn inflate, std::uint8_t* output);

}  // namespace sb::zip
