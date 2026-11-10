"""Build EU4 Speed Patch with the workspace-local Zig compiler."""
from __future__ import annotations

import shutil
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
ZIG = WORKSPACE / ".tools" / "zig-windows-x86_64-0.13.0" / "zig.exe"
BUILD = ROOT / "build"
DIST = ROOT / "dist"
VERSION = "0.1.0-experimental"


def run(arguments: list[str]) -> None:
    print("+", " ".join(str(value) for value in arguments), flush=True)
    subprocess.run([str(value) for value in arguments], check=True, cwd=WORKSPACE)


def main() -> None:
    if not ZIG.exists():
        raise SystemExit(f"Local Zig compiler not found: {ZIG}")
    BUILD.mkdir(exist_ok=True)
    (DIST / "plugins").mkdir(parents=True, exist_ok=True)
    common = [ZIG, "c++", "-target", "x86_64-windows-gnu", "-std=c++17",
              "-O2", "-Wall", "-Wextra", "-Werror"]

    run(common + ["-shared", "-static",
                  ROOT / "src" / "eu4_speed_patch.cpp",
                  ROOT / "src" / "call_patch.cpp",
                  ROOT / "src" / "profile.cpp",
                  "-I", ROOT / "src", "-o", BUILD / "eu4_speed_patch.dll", "-lbcrypt"])
    run(common + ["-static", "-municode", ROOT / "tools" / "check_executable.cpp",
                  ROOT / "src" / "profile.cpp", "-I", ROOT / "src",
                  "-o", BUILD / "speed_patch_check.exe", "-lbcrypt"])
    run(common + ["-static", ROOT / "tests" / "hook_tests.cpp",
                  ROOT / "src" / "call_patch.cpp", "-I", ROOT / "src",
                  "-o", BUILD / "hook_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "load_tests.cpp",
                  "-o", BUILD / "load_tests.exe"])

    for name in ("eu4_speed_patch.dll",):
        shutil.copy2(BUILD / name, DIST / "plugins" / name)
    for name in ("eu4_speed_patch.ini", "README.md"):
        shutil.copy2(ROOT / name, DIST / "plugins" / name)

    package = DIST / f"EU4SpeedPatch-1.37.5-v{VERSION}.zip"
    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted((DIST / "plugins").iterdir()):
            archive.write(path, Path("plugins") / path.name)
    print(f"Built {package}")


if __name__ == "__main__":
    main()
