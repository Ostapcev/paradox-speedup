"""Оценка стоимости проверки карточек фокуса по типам: МНК по focus_samples.csv.

    python tools/fit_cards.py <focus_samples.csv> [--top 25]

Модель: время вызова = base + сумма c[тип] по ожидающим карточкам. Решается неотрицательным
МНК (проекционный градиент, без numpy-зависимостей сверх numpy).
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

import numpy as np


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("csv", type=Path)
    p.add_argument("--top", type=int, default=25)
    a = p.parse_args()
    names: dict[int, str] = {}
    rows: list[tuple[int, float, list[int]]] = []
    text = a.csv.read_text(encoding="utf-8", errors="replace")
    # 0.1.0 писал строку T посреди строки S — вырезаем T где угодно
    type_line = re.compile("T,([0-9]+),([A-Za-z0-9_.:-]*)" + chr(13) + "?" + chr(10))
    for m in type_line.finditer(text):
        names[int(m.group(1))] = m.group(2)
    text = type_line.sub("", text)
    for line in text.splitlines():
        parts = line.split(",")
        if parts[0] == "T" and len(parts) >= 3:
            names[int(parts[1])] = ",".join(parts[2:])
        elif parts[0] == "S" and len(parts) >= 3:
            try:
                rows.append((int(parts[1]), float(parts[2]), [int(x) for x in parts[3:] if x]))
            except ValueError:
                pass  # последняя строка могла быть дописана не до конца
    n_types = max([max(names, default=-1)] + [max(ts, default=-1) for _, _, ts in rows]) + 1
    X = np.zeros((len(rows), n_types + 1))
    y = np.zeros(len(rows))
    for i, (_, dt, ts) in enumerate(rows):
        X[i, 0] = 1
        for t in ts:
            X[i, t + 1] += 1
        y[i] = dt
    total = y.sum()
    print(f"вызовов {len(rows)}, сумма {total / 1000:.0f} мс, среднее {y.mean():.1f} мкс, "
          f"медиана {np.median(y):.1f} мкс, max {y.max():.0f} мкс")
    per_country = Counter()
    for c, dt, _ in rows:
        per_country[c] += dt
    print("самые дорогие страны:", ", ".join(f"{c}: {v / 1000:.0f} мс" for c, v in per_country.most_common(8)))
    # неотрицательный МНК
    coef, *_ = np.linalg.lstsq(X, y, rcond=None)
    coef = np.maximum(coef, 0)
    L = np.linalg.norm(X, 2) ** 2
    for _ in range(3000):
        coef = np.maximum(coef - (X.T @ (X @ coef - y)) / L, 0)
    resid = y - X @ coef
    r2 = 1 - (resid ** 2).sum() / ((y - y.mean()) ** 2).sum()
    counts = X[:, 1:].sum(axis=0)
    print(f"base {coef[0]:.1f} мкс/вызов ({100 * coef[0] * len(rows) / total:.0f}% времени), R² {r2:.3f}\n")
    contrib = coef[1:] * counts
    order = np.argsort(-contrib)
    print("  доля    мс     ожид.   мкс/провер.  тип")
    for t in order[:a.top]:
        if contrib[t] <= 0:
            break
        print(f"  {100 * contrib[t] / total:5.1f}% {contrib[t] / 1000:6.0f} {int(counts[t]):8d} {coef[t + 1]:10.1f}   "
              f"{names.get(int(t), '?')}")


if __name__ == "__main__":
    main()
