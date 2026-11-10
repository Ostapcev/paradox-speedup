"""Map external-leaf callers to EU4 functions and nearest Windows exports."""
from __future__ import annotations

import argparse
import bisect
import csv
from pathlib import Path

from analyze import DEFAULT_INDEX, load_index
from map_exports import parse_exports

PREFERRED_BASE = 0x140000000


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--ntdll", type=Path, help="Optional matching ntdll.dll for export names")
    args = parser.parse_args()
    report = args.report.resolve()
    starts, index = load_index(args.index)
    export_starts: list[int] = []
    exports = {}
    if args.ntdll:
        export_starts, exports = parse_exports(args.ntdll)

    def locate_caller(rva: int) -> tuple[object, int]:
        address = PREFERRED_BASE + rva
        position = bisect.bisect_right(starts, address) - 1
        if position < 0:
            raise ValueError(f"Caller RVA 0x{rva:x} precedes the function index")
        start = starts[position]
        return index[start], address - start

    def locate_export(rva: int) -> str:
        position = bisect.bisect_right(export_starts, rva) - 1
        if position < 0:
            return ""
        nearest = exports[export_starts[position]]
        return f"{nearest.name}+0x{rva - nearest.rva:x}"

    rows = []
    with (report / "external_callers.csv").open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            caller_rva = int(row["caller_rva"], 16)
            function, offset = locate_caller(caller_rva)
            leaf_rva = int(row["leaf_rva"], 16)
            leaf_symbol = locate_export(leaf_rva) if row["leaf_module"].lower() == "ntdll.dll" else ""
            rows.append({
                "leaf_module": row["leaf_module"],
                "leaf_rva": row["leaf_rva"],
                "leaf_symbol": leaf_symbol,
                "caller_rva": row["caller_rva"],
                "caller_function_rva": f"0x{function.address - PREFERRED_BASE:x}",
                "caller_function": function.name,
                "caller_offset": f"0x{offset:x}",
                "caller_source": function.source,
                "estimated_cpu_ms": row["estimated_cpu_ms"],
                "percent_of_attributed": row["percent_of_attributed"],
                "samples": row["samples"],
            })

    output = report / "external_caller_hotspots.csv"
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else [
            "leaf_module", "leaf_rva", "leaf_symbol", "caller_rva", "caller_function_rva",
            "caller_function", "caller_offset", "caller_source", "estimated_cpu_ms",
            "percent_of_attributed", "samples"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {output}")
    for row in rows[:30]:
        print(f"{float(row['estimated_cpu_ms']):9.3f} ms  {row['leaf_module']}!{row['leaf_symbol'] or row['leaf_rva']}  <-  "
              f"{row['caller_function']}+{row['caller_offset']} ({row['caller_rva']})")


if __name__ == "__main__":
    main()
