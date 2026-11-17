#!/usr/bin/env python3
"""Сборка плагина Stellaris: build/<имя>.dll из <папка>/src/<имя>.cpp.

Использование:
    python stellaris_common/build_plugin.py stellaris_hello [--install]

Папка игры для --install: переменная STELLARIS_DIR, иначе C:\\Stellaris\\Stellaris.
--install копирует DLL в <игра>\\plugins\\ и ini (если его там ещё нет).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[1]
ZIG = Path(os.environ.get("ZIG", WORKSPACE / ".tools" / "zig-windows-x86_64-0.13.0" / "zig.exe"))
GAME_DIR = Path(os.environ.get("STELLARIS_DIR", r"C:\Stellaris\Stellaris"))


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        raise SystemExit(__doc__)
    root = (WORKSPACE / args[0]).resolve()
    name = root.name
    sources = sorted((root / "src").glob("*.cpp"))
    if not sources:
        raise SystemExit(f"нет исходников в {root / 'src'}")
    build = root / "build"
    build.mkdir(exist_ok=True)
    out = build / f"{name}.dll"
    command = [str(ZIG), "c++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O2",
               "-Wall", "-Wextra", "-Werror", "-shared", "-static",
               *map(str, sources),
               "-I", str(root / "src"), "-I", str(WORKSPACE / "stellaris_common"),
               "-o", str(out)]
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True, cwd=str(WORKSPACE))
    print("built:", out)
    if "--install" in sys.argv:
        plugins = GAME_DIR / "plugins"
        plugins.mkdir(exist_ok=True)
        shutil.copy2(out, plugins / out.name)
        ini = root / f"{name}.ini"
        if ini.exists() and not (plugins / ini.name).exists():
            shutil.copy2(ini, plugins / ini.name)
        print("installed into", plugins)


if __name__ == "__main__":
    main()
