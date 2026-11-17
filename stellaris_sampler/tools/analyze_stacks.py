"""Разбор stacks.csv из stellaris_sampler: полное (inclusive) время по функциям и по подсистемам.

Использование:
    python stellaris_sampler/tools/analyze_stacks.py ПАПКА_ПРОФИЛЯ [--diff ДРУГАЯ_ПАПКА] [--all-threads]

Пишет в папку профиля:
    inclusive_functions.csv  — функция, своё время (self), полное время (inclusive), % потока;
    subsystems.csv           — время, разложенное по шагам игрового тика и кадра (каждый сэмпл
                               попадает ровно в одну строку: по самому глубокому «якорю» в стеке);
    crosscut.csv             — сквозные группы (триггеры, модификаторы, RefreshKnowledge, malloc…):
                               доля сэмплов, где группа есть в стеке; группы пересекаются;
    folded.txt               — свёрнутые стеки для https://www.speedscope.app или flamegraph.pl.

По умолчанию считается только главный поток (поток с наибольшим числом сэмплов): в Stellaris он
и есть узкое место. Привязка адреса к функции — по ближайшему предыдущему началу функции из
базы разбора (analysis/db); адреса возвратов сэмплер уже сдвинул внутрь CALL.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import os
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]
IMAGE_BASE = 0x140000000
GAME_MODULE = "stellaris.exe"

# Разбор stellaris.exe 4.5.4 (Ghidra): C:\Stellaris\analysis, переменная STELLARIS_ANALYSIS.
ANALYSIS = Path(os.environ.get("STELLARIS_ANALYSIS", r"C:\Stellaris\analysis"))

# Якоря подсистем Stellaris 4.5.4: адрес начала функции -> название. Сэмпл относится
# к самому глубокому якорю в своём стеке. Размечено по профилям сейва test5000
# (5000 звёзд, 2026-11-14). Главный цикл 140336b40 намеренно не якорь: он же вызывается
# вложенно из 14024be40, и вложенные кадры ушли бы в «главный цикл».
ANCHORS: dict[int, str] = {
    0x140251780: "Тик gamestate (gamestate.cpp)",
    0x14025C850: "Тик: DailyUpdate (gamestate.cpp, 'Daily Tick!')",
    0x140252C40: "Тик: 140252c40 (вызывает сброс кэша расстояний 1408ffa40)",
    0x1402640B0: "Тик: 1402640b0",
    0x140261090: "Тик: месячное обновление (140261090)",
    0x140262950: "Тик: годовое обновление (140262950)",
    0x140E41710: "DailyUpdate: gamestate_global_1710",
    0x140AA9B50: "Отложенные события (delayed_events.cpp, проверка ToAbort)",
    0x140C67AC0: "Флоты (fleet.cpp)",
    0x14024BE40: "Кадр, отрисованный изнутри тика (14024be40 -> 141a52770)",
    0x1402653A0: "Загрузка сейва (frontendidler -> gamestate_4a60)",
    0x1408FFA40: "Сброс кэша расстояний между системами (dirty-флаг 14280e9d0)",
    0x140A59660: "Пересчёт кэша расстояний: все пары систем, A* на пару ('Recalc cached data')",
    # тело задания того же пересчёта в рабочих потоках джоб-системы (строка матрицы на источник)
    0x140A5BBB0: "Пересчёт кэша расстояний: все пары систем, A* на пару ('Recalc cached data')",
}

# Сквозные группы: имя -> правило по функции (адрес, имя, исходник). Группы пересекаются.
CROSSCUT = {
    "Выделение памяти (malloc/new)": lambda a, n, s: n in ("_malloc_base", "operator_new", "malloc"),
    "Скриптовые эффекты (source/effect_impl)": lambda a, n, s: "/effect_impl/" in s,
    "Скриптовые триггеры (source/trigger_impl)": lambda a, n, s: "/trigger_impl/" in s,
    "ИИ (source/ai)": lambda a, n, s: "/source/ai/" in s,
    "Парсер скриптов (pdx_parser)": lambda a, n, s: "/pdx_parser/" in s,
    "Джоб-система: parallel-for (141b45f40)": lambda a, n, s: a == 0x141B45F40,
    "Кэш расстояний: A* одной пары (140a5a4a0)": lambda a, n, s: a == 0x140A5A4A0,
    "Кадр изнутри тика (14024be40)": lambda a, n, s: a == 0x14024BE40,
}


DRIVER_MODULES = {"nvd3dumx.dll", "d3d9.dll", "nvwgf2umx.dll", "atidxx64.dll", "aticfx64.dll",
                  "igdumdim64.dll", "win32u.dll", "dxgi.dll", "nvldumdx.dll"}
# Системные вызовы, на которых поток стоит, а не считает (виден только в режиме --wall).
WAIT_MARKERS = ("Wait", "DelayExecution", "Sleep", "YieldExecution", "RemoveIoCompletion",
                "SignalAndWait", "ReplyWaitReceivePort")


def load_function_index():
    """Начала функций и имена из analysis/db/stellaris.db, восстановленные имена из
    rename_map.tsv, исходник из index/source_map.tsv, иначе модуль из func_class.tsv."""
    db = ANALYSIS / "db" / "stellaris.db"
    if not db.exists():
        raise SystemExit(f"нет базы разбора {db} (задайте STELLARIS_ANALYSIS)")
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    names = {int(a, 16): n for a, n in con.execute("SELECT addr, name FROM functions")}
    con.close()
    rename = ANALYSIS / "db" / "rename_map.tsv"
    if rename.exists():
        for line in rename.open(encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2 and not line.startswith("#"):
                names[int(parts[0], 16)] = parts[1]
    sources: dict[int, str] = {}
    func_class = ANALYSIS / "db" / "func_class.tsv"
    if func_class.exists():
        for line in func_class.open(encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2 and not line.startswith("#") and parts[1] != "unknown":
                sources[int(parts[0], 16)] = f"[{parts[1]}]"
    source_map = ANALYSIS.parent / "index" / "source_map.tsv"
    if source_map.exists():
        for line in source_map.open(encoding="utf-8"):
            if line.startswith("#"):
                continue
            path, _, rest = line.rstrip("\n").partition("\t")
            path = path.replace("\\", "/").replace("C:/mnt/gsg/stellaris/augustus/", "")
            for item in rest.partition("\t")[2].split(" | "):
                _, _, address = item.partition(" @ ")
                if address:
                    sources[int(address, 16)] = path
    starts = sorted(names)
    info = {start: (names[start], sources.get(start, "")) for start in starts}
    return starts, info


class Resolver:
    def __init__(self, profile: Path) -> None:
        self.starts, self.info = load_function_index()
        self.cache: dict[str, tuple[str, int | None, str, str]] = {}
        # symbols.csv (сэмплер, dbghelp): имена функций в системных DLL и драйверах.
        self.external: dict[str, str] = {}
        symbols = profile / "symbols.csv"
        if symbols.exists():
            with symbols.open(newline="", encoding="utf-8", errors="replace") as stream:
                for row in csv.DictReader(stream):
                    self.external[row["frame"]] = row["symbol"]

    def resolve(self, frame: str) -> tuple[str, int | None, str, str]:
        """frame 'module+0xRVA' -> (ключ функции, VA начала или None, имя, исходник)."""
        cached = self.cache.get(frame)
        if cached:
            return cached
        module, _, rva_text = frame.partition("+0x")
        rva = int(rva_text, 16) if rva_text else 0
        if module.lower() == GAME_MODULE:
            va = IMAGE_BASE + rva
            position = bisect.bisect_right(self.starts, va) - 1
            if position >= 0:
                start = self.starts[position]
                name, source = self.info[start]
                result = (f"{start:x}", start, name, source)
            else:
                result = (frame, None, frame, "")
        else:
            # Внешние модули: по имени из symbols.csv, иначе по модулю целиком.
            symbol = self.external.get(frame)
            name = f"[{module}] {symbol}" if symbol else f"[{module}]"
            result = (name, None, name, "")
        self.cache[frame] = result
        return result


def read_stacks(profile: Path):
    with (profile / "stacks.csv").open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            yield int(row["thread_id"]), float(row["estimated_cpu_ms"]), int(row["samples"]), \
                row["frames"].split(";") if row["frames"] else []


def analyze(profile: Path, all_threads: bool, thread: int | None = None):
    resolver = Resolver(profile)
    rows = list(read_stacks(profile))
    if not rows:
        raise SystemExit(f"{profile / 'stacks.csv'} пуст")
    per_thread = defaultdict(int)
    for tid, _, samples, _ in rows:
        per_thread[tid] += samples
    main_thread = thread if thread is not None else max(per_thread, key=per_thread.get)
    if not all_threads:
        rows = [row for row in rows if row[0] == main_thread]

    total_ms = sum(row[1] for row in rows)
    total_samples = sum(row[2] for row in rows)
    self_ms = defaultdict(float)
    incl_ms = defaultdict(float)
    incl_samples = defaultdict(int)
    meta: dict[str, tuple[str, str]] = {}
    buckets = defaultdict(float)
    crosscut = defaultdict(float)
    folded = defaultdict(int)

    for _, ms, samples, frames in rows:
        resolved = [resolver.resolve(frame) for frame in frames]  # корень -> лист
        seen = set()
        bucket = None
        groups = set()
        for key, start, name, source in resolved:
            meta[key] = (name, source)
            if key not in seen:
                seen.add(key)
                incl_ms[key] += ms
                incl_samples[key] += samples
            if start is not None and start in ANCHORS:
                bucket = ANCHORS[start]
            for group, rule in CROSSCUT.items():
                if start is not None and rule(start, name, source.replace("\\", "/")):
                    groups.add(group)
        if resolved:
            leaf_key = resolved[-1][0]
            self_ms[leaf_key] += ms
            leaf_name = resolved[-1][2]
            if bucket is None:
                bucket = "Вне главного цикла / не размечено"
            # Лист во внешнем модуле — уточняем: драйвер, ожидание, POPS.
            if leaf_name.startswith("["):
                leaf_module = leaf_name[1:leaf_name.index("]")].lower()
                if any(name.startswith("[pdxsdk.dll]") for _, _, name, _ in resolved):
                    bucket = "Онлайн-сервисы Paradox (PDXSDK.dll)"
                elif leaf_module in DRIVER_MODULES:
                    bucket = f"{bucket} · драйвер/D3D"
                elif any(marker in leaf_name for marker in WAIT_MARKERS):
                    bucket = f"{bucket} · ожидание ({leaf_name.split('] ')[-1]})"
        buckets[bucket] += ms
        for group in groups:
            crosscut[group] += ms
        folded[";".join(name for _, _, name, _ in resolved)] += samples

    return {
        "main_thread": main_thread, "threads": dict(per_thread), "total_ms": total_ms,
        "total_samples": total_samples, "self": self_ms, "incl": incl_ms,
        "incl_samples": incl_samples, "meta": meta, "buckets": buckets,
        "crosscut": crosscut, "folded": folded,
    }


def percent(value: float, total: float) -> str:
    return f"{100.0 * value / total:.2f}" if total else "0"


def write_outputs(profile: Path, result) -> None:
    total = result["total_ms"]
    with (profile / "inclusive_functions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["function", "name", "source_file", "self_ms", "inclusive_ms",
                         "self_percent", "inclusive_percent", "inclusive_samples"])
        for key in sorted(result["incl"], key=result["incl"].get, reverse=True):
            name, source = result["meta"][key]
            writer.writerow([key, name, source, f"{result['self'][key]:.3f}",
                             f"{result['incl'][key]:.3f}", percent(result["self"][key], total),
                             percent(result["incl"][key], total), result["incl_samples"][key]])
    with (profile / "subsystems.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["subsystem", "ms", "percent"])
        for name, ms in sorted(result["buckets"].items(), key=lambda item: -item[1]):
            writer.writerow([name, f"{ms:.3f}", percent(ms, total)])
    with (profile / "crosscut.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["group", "ms", "percent"])
        for name, ms in sorted(result["crosscut"].items(), key=lambda item: -item[1]):
            writer.writerow([name, f"{ms:.3f}", percent(ms, total)])
    with (profile / "folded.txt").open("w", encoding="utf-8") as stream:
        for stack, samples in sorted(result["folded"].items(), key=lambda item: -item[1]):
            stream.write(f"{stack} {samples}\n")


def print_summary(result, other=None, top: int = 30) -> None:
    total = result["total_ms"]
    print(f"Поток {result['main_thread']}: {result['total_samples']} сэмплов, {total:.0f} мс CPU")
    header = f"{'%':>7} {'мс':>9}"
    if other:
        header += f" {'было %':>7} {'Δ п.п.':>7}"
    print("\nПодсистемы (каждый сэмпл ровно в одной строке):")
    print(header + "  подсистема")
    names = sorted(set(result["buckets"]) | set(other["buckets"] if other else ()),
                   key=lambda name: -result["buckets"].get(name, 0.0))
    for name in names:
        ms = result["buckets"].get(name, 0.0)
        line = f"{percent(ms, total):>7} {ms:9.0f}"
        if other:
            before = 100.0 * other["buckets"].get(name, 0.0) / other["total_ms"]
            line += f" {before:7.2f} {100.0 * ms / total - before:+7.2f}"
        print(f"{line}  {name}")
    print("\nСквозные группы (пересекаются):")
    for name, ms in sorted(result["crosscut"].items(), key=lambda item: -item[1]):
        line = f"{percent(ms, total):>7} {ms:9.0f}"
        if other:
            before = 100.0 * other["crosscut"].get(name, 0.0) / other["total_ms"]
            line += f" {before:7.2f} {100.0 * ms / total - before:+7.2f}"
        print(f"{line}  {name}")
    print(f"\nФункции по полному времени (top {top}):")
    for key in sorted(result["incl"], key=result["incl"].get, reverse=True)[:top]:
        name, source = result["meta"][key]
        print(f"{percent(result['incl'][key], total):>7}% incl {percent(result['self'][key], total):>6}% self  "
              f"{key:>10} {name[:44]:44} {source}")


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--diff", type=Path, help="папка другого профиля для сравнения долей")
    parser.add_argument("--all-threads", action="store_true", help="считать все потоки, а не только главный")
    parser.add_argument("--thread", type=int, help="id потока вместо главного (для --wall-all)")
    parser.add_argument("--top", type=int, default=30)
    args = parser.parse_args()
    result = analyze(args.profile.resolve(), args.all_threads, args.thread)
    write_outputs(args.profile.resolve(), result)
    other = analyze(args.diff.resolve(), args.all_threads) if args.diff else None
    print_summary(result, other, args.top)
    print(f"\nФайлы: inclusive_functions.csv, subsystems.csv, crosscut.csv, folded.txt в {args.profile}")


if __name__ == "__main__":
    main()
