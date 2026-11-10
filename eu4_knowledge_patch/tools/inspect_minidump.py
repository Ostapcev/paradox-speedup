"""Print modules, exception context and candidate return addresses from a minidump."""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def main() -> None:
    path = Path(sys.argv[1])
    data = path.read_bytes()
    signature, _, stream_count, directory = struct.unpack_from("<IIII", data, 0)
    if signature != 0x504D444D:
        raise SystemExit("not a minidump")
    streams = {}
    for index in range(stream_count):
        kind, size, rva = struct.unpack_from("<III", data, directory + index * 12)
        streams[kind] = (rva, size)

    modules = []
    if 4 in streams:
        rva, _ = streams[4]
        count = struct.unpack_from("<I", data, rva)[0]
        for index in range(count):
            offset = rva + 4 + index * 108
            base, size = struct.unpack_from("<QI", data, offset)
            name_rva = struct.unpack_from("<I", data, offset + 20)[0]
            length = struct.unpack_from("<I", data, name_rva)[0]
            name = data[name_rva + 4:name_rva + 4 + length].decode("utf-16le")
            modules.append((base, size, name))

    def locate(address: int) -> str:
        for base, size, name in modules:
            if base <= address < base + size:
                return f"{Path(name).name}+0x{address - base:x} ({name})"
        return f"0x{address:x} (unmapped)"

    print("Relevant modules:")
    for base, size, name in modules:
        if "eu4" in name.lower():
            print(f"  base=0x{base:x} size=0x{size:x} {name}")

    exception_thread = None
    if 6 in streams:
        rva, _ = streams[6]
        exception_thread = struct.unpack_from("<I", data, rva)[0]
        code = struct.unpack_from("<I", data, rva + 8)[0]
        address = struct.unpack_from("<Q", data, rva + 24)[0]
        parameter_count = struct.unpack_from("<I", data, rva + 32)[0]
        parameters = struct.unpack_from("<15Q", data, rva + 40)
        context_size, context_rva = struct.unpack_from("<II", data, rva + 160)
        print(f"Exception thread={exception_thread} code=0x{code:08x} {locate(address)}")
        print(f"Exception parameters={parameters[:parameter_count]}")
        context = data[context_rva:context_rva + context_size]
        names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                 "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
        registers = dict(zip(names, struct.unpack_from("<17Q", context, 120)))
        for name in names:
            value = registers[name]
            suffix = f" {locate(value)}" if name == "rip" else ""
            print(f"  {name:>3}=0x{value:016x}{suffix}")
    else:
        registers = {}

    memory = []
    if 5 in streams:
        rva, _ = streams[5]
        count = struct.unpack_from("<I", data, rva)[0]
        for index in range(count):
            start, size, source = struct.unpack_from("<QII", data, rva + 4 + index * 16)
            memory.append((start, size, source))
    if 16 in streams:
        rva, _ = streams[16]
        count, base_rva = struct.unpack_from("<QQ", data, rva)
        source = base_rva
        for index in range(count):
            start, size = struct.unpack_from("<QQ", data, rva + 16 + index * 16)
            memory.append((start, size, source))
            source += size

    def read_memory(address: int, size: int) -> bytes:
        for start, length, source in memory:
            if start <= address and address + size <= start + length:
                offset = source + address - start
                return data[offset:offset + size]
        return b""

    if exception_thread is not None and 3 in streams:
        rva, _ = streams[3]
        count = struct.unpack_from("<I", data, rva)[0]
        for index in range(count):
            offset = rva + 4 + index * 48
            thread_id = struct.unpack_from("<I", data, offset)[0]
            if thread_id != exception_thread:
                continue
            stack_start, stack_size, stack_rva = struct.unpack_from("<QII", data, offset + 24)
            print(f"Exception stack start=0x{stack_start:x} size=0x{stack_size:x} rva=0x{stack_rva:x}")

    if "rsp" in registers:
        print("Candidate module addresses on exception stack:")
        stack = read_memory(registers["rsp"], 0x3000)
        if not stack:
            print("  stack memory is absent")
        for offset in range(0, len(stack) - 7, 8):
            value = struct.unpack_from("<Q", stack, offset)[0]
            if any(base <= value < base + size for base, size, _ in modules):
                print(f"  [rsp+0x{offset:04x}] {locate(value)}")


if __name__ == "__main__":
    main()
