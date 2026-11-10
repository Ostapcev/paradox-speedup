"""Aggregate sampler leaf addresses by module and nearest decompiled function.

The nearest preceding function is an attribution hint, not a symbol guarantee.
Review the reported offset and disassembly before treating a mapping as certain.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import pickle
from collections import defaultdict
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]
IMAGE_BASE = 0x140000000
UNCERTAIN_OFFSET = 0x10000


def write_csv(path: Path, fieldnames: list[str], rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("profile", type=Path)
    args = parser.parse_args()
    profile = args.profile.resolve()
    index = WORKSPACE / "eu4_kit" / "data" / "index"
    function_index = index / "funcs_named.pkl"
    if not function_index.exists():
        function_index = index / "funcs.pkl"
    with function_index.open("rb") as stream:
        functions = pickle.load(stream)
    with (index / "funcs.pkl").open("rb") as stream:
        original_functions = pickle.load(stream)
    with (index / "fnnames.json").open(encoding="utf-8") as stream:
        known_names = json.load(stream)

    starts = sorted(int(address, 16) for address in functions)
    by_start = {int(address, 16): (address, metadata) for address, metadata in functions.items()}

    def locate(rva: int) -> dict:
        va = IMAGE_BASE + rva
        position = bisect.bisect_right(starts, va) - 1
        if position < 0:
            return {"rva": 0, "name": "<before indexed functions>", "offset": rva,
                    "file": "", "line": "", "confidence": "unmapped"}
        start = starts[position]
        address, metadata = by_start[start]
        names = known_names.get(address, [])
        original_name = original_functions.get(address, {}).get("name", f"FUN_{address}")
        return {"rva": start - IMAGE_BASE,
                "name": names[0] if names else original_name,
                "offset": va - start, "file": metadata["file"], "line": metadata["line"],
                "confidence": "nearest_preceding" if va - start <= UNCERTAIN_OFFSET else "uncertain_large_offset"}

    with (profile / "leaf.csv").open(newline="", encoding="utf-8") as stream:
        leaves = list(csv.DictReader(stream))
    total_cpu = sum(float(row["estimated_cpu_ms"]) for row in leaves)
    modules = defaultdict(lambda: {"cpu": 0.0, "samples": 0, "addresses": 0})
    groups = defaultdict(lambda: {"cpu": 0.0, "samples": 0, "addresses": 0, "max_offset": 0,
                                  "confidence": "nearest_preceding"})
    mapped = []
    for row in leaves:
        cpu = float(row["estimated_cpu_ms"])
        samples = int(row["samples"])
        module = row["module"]
        modules[module]["cpu"] += cpu
        modules[module]["samples"] += samples
        modules[module]["addresses"] += 1
        result = {"module": module, "rva": row["rva"], "estimated_cpu_ms": f"{cpu:.3f}",
                  "samples": samples}
        if module.lower() == "eu4.exe":
            location = locate(int(row["rva"], 16))
            result.update({"function_rva": f"0x{location['rva']:x}", "function": location["name"],
                           "offset": f"0x{location['offset']:x}", "source_file": location["file"],
                           "source_line": location["line"], "confidence": location["confidence"]})
            key = (location["rva"], location["name"], location["file"], location["line"])
            group = groups[key]
            group["max_offset"] = max(group["max_offset"], location["offset"])
            if location["confidence"] != "nearest_preceding":
                group["confidence"] = location["confidence"]
        else:
            result.update({"function_rva": "", "function": f"[{module}]", "offset": row["rva"],
                           "source_file": "", "source_line": "", "confidence": "external_module"})
            key = (None, f"[{module}]", "", "")
            group = groups[key]
        group["cpu"] += cpu
        group["samples"] += samples
        group["addresses"] += 1
        mapped.append(result)

    module_rows = [{"module": module, "estimated_cpu_ms": f"{values['cpu']:.3f}",
                    "percent": f"{100 * values['cpu'] / total_cpu:.4f}",
                    "samples": values["samples"], "leaf_addresses": values["addresses"]}
                   for module, values in modules.items()]
    module_rows.sort(key=lambda row: float(row["estimated_cpu_ms"]), reverse=True)
    function_rows = [{"function_rva": f"0x{rva:x}" if rva is not None else "", "function": name,
                      "estimated_cpu_ms": f"{values['cpu']:.3f}",
                      "percent": f"{100 * values['cpu'] / total_cpu:.4f}",
                      "samples": values["samples"], "leaf_addresses": values["addresses"],
                      "max_offset": f"0x{values['max_offset']:x}", "source_file": file,
                      "source_line": line, "confidence": values["confidence"]}
                     for (rva, name, file, line), values in groups.items()]
    function_rows.sort(key=lambda row: float(row["estimated_cpu_ms"]), reverse=True)
    write_csv(profile / "modules_aggregated.csv",
              ["module", "estimated_cpu_ms", "percent", "samples", "leaf_addresses"], module_rows)
    write_csv(profile / "functions_aggregated.csv",
              ["function_rva", "function", "estimated_cpu_ms", "percent", "samples", "leaf_addresses",
               "max_offset", "source_file", "source_line", "confidence"], function_rows)
    write_csv(profile / "leaf_mapped.csv",
              ["module", "rva", "estimated_cpu_ms", "samples", "function_rva", "function", "offset",
               "source_file", "source_line", "confidence"], mapped)

    print(f"Attributed leaf CPU: {total_cpu:.3f} ms")
    print("\nModules:")
    for row in module_rows[:15]:
        print(f"{row['percent']:>8}%  {row['estimated_cpu_ms']:>10} ms  {row['module']}")
    print("\nFunctions / external modules (nearest-preceding attribution):")
    for row in function_rows[:35]:
        print(f"{row['percent']:>8}%  {row['estimated_cpu_ms']:>10} ms  "
              f"{row['function_rva']:>10} +{row['max_offset']:<8} {row['function']}")


if __name__ == "__main__":
    main()
