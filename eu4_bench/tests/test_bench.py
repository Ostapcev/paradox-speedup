"""Проверки bench.py без запуска игры: ini-правки на копиях, разбор tsv, сравнение прогонов.

Запуск: python eu4_bench/tests/test_bench.py
"""
from __future__ import annotations

import io
import json
import shutil
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bench  # noqa: E402

DAYS_IN_MONTH = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]


def dates(start_year: int, count: int):
    year, month, day = start_year, 1, 1
    for _ in range(count):
        yield year, month, day
        day += 1
        if day > DAYS_IN_MONTH[month - 1]:
            day, month = 1, month + 1
            if month > 12:
                month, year = 1, year + 1


def write_tsv(path: Path, base_ms: float, count: int) -> None:
    lines = ["Game Date \tTime Unit \tSeconds\n"]
    for index, (y, m, d) in enumerate(dates(1659, count)):
        ms = base_ms * (3.0 if d == 2 else 1.0) * (1.0 + 0.05 * ((index * 7919) % 11 - 5) / 5)
        if index == 100:
            ms = 60_000.0  # пауза
        if index == 200:
            ms = base_ms * 40  # автосохранение
        lines.append(f"{y}.{m}.{d}\tMicro\t0.000100\n")
        lines.append(f"{y}.{m}.{d}\tDay\t{ms / 1000:.6f}\n")
        if d == 1:
            lines.append(f"{y}.{m}.{d}\tMonth\t{ms * 30 / 1000:.6f}\n")
    path.write_text("".join(lines), encoding="utf-8")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit("FAIL: " + message)


def test_ini(tmp: Path) -> None:
    plugins = tmp / "plugins"
    plugins.mkdir()
    for ini in {value[0] for value in bench.PLUGIN_KEYS.values()}:
        shutil.copy2(bench.PLUGINS / ini, plugins / ini)
        (plugins / ini).with_suffix(".dll").write_bytes(b"")
    original = {ini.name: ini.read_text(encoding="utf-8") for ini in plugins.glob("*.ini")}
    bench.PLUGINS, bench.STATE = plugins, tmp / "state"
    bench.apply_modes(bench.PRESETS["all"])
    modes = bench.read_modes()
    check(modes == bench.PRESETS["all"], f"режимы после apply: {modes}")
    knowledge = (plugins / "eu4_knowledge_patch.ini").read_text(encoding="utf-8")
    check(knowledge.count("; parallel_shadow:") == 1, "комментарии ini должны сохраниться")
    bench.apply_modes(bench.PRESETS["vanilla"])
    check(all(value in ("disabled", "off") for key, value in bench.read_modes().items() if key not in ("renderint", "frameint", "mapskip")), "vanilla выключает всё")
    bench.restore_modes()
    restored = {ini.name: ini.read_text(encoding="utf-8") for ini in plugins.glob("*.ini")}
    check(restored == original, "restore должен вернуть ini байт в байт")
    check(bench.ini_set("[a]\nx=1\n", "b", "y", "2").endswith("[b]\ny=2\n"), "новая секция")
    check(bench.ini_set("[a]\nx=1\n[b]\n", "a", "z", "3") == "[a]\nx=1\nz=3\n[b]\n", "новый параметр в секции")
    print("PASS ini")


def test_parse_and_compare(tmp: Path) -> None:
    base, fast = tmp / "base.tsv", tmp / "fast.tsv"
    write_tsv(base, 100.0, 730)
    write_tsv(fast, 90.0, 730)
    summary = bench.summarize(base, 30, None, 15000.0)
    check(summary["days"] == 699, f"дней {summary['days']}")  # 730 − 30 разгон − 1 пауза
    check(summary["paused_days_dropped"] == 1, "пауза должна отброситься")
    check(len(summary["spikes"]) == 1 and summary["spikes"][0][0] == "1659.7.20", f"выброс {summary['spikes']}")
    check(abs(summary["median_ms"] - 100.0) < 6, f"медиана {summary['median_ms']}")
    check(summary["month_start_mean_ms"] > 2.5 * summary["other_day_mean_ms"], "1-е числа дороже")
    results = tmp / "results"
    results.mkdir()
    bench.RESULTS = results
    for label, path in (("base", base), ("fast", fast)):
        data = bench.summarize(path, 30, None, 15000.0)
        data["label"] = label
        (results / f"{label}_1.json").write_text(json.dumps(data), encoding="utf-8")
    buffer = io.StringIO()
    with redirect_stdout(buffer):
        bench.command_compare(type("A", (), {"base": "base", "other": "fast"})())
    text = buffer.getvalue()
    check("-10.0%" in text, "ожидалось −10.0%:\n" + text)
    check("×1.111" in text, "ожидалось ×1.111:\n" + text)
    print("PASS parse/compare")
    print(text)


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    check(bench.user_dir().name == "Europa Universalis IV", f"user_dir {bench.user_dir()}")
    with tempfile.TemporaryDirectory() as name:
        tmp = Path(name)
        test_ini(tmp)
        test_parse_and_compare(tmp)
    print("ALL PASS")


if __name__ == "__main__":
    main()
