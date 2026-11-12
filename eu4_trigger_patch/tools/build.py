"""Build and test the EU4 diplomatic trigger shadow prototype."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
KNOWLEDGE = WORKSPACE / "eu4_knowledge_patch"
SPEED = WORKSPACE / "eu4_speed_patch"
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
                  ROOT / "src" / "eu4_trigger_patch.cpp",
                  ROOT / "src" / "trigger_batch.cpp",
                  ROOT / "src" / "trigger_hooks.S",
                  ROOT / "src" / "parallel_pool.cpp",
                  ROOT / "src" / "parallel_patch.cpp",
                  ROOT / "src" / "profile.cpp",
                  SPEED / "src" / "call_patch.cpp",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-I", SPEED / "src",
                  "-o", BUILD / "eu4_trigger_patch.dll"])
    run(common + ["-static", "-municode", ROOT / "tools" / "check_executable.cpp",
                  ROOT / "src" / "profile.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "trigger_patch_check.exe"])
    run(common + ["-static", ROOT / "tests" / "trigger_batch_tests.cpp",
                  ROOT / "src" / "trigger_batch.cpp", ROOT / "src" / "parallel_pool.cpp",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "trigger_batch_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "trigger_hook_tests.cpp",
                  ROOT / "src" / "trigger_hooks.S", ROOT / "src" / "parallel_patch.cpp",
                  ROOT / "tests" / "entry_fixture.S", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "trigger_hook_tests.exe"])
    run(common + ["-static", KNOWLEDGE / "tests" / "parallel_pool_tests.cpp",
                  ROOT / "src" / "parallel_pool.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "parallel_pool_tests.exe"])
    run(common + ["-static", KNOWLEDGE / "tests" / "parallel_loop_tests.cpp",
                  KNOWLEDGE / "src" / "parallel_loops.S", ROOT / "src" / "parallel_patch.cpp",
                  KNOWLEDGE / "tests" / "parallel_fixture.S", "-I", KNOWLEDGE / "src",
                  "-o", BUILD / "parallel_loop_tests.exe"])
    run(common + ["-static", SPEED / "tests" / "hook_tests.cpp",
                  SPEED / "src" / "call_patch.cpp", "-I", SPEED / "src",
                  "-o", BUILD / "call_patch_tests.exe"])

    for name in ("trigger_batch_tests.exe", "trigger_hook_tests.exe",
                 "parallel_pool_tests.exe", "parallel_loop_tests.exe", "call_patch_tests.exe"):
        run([BUILD / name])
    run([BUILD / "trigger_patch_check.exe", WORKSPACE / "Europa Universalis IV" / "eu4.exe"])

    for name in ("eu4_trigger_patch.dll", "eu4_trigger_patch.ini"):
        source = BUILD / name if name.endswith(".dll") else ROOT / name
        shutil.copy2(source, DIST / "plugins" / name)
    print(f"Built and verified {DIST / 'plugins' / 'eu4_trigger_patch.dll'}")


if __name__ == "__main__":
    main()
