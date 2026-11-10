"""Build the standalone EU4 CPU sampler with the workspace-local Zig compiler."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
ZIG = WORKSPACE / ".tools" / "zig-windows-x86_64-0.13.0" / "zig.exe"
BUILD = ROOT / "build"


def run(arguments: list[object]) -> None:
    print("+", " ".join(str(value) for value in arguments), flush=True)
    subprocess.run([str(value) for value in arguments], check=True, cwd=WORKSPACE)


def main() -> None:
    if not ZIG.exists():
        raise SystemExit(f"Local Zig compiler not found: {ZIG}")
    BUILD.mkdir(exist_ok=True)
    common: list[object] = [ZIG, "c++", "-target", "x86_64-windows-gnu", "-std=c++17",
                            "-O2", "-Wall", "-Wextra", "-Werror"]
    run(common + ["-static", "-municode", ROOT / "src" / "eu4_sampler.cpp",
                  "-o", BUILD / "eu4_sampler.exe", "-ldbghelp"])
    run(common + ["-static", ROOT / "tests" / "sampler_target.cpp",
                  "-o", BUILD / "sampler_target.exe"])
    shutil.copy2(BUILD / "eu4_sampler.exe", ROOT / "eu4_sampler.exe")
    print(f"Built {ROOT / 'eu4_sampler.exe'}")


if __name__ == "__main__":
    main()
