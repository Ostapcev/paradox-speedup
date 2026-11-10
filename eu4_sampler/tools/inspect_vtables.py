"""Inspect virtual-function pointers stored at RVAs in the EU4 image."""
from __future__ import annotations

import argparse
import bisect
import csv
import struct
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]
EXE = WORKSPACE / "Europa Universalis IV" / "eu4.exe"
INDEX = WORKSPACE / "eu4_kit" / "data" / "findings" / "eu4_function_index.csv"
PREFERRED_BASE = 0x140000000


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("rvas", nargs="+", type=lambda value: int(value, 0))
    args = parser.parse_args()
    image = EXE.read_bytes()
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    sections = pe + 24 + optional_size
    ranges = []
    for index in range(section_count):
        offset = sections + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, offset + 8
        )
        ranges.append((virtual_address, raw_offset, raw_size, virtual_size))

    def to_offset(rva: int) -> int:
        for virtual_address, raw_offset, raw_size, virtual_size in ranges:
            if virtual_address <= rva < virtual_address + max(raw_size, virtual_size):
                result = raw_offset + rva - virtual_address
                if result + 8 > len(image):
                    break
                return result
        raise ValueError(f"RVA {rva:#x} is not backed by the executable image")

    names: dict[int, str] = {}
    with INDEX.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            address = int(row["address"], 16)
            method = row["method_names"].split(";")[0]
            names[address] = method or row["ghidra_name"]
    starts = sorted(names)

    def name_at(address: int) -> str:
        position = bisect.bisect_right(starts, address) - 1
        if position < 0:
            return "unmapped"
        start = starts[position]
        suffix = f"+0x{address - start:x}" if address != start else ""
        return f"{names[start]}{suffix}"

    for vtable_rva in args.rvas:
        print(f"vtable RVA {vtable_rva:#x} (VA {PREFERRED_BASE + vtable_rva:#x})")
        for slot in range(4):
            address = struct.unpack_from("<Q", image, to_offset(vtable_rva + slot * 8))[0]
            rva = address - PREFERRED_BASE
            if 0 <= rva < len(image):
                print(f"  +0x{slot * 8:02x}: {address:#x}  RVA {rva:#x}  {name_at(address)}")
            else:
                print(f"  +0x{slot * 8:02x}: {address:#x}")


if __name__ == "__main__":
    main()
