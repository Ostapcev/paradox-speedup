"""Раскладка времени функции по её прямым потомкам (по стекам главного потока).

    python tools/children.py <профиль> <адрес функции> [--depth N] [--all-threads] [--min 0.3]

Для каждого сэмпла, где функция есть в стеке (берётся самое верхнее её вхождение), время
относится к кадру, вызванному из неё (или «self», если она лист). --depth 2 раскрывает ещё уровень.
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
    p.add_argument("function")
    p.add_argument("--depth", type=int, default=1)
    p.add_argument("--all-threads", action="store_true")
    p.add_argument("--thread", help="id потока вместо главного (для --wall-all)")
    p.add_argument("--min", type=float, default=0.3, help="минимальная доля от функции, %%")
    a = p.parse_args()
    target = int(a.function, 16)
    resolver = Resolver(a.profile)
    rows = list(read_stacks(a.profile))
    per_thread = defaultdict(int)
    for thread, _, samples, _ in rows:
        per_thread[thread] += samples
    main_thread = type(next(iter(per_thread)))(a.thread) if a.thread else max(per_thread, key=per_thread.get)
    if not a.all_threads:
        rows = [r for r in rows if r[0] == main_thread]
    total_all = sum(r[1] for r in rows)
    tree: dict[tuple, float] = defaultdict(float)
    names: dict[str, str] = {}
    inside = 0.0
    for _, ms, _, frames in rows:
        resolved = [resolver.resolve(f) for f in frames]
        idx = next((i for i, r in enumerate(resolved) if r[1] == target), None)
        if idx is None:
            continue
        inside += ms
        path = []
        for d in range(1, a.depth + 1):
            j = idx + d
            if j >= len(resolved):
                path.append("self")
                break
            key, _, name, source = resolved[j]
            names[key] = f"{name[:48]:48} {source}"
            path.append(key)
            tree[tuple(path)] += ms
        else:
            continue
        tree[tuple(path)] += ms
    print(f"{a.function}: {inside:.0f} мс = {100 * inside / total_all:.1f}% "
          f"{'всех потоков' if a.all_threads else 'главного потока'}")
    for path in sorted(tree, key=lambda k: (k[:-1], -tree[k])):
        pass
    def show(prefix: tuple, level: int) -> None:
        kids = [k for k in tree if len(k) == level + 1 and k[:level] == prefix]
        for k in sorted(kids, key=lambda k: -tree[k]):
            share = 100 * tree[k] / inside if inside else 0
            if share < a.min:
                continue
            label = "self" if k[-1] == "self" else f"{k[-1]:>10} {names.get(k[-1], '')}"
            print(f"{'  ' * level}{share:6.1f}% {tree[k]:8.0f} мс  {label}")
            if k[-1] != "self":
                show(k, level + 1)
    show((), 0)


if __name__ == "__main__":
    main()
