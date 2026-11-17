"""Кто вызывает функцию: цепочки вызывающих (по стекам главного потока).

    python tools/parents.py <профиль> <адрес функции> [--depth 3] [--thread TID] [--top 12]

Для каждого сэмпла, где функция есть в стеке (самое верхнее её вхождение), время относится к цепочке
из --depth ближайших вызывающих (ближайший первым).
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
    p.add_argument("--depth", type=int, default=3)
    p.add_argument("--thread", help="id потока вместо главного (для --wall-all)")
    p.add_argument("--top", type=int, default=12)
    a = p.parse_args()
    target = int(a.function, 16)
    resolver = Resolver(a.profile)
    rows = list(read_stacks(a.profile))
    per_thread = defaultdict(int)
    for thread, _, samples, _ in rows:
        per_thread[thread] += samples
    main_thread = type(next(iter(per_thread)))(a.thread) if a.thread else max(per_thread, key=per_thread.get)
    rows = [r for r in rows if r[0] == main_thread]
    total = sum(r[1] for r in rows)
    chains: dict[tuple, float] = defaultdict(float)
    names: dict[str, str] = {}
    inside = 0.0
    for _, ms, _, frames in rows:
        resolved = [resolver.resolve(f) for f in frames]
        idx = next((i for i, r in enumerate(resolved) if r[1] == target), None)
        if idx is None:
            continue
        inside += ms
        chain = []
        for j in range(idx - 1, max(idx - 1 - a.depth, -1), -1):
            key, _, name, _ = resolved[j]
            names[key] = name
            chain.append(key)
        chains[tuple(chain)] += ms
    print(f"{a.function}: {inside:.0f} мс = {100 * inside / total:.1f}% главного потока")
    for chain, ms in sorted(chains.items(), key=lambda kv: -kv[1])[: a.top]:
        text = " < ".join(f"{k} {names[k][:28]}" for k in chain)
        print(f"  {100 * ms / total:5.2f}%  {text}")


if __name__ == "__main__":
    main()
