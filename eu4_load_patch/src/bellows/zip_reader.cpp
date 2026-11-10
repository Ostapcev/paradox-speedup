// From Bellows (MIT, (c) Happy Dwarf Team), plugin/src/modules/zip_reader.cpp; see ../../LICENSE.bellows.txt.
#include "zip_reader.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace sb::zip {
namespace {

constexpr std::size_t kEndRecordSize = 22;
constexpr std::size_t kMaximumComment = 0xFFFF;
constexpr std::uint32_t kEndSignature = 0x06054B50;
constexpr std::uint32_t kCentralSignature = 0x02014B50;
constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::size_t kCentralRecordSize = 46;
constexpr std::size_t kLocalRecordSize = 30;

std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool AddRange(std::uint64_t start, std::uint64_t length, std::uint64_t limit, std::uint64_t& end) {
    if (start > limit || length > limit - start) return false;
    end = start + length;
    return true;
}

bool FileSize(HANDLE file, std::uint64_t& size) {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(file, &value) || value.QuadPart < 0) return false;
    size = static_cast<std::uint64_t>(value.QuadPart);
    return true;
}

using RtlComputeCrc32Fn = ULONG(NTAPI*)(ULONG, const UCHAR*, ULONG);

RtlComputeCrc32Fn ResolveCrc() {
    static const auto fn = reinterpret_cast<RtlComputeCrc32Fn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlComputeCrc32"));
    return fn;
}

}  // namespace

bool ReadAt(HANDLE file, std::uint64_t offset, void* destination, std::size_t length) {
    auto* out = static_cast<std::uint8_t*>(destination);
    while (length > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length, 64u << 20));
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!ReadFile(file, out, chunk, &read, &position) || read != chunk) return false;
        out += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool ParseCentralDirectory(HANDLE file, const std::unordered_set<std::string>& targets,
                           std::unordered_map<std::string, Entry>& out) {
    out.clear();
    std::uint64_t fileSize = 0;
    if (!FileSize(file, fileSize) || fileSize < kEndRecordSize) return false;
    const std::uint64_t tailLength = std::min<std::uint64_t>(fileSize, kEndRecordSize + kMaximumComment);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tailLength));
    if (!ReadAt(file, fileSize - tailLength, tail.data(), tail.size())) return false;

    // Last end record whose comment reaches exactly the end of the file.
    const std::uint8_t* end = nullptr;
    for (std::size_t index = tail.size() - kEndRecordSize;; --index) {
        const std::uint8_t* candidate = tail.data() + index;
        std::uint64_t stop = 0;
        if (U32(candidate) == kEndSignature &&
            AddRange(fileSize - tailLength + index, kEndRecordSize + U16(candidate + 20), fileSize, stop) &&
            stop == fileSize) {
            end = candidate;
            break;
        }
        if (index == 0) break;
    }
    if (end == nullptr) return false;
    const std::uint16_t disk = U16(end + 4);
    const std::uint16_t centralDisk = U16(end + 6);
    const std::uint16_t entriesOnDisk = U16(end + 8);
    const std::uint16_t entriesTotal = U16(end + 10);
    const std::uint32_t centralSize = U32(end + 12);
    const std::uint32_t centralOffset = U32(end + 16);
    std::uint64_t centralEnd = 0;
    if (disk != 0 || centralDisk != 0 || entriesOnDisk != entriesTotal || entriesTotal == 0xFFFF ||
        centralSize == 0xFFFFFFFF || centralOffset == 0xFFFFFFFF ||
        !AddRange(centralOffset, centralSize, fileSize, centralEnd)) {
        return false;
    }
    std::vector<std::uint8_t> central(centralSize);
    if (!ReadAt(file, centralOffset, central.data(), central.size())) return false;

    std::size_t position = 0;
    std::uint32_t records = 0;
    while (position < central.size()) {
        if (central.size() - position < kCentralRecordSize) return false;
        const std::uint8_t* header = central.data() + position;
        if (U32(header) != kCentralSignature) return false;
        const std::size_t nameLength = U16(header + 28);
        const std::size_t recordLength = kCentralRecordSize + nameLength + U16(header + 30) + U16(header + 32);
        if (recordLength > central.size() - position) return false;
        if (++records > entriesTotal) return false;
        std::string name(reinterpret_cast<const char*>(header + kCentralRecordSize), nameLength);
        if (targets.empty() || targets.count(name) != 0) {
            Entry entry;
            entry.flags = U16(header + 8);
            entry.method = U16(header + 10);
            entry.crc32 = U32(header + 16);
            entry.compressedSize = U32(header + 20);
            entry.uncompressedSize = U32(header + 24);
            entry.localOffset = U32(header + 42);
            entry.unsupported = (entry.flags & 0x0001) != 0 || (entry.method != 0 && entry.method != 8) ||
                                U16(header + 6) >= 45 || U16(header + 34) == 0xFFFF ||
                                entry.localOffset == 0xFFFFFFFF || entry.compressedSize == 0xFFFFFFFF ||
                                entry.uncompressedSize == 0xFFFFFFFF;
            const auto found = out.find(name);
            if (found != out.end()) {
                found->second.duplicate = true;
            } else {
                out.emplace(std::move(name), entry);
            }
        }
        position += recordLength;
    }
    if (records != entriesTotal) {
        out.clear();
        return false;
    }
    return true;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    const auto fn = ResolveCrc();
    ULONG crc = 0;
    while (size > 0) {
        const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 30));
        crc = fn(crc, data, chunk);
        data += chunk;
        size -= chunk;
    }
    return crc;
}

