"""Бенчмарк скорости EU4: миллисекунды на игровой день при разных наборах плагинов.

Источник времени — встроенный в игру таймер: запуск с параметром -gamestatetimer пишет
logs/gametimer_ГГГГММДД_ччммсс.tsv (eu4/gamestatetimer.cpp, 140daa4e0; строки пишет
140daa950 из дневного тика 140758430). Строка «Day» — реальное время между двумя соседними
дневными тиками, то есть вместе с отрисовкой кадров. Паузы видны как очень длинные дни
и отбрасываются порогом --pause-ms.

Каждый прогон стартует с одного эталонного сохранения: оно копируется в
«save games/eu4_bench_run.eu4», а continue_game.json временно указывает на эту копию.
Игра запускается с -continuelastsave, поэтому сразу загружает его.

Типичный цикл:
    python eu4_bench/bench.py run vanilla --save "C:/…/save games/Observer.eu4"
        (в игре: снять паузу, скорость 5, подождать нужное число игровых лет, выйти из игры)
    python eu4_bench/bench.py report
    python eu4_bench/bench.py run all --save …
    python eu4_bench/bench.py report
    python eu4_bench/bench.py compare vanilla all

Остальное: presets, apply ПРЕСЕТ, modes, restore, parse ФАЙЛ.tsv.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parent
GAME = WORKSPACE / "Europa Universalis IV"
PLUGINS = GAME / "plugins"
RESULTS = ROOT / "results"
STATE = ROOT / "state"
RUN_SAVE_NAME = "eu4_bench_run.eu4"

# Плагин -> (ini, {ключ пресета: (секция, параметр)}).
PLUGIN_KEYS = {
    "speed": ("eu4_speed_patch.ini", "speed", "mode", {"balanced", "profile", "disabled"}),
    "knowledge": ("eu4_knowledge_patch.ini", "knowledge", "mode",
                  {"parallel_shadow", "parallel", "optimized", "shadow", "disabled"}),
    "scheduler": ("eu4_knowledge_patch.ini", "scheduler", "wait", {"efficient", "disabled"}),
    "modifier": ("eu4_modifier_patch.ini", "modifier", "mode",
                 {"baseline", "profile", "optimize", "disabled"}),
    "trigger": ("eu4_trigger_patch.ini", "trigger", "mode",
                {"parallel_shadow", "shadow", "disabled"}),
    "ai": ("eu4_ai_serial.ini", "ai", "mode", {"serial", "timing", "tasks", "disabled"}),
    "flagcache": ("eu4_ai_serial.ini", "ai", "unit_flag_cache", {"off", "shadow", "on"}),
    "memo": ("eu4_ai_serial.ini", "ai", "search_memo", {"off", "shadow", "on"}),
    "ratiocache": ("eu4_ai_serial.ini", "ai", "country_ratio_cache", {"off", "shadow", "on"}),
    "scorecache": ("eu4_ai_serial.ini", "ai", "province_score_cache", {"off", "shadow", "on"}),
    "aiheavy": ("eu4_ai_serial.ini", "ai", "sample_heavy", {"0", "1"}),
    "monthsample": ("eu4_ai_serial.ini", "ai", "sample_monthly", {"0", "1", "2"}),
    "relpf": ("eu4_country_patch.ini", "country", "relpf", {"off", "on"}),
    "pmpf": ("eu4_country_patch.ini", "country", "pmpf", {"off", "on"}),
    "merc": ("eu4_country_patch.ini", "country", "merc", {"off", "shadow", "on"}),
    "hist": ("eu4_country_patch.ini", "country", "hist", {"off", "shadow", "on"}),
    "maint": ("eu4_country_patch.ini", "country", "maint", {"off", "shadow", "on"}),
    "provreuse": ("eu4_load_patch.ini", "load", "provreuse", {"off", "on", "verify", "verify0"}),
    "pathcache": ("eu4_ai_serial.ini", "ai", "path_cache", {"off", "shadow", "on"}),
    "treeprobe": ("eu4_ai_serial.ini", "ai", "tree_probe", {"0", "1"}),
    "treewalk": ("eu4_ai_serial.ini", "ai", "tree_walk", {"off", "serial", "probe", "parallel"}),
    "treeworkers": ("eu4_ai_serial.ini", "ai", "tree_workers", "number"),
    "render": ("eu4_render_patch.ini", "render", "mode", {"count", "throttle", "disabled"}),
    "renderint": ("eu4_render_patch.ini", "render", "tick_interval", "number"),
    "frameint": ("eu4_render_patch.ini", "render", "frame_interval", "number"),
    "saveformat": ("eu4_save_patch.ini", "save", "format", {"off", "count", "shadow", "on", "ab"}),
    "pipe": ("eu4_save_patch.ini", "save", "pipe", {"off", "verify", "on"}),
    "runint": ("eu4_render_patch.ini", "render", "running_frame_interval", "number"),
    "mapskip": ("eu4_render_patch.ini", "render", "skip_map_update", {"0", "1"}),
    "alertint": ("eu4_render_patch.ini", "render", "alert_interval", "number"),
    "iconint": ("eu4_render_patch.ini", "render", "map_icons_interval", "number"),
    "iconhide": ("eu4_render_patch.ini", "render", "map_icons_hide", {"", "build", "all"}),
    "texthide": ("eu4_render_patch.ini", "render", "map_icons_text_hide", {"", "build", "all"}),
    "govattr": ("eu4_gov_patch.ini", "gov", "attr_cache", {"off", "shadow", "on"}),
    "alloc": ("eu4_alloc_patch.ini", "alloc", "mode", {"off", "on"}),
    "allocmax": ("eu4_alloc_patch.ini", "alloc", "max_small", "number"),
    "allocdirect": ("eu4_alloc_patch.ini", "alloc", "direct", {"0", "1"}),
    "estates": ("eu4_country_patch.ini", "country", "estates", {"off", "shadow", "on"}),
    "strength": ("eu4_country_patch.ini", "country", "strength", {"off", "shadow", "on"}),
    "cworkers": ("eu4_country_patch.ini", "country", "workers", "number"),
    "colony": ("eu4_country_patch.ini", "country", "colony", {"off", "shadow", "on"}),
    "reforms": ("eu4_country_patch.ini", "country", "reforms", {"off", "shadow", "on"}),
    "cb": ("eu4_country_patch.ini", "country", "cb", {"off", "shadow", "on"}),
    "cbspin": ("eu4_country_patch.ini", "country", "cb_spin_us", "number"),
    "provmod": ("eu4_country_patch.ini", "country", "provmod", {"off", "shadow", "on"}),
    "pmserial": ("eu4_country_patch.ini", "country", "provmod_serial", "number"),
    "pathstat": ("eu4_country_patch.ini", "country", "pathstat", "number"),
    "pops": ("eu4_pops_patch.ini", "pops", "poll_sleep", {"off", "zero"}),
    "save": ("eu4_save_patch.ini", "save", "mode", {"off", "shadow", "on"}),
    "loc": ("eu4_loc_patch.ini", "loc", "mode", {"off", "shadow", "on"}),
    "blocks": ("eu4_load_patch.ini", "load", "blocks", {"off", "shadow", "on"}),
    "subst": ("eu4_load_patch.ini", "load", "subst", {"off", "shadow", "on"}),
    "exists": ("eu4_load_patch.ini", "load", "exists", {"off", "shadow", "on"}),
    "token": ("eu4_load_patch.ini", "load", "token", {"off", "shadow", "on"}),
    "verifypath": ("eu4_load_patch.ini", "load", "verifypath", {"off", "shadow", "on"}),
    "history": ("eu4_load_patch.ini", "load", "history", {"off", "shadow", "on"}),
    "transfer": ("eu4_load_patch.ini", "load", "transfer", {"off", "shadow", "on"}),
    "range": ("eu4_load_patch.ini", "load", "range", {"off", "shadow", "on"}),
    "port": ("eu4_load_patch.ini", "load", "port", {"off", "shadow", "on"}),
    "save": ("eu4_load_patch.ini", "load", "save", {"off", "shadow", "on"}),
    "savepar": ("eu4_load_patch.ini", "load", "save_parallel", {"off", "shadow", "on"}),
    "provview": ("eu4_load_patch.ini", "load", "provview", {"off", "shadow", "on"}),
    "provfree": ("eu4_load_patch.ini", "load", "provfree", {"off", "shadow", "on"}),
    "prefetch": ("eu4_load_patch.ini", "load", "prefetch", {"off", "shadow", "on"}),
    "warmup": ("eu4_load_patch.ini", "load", "warmup", {"off", "on"}),
    "merge": ("eu4_modifier_patch.ini", "modifier", "merge", {"off", "shadow", "on"}),
    "menu": ("eu4_menu_patch.ini", "menu", "mode", {"off", "on"}),
    "det": ("eu4_det_patch.ini", "det", "mode", {"off", "dump", "seed", "trace"}),
}

_OFF = {"speed": "disabled", "knowledge": "disabled", "scheduler": "disabled",
        "modifier": "disabled", "trigger": "disabled", "ai": "disabled",
        "flagcache": "off", "ratiocache": "off", "memo": "off",
        "render": "disabled", "renderint": "0.5", "frameint": "0", "mapskip": "0",
        "govattr": "off", "alloc": "off", "estates": "off", "strength": "off", "pops": "off", "scorecache": "off", "save": "off", "pathcache": "off", "loc": "off", "blocks": "off", "subst": "off", "exists": "off", "token": "off", "verifypath": "off",
        "history": "off", "transfer": "off", "range": "off", "port": "off", "save": "off", "savepar": "off", "provview": "off", "provfree": "off", "prefetch": "off", "warmup": "off", "merge": "off",
        "menu": "off"}
# Режимы shadow/profile добавляют измерения и для сравнения скорости не годятся.
PRESETS: dict[str, dict[str, str]] = {
    "vanilla": dict(_OFF),
    "speed": {**_OFF, "speed": "balanced"},
    "knowledge": {**_OFF, "knowledge": "parallel", "scheduler": "efficient"},
    "knowledge_optimized": {**_OFF, "knowledge": "optimized"},
    "modifier": {**_OFF, "modifier": "optimize"},
    "ai_serial": {**_OFF, "ai": "serial"},
    "all": {**_OFF, "speed": "balanced", "knowledge": "parallel", "scheduler": "efficient",
            "modifier": "optimize"},
}


# ---------------------------------------------------------------- пути пользователя

def documents_dir() -> Path:
    """Папка «Документы» с учётом перенаправления (OneDrive и т. п.)."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                        ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]
        # FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}
        guid = GUID(0xFDD39AD0, 0x238F, 0x46AF,
                    (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
        path = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(guid), 0, None,
                                                      ctypes.byref(path)) == 0:
            result = Path(path.value)
            ctypes.windll.ole32.CoTaskMemFree(path)
            return result
    return Path.home() / "Documents"


def user_dir() -> Path:
    custom = (GAME / "userdir.txt")
    if custom.exists() and custom.read_text(encoding="utf-8", errors="ignore").strip():
        return Path(custom.read_text(encoding="utf-8", errors="ignore").strip())
    return documents_dir() / "Paradox Interactive" / "Europa Universalis IV"


# ---------------------------------------------------------------- ini плагинов

def read_modes() -> dict[str, str | None]:
    modes: dict[str, str | None] = {}
    for key, (ini, section, option, _) in PLUGIN_KEYS.items():
        path = PLUGINS / ini
        dll = path.with_suffix(".dll")
        if not path.exists() or not dll.exists():
            modes[key] = None
            continue
        modes[key] = ini_get(path.read_text(encoding="utf-8"), section, option)
    return modes


def ini_get(text: str, section: str, option: str) -> str | None:
    current = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1].strip().lower()
        elif current == section and not stripped.startswith(";") and "=" in stripped:
            name, _, value = stripped.partition("=")
            if name.strip().lower() == option:
                return value.split(";")[0].strip()
    return None


