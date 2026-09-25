#!/usr/bin/env python3
"""Runs the lexec and stdexec pool benchmarks alternately and prints each metric's median.

usage: compare.py BUILD_DIR [RUNS]   (BUILD_DIR holds bench/lexec_pool_bench and bench/stdexec_pool_bench)
"""

import pathlib
import statistics
import subprocess
import sys

# Fields that name a benchmark's configuration rather than a measurement.
CONFIGURATION = {"submitters"}


def run(executable: pathlib.Path) -> dict[str, float]:
    """Parses lines of `name key=value...` into {"name config key": value}."""
    output = subprocess.run([str(executable)], check=True, capture_output=True, text=True).stdout
    metrics = {}
    for line in output.splitlines():
        name, *fields = line.split()
        pairs = [field.split("=") for field in fields]
        config = " ".join(f"{key}={value}" for key, value in pairs if key in CONFIGURATION)
        for key, value in pairs:
            if key not in CONFIGURATION:
                metrics[" ".join(part for part in (name, config, key) if part)] = float(value)
    return metrics


def main() -> int:
    build = pathlib.Path(sys.argv[1])
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    results = {"lexec": [], "stdexec": []}
    for _ in range(runs):
        results["lexec"].append(run(build / "bench" / "lexec_pool_bench"))
        results["stdexec"].append(run(build / "bench" / "stdexec_pool_bench"))
    print(f"{'metric':<50} {'lexec':>12} {'stdexec':>12}")
    for key in results["lexec"][0]:
        lexec = statistics.median(r[key] for r in results["lexec"])
        stdexec = statistics.median(r[key] for r in results["stdexec"])
        print(f"{key:<50} {lexec:>12.4g} {stdexec:>12.4g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
