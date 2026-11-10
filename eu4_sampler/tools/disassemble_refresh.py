"""Disassemble sampled hot addresses inside CCountry::RefreshKnowledge."""
from __future__ import annotations

import bisect
import csv
import struct
import sys
from pathlib import Path

VENDOR = Path(__file__).resolve().parent / "vendor"
sys.path.insert(0, str(VENDOR))
import capstone  # noqa: E402

WORKSPACE = Path(__file__).resolve().parents[2]
EXE = WORKSPACE / "Europa Universalis IV" / "eu4.exe"
REPORT = WORKSPACE / "Europa Universalis IV" / "profiler" / "eu4_sampler_latest"
INDEX = WORKSPACE / "eu4_kit" / "data" / "findings" / "eu4_function_index.csv"
OUTPUT = WORKSPACE / "eu4_sampler" / "build" / "refresh_knowledge_hot_disasm.txt"
START = 0x93FE30
END = 0x9423F0
PREFERRED_BASE = 0x140000000


def main() -> None:
    image = EXE.read_bytes()
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    sections = pe + 24 + optional_size
    code = None
    for index in range(section_count):
        offset = sections + index * 40
        name = image[offset:offset + 8].rstrip(b"\0")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", image, offset + 8)
        if name == b".text" or virtual_address <= START < virtual_address + max(virtual_size, raw_size):
            if virtual_address <= START and START - virtual_address < raw_size:
                code = (virtual_address, raw_offset, raw_size)
                break
    if code is None:
        raise SystemExit("No PE section contains RefreshKnowledge")
    section_rva, raw_offset, raw_size = code
    start_offset = raw_offset + START - section_rva
    end_offset = raw_offset + END - section_rva
    bytes_to_decode = image[start_offset:end_offset]

    names: dict[int, str] = {}
    with INDEX.open(newline="", encoding="utf-8-sig") as file:
        for row in csv.DictReader(file):
            names[int(row["address"], 16)] = row["ghidra_name"]
    starts = sorted(names)
    samples: dict[int, int] = {}
    with (REPORT / "functions.csv").open(newline="", encoding="utf-8-sig") as file:
        for row in csv.DictReader(file):
            rva = int(row["rva"], 16)
            count = int(row["leaf_samples"])
            if START <= rva < END and count:
                samples[rva] = count

    engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    engine.detail = True
    instructions = list(engine.disasm(bytes_to_decode, PREFERRED_BASE + START))
    by_address = {instruction.address: index for index, instruction in enumerate(instructions)}
    blocks: list[tuple[int, int]] = []
    for rva in samples:
        address = PREFERRED_BASE + rva
        if address not in by_address:
            print(f"Warning: sampled address is not an instruction boundary: {address:x}")
            continue
        center = by_address[address]
        begin = max(0, center - 2)
        end = min(len(instructions), center + 35)
        if blocks and begin <= blocks[-1][1]:
            blocks[-1] = (blocks[-1][0], max(blocks[-1][1], end))
        else:
            blocks.append((begin, end))

    lines = [
        f"RefreshKnowledge {PREFERRED_BASE + START:x}..{PREFERRED_BASE + END:x}",
        f"{len(samples)} sampled leaf addresses; blocks show seven instructions of context.",
        "",
    ]
    for begin, end in blocks:
        lines.append("-" * 100)
        for instruction in instructions[begin:end]:
            rva = instruction.address - PREFERRED_BASE
            annotation = []
            if rva in samples:
                annotation.append(f"HOT leaf_samples={samples[rva]}")
            if instruction.mnemonic.startswith("call") and instruction.operands:
                operand = instruction.operands[0]
                if operand.type == capstone.x86.X86_OP_IMM:
                    target = operand.imm
                    position = bisect.bisect_right(starts, target) - 1
                    if position >= 0:
                        annotation.append(f"target={names[starts[position]]}+{target - starts[position]:x}")
            marker = ">>" if rva in samples else "  "
            lines.append(f"{marker} {instruction.address:09x}  {instruction.mnemonic:9} {instruction.op_str:65} {'; '.join(annotation)}")
        lines.append("")
    OUTPUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {OUTPUT}: {len(samples)} hot addresses, {len(blocks)} merged blocks")


if __name__ == "__main__":
    main()
