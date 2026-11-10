"""Summarize cumulative statistics from eu4_modifier_patch.log."""
from __future__ import annotations

import re
import sys
from pathlib import Path


def main() -> None:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parents[2] / "Europa Universalis IV" / "plugins" /
        "eu4_modifier_patch.log"
    )
    ticks: list[int] = []
    stats: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        prefix = re.match(r"\[\d+:(\d+)\]", line)
        if prefix:
            ticks.append(int(prefix.group(1)))
        if " stats " not in line:
            continue
        current: dict[str, int] = {}
        for key, value in re.findall(r"([a-z_]+)=(\d+)", line):
            current[key] = int(value)
        buckets = re.search(r"buckets\[[^]]+\]=([\d,]+)", line)
        if buckets:
            for index, value in enumerate(buckets.group(1).split(",")):
                current[f"bucket_{index}"] = int(value)
        stats = current
    if not stats:
        raise SystemExit("No statistics records found")
    elapsed_ms = ticks[-1] - ticks[0]
    calls = stats["calls"]
    dynamic = stats["dynamic_calls"]
    heavy = sum(stats[f"bucket_{index}"] for index in (4, 5, 6))
    comparisons = stats["linear_comparisons"]
    original_ms = stats["original_total_ms"]
    measurement_ms = stats["measurement_total_ms"]
    print(f"elapsed_ms={elapsed_ms}")
    print(f"calls={calls}")
    print(f"calls_per_second={calls * 1000 / elapsed_ms:.1f}")
    print(f"dynamic_calls={dynamic} ({dynamic / calls * 100:.2f}%)")
    print(f"heavy_calls_over_255_comparisons={heavy} ({heavy / calls * 100:.2f}% of calls, "
          f"{heavy / dynamic * 100:.2f}% of dynamic)")
    print(f"zero_comparison_calls={stats['bucket_0']} ({stats['bucket_0'] / calls * 100:.2f}%)")
    print(f"linear_comparisons={comparisons}")
    print(f"comparisons_per_dynamic_input={comparisons / stats['dynamic_inputs']:.2f}")
    print(f"original_total_ms={original_ms} ({original_ms / elapsed_ms * 100:.3f}% of elapsed)")
    print(f"measurement_total_ms={measurement_ms} ({measurement_ms / elapsed_ms * 100:.3f}% of elapsed)")
    print(f"max_call_comparisons={stats['max_call_comparisons']}")
    print(f"max_destination={stats['max_destination']} max_source={stats['max_source']}")
    print(f"invalid={stats['invalid']} aliases={stats['aliases']}")


if __name__ == "__main__":
    main()