bool ReadMember(HANDLE file, const std::string& name, const Entry& entry, InflateFn inflate, std::uint8_t* output) {
    if (entry.unsupported || entry.duplicate || ResolveCrc() == nullptr) return false;
    std::uint64_t fileSize = 0;
    if (!FileSize(file, fileSize)) return false;
    std::uint8_t local[kLocalRecordSize];
    if (!ReadAt(file, entry.localOffset, local, sizeof(local)) || U32(local) != kLocalSignature) return false;
    const std::uint16_t localFlags = U16(local + 6);
    const std::uint16_t localMethod = U16(local + 8);
    const std::size_t nameLength = U16(local + 26);
    const std::size_t extraLength = U16(local + 28);
    if ((localFlags & 0x0001) != 0 || localMethod != entry.method || nameLength != name.size()) return false;
    std::string localName(nameLength, '\0');
    if (nameLength != 0 && !ReadAt(file, entry.localOffset + kLocalRecordSize, localName.data(), nameLength)) return false;
    if (localName != name) return false;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataEnd = 0;
    if (!AddRange(entry.localOffset, kLocalRecordSize + nameLength + extraLength, fileSize, dataOffset) ||
        !AddRange(dataOffset, entry.compressedSize, fileSize, dataEnd)) {
        return false;
    }
    // Without a data descriptor the local sizes and CRC must equal the central ones.
    if ((entry.flags & 0x0008) == 0 &&
        (U32(local + 14) != entry.crc32 || U32(local + 18) != entry.compressedSize ||
         U32(local + 22) != entry.uncompressedSize)) {
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(entry.uncompressedSize);
    if (entry.method == 0) {
        if (entry.compressedSize != entry.uncompressedSize || !ReadAt(file, dataOffset, output, size)) return false;
    } else {
        if (inflate == nullptr) return false;
        std::unique_ptr<std::uint8_t[]> input(new std::uint8_t[static_cast<std::size_t>(entry.compressedSize) + 1]);
        if (!ReadAt(file, dataOffset, input.get(), static_cast<std::size_t>(entry.compressedSize))) return false;
        if (!inflate(input.get(), static_cast<std::size_t>(entry.compressedSize), output, size)) return false;
    }
    return Crc32(output, size) == entry.crc32;
}

}  // namespace sb::zip