def ini_set(text: str, section: str, option: str, value: str) -> str:
    """Меняет только значение параметра, сохраняя комментарии и порядок строк."""
    lines = text.splitlines(keepends=True)
    current = None
    section_end = None
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if current == section:
                section_end = index
            current = stripped[1:-1].strip().lower()
        elif current == section and not stripped.startswith(";") and "=" in stripped:
            name = stripped.partition("=")[0]
            if name.strip().lower() == option:
                newline = "\r\n" if line.endswith("\r\n") else "\n"
                lines[index] = f"{option}={value}{newline}"
                return "".join(lines)
    if current == section:
        section_end = len(lines)
    if section_end is None:
        return text + ("" if text.endswith("\n") or not text else "\n") + f"\n[{section}]\n{option}={value}\n"
    lines.insert(section_end, f"{option}={value}\n")
    return "".join(lines)


def apply_modes(wanted: dict[str, str]) -> None:
    STATE.mkdir(parents=True, exist_ok=True)
    backup = STATE / "ini_backup"
    if not backup.exists():  # первая копия — исходные настройки пользователя, для restore
        backup.mkdir()
        for ini, *_ in {value[0]: value for value in PLUGIN_KEYS.values()}.values():
            if (PLUGINS / ini).exists():
                shutil.copy2(PLUGINS / ini, backup / ini)
    for key, value in wanted.items():
        ini, section, option, allowed = PLUGIN_KEYS[key]
        if allowed == "number":  # интервалы в секундах: любое неотрицательное число
            try:
                ok = float(value) >= 0 or (key == "treeworkers" and int(value) == -1)
            except ValueError:
                ok = False
            if not ok:
                raise SystemExit(f"{key}: нужно неотрицательное число секунд, получено {value!r}")
        elif value not in allowed:
            raise SystemExit(f"{key}: недопустимый режим {value!r}, варианты: {sorted(allowed)}")
        path = PLUGINS / ini
        if not path.exists():
            if value != "disabled":
                raise SystemExit(f"Нет {path}: плагин {key} не установлен")
            continue
        text = path.read_text(encoding="utf-8")
        path.write_text(ini_set(text, section, option, value), encoding="utf-8", newline="")


