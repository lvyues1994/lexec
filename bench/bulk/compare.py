#!/usr/bin/env python3
"""Runs the lexec, stdexec, and hand-written bulk benchmarks alternately and prints each
measurement's median, with the speed-up over one thread of the same implementation.

usage: compare.py BUILD_DIR [RUNS]   (BUILD_DIR holds bench/{lexec,stdexec,manual}_bulk_bench)
"""

import pathlib
import statistics
import subprocess
import sys

IMPLEMENTATIONS = ["lexec", "stdexec", "manual"]


def run(executable: pathlib.Path) -> dict[tuple[str, int], float]:
    """Parses lines of `bulk workload=W threads=T us=X` into {(W, T): X}."""
    output = subprocess.run([str(executable)], check=True, capture_output=True, text=True).stdout
    results = {}
    for line in output.splitlines():
        fields = dict(field.split("=") for field in line.split()[1:])
        results[(fields["workload"], int(fields["threads"]))] = float(fields["us"])
    return results


def main() -> int:
    build = pathlib.Path(sys.argv[1])
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    samples = {name: [] for name in IMPLEMENTATIONS}
    for _ in range(runs):
        for name in IMPLEMENTATIONS:
            samples[name].append(run(build / "bench" / f"{name}_bulk_bench"))
    medians = {
        name: {key: statistics.median(r[key] for r in samples[name]) for key in samples[name][0]}
        for name in IMPLEMENTATIONS
    }
    print(f"{'workload':<9} {'threads':>7}" + "".join(f" {name + ' us':>12} {'x':>6}" for name in IMPLEMENTATIONS))
    for workload, threads in medians["lexec"]:
        row = f"{workload:<9} {threads:>7}"
        for name in IMPLEMENTATIONS:
            time = medians[name][(workload, threads)]
            row += f" {time:>12.1f} {medians[name][(workload, 1)] / time:>6.2f}"
        print(row)
    return 0


if __name__ == "__main__":
    sys.exit(main())
