"""Map sampled EU4 RVAs to the decompiled function index and rank CPU hotspots."""
from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import math
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

EXPECTED_EU4_SHA256 = "9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a"
WORKSPACE = Path(__file__).resolve().parents[2]
DEFAULT_INDEX = WORKSPACE / "eu4_kit" / "data" / "findings" / "eu4_function_index.csv"


@dataclass
class Function:
    address: int
    name: str
    source: str
    confidence: str
    methods: str


@dataclass
class Measurement:
    leaf_ms: float = 0.0
    inclusive_ms: float = 0.0
    leaf_samples: int = 0
    inclusive_samples: int = 0
    addresses: set[str] = field(default_factory=set)

    def add(self, row: dict[str, str]) -> None:
        self.leaf_ms += float(row["leaf_cpu_ms"])
        self.inclusive_ms += float(row["inclusive_cpu_ms"])
        self.leaf_samples += int(row["leaf_samples"])
        self.inclusive_samples += int(row["inclusive_samples"])
        self.addresses.add(row["rva"])


def metadata(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
    return result


def load_index(path: Path) -> tuple[list[int], dict[int, Function]]:
    functions: dict[int, Function] = {}
    with path.open(newline="", encoding="utf-8-sig") as file:
        for row in csv.DictReader(file):
            address = int(row["address"], 16)
            functions[address] = Function(
                address=address,
                name=row["ghidra_name"],
                source=row["source_file"] or "unknown",
                confidence=row["source_confidence"],
                methods=row["method_names"],
            )
    return sorted(functions), functions


def subsystem(source: str) -> str:
    if not source or source == "unknown":
        return "unknown / unclassified"
    parts = source.replace("\\", "/").split("/")
    if len(parts) >= 2 and parts[0] in {"eu4", "ext"}:
        return f"{parts[0]}/{parts[1]}"
    return parts[0]


def write_csv(path: Path, rows: list[tuple[Function, Measurement]], total_ms: float) -> None:
    with path.open("w", newline="", encoding="utf-8-sig") as file:
        writer = csv.writer(file)
        writer.writerow([
            "function_address", "function", "source_file", "source_confidence", "methods",
            "leaf_cpu_ms", "leaf_percent", "inclusive_cpu_ms", "inclusive_percent",
            "leaf_samples", "inclusive_samples", "sampled_rvas",
        ])
        for function, stat in rows:
            writer.writerow([
                f"{function.address:09x}", function.name, function.source, function.confidence,
                function.methods, f"{stat.leaf_ms:.3f}", f"{stat.leaf_ms * 100 / total_ms:.4f}",
                f"{stat.inclusive_ms:.3f}", f"{stat.inclusive_ms * 100 / total_ms:.4f}",
                stat.leaf_samples, stat.inclusive_samples, ";".join(sorted(stat.addresses)),
            ])


def markdown_table(rows: list[tuple[Function, Measurement]], total_ms: float, limit: int) -> list[str]:
    lines = [
        "| Функция | Исходник | Собств. CPU | Собств. % | Inclusive % |",
        "|---|---|---:|---:|---:|",
    ]
    for function, stat in rows[:limit]:
        source = subsystem(function.source).replace("|", "\\|")
        methods = f" ({function.methods})" if function.methods else ""
        name = f"`{function.name}{methods}`"
        lines.append(
            f"| {name} | {source} | {stat.leaf_ms:.1f} мс | "
            f"{stat.leaf_ms * 100 / total_ms:.2f} % | {stat.inclusive_ms * 100 / total_ms:.2f} % |"
        )
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="Directory containing functions.csv and metadata.txt")
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--allow-hash-mismatch", action="store_true")
    args = parser.parse_args()
    report = args.report.resolve()
    meta = metadata(report / "metadata.txt")
    executable = Path(meta["executable"])
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    if digest != EXPECTED_EU4_SHA256 and not args.allow_hash_mismatch:
        raise SystemExit(
            f"Executable hash mismatch: {digest}. Function names would be unreliable; "
            "use the matching EU4 1.37.5.0 Inca executable."
        )

    starts, index = load_index(args.index)
    grouped: dict[int, Measurement] = defaultdict(Measurement)
    with (report / "functions.csv").open(newline="", encoding="utf-8-sig") as file:
        for row in csv.DictReader(file):
            rva = int(row["rva"], 16)
            virtual_address = 0x140000000 + rva
            position = bisect.bisect_right(starts, virtual_address) - 1
            if position < 0:
                continue
            start = starts[position]
            grouped[start].add(row)

    rows = [(index[address], stat) for address, stat in grouped.items()]
    by_leaf = sorted(rows, key=lambda item: item[1].leaf_ms, reverse=True)
    by_inclusive = sorted(rows, key=lambda item: item[1].inclusive_ms, reverse=True)
    total_ms = float(meta["attributed_cpu_ms"])
    if not math.isfinite(total_ms) or total_ms <= 0:
        raise SystemExit("No attributed CPU time in this report")
    write_csv(report / "hotspots.csv", by_leaf, total_ms)

    by_subsystem: dict[str, Measurement] = defaultdict(Measurement)
    for function, stat in rows:
        by_subsystem[subsystem(function.source)].leaf_ms += stat.leaf_ms
        by_subsystem[subsystem(function.source)].inclusive_ms += stat.inclusive_ms
    subsystem_rows = sorted(by_subsystem.items(), key=lambda item: item[1].leaf_ms, reverse=True)

    with (report / "threads.csv").open(newline="", encoding="utf-8-sig") as file:
        thread_rows = sorted(csv.DictReader(file), key=lambda row: float(row["estimated_cpu_ms"]), reverse=True)
    with (report / "leaf.csv").open(newline="", encoding="utf-8-sig") as file:
        external = [row for row in csv.DictReader(file) if row["module"] != executable.name][:12]

    lines = [
        "# Профиль нагрузки EU4",
        "",
        f"- Длительность съёма: {float(meta['elapsed_ms']) / 1000:.1f} с.",
        f"- Стеков с CPU-активностью: {meta['captured_samples']}.",
        f"- Оценка CPU всех доступных потоков: {float(meta['total_cpu_ms']) / 1000:.3f} с.",
        f"- Оценка CPU в стеках EU4: {total_ms / 1000:.3f} с.",
        f"- Покрытие стеками EU4: {total_ms * 100 / float(meta['total_cpu_ms']):.1f} %.",
        "",
        "Собственное время (leaf) — работа непосредственно внутри функции. Inclusive включает вложенные вызовы, "
        "поэтому его проценты нельзя складывать. Это статистическая оценка по снятым стекам, а не точный бенчмарк.",
        "",
        "## Основные функции по собственному CPU",
        "",
        *markdown_table(by_leaf, total_ms, 30),
        "",
        "## Основные функции с учётом вложенных вызовов",
        "",
        *markdown_table(by_inclusive, total_ms, 20),
        "",
        "## Подсистемы по собственному CPU",
        "",
        "| Подсистема | Собств. CPU | Собств. % | Inclusive % |",
        "|---|---:|---:|---:|",
    ]
    for name, stat in subsystem_rows[:25]:
        lines.append(
            f"| {name} | {stat.leaf_ms:.1f} мс | {stat.leaf_ms * 100 / total_ms:.2f} % | "
            f"{stat.inclusive_ms * 100 / total_ms:.2f} % |"
        )
    lines.extend([
        "",
        "## Потоки по суммарному CPU",
        "",
        "| Thread ID | CPU | В стеках EU4 | Стеков |",
        "|---:|---:|---:|---:|",
    ])
    for row in thread_rows[:20]:
        lines.append(
            f"| {row['thread_id']} | {float(row['estimated_cpu_ms']):.1f} мс | "
            f"{float(row['attributed_cpu_ms']):.1f} мс | {row['samples']} |"
        )
    if external:
        lines.extend([
            "",
            "## Внешние модули в стеках EU4",
            "",
            "| Модуль | RVA | CPU | % |",
            "|---|---:|---:|---:|",
        ])
        for row in external:
            lines.append(
                f"| {row['module']} | {row['rva']} | {float(row['estimated_cpu_ms']):.1f} мс | "
                f"{float(row['percent_of_attributed']):.2f} % |"
            )
    output = report / "hotspots.md"
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {output}")
    print("Top leaf functions:")
    for function, stat in by_leaf[:10]:
        print(f"  {stat.leaf_ms * 100 / total_ms:6.2f}% {function.name} {subsystem(function.source)}")


if __name__ == "__main__":
    main()
