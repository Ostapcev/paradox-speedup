"""Build and test the read-only EU4 modifier merge profiler with local Zig."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
ZIG = WORKSPACE / ".tools" / "zig-windows-x86_64-0.13.0" / "zig.exe"
BUILD = ROOT / "build"
DIST = ROOT / "dist"


def run(arguments: list[object]) -> None:
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
                  ROOT / "src" / "eu4_modifier_patch.cpp",
                  ROOT / "src" / "caller_stats.cpp",
                  ROOT / "src" / "function_patch.cpp",
                  ROOT / "src" / "function_stats.cpp",
                  ROOT / "src" / "measurement.cpp",
                  ROOT / "src" / "profile.cpp",
                  ROOT / "src" / "search.cpp",
                  ROOT / "src" / "search_relay.S",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "eu4_modifier_patch.dll", "-lbcrypt"])
    run(common + ["-static", "-municode", ROOT / "tools" / "check_executable.cpp",
                  ROOT / "src" / "profile.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "modifier_patch_check.exe", "-lbcrypt"])
    run(common + ["-static", ROOT / "tests" / "function_patch_tests.cpp",
                  ROOT / "src" / "function_patch.cpp", ROOT / "tests" / "function_fixture.S",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "function_patch_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "measurement_tests.cpp",
                  ROOT / "src" / "measurement.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "measurement_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "caller_stats_tests.cpp",
                  ROOT / "src" / "caller_stats.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "caller_stats_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "function_stats_tests.cpp",
                  ROOT / "src" / "function_stats.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "function_stats_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "search_tests.cpp",
                  ROOT / "src" / "function_patch.cpp", ROOT / "src" / "search.cpp",
                  ROOT / "src" / "search_relay.S", ROOT / "tests" / "search_fixture.S",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "search_tests.exe"])

    for name in ("function_patch_tests.exe", "measurement_tests.exe", "caller_stats_tests.exe",
                 "function_stats_tests.exe", "search_tests.exe"): 
        run([BUILD / name])
    run([sys.executable, ROOT / "tests" / "lightweight_path_tests.py"])
    # the offline check needs the Steam exe (the test copy may hold the GOG build)
    run([BUILD / "modifier_patch_check.exe", WORKSPACE / "eu4_kit" / "binary" / "eu4.exe"])

    for name in ("eu4_modifier_patch.dll", "eu4_modifier_patch.ini"):
        source = BUILD / name if name.endswith(".dll") else ROOT / name
        shutil.copy2(source, DIST / "plugins" / name)
    print(f"Built and verified {DIST / 'plugins' / 'eu4_modifier_patch.dll'}")


if __name__ == "__main__":
    main()
