"""Compare the latest modifier-patch run in each log at equivalent game progress."""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class FunctionSample:
    tick: int
    calls: int
    inclusive_ms: int
    stats: dict[str, int]


@dataclass
class Run:
    pid: int
    version: str
    start_tick: int
    stats: list[tuple[int, dict[str, int]]] = field(default_factory=list)
    functions: dict[str, list[FunctionSample]] = field(default_factory=dict)


PREFIX = re.compile(r"\[(\d+):(\d+)\]")
INIT = re.compile(r"initialized version=([^ ]+)")
FUNCTION = re.compile(
    r" function rank=\d+ sort=time name=([^ ]+) rva=([^ ]+) "
    r"calls=(\d+) inclusive_ms=(\d+)"
)


def values(line: str) -> dict[str, int]:
    return {key: int(value) for key, value in re.findall(r"([a-z_]+)=(\d+)", line)}


def latest_run(path: Path, required_pid: int = 0) -> Run:
    current: Run | None = None
    latest_complete: Run | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        prefix = PREFIX.match(line)
        if not prefix:
            continue
        pid, tick = (int(value) for value in prefix.groups())
        initialized = INIT.search(line)
        if initialized:
            if current and current.stats and current.functions.get("country_recalculation"):
                latest_complete = current
            current = Run(pid, initialized.group(1), tick) if not required_pid or pid == required_pid else None
            continue
        if current is None or pid != current.pid:
            continue
        if " stats " in line:
            current.stats.append((tick, values(line)))
            continue
        function = FUNCTION.search(line)
        if function and current.stats:
            name, _, calls, inclusive_ms = function.groups()
            current.functions.setdefault(name, []).append(FunctionSample(
                tick, int(calls), int(inclusive_ms), current.stats[-1][1]
            ))
    if current and current.stats and current.functions.get("country_recalculation"):
        latest_complete = current
    if latest_complete is None:
        raise ValueError(f"No complete run found in {path}")
    return latest_complete


def last_function(run: Run, name: str) -> FunctionSample:
    return run.functions[name][-1]


def closest_function(run: Run, name: str, target_calls: int) -> FunctionSample:
    return min(run.functions[name], key=lambda sample: abs(sample.calls - target_calls))


def report(label: str, run: Run, sample: FunctionSample) -> None:
    stats = sample.stats
    elapsed_ms = sample.tick - run.start_tick
    merge_calls = stats["calls"]
    country_average_us = sample.inclusive_ms * 1000 / sample.calls
    merge_average_ns = stats["original_total_ms"] * 1_000_000 / merge_calls
    measurement_average_ns = stats["measurement_total_ms"] * 1_000_000 / merge_calls
    print(f"{label}: version={run.version} pid={run.pid} elapsed_s={elapsed_ms / 1000:.3f}")
    print(f"  merge_calls={merge_calls} original_total_ms={stats['original_total_ms']} "
          f"measurement_total_ms={stats['measurement_total_ms']}")
    print(f"  merge_original_avg_ns={merge_average_ns:.2f} "
          f"merge_measurement_avg_ns={measurement_average_ns:.2f}")
    print(f"  country_calls={sample.calls} country_inclusive_ms={sample.inclusive_ms} "
          f"country_avg_us={country_average_us:.3f} "
          f"country_calls_per_second={sample.calls * 1000 / elapsed_ms:.2f}")


def main() -> None:
    if len(sys.argv) not in (3, 4):
        raise SystemExit("Usage: compare_runs.py BASELINE_LOG CANDIDATE_LOG [CANDIDATE_PID]")
    baseline = latest_run(Path(sys.argv[1]))
    candidate = latest_run(Path(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) == 4 else 0)
    baseline_final = last_function(baseline, "country_recalculation")
    candidate_final = last_function(candidate, "country_recalculation")

    print("=== Final records ===")
    report("baseline", baseline, baseline_final)
    report("candidate", candidate, candidate_final)

    target = min(baseline_final.calls, candidate_final.calls)
    baseline_match = closest_function(baseline, "country_recalculation", target)
    candidate_match = closest_function(candidate, "country_recalculation", target)
    print(f"\n=== Closest common progress: {target} country recalculations ===")
    report("baseline", baseline, baseline_match)
    report("candidate", candidate, candidate_match)

    baseline_country_avg = baseline_match.inclusive_ms * 1000 / baseline_match.calls
    candidate_country_avg = candidate_match.inclusive_ms * 1000 / candidate_match.calls
    baseline_merge_avg = baseline_match.stats["original_total_ms"] * 1_000_000 / baseline_match.stats["calls"]
    candidate_merge_avg = candidate_match.stats["original_total_ms"] * 1_000_000 / candidate_match.stats["calls"]
    baseline_rate = baseline_match.calls * 1000 / (baseline_match.tick - baseline.start_tick)
    candidate_rate = candidate_match.calls * 1000 / (candidate_match.tick - candidate.start_tick)
    print("\n=== Candidate relative to baseline ===")
    print(f"country_average_change_pct={(candidate_country_avg / baseline_country_avg - 1) * 100:.2f}")
    print(f"country_rate_change_pct={(candidate_rate / baseline_rate - 1) * 100:.2f}")
    print(f"merge_original_average_change_pct={(candidate_merge_avg / baseline_merge_avg - 1) * 100:.2f}")

    rows: list[tuple[int, str, FunctionSample, FunctionSample]] = []
    for name, samples in candidate.functions.items():
        if name not in baseline.functions:
            continue
        baseline_sample = min(baseline.functions[name], key=lambda sample: abs(sample.tick - baseline_match.tick))
        candidate_sample = min(samples, key=lambda sample: abs(sample.tick - candidate_match.tick))
        rows.append((candidate_sample.inclusive_ms, name, baseline_sample, candidate_sample))
    print("\n=== Functions at common progress, sorted by candidate inclusive time ===")
    for _, name, baseline_sample, candidate_sample in sorted(rows, reverse=True):
        baseline_average = baseline_sample.inclusive_ms * 1000 / baseline_sample.calls
        candidate_average = candidate_sample.inclusive_ms * 1000 / candidate_sample.calls
        print(f"{name}: baseline_ms={baseline_sample.inclusive_ms} candidate_ms={candidate_sample.inclusive_ms} "
              f"baseline_avg_us={baseline_average:.2f} candidate_avg_us={candidate_average:.2f} "
              f"baseline_calls={baseline_sample.calls} candidate_calls={candidate_sample.calls}")


if __name__ == "__main__":
    main()
