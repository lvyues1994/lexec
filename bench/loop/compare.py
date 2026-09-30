#!/usr/bin/env python3
"""Runs the lexec and stdexec loop benchmarks alternately and prints each metric's median.

usage: compare.py BUILD_DIR [RUNS]   (BUILD_DIR holds bench/lexec_loop_bench and bench/stdexec_loop_bench)
"""

import pathlib
import statistics
import subprocess
import sys


def run(executable: pathlib.Path) -> dict[str, float]:
    """Parses lines of `name key=value...` into {"name key": value}."""
    output = subprocess.run([str(executable)], check=True, capture_output=True, text=True).stdout
    metrics = {}
    for line in output.splitlines():
        name, *fields = line.split()
        for key, value in (field.split("=") for field in fields):
            metrics[f"{name} {key}"] = float(value)
    return metrics


def main() -> int:
    build = pathlib.Path(sys.argv[1])
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    results = {"lexec": [], "stdexec": []}
    for _ in range(runs):
        results["lexec"].append(run(build / "bench" / "lexec_loop_bench"))
        results["stdexec"].append(run(build / "bench" / "stdexec_loop_bench"))
    print(f"{'metric':<40} {'lexec':>12} {'stdexec':>12}")
    for key in results["lexec"][0]:
        lexec = statistics.median(r[key] for r in results["lexec"])
        stdexec = statistics.median(r[key] for r in results["stdexec"])
        print(f"{key:<40} {lexec:>12.4g} {stdexec:>12.4g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