def restore_modes() -> None:
    backup = STATE / "ini_backup"
    if not backup.exists():
        print("Резервной копии ini нет — восстанавливать нечего.")
        return
    for file in backup.iterdir():
        shutil.copy2(file, PLUGINS / file.name)
        print(f"Восстановлен {PLUGINS / file.name}")
    shutil.rmtree(backup)


# ---------------------------------------------------------------- разбор tsv

DATE = re.compile(r"^\s*(-?\d+)[._](\d+)[._](\d+)\s*$")  # в логе игры: 1659_01_03


def parse_date(text: str) -> tuple[int, int, int] | None:
    match = DATE.match(text)
    return tuple(int(part) for part in match.groups()) if match else None


def read_timer(path: Path) -> list[tuple[tuple[int, int, int], str, float]]:
    rows = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            parts = [part.strip() for part in line.rstrip("\n").split("\t")]
            if len(parts) != 3:
                continue
            date = parse_date(parts[0])
            if not date:
                continue  # заголовок «Game Date / Time Unit / Seconds»
            try:
                rows.append((date, parts[1], float(parts[2])))
            except ValueError:
                continue
    return rows


def date_text(date: tuple[int, int, int]) -> str:
    return f"{date[0]}.{date[1]}.{date[2]}"


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def summarize(path: Path, skip_days: int, max_days: int | None, pause_ms: float,
              date_from=None, date_to=None) -> dict:
    rows = read_timer(path)
    days = [(date, seconds * 1000.0) for date, unit, seconds in rows if unit == "Day"]
    if not days:
        raise SystemExit(f"В {path} нет строк Day — игра шла без -gamestatetimer или не снималась с паузы")
    first_date = days[0][0]
    days = days[skip_days:]
    if date_from:
        days = [day for day in days if day[0] >= date_from]
    if date_to:
        days = [day for day in days if day[0] <= date_to]
    if max_days:
        days = days[:max_days]
    paused = [day for day in days if day[1] > pause_ms]
    running = [day for day in days if day[1] <= pause_ms]
    if not running:
        raise SystemExit("После отсева пауз не осталось дней")
    values = [ms for _, ms in running]
    # Строка «Day» с датой N — это время с прошлой отметки, то есть работа тика N−1 и кадры
    # после него: месячный тик 1-го числа выполняется после записи отметки и попадает в строку 2-го.
    month_starts = [ms for date, ms in running if date[2] == 2]
    other_days = [ms for date, ms in running if date[2] != 2]
    median = statistics.median(values)
    # Выброс — необычно долгий день вне месячного тика (автосохранение, война, событие).
    spikes = [(date, ms) for date, ms in running if date[2] != 2 and ms > 5 * median]
    spike_dates = {date for date, _ in spikes}
    return {
        "file": str(path), "first_date": date_text(first_date),
        "from": date_text(running[0][0]), "to": date_text(running[-1][0]),
        "days": len(running), "paused_days_dropped": len(paused), "skip_days": skip_days,
        "pause_ms": pause_ms,
        "mean_ms": statistics.fmean(values), "median_ms": median,
        "p90_ms": percentile(values, 0.9), "p99_ms": percentile(values, 0.99),
        "stdev_ms": statistics.pstdev(values),
        "mean_without_spikes_ms": statistics.fmean([ms for date, ms in running if date not in spike_dates]),
        "month_start_mean_ms": statistics.fmean(month_starts) if month_starts else None,
        "other_day_mean_ms": statistics.fmean(other_days) if other_days else None,
        "spikes": [[date_text(date), round(ms, 1)] for date, ms in spikes[:50]],
        "paused": [[date_text(date), round(ms, 1)] for date, ms in paused[:50]],
        "game_years_per_hour": 3_600_000.0 / (statistics.fmean(values) * 365.0),
        "per_day": {date_text(date): round(ms, 3) for date, ms in running},
    }


