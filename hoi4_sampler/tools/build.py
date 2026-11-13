"""Build the standalone HOI4 CPU sampler with the Zig compiler."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZIG = Path(os.environ.get(
    "ZIG", r"C:\Project europe\.tools\zig-windows-x86_64-0.13.0\zig.exe"))
BUILD = ROOT / "build"


def run(arguments: list[object]) -> None:
    print("+", " ".join(str(value) for value in arguments), flush=True)
    subprocess.run([str(value) for value in arguments], check=True, cwd=ROOT)


def main() -> None:
    if not ZIG.exists():
        raise SystemExit(f"Zig compiler not found: {ZIG}")
    BUILD.mkdir(exist_ok=True)
    common: list[object] = [ZIG, "c++", "-target", "x86_64-windows-gnu", "-std=c++17",
                            "-O2", "-Wall", "-Wextra", "-Werror"]
    run(common + ["-static", "-municode", ROOT / "src" / "hoi4_sampler.cpp",
                  "-o", BUILD / "hoi4_sampler.exe", "-ldbghelp", "-lwinmm"])
    shutil.copy2(BUILD / "hoi4_sampler.exe", ROOT / "hoi4_sampler.exe")
    print(f"Built {ROOT / 'hoi4_sampler.exe'}")


if __name__ == "__main__":
    main()
