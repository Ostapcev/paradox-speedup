"""Exercise the sampler against a controlled busy process and verify stack capture."""
from __future__ import annotations

import csv
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
OUTPUT = BUILD / "selftest-report"


def main() -> None:
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    target = subprocess.Popen([str(BUILD / "sampler_target.exe")], cwd=BUILD)
    try:
        time.sleep(0.25)
        result = subprocess.run([
            str(BUILD / "eu4_sampler.exe"), "--pid", str(target.pid),
            "--seconds", "3", "--interval-ms", "1", "--output", str(OUTPUT),
        ], cwd=BUILD, timeout=30)
        if result.returncode != 0:
            raise SystemExit(f"Sampler failed with exit code {result.returncode}")
    finally:
        target.terminate()
        target.wait(timeout=5)

    with (OUTPUT / "functions.csv").open(newline="", encoding="utf-8") as file:
        functions = list(csv.DictReader(file))
    with (OUTPUT / "threads.csv").open(newline="", encoding="utf-8") as file:
        threads = list(csv.DictReader(file))
    with (OUTPUT / "external_callers.csv").open(newline="", encoding="utf-8") as file:
        reader = csv.DictReader(file)
        expected = "leaf_module,leaf_address,leaf_rva,caller_address,caller_rva,estimated_cpu_ms,percent_of_attributed,samples"
        if reader.fieldnames != expected.split(","):
            raise SystemExit("External-caller report has an invalid header")
    metadata = (OUTPUT / "metadata.txt").read_text(encoding="utf-8")
    inclusive = [row for row in functions if int(row["inclusive_samples"]) > 0]
    leaf = [row for row in functions if int(row["leaf_samples"]) > 0]
    if len(inclusive) < 2:
        raise SystemExit(f"Stack unwinding failed: only {len(inclusive)} target functions captured")
    if not leaf:
        raise SystemExit("No leaf samples captured")
    if not any(int(row["samples"]) > 0 for row in threads):
        raise SystemExit("No per-thread samples captured")
    if "captured_samples=0" in metadata:
        raise SystemExit("Metadata reports no captured samples")
    with (OUTPUT / "stacks.csv").open(newline="", encoding="utf-8") as file:
        stacks = list(csv.DictReader(file))
    if not stacks:
        raise SystemExit("stacks.csv is empty")
    stack_samples = sum(int(row["samples"]) for row in stacks)
    leaf_samples = sum(int(row["leaf_samples"]) for row in functions)
    if stack_samples != leaf_samples:
        raise SystemExit(f"stacks.csv samples {stack_samples} != leaf samples {leaf_samples}")
    # Корень -> лист: последний кадр каждого стека — это лист, он должен быть в leaf.csv.
    with (OUTPUT / "leaf.csv").open(newline="", encoding="utf-8") as file:
        leaf_rvas = {(row["module"], int(row["rva"], 16)) for row in csv.DictReader(file)}
    for row in stacks:
        module, rva = row["frames"].split(";")[-1].split("+0x")
        if (module, int(rva, 16)) not in leaf_rvas:
            raise SystemExit(f"Stack leaf {module}+0x{rva} is not a sampled leaf")
        frames = row["frames"].split(";")
        if len(frames) >= 2:
            parent_module, parent_rva = frames[-2].split("+0x")
            if parent_module == module and int(rva, 16) - int(parent_rva, 16) == 1:
                raise SystemExit(f"Duplicated leaf frame in stack: {row['frames']}")
    deepest = max(len(row["frames"].split(";")) for row in stacks)
    if deepest < 4:
        raise SystemExit(f"Stacks too shallow ({deepest} frames); unwinding is broken")
    print(f"PASS: {len(leaf)} leaf addresses, {len(inclusive)} inclusive stack addresses, "
          f"{len(stacks)} unique stacks, deepest {deepest} frames")
    print("Top target addresses:")
    for row in inclusive[:5]:
        print(f"  {row['rva']} leaf={row['leaf_cpu_ms']} ms inclusive={row['inclusive_cpu_ms']} ms")


if __name__ == "__main__":
    main()