def print_summary(label: str, summary: dict) -> None:
    print(f"[{label}] {summary['from']} — {summary['to']}: {summary['days']} дней "
          f"(пропущено на разгон {summary['skip_days']}, пауз отброшено {summary['paused_days_dropped']})")
    print(f"  среднее {summary['mean_ms']:.1f} мс/день, медиана {summary['median_ms']:.1f}, "
          f"p90 {summary['p90_ms']:.1f}, p99 {summary['p99_ms']:.1f}")
    print(f"  без выбросов (>5× медианы, кроме месячного тика) {summary['mean_without_spikes_ms']:.1f} мс/день; "
          f"выбросов {len(summary['spikes'])}")
    if summary["spikes"]:
        print("  выбросы (дата, мс): " + ", ".join(f"{d} {ms:.0f}" for d, ms in summary["spikes"][:8]))
    if summary["paused"]:
        print("  отброшены как пауза: " + ", ".join(f"{d} {ms / 1000:.0f} с" for d, ms in summary["paused"][:8]))
    if summary["month_start_mean_ms"] is not None:
        print(f"  дни с месячным тиком (строки 2-го числа) {summary['month_start_mean_ms']:.1f} мс, "
              f"остальные дни {summary['other_day_mean_ms']:.1f} мс")
    print(f"  ≈ {summary['game_years_per_hour']:.1f} игровых лет в час")


