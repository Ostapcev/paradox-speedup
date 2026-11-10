"""Locate direct x64 CALL sites for selected EU4 functions."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


def load_pe(path: Path):
    data = path.read_bytes()
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("Not a PE file")
    file_header = pe_offset + 4
    section_count = struct.unpack_from("<H", data, file_header + 2)[0]
    optional_size = struct.unpack_from("<H", data, file_header + 16)[0]
    optional = file_header + 20
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))
    return data, image_base, sections


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("targets", nargs="+", type=lambda value: int(value, 16))
    args = parser.parse_args()
    data, image_base, sections = load_pe(args.executable)
    targets = set(args.targets)
    for name, virtual_address, virtual_size, raw_offset, raw_size in sections:
        del virtual_size
        for offset in range(max(0, raw_size - 4)):
            if data[raw_offset + offset] != 0xE8:
                continue
            call_va = image_base + virtual_address + offset
            displacement = struct.unpack_from("<i", data, raw_offset + offset + 1)[0]
            destination = call_va + 5 + displacement
            if destination in targets:
                site = raw_offset + offset
                print(
                    f"call={call_va:09x} target={destination:09x} "
                    f"section={name} file_offset=0x{site:x} "
                    f"bytes={data[site:site + 5].hex()}"
                )


if __name__ == "__main__":
    main()
