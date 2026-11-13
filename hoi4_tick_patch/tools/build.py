"""Build HOI4 Tick Patch with the EU4 workspace Zig compiler and install it."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
ZIG = Path(r"C:\Project europe\.tools\zig-windows-x86_64-0.13.0\zig.exe")
BUILD = ROOT / "build"
PLUGINS = WORKSPACE / "Hearts of Iron IV" / "plugins"


def main() -> None:
    BUILD.mkdir(exist_ok=True)
    command = [str(ZIG), "c++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O2", "-Wall",
               "-Wextra", "-Werror", "-shared", "-static",
               str(ROOT / "src" / "hoi4_tick_patch.cpp"),
               "-I", str(ROOT / "src"), "-I", str(WORKSPACE / "hoi4_common"), "-o", str(BUILD / "hoi4_tick_patch.dll")]
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True, cwd=str(WORKSPACE))
    if "--install" in sys.argv:
        PLUGINS.mkdir(exist_ok=True)
        shutil.copy2(BUILD / "hoi4_tick_patch.dll", PLUGINS / "hoi4_tick_patch.dll")
        if not (PLUGINS / "hoi4_tick_patch.ini").exists():
            shutil.copy2(ROOT / "hoi4_tick_patch.ini", PLUGINS / "hoi4_tick_patch.ini")
        print(f"Installed into {PLUGINS}")
    else:
        print(f"Built {BUILD / 'hoi4_tick_patch.dll'} (use --install to deploy)")


if __name__ == "__main__":
    main()
