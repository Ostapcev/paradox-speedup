from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).with_name("vendor")))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def rva_to_offset(data: bytes, rva: int) -> int:
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    for index in range(sections):
        section = table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, section + 8)
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            delta = rva - virtual_address
            if delta >= raw_size:
                raise ValueError("RVA is outside the file-backed section data")
            return raw_offset + delta
    raise ValueError(f"RVA 0x{rva:x} is not mapped by any section")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: inspect_dll_rva.py <dll> <rva-in-hex>")
    path = Path(sys.argv[1])
    rva = int(sys.argv[2], 16)
    data = path.read_bytes()
    start = max(0, rva - 0x80)
    offset = rva_to_offset(data, start)
    end = min(len(data), rva_to_offset(data, rva) + 0x80)
    code = data[offset:end]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for insn in md.disasm(code, start):
        marker = "  <== FAULT" if insn.address == rva else ""
        print(f"0x{insn.address:08x}: {insn.bytes.hex():<24} "
              f"{insn.mnemonic:<8} {insn.op_str}{marker}")
        if insn.address >= rva + 0x70:
            break


if __name__ == "__main__":
    main()
