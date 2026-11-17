"""Работа и ожидание по потокам для профиля --wall-all.

    python tools/threads_wait.py <профиль> [--top 12] [--min-share 1]

Для каждого потока (по убыванию работы): реальное время, доля работы и ожидания, где ждёт
(вызывающий кадр игры + системная функция) и самые тяжёлые функции игры (полное время).
Ожидание = глубочайший кадр игры вызывает функцию ожидания (Sleep*, SwitchToThread, Wait*,
SleepConditionVariable*, NtDelayExecution, NtYield...).
"""
from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_stacks import Resolver  # noqa: E402

WAIT_WORDS = ("Sleep", "SwitchToThread", "Wait", "Yield", "DelayExecution", "ConditionVariable",
              "WaitOnAddress", "NtRemoveIoCompletion", "GetQueuedCompletionStatus")


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("profile", type=Path)
    p.add_argument("--top", type=int, default=12)
    p.add_argument("--min-share", type=float, default=1.0, help="печатать потоки с работой ≥ N%% главного")
    a = p.parse_args()
    csv.field_size_limit(10 ** 9)
    sym = {r["frame"]: r["symbol"] for r in csv.DictReader(open(a.profile / "symbols.csv", encoding="utf-8"))}
    res = Resolver(a.profile)
    total = defaultdict(float)
    wait = defaultdict(float)
    wait_at = defaultdict(lambda: defaultdict(float))
    work_incl = defaultdict(lambda: defaultdict(float))
    names: dict[str, str] = {}
    for r in csv.DictReader(open(a.profile / "stacks.csv", encoding="utf-8")):
        t = r["thread_id"]
        ms = float(r["estimated_cpu_ms"])
        fr = r["frames"].split(";")
        total[t] += ms
        idx = max((i for i, f in enumerate(fr) if f.startswith("stellaris.exe")), default=None)
        sysf = fr[idx + 1] if idx is not None and idx + 1 < len(fr) else (fr[-1] if fr else "")
        s = sym.get(sysf, "")
        is_wait = ("Wake" not in s and any(w in s for w in WAIT_WORDS)) or (idx is None and bool(fr))
        if is_wait:
            wait[t] += ms
            caller = res.resolve(fr[idx]) if idx is not None else ("(вне игры)", None, "", "")
            wait_at[t][f"{caller[0]:>10} {caller[2][:40]} -> {sysf.split('+')[0]}!{s}"] += ms
            continue
        seen = set()
        for f in fr:
            if not f.startswith("stellaris.exe"):
                continue
            key, _, name, source = res.resolve(f)
            if key in seen:
                continue
            seen.add(key)
            names[key] = f"{name[:40]} {source}"
            work_incl[t][key] += ms
    work = {t: total[t] - wait[t] for t in total}
    main_t = max(work, key=work.get)
    print(f"потоков с сэмплами: {len(total)}; главный {main_t}")
    tot_work = sum(work.values())
    print(f"работа всех потоков: {tot_work / 1000:.1f} с; главного {work[main_t] / 1000:.1f} с "
          f"({100 * work[main_t] / tot_work:.0f}% всей работы)\n")
    for t in sorted(work, key=work.get, reverse=True):
        if work[t] < a.min_share / 100 * work[main_t]:
            continue
        print(f"поток {t}: реальное {total[t] / 1000:.1f} с, работа {100 * work[t] / total[t]:.0f}%, "
              f"ожидание {100 * wait[t] / total[t]:.0f}%")
        for k, v in sorted(wait_at[t].items(), key=lambda x: -x[1])[:4]:
            print(f"    ждёт {100 * v / total[t]:5.1f}%  {k}")
        if t == main_t or work[t] > 0.2 * work[main_t]:
            for k, v in sorted(work_incl[t].items(), key=lambda x: -x[1])[1:a.top]:
                print(f"    раб. {100 * v / total[t]:5.1f}%  {k:>10} {names[k]}")
        print()


if __name__ == "__main__":
    main()