# ---------------------------------------------------------------- команды

def eu4_running() -> bool:
    if os.name != "nt":
        return False
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq eu4.exe", "/NH"],
                            capture_output=True, text=True).stdout
    return "eu4.exe" in output.lower()


def command_run(args) -> None:
    if args.preset not in PRESETS and args.preset != "current":
        raise SystemExit(f"Неизвестный пресет {args.preset!r}; есть: current, {', '.join(PRESETS)}")
    if eu4_running():
        raise SystemExit("eu4.exe уже запущен — закройте игру перед прогоном")
    save = Path(args.save).resolve() if args.save else None
    state_save = STATE / "reference_save.txt"
    if save is None and state_save.exists():
        save = Path(state_save.read_text(encoding="utf-8").strip())
    if save is None or not save.exists():
        raise SystemExit("Укажите эталонное сохранение: --save ПУТЬ.eu4 (запоминается для следующих прогонов)")
    STATE.mkdir(parents=True, exist_ok=True)
    state_save.write_text(str(save), encoding="utf-8")

    wanted = dict(PRESETS.get(args.preset, {}))
    for item in args.set or []:
        key, _, value = item.partition("=")
        if key not in PLUGIN_KEYS:
            raise SystemExit(f"--set {item}: неизвестный ключ, есть {sorted(PLUGIN_KEYS)}")
        wanted[key] = value
    apply_modes(wanted)
    modes = read_modes()

    udir = user_dir()
    saves = udir / "save games"
    shutil.copy2(save, saves / RUN_SAVE_NAME)
    continue_file = udir / "continue_game.json"
    if continue_file.exists() and not (STATE / "continue_game.json.bak").exists():
        shutil.copy2(continue_file, STATE / "continue_game.json.bak")
    continue_file.write_text(json.dumps({
        "title": f"eu4_bench {args.label or args.preset}", "desc": "",
        "date": time.ctime() + "\n", "filename": f"save games/{RUN_SAVE_NAME}",
    }, indent="\t"), encoding="utf-8")

    label = args.label or args.preset
    manifest = {"label": label, "preset": args.preset, "modes": modes, "save": str(save),
                "started": time.time(), "started_text": time.strftime("%Y-%m-%d %H:%M:%S"),
                "extra_args": args.extra or []}
    (STATE / "last_run.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    command = [str(GAME / "eu4.exe"), "-gamestatetimer", "-continuelastsave", *(args.extra or [])]
    print(f"Прогон «{label}», режимы плагинов: {modes}")
    print("Запуск: " + " ".join(command))
    if args.no_launch:
        print("Игра не запущена (--no-launch): запустите её сами, например через Win+R, "
              "чтобы она не попала в Job с ограничением CPU.")
    else:
        launch_detached(command)
    print("\nВ игре: снимите паузу, поставьте скорость 5 и подождите нужный срок "
          "(для сравнения — хотя бы 1–2 игровых года). Затем выйдите из игры и выполните:\n"
          "  python eu4_bench/bench.py report")


def launch_detached(command: list[str]) -> None:
    """Запуск игры через Проводник (ярлык .lnk), а не дочерним процессом.

    Дочерний процесс наследует job object вызывающей оболочки. Из песочницы Claude Code
    главный поток игры получал лишь ~20% CPU при почти пустой системе (проверено 2026-11-06:
    67 мс/день, из них ~13 мс CPU). Проводник запускает ярлык от своего имени.
    """
    if os.name != "nt":
        subprocess.Popen(command, cwd=GAME)
        return
    STATE.mkdir(parents=True, exist_ok=True)
    link = STATE / "eu4_bench_launch.lnk"
    arguments = subprocess.list2cmdline(command[1:])
    script = (
        "$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:EU4B_LNK); "
        "$s.TargetPath = $env:EU4B_EXE; $s.Arguments = $env:EU4B_ARGS; "
        "$s.WorkingDirectory = $env:EU4B_DIR; $s.Save()")
    environment = {**os.environ, "EU4B_LNK": str(link), "EU4B_EXE": command[0],
                   "EU4B_ARGS": arguments, "EU4B_DIR": str(GAME)}
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                   check=True, env=environment)
    subprocess.run(["explorer.exe", str(link)])  # explorer возвращает 1 даже при успехе


