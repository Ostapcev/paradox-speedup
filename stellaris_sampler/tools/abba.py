"""Сравнение прогонов A/B по журналам -gamestatetimer на одних и тех же датах.

    python tools/abba.py --from 2300.01.02 --to 2300.06.30 A=файл.tsv B=файл.tsv B=... A=...

Для каждого прогона: сумма времени дней в диапазоне дат, медиана дня. Затем средние по A и B
и ускорение A/B (сумма и медиана), плюс попарное сравнение соседних A и B (дрейф по порядку).
"""
from __future__ import annotations

import argparse
import statistics
import sys
from pathlib import Path


def days(path: Path, lo: str, hi: str) -> dict[str, float]:
    out: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) == 3 and parts[1] == "Day" and lo <= parts[0] <= hi:
            out[parts[0]] = float(parts[2])
    return out


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("--from", dest="lo", required=True)
    p.add_argument("--to", dest="hi", required=True)
    p.add_argument("runs", nargs="+", help="МЕТКА=файл.tsv")
    a = p.parse_args()
    runs = []
    for r in a.runs:
        label, _, f = r.partition("=")
        runs.append((label, Path(f), days(Path(f), a.lo, a.hi)))
    common = set.intersection(*(set(d) for _, _, d in runs))
    if not common:
        sys.exit("нет общих дат")
    print(f"общих дней: {len(common)} ({min(common)} .. {max(common)})")
    by_label: dict[str, list[tuple[float, float]]] = {}
    for label, f, d in runs:
        t = [d[k] for k in common]
        s, m = sum(t), statistics.median(t)
        by_label.setdefault(label, []).append((s, m))
        print(f"  {label}  сумма {s:7.1f} с  медиана {1000 * m:5.0f} мс  макс {1000 * max(t):6.0f} мс  {f.name}")
    for label, v in by_label.items():
        print(f"{label}: сумма {statistics.mean(x[0] for x in v):.1f} с, медиана {1000 * statistics.mean(x[1] for x in v):.0f} мс")
    if "A" in by_label and "B" in by_label:
        sa = statistics.mean(x[0] for x in by_label["A"]); sb = statistics.mean(x[0] for x in by_label["B"])
        ma = statistics.mean(x[1] for x in by_label["A"]); mb = statistics.mean(x[1] for x in by_label["B"])
        print(f"A/B: по сумме x{sa / sb:.3f} ({100 * (sa - sb) / sa:+.1f}%), по медиане x{ma / mb:.3f}")
        # поденное сравнение: медиана отношений A/B по каждому дню
        da = [r[2] for r in runs if r[0] == "A"]; db = [r[2] for r in runs if r[0] == "B"]
        ratios = [statistics.mean(x[k] for x in da) / statistics.mean(x[k] for x in db) for k in common]
        print(f"медиана поденных отношений A/B: x{statistics.median(ratios):.3f}")


if __name__ == "__main__":
    main()
