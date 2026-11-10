"""Find machine-code accesses to province core/claim container fields.

This is a static triage aid, not a proof that every indirect write is covered.
It reports direct writes and addresses of the containers separately from reads.
"""
from __future__ import annotations

import bisect
import csv
import struct
import sys
from collections import defaultdict
from pathlib import Path

VENDOR = Path(__file__).resolve().parent / "vendor"
sys.path.insert(0, str(VENDOR))
import capstone  # noqa: E402
from capstone.x86 import X86_OP_MEM  # noqa: E402

WORKSPACE = Path(__file__).resolve().parents[2]
EXE = WORKSPACE / "Europa Universalis IV" / "eu4.exe"
INDEX = WORKSPACE / "eu4_kit" / "data" / "findings" / "eu4_function_index.csv"
OUTPUT = WORKSPACE / "eu4_sampler" / "build" / "province_container_accesses.txt"
BASE = 0x140000000
FIELDS = {
    0x490: "cores.begin",
    0x498: "cores.end",
    0x4A0: "cores.capacity",
    0x4C0: "claims.head",
    0x4C8: "claims.tail",
    0x4D0: "claims.count",
}


def main() -> None:
    image = EXE.read_bytes()
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    sections = pe + 24 + optional_size
    text = None
    for index in range(section_count):
        offset = sections + index * 40
        name = image[offset:offset + 8].rstrip(b"\0")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", image, offset + 8)
        if name == b".text":
            text = (rva, raw_offset, min(virtual_size, raw_size))
            break
    if text is None:
        raise SystemExit("No .text section")
    rva, raw_offset, size = text

    functions: dict[int, tuple[str, str, int]] = {}
    with INDEX.open(newline="", encoding="utf-8-sig") as file:
        for row in csv.DictReader(file):
            functions[int(row["address"], 16)] = (
                row["ghidra_name"], row["chunk_file"], int(row["chunk_line"])
            )
    starts = sorted(functions)

    engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    engine.detail = True
    engine.skipdata = True
    accesses: dict[str, list[str]] = defaultdict(list)
    counts = defaultdict(int)
    for insn in engine.disasm(image[raw_offset:raw_offset + size], BASE + rva):
        if insn.id == 0:  # Capstone data skipped in SKIPDATA mode has no operands.
            continue
        for operand_index, operand in enumerate(insn.operands):
            if operand.type != X86_OP_MEM or operand.mem.disp not in FIELDS:
                continue
            field = FIELDS[operand.mem.disp]
            position = bisect.bisect_right(starts, insn.address) - 1
            if position < 0:
                continue
            function_start = starts[position]
            name, chunk, line = functions[function_start]
            is_write = (
                operand_index == 0
                and (
                    bool(operand.access & capstone.CS_AC_WRITE)
                    or operand.access == 0  # Capstone leaves access unknown on some RMW instructions.
                )
                and insn.mnemonic not in {"lea", "cmp", "test", "bt"}
            )
            if insn.mnemonic == "lea":
                kind = "address"
            elif is_write:
                kind = "write"
            else:
                kind = "read"
            counts[kind] += 1
            accesses[kind].append(
                f"{insn.address:09x}  {field:16} {insn.mnemonic:9} {insn.op_str:58} "
                f"{name}+{insn.address - function_start:x} ({chunk}:{line})"
            )
            break

    lines = [
        "Static triage for direct machine-code accesses at province container offsets.",
        "WARNING: identical offsets in unrelated classes and indirect element writes remain possible.",
        "Every write/address entry needs its enclosing function checked before relying on it.",
        "",
    ]
    for kind in ("write", "address", "read"):
        lines.append(f"===== {kind.upper()} ({counts[kind]}) =====")
        lines.extend(accesses[kind])
        lines.append("")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {OUTPUT}: " + ", ".join(f"{kind}={counts[kind]}" for kind in counts))


if __name__ == "__main__":
    main()
