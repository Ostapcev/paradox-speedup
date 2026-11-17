"""Статистика дней из журнала -gamestatetimer (gametimer_*.tsv).

    python tools/day_stats.py [файл.tsv ...]    (без аргументов — последний журнал игры)

Day — полное время игрового дня (с, реальное); Month/Year — полное время периода (сумма дней),
а не стоимость месячного обновления.
"""
from __future__ import annotations

import os
import statistics
import sys
from pathlib import Path

LOGS = Path(os.environ.get("USERPROFILE", "")) / "Documents/Paradox Interactive/Stellaris/logs"


def stats(path: Path) -> None:
    days = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) == 3 and parts[1] == "Day":
            days.append((parts[0], float(parts[2])))
    if not days:
        print(f"{path.name}: дней нет")
        return
    t = [d for _, d in days]
    q = statistics.quantiles(t, n=10) if len(t) >= 10 else [0] * 9
    print(f"{path.name}: {days[0][0]} .. {days[-1][0]}, дней {len(t)}, сумма {sum(t):.1f} с")
    print(f"  день: медиана {1000 * statistics.median(t):.0f} мс, среднее {1000 * statistics.mean(t):.0f} мс, "
          f"p10 {1000 * q[0]:.0f}, p90 {1000 * q[8]:.0f}, макс {1000 * max(t):.0f} мс; "
          f"{len(t) / sum(t):.2f} дня/с")


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    files = [Path(a) for a in sys.argv[1:]] or sorted(LOGS.glob("gametimer_*.tsv"), key=os.path.getmtime)[-1:]
    for f in files:
        stats(f)


if __name__ == "__main__":
    main()
