"""Самые «горячие» функции (по собственному времени) внутри поддерева заданной функции.

    python tools/hotspots.py <профиль> <адрес> [--top 25] [--all-threads]

Для каждого сэмпла, где функция есть в стеке, собственное время листа и полное время каждой
функции НИЖЕ неё складываются; печатаются обе таблицы (доли — от времени поддерева).
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_stacks import Resolver, read_stacks  # noqa: E402


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("profile", type=Path)
    p.add_argument("function", nargs="+")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--all-threads", action="store_true")
    p.add_argument("--thread", help="id потока вместо главного (для --wall-all)")
    a = p.parse_args()
    targets = {int(f, 16) for f in a.function}
    resolver = Resolver(a.profile)
    rows = list(read_stacks(a.profile))
    per_thread = defaultdict(int)
    for thread, _, samples, _ in rows:
        per_thread[thread] += samples
    main_thread = type(next(iter(per_thread)))(a.thread) if a.thread else max(per_thread, key=per_thread.get)
    if not a.all_threads:
        rows = [r for r in rows if r[0] == main_thread]
    total_all = sum(r[1] for r in rows)
    self_ms: dict[str, float] = defaultdict(float)
    incl_ms: dict[str, float] = defaultdict(float)
    names: dict[str, str] = {}
    inside = 0.0
    for _, ms, _, frames in rows:
        resolved = [resolver.resolve(f) for f in frames]
        idx = next((i for i, r in enumerate(resolved) if r[1] in targets), None)
        if idx is None:
            continue
        inside += ms
        seen = set()
        for key, _, name, source in resolved[idx:]:
            names[key] = f"{name[:44]:44} {source}"
            if key not in seen:
                seen.add(key)
                incl_ms[key] += ms
        self_ms[resolved[-1][0]] += ms
    print(f"поддерево {' '.join(a.function)}: {inside:.0f} мс = {100 * inside / total_all:.1f}% "
          f"{'всех потоков' if a.all_threads else 'главного потока'}")
    print("\nсобственное время:")
    for k in sorted(self_ms, key=self_ms.get, reverse=True)[:a.top]:
        print(f"  {100 * self_ms[k] / inside:5.1f}% {self_ms[k]:7.0f} мс  {k:>10} {names[k]}")
    print("\nполное время:")
    for k in sorted(incl_ms, key=incl_ms.get, reverse=True)[:a.top]:
        print(f"  {100 * incl_ms[k] / inside:5.1f}% {incl_ms[k]:7.0f} мс  {k:>10} {names[k]}")


if __name__ == "__main__":
    main()