def latest_timer_file(after: float | None) -> Path:
    logs = user_dir() / "logs"
    files = sorted(logs.glob("gametimer_*.tsv"), key=lambda path: path.stat().st_mtime)
    if after is not None:
        files = [path for path in files if path.stat().st_mtime >= after]
    if not files:
        raise SystemExit(f"В {logs} нет нового gametimer_*.tsv — игра запускалась с -gamestatetimer?")
    return files[-1]


def command_report(args) -> None:
    manifest_path = STATE / "last_run.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else None
    if args.file:
        path = Path(args.file)
    else:
        path = latest_timer_file(manifest["started"] if manifest else None)
    label = args.label or (manifest["label"] if manifest else path.stem)
    summary = summarize(path, args.skip_days, args.days, args.pause_ms)
    summary["label"] = label
    summary["manifest"] = manifest
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    shutil.copy2(path, RESULTS / f"{label}_{stamp}.tsv")
    (RESULTS / f"{label}_{stamp}.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1),
                                                   encoding="utf-8")
    print_summary(label, summary)
    print(f"\nСохранено: {RESULTS / f'{label}_{stamp}.json'}")
    # continue_game.json возвращается к исходному, чтобы «Продолжить» в меню вёл туда же, что и раньше.
    backup = STATE / "continue_game.json.bak"
    if backup.exists() and not args.keep_continue:
        shutil.copy2(backup, user_dir() / "continue_game.json")
        backup.unlink()


