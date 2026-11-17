#!/usr/bin/env python3
"""Сборка VERSION.dll (прокси-загрузчик плагинов Stellaris).

Использование: python tools/build.py [--install] [--test]
Папка игры: переменная STELLARIS_DIR, иначе C:\\Stellaris\\Stellaris.
  --install  копирует VERSION.dll в папку игры и создаёт там plugins\\
  --test     собирает и запускает tests/proxy_test.c и ping-плагин
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIG = os.environ.get(
    "ZIG",
    r"C:\Project europe\.tools\zig-windows-x86_64-0.13.0\zig.exe",
)
GAME_DIR = os.environ.get("STELLARIS_DIR", r"C:\Stellaris\Stellaris")
BUILD = os.path.join(ROOT, "build")


def zig_cc(*args):
    cmd = [ZIG, "cc", "-target", "x86_64-windows-gnu", "-O2", *args]
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=ROOT)


def run_tests(dll):
    tdir = os.path.join(BUILD, "test")
    os.makedirs(os.path.join(tdir, "plugins"), exist_ok=True)
    shutil.copy2(dll, os.path.join(tdir, "VERSION.dll"))
    zig_cc("-shared", "-o", os.path.join(tdir, "plugins", "ping_plugin.dll"),
           os.path.join(ROOT, "tests", "ping_plugin.c"))
    exe = os.path.join(tdir, "proxy_test.exe")
    zig_cc("-o", exe, os.path.join(ROOT, "tests", "proxy_test.c"))
    log = os.path.join(tdir, "plugins", "_loader.log")
    if os.path.exists(log):
        os.remove(log)
    subprocess.check_call([exe], cwd=tdir)
    with open(log, encoding="utf-8", errors="replace") as f:
        text = f.read()
    print(text)
    if "OK    ping_plugin.dll" not in text:
        sys.exit("ERROR: ping_plugin.dll не загрузился")
    if not os.path.exists(os.path.join(tdir, "plugins", "ping_plugin.loaded")):
        sys.exit("ERROR: ping_plugin не отработал")
    print("LOADER TEST PASSED")


def main():
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "VERSION.dll")
    zig_cc("-shared", "-o", out, os.path.join(ROOT, "src", "version.c"))
    print("built:", out)
    if "--test" in sys.argv:
        run_tests(out)
    if "--install" in sys.argv:
        os.makedirs(os.path.join(GAME_DIR, "plugins"), exist_ok=True)
        dst = os.path.join(GAME_DIR, "VERSION.dll")
        if os.path.exists(dst):
            print("ERROR: VERSION.dll уже существует в папке игры:", dst)
            sys.exit(1)
        shutil.copy2(out, dst)
        print("installed:", dst)


if __name__ == "__main__":
    main()
