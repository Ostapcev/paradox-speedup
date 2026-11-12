"""Build EU4 Load Patch with the workspace-local Zig compiler and install it."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
ZIG = WORKSPACE / ".tools" / "zig-windows-x86_64-0.13.0" / "zig.exe"
BUILD = ROOT / "build"
PLUGINS = WORKSPACE / "Europa Universalis IV" / "plugins"


def main() -> None:
    BUILD.mkdir(exist_ok=True)
    command = [ZIG, "c++", "-target", "x86_64-windows-gnu", "-std=c++20", "-O2", "-Wall",
               "-Wextra", "-Werror", "-shared", "-static",
               ROOT / "src" / "eu4_load_patch.cpp", ROOT / "src" / "call_patch.cpp",
               ROOT / "src" / "bellows" / "shim.cpp", ROOT / "src" / "bellows" / "ram_prefetch.cpp",
               ROOT / "src" / "bellows" / "open_warmup.cpp", ROOT / "src" / "bellows" / "access_profile.cpp",
               ROOT / "src" / "bellows" / "zip_reader.cpp",
               "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "eu4_load_patch.dll", "-lole32", "-lshell32"]
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run([str(value) for value in command], check=True, cwd=WORKSPACE)
    shutil.copy2(BUILD / "eu4_load_patch.dll", PLUGINS / "eu4_load_patch.dll")
    if not (PLUGINS / "eu4_load_patch.ini").exists():
        shutil.copy2(ROOT / "eu4_load_patch.ini", PLUGINS / "eu4_load_patch.ini")
    print(f"Installed into {PLUGINS}")


if __name__ == "__main__":
    main()