def load_result(name: str) -> dict:
    path = Path(name)
    if path.suffix == ".json" and path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    candidates = sorted(RESULTS.glob(f"{name}_*.json"))
    if not candidates:
        raise SystemExit(f"Нет результатов для «{name}» в {RESULTS}")
    return json.loads(candidates[-1].read_text(encoding="utf-8"))


def command_compare(args) -> None:
    base = load_result(args.base)
    other = load_result(args.other)
    common = sorted(set(base["per_day"]) & set(other["per_day"]),
                    key=lambda text: tuple(int(part) for part in text.split(".")))
    if not common:
        raise SystemExit("У прогонов нет общих игровых дат — они начинались с разных сохранений?")
    a = [base["per_day"][day] for day in common]
    b = [other["per_day"][day] for day in common]
    mean_a, mean_b = statistics.fmean(a), statistics.fmean(b)
    ratios = [y / x for x, y in zip(a, b) if x > 0]
    # Блочный бутстрэп по 30 дней: соседние дни коррелированы (месячный тик, войны).
    import random
    rng = random.Random(12345)
    block = 30
    blocks = [list(range(start, min(start + block, len(common)))) for start in range(0, len(common), block)]
    estimates = []
    for _ in range(2000):
        chosen = [index for _ in blocks for index in rng.choice(blocks)]
        sa = sum(a[index] for index in chosen)
        sb = sum(b[index] for index in chosen)
        estimates.append(sb / sa)
    estimates.sort()
    low, high = estimates[int(0.025 * len(estimates))], estimates[int(0.975 * len(estimates)) - 1]
    print(f"Общие даты: {common[0]} — {common[-1]}, {len(common)} дней")
    print(f"  {base['label']:>14}: {mean_a:8.1f} мс/день   (режимы {base.get('manifest', {}) and base['manifest'].get('modes')})")
    print(f"  {other['label']:>14}: {mean_b:8.1f} мс/день   (режимы {other.get('manifest', {}) and other['manifest'].get('modes')})")
    change = (mean_b / mean_a - 1.0) * 100.0
    print(f"  время дня: {change:+.1f}% (95% интервал {100 * (low - 1):+.1f}% … {100 * (high - 1):+.1f}%), "
          f"медиана попарных отношений {statistics.median(ratios):.3f}")
    print(f"  скорость: ×{mean_a / mean_b:.3f}")
    if low <= 1.0 <= high:
        print("  Разница в пределах шума: нужен более длинный прогон или повтор.")
    note = "  Внимание: после расхождения ИИ состояние мира различается; сравнение честнее на первых 1–2 годах."
    print(note)


def command_parse(args) -> None:
    summary = summarize(Path(args.file), args.skip_days, args.days, args.pause_ms)
    print_summary(Path(args.file).stem, summary)


