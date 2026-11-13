#!/usr/bin/env python3
"""Сборка VERSION.dll (прокси-загрузчик плагинов HOI4).

Использование: python tools/build.py [--install]
Папка игры для --install: переменная HOI4_DIR, иначе ../Hearts of Iron IV рядом с репозиторием.
Без --install только собирает build/VERSION.dll.
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIG = os.environ.get(
    "ZIG",
    r"C:\Project europe\.tools\zig-windows-x86_64-0.13.0\zig.exe",
)
GAME_DIR = os.environ.get("HOI4_DIR", os.path.join(os.path.dirname(ROOT), "Hearts of Iron IV"))

def main():
    install = "--install" in sys.argv
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    out = os.path.join(ROOT, "build", "VERSION.dll")
    cmd = [
        ZIG, "cc",
        "-target", "x86_64-windows-gnu",
        "-shared", "-O2",
        "-o", out,
        os.path.join(ROOT, "src", "version.c"),
    ]
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=ROOT)
    print("built:", out)
    if install:
        os.makedirs(os.path.join(GAME_DIR, "plugins"), exist_ok=True)
        dst = os.path.join(GAME_DIR, "VERSION.dll")
        if os.path.exists(dst):
            print("ERROR: VERSION.dll уже существует в папке игры:", dst)
            sys.exit(1)
        shutil.copy2(out, dst)
        print("installed:", dst)

if __name__ == "__main__":
    main()
