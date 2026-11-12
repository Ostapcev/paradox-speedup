"""Build and test the EU4 RefreshKnowledge shadow verifier with local Zig."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
SPEED_PATCH = WORKSPACE / "eu4_speed_patch"
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
                  ROOT / "src" / "eu4_knowledge_patch.cpp",
                  ROOT / "src" / "profile.cpp",
                  ROOT / "src" / "scheduler_wait.cpp",
                  ROOT / "src" / "shadow_hook.S",
                  ROOT / "src" / "optimized_loops.S",
                  ROOT / "src" / "optimized_loop_patch.cpp",
                  ROOT / "src" / "parallel_loops.S",
                  ROOT / "src" / "parallel_pool.cpp",
                  ROOT / "src" / "parallel_patch.cpp",
                  ROOT / "src" / "scheduler_wait.S",
                  SPEED_PATCH / "src" / "call_patch.cpp",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-I", SPEED_PATCH / "src",
                  "-o", BUILD / "eu4_knowledge_patch.dll", "-lbcrypt"])
    run(common + ["-static", "-municode", ROOT / "tools" / "check_executable.cpp",
                  ROOT / "src" / "profile.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "knowledge_patch_check.exe", "-lbcrypt"])
    run(common + ["-static", ROOT / "tests" / "equivalence_tests.cpp",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "equivalence_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "two_phase_tests.cpp",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "two_phase_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "parallel_pool_tests.cpp",
                  ROOT / "src" / "parallel_pool.cpp", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "parallel_pool_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "shadow_hook_tests.cpp",
                  ROOT / "src" / "shadow_hook.S", ROOT / "tests" / "wrapper_fixture.S",
                  "-o", BUILD / "shadow_hook_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "optimized_loop_tests.cpp",
                  ROOT / "src" / "optimized_loops.S",
                  ROOT / "src" / "optimized_loop_patch.cpp",
                  ROOT / "tests" / "optimized_fixture.S", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "optimized_loop_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "parallel_loop_tests.cpp",
                  ROOT / "src" / "parallel_loops.S", ROOT / "src" / "parallel_patch.cpp",
                  ROOT / "tests" / "parallel_fixture.S", "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include",
                  "-o", BUILD / "parallel_loop_tests.exe"])
    run(common + ["-static", ROOT / "tests" / "scheduler_wait_tests.cpp",
                  ROOT / "src" / "scheduler_wait.cpp", ROOT / "src" / "scheduler_wait.S",
                  ROOT / "src" / "parallel_patch.cpp", ROOT / "tests" / "scheduler_fixture.S",
                  "-I", ROOT / "src", "-I", WORKSPACE / "eu4_port" / "include", "-o", BUILD / "scheduler_wait_tests.exe"])
    run(common + ["-static", SPEED_PATCH / "tests" / "hook_tests.cpp",
                  SPEED_PATCH / "src" / "call_patch.cpp", "-I", SPEED_PATCH / "src",
                  "-o", BUILD / "call_patch_tests.exe"])

    for name in ("equivalence_tests.exe", "two_phase_tests.exe", "parallel_pool_tests.exe",
                 "shadow_hook_tests.exe", "optimized_loop_tests.exe",
                 "parallel_loop_tests.exe", "scheduler_wait_tests.exe", "call_patch_tests.exe"):
        run([BUILD / name])
    run([BUILD / "knowledge_patch_check.exe", WORKSPACE / "Europa Universalis IV" / "eu4.exe"])

    for name in ("eu4_knowledge_patch.dll", "eu4_knowledge_patch.ini"):
        source = BUILD / name if name.endswith(".dll") else ROOT / name
        shutil.copy2(source, DIST / "plugins" / name)
    print(f"Built and verified {DIST / 'plugins' / 'eu4_knowledge_patch.dll'}")


if __name__ == "__main__":
    main()
