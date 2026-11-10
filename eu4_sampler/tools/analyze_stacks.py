"""Разбор stacks.csv из eu4_sampler: полное (inclusive) время по функциям и по подсистемам.

Использование:
    python eu4_sampler/tools/analyze_stacks.py ПАПКА_ПРОФИЛЯ [--diff ДРУГАЯ_ПАПКА] [--all-threads]

Пишет в папку профиля:
    inclusive_functions.csv  — функция, своё время (self), полное время (inclusive), % потока;
    subsystems.csv           — время, разложенное по шагам игрового тика и кадра (каждый сэмпл
                               попадает ровно в одну строку: по самому глубокому «якорю» в стеке);
    crosscut.csv             — сквозные группы (триггеры, модификаторы, RefreshKnowledge, malloc…):
                               доля сэмплов, где группа есть в стеке; группы пересекаются;
    folded.txt               — свёрнутые стеки для https://www.speedscope.app или flamegraph.pl.

По умолчанию считается только главный поток (поток с наибольшим числом сэмплов): в EU4 он
и есть узкое место. Привязка адреса к функции — по ближайшему предыдущему началу функции из
индекса eu4_kit; адреса возвратов сэмплер уже сдвинул внутрь CALL.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import pickle
import sys
from collections import defaultdict
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]
IMAGE_BASE = 0x140000000
EU4_MODULE = "eu4.exe"

# Якоря подсистем: адрес начала функции -> название. Источник — eu4_kit/docs/02_engine_subsystems.md
# («Игровой тик»). Сэмпл относится к самому глубокому якорю в своём стеке, поэтому вложенные
# шаги (например, отрисовка внутри тика 140769ea0) перекрывают внешние.
ANCHORS: dict[int, str] = {
    0x1408757E0: "Главный цикл: прочее",
    0x1414C5810: "Кадр: обновление сцены/интерфейса",
    0x1414CA360: "Сессия: обработка команд (CSession::Update)",
    0x140B79520: "Сессия: выполнение команды дня",
    0x140758430: "Дневной тик: прочее (дата, пинг, юниты)",
    0x140769EA0: "Отрисовка внутри тика (140769ea0)",
    0x14075BBE0: "День: прочее основного обновления",
    0x140759640: "День 2: сообщения о войне и мире",
    0x140760250: "День 3: армии и флоты",
    0x14075AB30: "День 4: список стран",
    0x140B3BE80: "День 5: торговые компании",
    0x14075AE70: "День 6: папство, реформация",
    0x140715400: "День 8: Небесная империя",
    0x14077F340: "День 9: пульсы on_actions, MTTH (параллельно)",
    0x1402ECEC0: "День 9: пульсы on_actions, MTTH (параллельно)",
    0x14077F7F0: "День 9: пульсы on_actions, MTTH (параллельно)",
    0x14075B0D0: "День 10: дневное обновление стран",
    0x1402EE280: "День 10: дневное обновление стран",
    0x14070AF70: "День 11: инциденты СРИ",
    0x140A89360: "День 11: распространение революции",
    0x14077FE10: "День 12: провинции (параллельно)",
    0x1407802C0: "День 12: провинции (параллельно)",
    0x14075B2D0: "День 12: дневное обновление провинций",
    0x1409F52E0: "День 12: дневное обновление провинций",
    0x1402485B0: "День 13: бои",
    0x14075B4B0: "День 14: ИИ (дневной)",
    0x140D91A10: "День 15: военный счёт",
    0x14076E860: "День 16: шаблоны армий",
    0x140DB2370: "Дневной тик: менеджер юнитов",
    0x14075D690: "Месяц: прочее",
    0x140771000: "Месяц 1: 140771000",
    0x140D359A0: "Месяц 4: цены товаров",
    0x140780880: "Месяц 6: провинции (параллельно)",
    0x1409F35C0: "Месяц 7: месячное обновление провинций",
    0x14077E9E0: "Месяц 9: страны (параллельно)",
    0x14077EE90: "Месяц 9: страны (параллельно)",
    0x140780D30: "Месяц 9: страны (параллельно)",
    0x1402F36C0: "Месяц 10: месячное обновление стран",
    0x1401B69A0: "Месяц 11: пул советников",
    0x14009C0A0: "Месяц 12: ИИ (месячный)",
    0x14075E260: "Год: годовой тик",
}

# Сквозные группы: имя -> правило по функции (адрес, имя, исходник). Группы пересекаются.
CROSSCUT = {
    "Триггеры (trigger*.cpp)": lambda a, n, s: "trigger" in s,
    "Эффекты (effect*.cpp)": lambda a, n, s: "effect" in s,
    "Модификаторы (modifier.cpp)": lambda a, n, s: s.endswith("modifier.cpp"),
    "Поиск модификатора 1408ea420/1408ea730": lambda a, n, s: a in (0x1408EA420, 0x1408EA730),
    "RefreshKnowledge 14093fe30": lambda a, n, s: a == 0x14093FE30,
    "Кэш страны (country_cached_data.cpp)": lambda a, n, s: "country_cached_data" in s,
    "Оценка провинции армейским ИИ 1400f8ea0": lambda a, n, s: a == 0x1400F8EA0,
    "Армейский ИИ (ai_army.cpp)": lambda a, n, s: "ai_army" in s,
    "ИИ (ai*.cpp)": lambda a, n, s: "/ai" in s,
    "Торговля (trade*.cpp)": lambda a, n, s: "/trade" in s,
    "Сохранение (savegame*)": lambda a, n, s: "savegame" in s,
    "Графика (graphics/, gfx, pdx_entity)": lambda a, n, s: "graphics" in s or "gfx" in s,
    "Интерфейс (gui)": lambda a, n, s: "gui" in s,
    "Выделение памяти (malloc/new)": lambda a, n, s: n in ("_malloc_base", "operator_new"),
}


DRIVER_MODULES = {"nvd3dumx.dll", "d3d9.dll", "nvwgf2umx.dll", "atidxx64.dll", "aticfx64.dll",
                  "igdumdim64.dll", "win32u.dll", "dxgi.dll", "nvldumdx.dll"}
# Системные вызовы, на которых поток стоит, а не считает (виден только в режиме --wall).
WAIT_MARKERS = ("Wait", "DelayExecution", "Sleep", "YieldExecution", "RemoveIoCompletion",
                "SignalAndWait", "ReplyWaitReceivePort")


def load_function_index():
    index = WORKSPACE / "eu4_kit" / "data" / "index"
    with (index / "funcs.pkl").open("rb") as stream:
        functions = pickle.load(stream)
    with (index / "fnnames.json").open(encoding="utf-8") as stream:
        names = json.load(stream)
    sources: dict[str, str] = {}
    index_csv = WORKSPACE / "eu4_kit" / "data" / "findings" / "eu4_function_index.csv"
    with index_csv.open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["source_file"]:
                sources[row["address"]] = row["source_file"]
    starts = sorted(int(address, 16) for address in functions)
    info = {}
    for start in starts:
        key = f"{start:x}"
        name = (names.get(key) or [functions[key].get("name") or f"FUN_{key}"])[0]
        info[start] = (name, sources.get(key, ""))
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
        if module.lower() == EU4_MODULE:
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


def analyze(profile: Path, all_threads: bool):
    resolver = Resolver(profile)
    rows = list(read_stacks(profile))
    if not rows:
        raise SystemExit(f"{profile / 'stacks.csv'} пуст")
    per_thread = defaultdict(int)
    for thread, _, samples, _ in rows:
        per_thread[thread] += samples
    main_thread = max(per_thread, key=per_thread.get)
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
                if any(name.startswith("[pops_api.dll]") for _, _, name, _ in resolved):
                    bucket = "Онлайн-сервисы POPS (pops_api.dll)"
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
    parser.add_argument("--top", type=int, default=30)
    args = parser.parse_args()
    result = analyze(args.profile.resolve(), args.all_threads)
    write_outputs(args.profile.resolve(), result)
    other = analyze(args.diff.resolve(), args.all_threads) if args.diff else None
    print_summary(result, other, args.top)
    print(f"\nФайлы: inclusive_functions.csv, subsystems.csv, crosscut.csv, folded.txt в {args.profile}")


if __name__ == "__main__":
    main()
