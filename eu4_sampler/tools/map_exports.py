"""Map RVAs to the nearest preceding PE export without external dependencies."""
from __future__ import annotations

import argparse
import bisect
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Export:
    rva: int
    name: str


def parse_exports(path: Path) -> tuple[list[int], dict[int, Export]]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Not a PE file")
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected a PE32+ image")
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    export_rva, _ = struct.unpack_from("<II", data, optional + 112)
    sections = []
    section_table = optional + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_address))

    def file_offset(rva: int) -> int:
        for virtual_address, size, raw_address in sections:
            if virtual_address <= rva < virtual_address + size:
                return raw_address + rva - virtual_address
        if rva < optional:
            return rva
        raise ValueError(f"RVA 0x{rva:x} is outside the image sections")

    directory = file_offset(export_rva)
    fields = struct.unpack_from("<IIHHIIIIIII", data, directory)
    function_count, name_count = fields[6], fields[7]
    functions_rva, names_rva, ordinals_rva = fields[8], fields[9], fields[10]
    functions_offset = file_offset(functions_rva)
    names_offset = file_offset(names_rva)
    ordinals_offset = file_offset(ordinals_rva)
    exports: dict[int, Export] = {}
    for index in range(name_count):
        name_rva = struct.unpack_from("<I", data, names_offset + index * 4)[0]
        name_offset = file_offset(name_rva)
        end = data.index(0, name_offset)
        name = data[name_offset:end].decode("ascii", errors="replace")
        ordinal = struct.unpack_from("<H", data, ordinals_offset + index * 2)[0]
        if ordinal >= function_count:
            continue
        rva = struct.unpack_from("<I", data, functions_offset + ordinal * 4)[0]
        exports[rva] = Export(rva, name)
    starts = sorted(exports)
    return starts, exports


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("rvas", nargs="+", help="Hexadecimal or decimal RVAs")
    args = parser.parse_args()
    starts, exports = parse_exports(args.image)
    for text in args.rvas:
        rva = int(text, 0)
        position = bisect.bisect_right(starts, rva) - 1
        if position < 0:
            print(f"0x{rva:x}: no preceding export")
            continue
        nearest = exports[starts[position]]
        print(f"0x{rva:x}: {nearest.name}+0x{rva - nearest.rva:x} (export RVA 0x{nearest.rva:x})")


if __name__ == "__main__":
    main()
