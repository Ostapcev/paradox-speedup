"""Print PE headers, imports, and exports used by the build verification."""
from __future__ import annotations

import argparse
from pathlib import Path

import pefile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--imports", action="store_true")
    args = parser.parse_args()
    pe = pefile.PE(str(args.binary), fast_load=False)
    print(f"machine=0x{pe.FILE_HEADER.Machine:04x}")
    print(f"optional_header_size=0x{pe.FILE_HEADER.SizeOfOptionalHeader:x}")
    print(f"image_base=0x{pe.OPTIONAL_HEADER.ImageBase:x}")
    print(f"size_of_image=0x{pe.OPTIONAL_HEADER.SizeOfImage:x}")
    print(f"size_of_headers=0x{pe.OPTIONAL_HEADER.SizeOfHeaders:x}")
    for section in pe.sections:
        name = section.Name.rstrip(b"\0").decode("ascii", "replace")
        print(
            f"section={name} rva=0x{section.VirtualAddress:x} "
            f"virtual_size=0x{section.Misc_VirtualSize:x} flags=0x{section.Characteristics:x}"
        )
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for export in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            print(f"export={export.name.decode('ascii', 'replace')} rva=0x{export.address:x}")
    if args.imports and hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            print(f"import_dll={entry.dll.decode('ascii', 'replace')}")
            for item in entry.imports:
                name = item.name.decode("ascii", "replace") if item.name else f"ordinal:{item.ordinal}"
                print(f"  {name}")


if __name__ == "__main__":
    main()