def command_profile(args) -> None:
    """Снять профиль со стеками с запущенной игры и разложить его по подсистемам."""
    if not eu4_running():
        raise SystemExit("eu4.exe не запущен: загрузите сохранение, снимите паузу, скорость 5")
    manifest_path = STATE / "last_run.json"
    label = args.label or (json.loads(manifest_path.read_text(encoding="utf-8"))["label"]
                           if manifest_path.exists() else "manual")
    kind = "wall" if args.wall else "profile"
    output = RESULTS / f"{label}_{kind}_{time.strftime('%Y%m%d_%H%M%S')}"
    sampler = WORKSPACE / "eu4_sampler" / "eu4_sampler.exe"
    subprocess.run([str(sampler), "--seconds", str(args.seconds), "--interval-ms", str(args.interval_ms),
                    "--output", str(output), *(["--wall"] if args.wall else [])], check=True)
    (output / "modes.json").write_text(json.dumps(read_modes(), indent=1), encoding="utf-8")
    command = [sys.executable, str(WORKSPACE / "eu4_sampler" / "tools" / "analyze_stacks.py"), str(output)]
    if args.diff:
        command += ["--diff", args.diff]
    subprocess.run(command, check=True)


def command_presets(_args) -> None:
    for name, modes in PRESETS.items():
        print(f"{name:>20}: " + ", ".join(f"{key}={value}" for key, value in modes.items()))
    print(f"{'current':>20}: ini не меняются")


def main() -> None:
    if not sys.stdout.isatty():
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="применить пресет и запустить игру с эталонным сохранением")
    run.add_argument("preset")
    run.add_argument("--save", help="эталонное сохранение .eu4 (запоминается)")
    run.add_argument("--label", help="имя прогона в results (по умолчанию — имя пресета)")
    run.add_argument("--set", action="append", metavar="КЛЮЧ=РЕЖИМ",
                     help="переопределить режим, например --set knowledge=optimized")
    run.add_argument("--no-launch", action="store_true",
                     help="только подготовить ini и сейв, игру запустить вручную")
    run.add_argument("--extra", nargs=argparse.REMAINDER, help="доп. параметры eu4.exe")
    run.set_defaults(func=command_run)

    for name, func, help_text in (("report", command_report, "разобрать последний gametimer_*.tsv"),
                                  ("parse", command_parse, "разобрать указанный tsv без сохранения")):
        item = sub.add_parser(name, help=help_text)
        if name == "parse":
            item.add_argument("file")
        else:
            item.add_argument("--file", help="конкретный tsv вместо последнего")
            item.add_argument("--label")
            item.add_argument("--keep-continue", action="store_true",
                              help="не возвращать исходный continue_game.json")
        item.add_argument("--skip-days", type=int, default=30, help="дней разгона после загрузки (30)")
        item.add_argument("--days", type=int, help="взять не больше N дней")
        item.add_argument("--pause-ms", type=float, default=15000.0,
                          help="день длиннее этого считается паузой и отбрасывается (15000)")
        item.set_defaults(func=func)

    compare = sub.add_parser("compare", help="сравнить два прогона по общим датам")
    compare.add_argument("base")
    compare.add_argument("other")
    compare.set_defaults(func=command_compare)

    profile = sub.add_parser("profile", help="профиль со стеками с запущенной игры (eu4_sampler)")
    profile.add_argument("--seconds", type=int, default=60)
    profile.add_argument("--interval-ms", type=int, default=5)
    profile.add_argument("--label")
    profile.add_argument("--wall", action="store_true",
                         help="главный поток по реальному времени, включая ожидания (иначе — по CPU)")
    profile.add_argument("--diff", help="папка другого профиля для сравнения долей подсистем")
    profile.set_defaults(func=command_profile)

    sub.add_parser("presets", help="список пресетов").set_defaults(func=command_presets)
    apply = sub.add_parser("apply", help="только записать режимы в ini, без запуска")
    apply.add_argument("preset")
    apply.set_defaults(func=lambda a: (apply_modes(PRESETS[a.preset]), print(read_modes())))
    sub.add_parser("modes", help="текущие режимы плагинов").set_defaults(func=lambda a: print(read_modes()))
    sub.add_parser("restore", help="вернуть ini плагинов к состоянию до первого прогона") \
        .set_defaults(func=lambda a: restore_modes())

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
