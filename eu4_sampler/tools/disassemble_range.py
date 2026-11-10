"""Disassemble an RVA range from the profiled EU4 executable."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

VENDOR = Path(__file__).resolve().parent / "vendor"
sys.path.insert(0, str(VENDOR))
import capstone  # noqa: E402

WORKSPACE = Path(__file__).resolve().parents[2]
EXE = WORKSPACE / "Europa Universalis IV" / "eu4.exe"
PREFERRED_BASE = 0x140000000


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("start", type=lambda value: int(value, 0))
    parser.add_argument("end", type=lambda value: int(value, 0))
    args = parser.parse_args()
    image = EXE.read_bytes()
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    sections = pe + 24 + optional_size
    for index in range(section_count):
        offset = sections + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, offset + 8
        )
        if virtual_address <= args.start < virtual_address + raw_size:
            start_offset = raw_offset + args.start - virtual_address
            end_offset = raw_offset + args.end - virtual_address
            data = image[start_offset:end_offset]
            break
    else:
        raise SystemExit("RVA range is outside executable file sections")

    engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    engine.detail = True
    for instruction in engine.disasm(data, PREFERRED_BASE + args.start):
        annotation = ""
        if instruction.mnemonic == "call" and instruction.operands:
            operand = instruction.operands[0]
            if operand.type == capstone.x86.X86_OP_IMM:
                annotation = f" ; target RVA {operand.imm - PREFERRED_BASE:#x}"
        print(
            f"{instruction.address:09x}  {instruction.bytes.hex(' '):<30} "
            f"{instruction.mnemonic:9} {instruction.op_str}{annotation}"
        )


if __name__ == "__main__":
    main()
