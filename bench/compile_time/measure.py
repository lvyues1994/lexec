#!/usr/bin/env python3
"""Reports the median wall-clock time and the peak memory of compiling each probe.

usage: measure.py [COMPILER...]   (defaults to g++ and clang++)
"""

import os
import pathlib
import resource
import statistics
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
INCLUDE = HERE.parents[1] / "include"
PROBES = ["include_only.cpp", "deep_pipeline.cpp"]
MODES = {"-O0": ["-O0"], "-O0 -g": ["-O0", "-g"]}
RUNS = 5
# A regressing probe must fail here instead of exhausting the machine's memory.
ADDRESS_SPACE_LIMIT = 8 << 30


def limit_address_space() -> None:
    resource.setrlimit(resource.RLIMIT_AS, (ADDRESS_SPACE_LIMIT, ADDRESS_SPACE_LIMIT))


def compile_once(command: list[str]) -> tuple[float, int]:
    """Returns the wall-clock seconds and the peak resident memory in MiB."""
    start = time.perf_counter()
    process = subprocess.Popen(command, preexec_fn=limit_address_space)
    _, status, usage = os.wait4(process.pid, 0)
    seconds = time.perf_counter() - start
    if os.waitstatus_to_exitcode(status) != 0:
        raise SystemExit(f"compilation failed: {' '.join(command)}")
    return seconds, usage.ru_maxrss // 1024


def main() -> int:
    compilers = sys.argv[1:] or ["g++", "clang++"]
    with tempfile.TemporaryDirectory() as scratch:
        output = str(pathlib.Path(scratch) / "probe.o")
        for compiler in compilers:
            version = subprocess.run([compiler, "--version"], check=True, capture_output=True, text=True)
            print(version.stdout.splitlines()[0])
            for probe in PROBES:
                for mode, flags in MODES.items():
                    command = [compiler, "-std=c++17", *flags, "-c", f"-I{INCLUDE}", str(HERE / probe), "-o", output]
                    runs = [compile_once(command) for _ in range(RUNS)]
                    seconds = statistics.median(run[0] for run in runs)
                    peak = max(run[1] for run in runs)
                    print(f"  {probe:<20} {mode:<7} median {seconds:.2f}s  peak {peak} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
